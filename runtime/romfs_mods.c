// Loose RomFS mod files in place of sts.ard entries, switchable while the game runs.
//
// Layout, under the mods folder of runtime/mods.c (XCX_MOD_DIR, else "mods" beside xcx.exe):
//   romfs-enabled.txt      one mod id per line, '#' starts a comment; earlier lines win when two mods have a file
//   <id>/romfs/<path>      a file that replaces (or adds) the game file <path>, e.g. <id>/romfs/chr/np/np009001.wimdo
//
// The archive device (sts.arh/sts.ard) keeps a list of paths it declines, which the game then opens as loose
// rom:/<path> files (recomp/romfs_mods.py). Where the archive reads that list for a path, romfs_mod_declines hands it
// a list holding just that path when an enabled mod has it; the game's open of rom:/<path> then reaches fs_host.c,
// which asks romfs_mod_file for the mod's copy.
//
// The list file is read again at most twice a second, and the file map is rebuilt when its bytes change, so a mod
// manager switches mods by rewriting romfs-enabled.txt. A changed mod applies to files the game opens after that;
// what it has already loaded stays until it loads the file again.
//
// Data tables (bdat/*.bdat) are merged instead (runtime/bdat_merge.c, docs/bdat-merge.md) when two or more enabled
// mods have the file, or when any has some of its tables as bdat-toolset JSON (bdat/<file>/<table>.json, as
// bdat/common/ITM_BeaconList.json for bdat/common.bdat). The map keeps every enabled mod's copy of such files in
// priority order, and the first open of the data table after a rebuild asks bdat_merge_serve for the merged file,
// which then stands in for the top mod's copy. When it can't be merged, the top mod's copy is served as before (or,
// with only JSON tables, the game's own copy). The game reads them only at start-up, so a list changed later can be
// merged ahead for the next start (romfs_mods_premerge).
#include "romfs_mods.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bdat_merge.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define LIST_CAP 4096          // bytes of romfs-enabled.txt
#define MOD_CAP 32             // enabled mods
#define CHECK_MS 500           // how often the list file is read again
#define AUTO_MS 1500           // with bdat_merge_auto, how long a changed list stays put before it is merged ahead

// key: lowercase game path without a leading '/'. host: the top mod's copy. For a data table or a JSON table that
// several mods have: every copy and its mod's id (alts[0] == host).
typedef struct {
    char *key, *host;
    char **alts, **ids;
    int nalts;
} ModFile;

// A data table merged since the last rebuild: its key and the file served for it (NULL: none).
typedef struct { char *key, *path; } Merged;
#define MERGED_CAP 16

static PlatMutex lock = PLAT_MUTEX_INIT;   // guards files / nfiles (shared for lookups)
static ModFile *files;
static size_t nfiles;
static uint64_t files_gen;                 // counts rebuilds, so a merge made for an older map is not kept
static char order[MOD_CAP][65];            // the enabled mods with a romfs folder, highest priority first
static int norder;
static Merged merged[MERGED_CAP];
static int nmerged;
// The data tables the game read at start-up and the mods each was made from, in priority order ("a,b,"), kept across
// rebuilds: the game doesn't read them again, so a later list that would make them otherwise needs a restart.
static Merged started[MERGED_CAP];
static int nstarted;
static char start_mount[64];               // the mount the game read them from
// The merges made ahead (romfs_mods_premerge): each data table of started and its mods then, as in started.
static Merged ahead[MERGED_CAP];
static int nahead, ahead_busy;
static uint64_t changed_at;                // when the list last changed (rebuild); under check_lock
static int auto_pending;                   // since then, not yet looked at for bdat_merge_auto; under check_lock
static int stale_now(void);
static int ahead_state(void);
static PlatMutex check_lock = PLAT_MUTEX_INIT;
static char root[4096];
static int root_state;                     // 0 not looked up, 1 found, -1 none
static char list_seen[LIST_CAP + 1];
static int64_t list_len_seen = -2;         // -2: never read
static uint64_t next_check_ms;
static int claim_logs;

static int mods_root(void) {
    if (root_state) return root_state > 0;
    root_state = -1;
    const char *configured = getenv("XCX_MOD_DIR");
    if (configured && *configured) {
        if (strlen(configured) >= sizeof root - 64) return 0;
        strcpy(root, configured);
    } else {
        if (!plat_exe_path(root, sizeof root - 64)) return 0;
        char *slash = strrchr(root, '/'), *back = strrchr(root, '\\');
        if (back && (!slash || back > slash)) slash = back;
        if (!slash) return 0;
        strcpy(slash + 1, "mods");
    }
    root_state = 1;
    return 1;
}

typedef struct {
    ModFile *v;
    size_t n, cap;
    char host[4096];
    size_t hlen;
    char key[1024];
    size_t klen;
    const char *id;                        // the mod being walked
} Walk;

static int ends(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    return n > t && !strcmp(s + n - t, tail);
}

// A data table (bdat/....bdat) or a table of one as bdat-toolset JSON (bdat/.../<table>.json).
static int mergeable(const char *key) {
    return !strncmp(key, "bdat/", 5) && strlen(key) > 10 && (ends(key, ".bdat") || ends(key, ".json"));
}

// The mod id of a host path <root>/<id>/romfs/..., into id.
static void id_of(const char *host, char *id, size_t cap) {
    size_t r = strlen(root);
    const char *p = strlen(host) > r ? host + r + 1 : host, *e = strchr(p, '/');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(id, p, n);
    id[n] = 0;
}

// Another mod's copy of a data table the map already has, after the earlier mods' copies.
static int add_alt(ModFile *f, const char *host, const char *id) {
    char **a = realloc(f->alts, (size_t)(f->nalts + 2) * sizeof *a), **d;
    if (!a) return 0;
    f->alts = a;
    if (!(d = realloc(f->ids, (size_t)(f->nalts + 2) * sizeof *d))) return 0;
    f->ids = d;
    if (!f->nalts) {
        char first[80];
        id_of(f->host, first, sizeof first);
        a[0] = f->host;
        d[0] = strdup(first);
        f->nalts = 1;
    }
    a[f->nalts] = strdup(host);
    d[f->nalts] = strdup(id);
    f->nalts++;
    return 1;
}

static void free_file(ModFile *f) {
    for (int i = 1; i < f->nalts; i++) free(f->alts[i]);
    for (int i = 0; i < f->nalts; i++) free(f->ids[i]);
    free(f->alts);
    free(f->ids);
    free(f->key);
    free(f->host);
}

static int add_file(Walk *w) {
    for (size_t i = 0; i < w->n; i++)   // an earlier mod's file wins; lists are small enough for a scan at rebuild
        if (!strcmp(w->v[i].key, w->key)) return mergeable(w->key) ? add_alt(&w->v[i], w->host, w->id) : 1;
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 256;
        ModFile *v = realloc(w->v, cap * sizeof *v);
        if (!v) return 0;
        w->v = v;
        w->cap = cap;
    }
    memset(&w->v[w->n], 0, sizeof w->v[w->n]);
    w->v[w->n].key = strdup(w->key);
    w->v[w->n].host = strdup(w->host);
    if (!w->v[w->n].key || !w->v[w->n].host) return 0;
    w->n++;
    return 1;
}

static int walk_entry(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Walk *w = ctx;
    size_t n = strlen(name), hl = w->hlen, kl = w->klen;
    if (!n || !strcmp(name, ".") || !strcmp(name, "..") || hl + n + 2 >= sizeof w->host ||
        kl + n + 2 >= sizeof w->key)
        return 1;   // skipped; plat_fs_list goes on while the callback returns nonzero
    w->host[hl] = '/';
    memcpy(w->host + hl + 1, name, n + 1);
    if (kl) w->key[kl] = '/';
    char *k = w->key + (kl ? kl + 1 : 0);
    for (size_t i = 0; i <= n; i++) k[i] = (char)tolower((unsigned char)name[i]);
    w->hlen = hl + 1 + n;
    w->klen = (kl ? kl + 1 : 0) + n;
    if (kind == PLAT_FS_DIR) plat_fs_list(w->host, walk_entry, w);
    else if (kind == PLAT_FS_FILE) add_file(w);
    w->hlen = hl;
    w->klen = kl;
    w->host[hl] = 0;
    w->key[kl] = 0;
    return 1;
}

static int by_key(const void *a, const void *b) { return strcmp(((const ModFile *)a)->key, ((const ModFile *)b)->key); }

// The Lua mods' id rule: a-z first, then a-z 0-9 . _ -, at most 64 characters.
static int valid_id(const char *id, size_t n) {
    if (!n || n > 64 || !islower((unsigned char)id[0])) return 0;
    for (size_t i = 0; i < n; i++)
        if (!islower((unsigned char)id[i]) && !isdigit((unsigned char)id[i]) && !strchr("._-", id[i])) return 0;
    return 1;
}

// Rebuilds the file map from the list file's text (NUL-terminated).
static void rebuild(const char *list) {
    Walk w = {0};
    int mods = 0;
    char line[256], ids[MOD_CAP][65];
    for (const char *p = list; *p && mods < MOD_CAP;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n && n < sizeof line && p[0] != '#') {
            memcpy(line, p, n);
            line[n] = 0;
            if (valid_id(line, n)) {
                int len = snprintf(w.host, sizeof w.host, "%s/%s/romfs", root, line);
                if (len > 0 && (size_t)len < sizeof w.host && plat_fs_kind(w.host) == PLAT_FS_DIR) {
                    w.hlen = (size_t)len;
                    w.klen = 0;
                    w.key[0] = 0;
                    w.id = line;
                    plat_fs_list(w.host, walk_entry, &w);
                    memcpy(ids[mods], line, n + 1);
                    mods++;
                } else {
                    rt_log("romfs mods: %s has no romfs folder", line);
                }
            } else if (n) {
                rt_log("romfs mods: romfs-enabled.txt has an invalid id");
            }
        }
        if (!e) break;
        p = e + 1;
    }
    if (w.n) qsort(w.v, w.n, sizeof *w.v, by_key);
    plat_mutex_lock(&lock);
    ModFile *old = files;
    size_t nold = nfiles;
    files = w.v;
    nfiles = w.n;
    files_gen++;
    memcpy(order, ids, sizeof order);
    norder = mods;
    Merged gone[MERGED_CAP];
    int ngone = nmerged;
    memcpy(gone, merged, sizeof gone);
    nmerged = 0;
    plat_mutex_unlock(&lock);
    for (int i = 0; i < ngone; i++) {
        free(gone[i].key);
        free(gone[i].path);
    }
    for (size_t i = 0; i < nold; i++) free_file(&old[i]);
    free(old);
    rt_log("romfs mods: %d mod%s, %zu file%s", mods, mods == 1 ? "" : "s", w.n, w.n == 1 ? "" : "s");
}

// Reads romfs-enabled.txt again when CHECK_MS passed, and rebuilds when it changed. One thread checks at a time;
// the others go on with the current map.
static void maybe_reload(void) {
    uint64_t now = plat_now_ms();
    if (now < __atomic_load_n(&next_check_ms, __ATOMIC_RELAXED) || !plat_mutex_trylock(&check_lock)) return;
    __atomic_store_n(&next_check_ms, now + CHECK_MS, __ATOMIC_RELAXED);
    if (mods_root()) {
        char path[4200], buf[LIST_CAP + 1];
        snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
        int64_t len = -1;
        PlatFile *f = plat_file_open(path);
        if (f) {
            len = plat_file_pread(f, buf, LIST_CAP, 0);
            plat_file_close(f);
        }
        if (len < 0) len = -1;
        buf[len > 0 ? len : 0] = 0;
        if (len != list_len_seen || (len > 0 && memcmp(buf, list_seen, (size_t)len))) {
            memcpy(list_seen, buf, (size_t)(len > 0 ? len : 0) + 1);
            list_len_seen = len;
            if (len >= 0 || nfiles) rebuild(buf);
            changed_at = now;
            auto_pending = 1;
        }
    }
    // bdat_merge_auto: once a changed list has stayed put a moment (a few Up/Down clicks are one change), it is merged
    // ahead, if the game has read its tables and they would now be made otherwise. A merge still running is waited for.
    if (auto_pending && now - changed_at >= AUTO_MS && !__atomic_load_n(&ahead_busy, __ATOMIC_ACQUIRE)) {
        auto_pending = 0;
        if (cfg_bool("XCX_BDAT_MERGE_AUTO") && stale_now() && ahead_state() != 2 && romfs_mods_premerge())
            rt_log("romfs mods: the list changed; merging its data tables ahead for the next start");
    }
    plat_mutex_unlock(&check_lock);
}

// The entry for key, or NULL; under lock.
static ModFile *find(const char *key) {
    size_t lo = 0, hi = nfiles;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int d = strcmp(files[mid].key, key);
        if (!d) return &files[mid];
        if (d < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

// The entries of key's JSON tables (bdat/common.bdat: bdat/common/<table>.json), as [*lo, *hi); under lock.
static void json_range(const char *key, size_t *lo, size_t *hi) {
    char prefix[1100];
    size_t n = strlen(key) - 5;   // without ".bdat"
    *lo = *hi = 0;
    if (n + 2 > sizeof prefix) return;
    memcpy(prefix, key, n);
    prefix[n++] = '/';
    prefix[n] = 0;
    size_t a = 0, b = nfiles;
    while (a < b) {
        size_t mid = (a + b) / 2;
        if (strcmp(files[mid].key, prefix) < 0) a = mid + 1;
        else b = mid;
    }
    *lo = a;
    while (a < nfiles && !strncmp(files[a].key, prefix, n)) a++;
    *hi = a;
}

static int is_json_of(const char *key, size_t prefix) {   // directly in the table's folder, a .json
    return !strchr(key + prefix, '/') && ends(key, ".json");
}

// Mod id's copy among an entry's copies, or NULL.
static const char *copy_of(const ModFile *f, const char *id) {
    char top[80];
    if (f->nalts < 2) {
        id_of(f->host, top, sizeof top);
        return strcmp(top, id) ? NULL : f->host;
    }
    for (int i = 0; i < f->nalts; i++)
        if (!strcmp(f->ids[i], id)) return f->alts[i];
    return NULL;
}

// The enabled mods that have a copy of data table key or JSON tables of it, highest priority first, as "a,b," into
// sig. Under the map's lock.
static void table_inputs(const char *key, char *sig, size_t cap) {
    size_t lo = 0, hi = 0, pre = strlen(key) - 4, n = 0;
    ModFile *f = find(key);
    json_range(key, &lo, &hi);
    sig[0] = 0;
    for (int m = 0; m < norder; m++) {
        int has = f && copy_of(f, order[m]);
        for (size_t i = lo; !has && i < hi; i++) has = is_json_of(files[i].key, pre) && copy_of(&files[i], order[m]);
        if (has && n + strlen(order[m]) + 2 <= cap) n += (size_t)snprintf(sig + n, cap - n, "%s,", order[m]);
    }
}

// Per enabled mod with a copy of data table key or JSON tables of it, in priority order: those files as in[] (the
// count), with copies of the paths (and each mod's JSON list) for the caller to free with free_gathered. *merge: 1
// when they are to be merged (JSON tables, or two or more full copies). Under the map's lock.
static int gather(const char *key, BdatModFiles *in, char **mem, int *nmem, int *ok, int *merge) {
    ModFile *f = find(key);
    size_t lo = 0, hi = 0, pre = strlen(key) - 4;
    int nin = 0, full = 0, json_any = 0;
    json_range(key, &lo, &hi);
    *nmem = 0;
    *ok = 1;
    for (int m = 0; m < norder; m++) {
        const char *copy_full = f ? copy_of(f, order[m]) : NULL;
        const char **json = NULL;
        int njson = 0;
        for (size_t i = lo; i < hi && njson < 64; i++) {
            const char *p = is_json_of(files[i].key, pre) ? copy_of(&files[i], order[m]) : NULL;
            if (!p) continue;
            if (!json && !(json = calloc(64, sizeof *json))) *ok = 0;
            if (json) json[njson++] = mem[(*nmem)++] = strdup(p);
        }
        if (!copy_full && !njson) continue;
        char *id = mem[(*nmem)++] = strdup(order[m]), *copy = NULL;
        if (copy_full) copy = mem[(*nmem)++] = strdup(copy_full);
        in[nin++] = (BdatModFiles){id, copy, json, njson};
        full += copy_full != NULL;
        json_any |= njson > 0;
    }
    for (int i = 0; i < *nmem; i++) *ok &= mem[i] != NULL;
    *merge = json_any || full >= 2;
    return nin;
}

static void free_gathered(BdatModFiles *in, int nin, char **mem, int nmem) {
    for (int i = 0; i < nin; i++) free((void *)in[i].json);
    for (int i = 0; i < nmem; i++) free(mem[i]);
}

// The host path of the mod file for key (lowercase, no leading '/'), copied to out; 0 when no enabled mod has it.
// mount (the RomFS mount name; NULL when only asking whether a mod has the file) lets a data table be merged.
static int lookup(const char *key, char *out, size_t cap, const char *mount) {
    plat_mutex_lock_shared(&lock);
    if (mount && out && !strncmp(key, "bdat/", 5) && ends(key, ".bdat")) {   // the game reading a data table
        if (!start_mount[0] && strlen(mount) < sizeof start_mount) {
            plat_mutex_unlock_shared(&lock);
            plat_mutex_lock(&lock);
            if (!start_mount[0]) snprintf(start_mount, sizeof start_mount, "%s", mount);
            plat_mutex_unlock(&lock);
            plat_mutex_lock_shared(&lock);
        }
        int known = 0;
        for (int i = 0; i < nstarted; i++) known |= !strcmp(started[i].key, key);
        if (!known) {
            char sig[MOD_CAP * 66];
            table_inputs(key, sig, sizeof sig);
            plat_mutex_unlock_shared(&lock);
            plat_mutex_lock(&lock);
            known = 0;
            for (int i = 0; i < nstarted; i++) known |= !strcmp(started[i].key, key);
            if (!known && nstarted < MERGED_CAP && (started[nstarted].key = strdup(key))) {
                started[nstarted].path = strdup(sig);
                nstarted++;
            }
            plat_mutex_unlock(&lock);
            plat_mutex_lock_shared(&lock);
        }
    }
    ModFile *f = find(key);
    size_t lo = 0, hi = 0, nj = 0;
    int table = !strncmp(key, "bdat/", 5) && ends(key, ".bdat");
    if (table) {
        json_range(key, &lo, &hi);
        for (size_t i = lo; i < hi; i++) nj += (size_t)is_json_of(files[i].key, strlen(key) - 4);
    }
    if (!f && !nj) {
        plat_mutex_unlock_shared(&lock);
        return 0;
    }
    int merge = table && (nj || f->nalts >= 2);
    if (!merge || !mount || !out) {
        if (out && f && strlen(f->host) < cap) strcpy(out, f->host);
        plat_mutex_unlock_shared(&lock);
        return f || !out;
    }
    for (int i = 0; i < nmerged; i++)
        if (!strcmp(merged[i].key, key)) {
            int ok = merged[i].path && strlen(merged[i].path) < cap;
            if (ok) strcpy(out, merged[i].path);
            plat_mutex_unlock_shared(&lock);
            return ok;
        }
    // the first open since the rebuild: merged outside the map's lock, from copies of the paths, per mod in priority
    // order its full copy (if any) and its JSON tables
    uint64_t gen = files_gen;
    BdatModFiles in[MOD_CAP];
    char *mem[MOD_CAP * 66];
    int nmem = 0, ok = 1, many = 0;
    int nin = gather(key, in, mem, &nmem, &ok, &many);
    (void)many;   // decided above, from the map as the game reads it
    plat_mutex_unlock_shared(&lock);
    char path[4096];
    ok = ok && bdat_merge_serve(mount, key, in, nin, path, sizeof path);
    const char *served = ok ? path : NULL;
    for (int i = 0; !served && i < nin; i++) served = in[i].full;   // not merged: the top mod's whole copy
    if (served && strlen(served) < cap) strcpy(out, served);
    else served = NULL;
    plat_mutex_lock(&lock);
    int known = 0;
    for (int i = 0; i < nmerged; i++) known |= !strcmp(merged[i].key, key);
    if (files_gen == gen && !known && nmerged < MERGED_CAP) {
        merged[nmerged].key = strdup(key);
        merged[nmerged].path = served ? strdup(served) : NULL;
        if (merged[nmerged].key) nmerged++;
    }
    plat_mutex_unlock(&lock);
    free_gathered(in, nin, mem, nmem);
    return served != NULL;
}

// key from a game path: drops a "<mount>:" prefix and leading slashes, lowercases.
static int make_key(const char *path, char *key, size_t cap) {
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;
    while (*path == '/' || *path == '\\') path++;
    size_t n = strlen(path);
    if (!n || n >= cap) return 0;
    for (size_t i = 0; i <= n; i++) key[i] = path[i] == '\\' ? '/' : (char)tolower((unsigned char)path[i]);
    return 1;
}

// A printable guest C string at p (at least 2 characters), copied to out; 0 when there is none.
static int guest_string(uint64_t p, char *out, size_t cap) {
    if (!p || !plat_mem_readable(p, 1)) return 0;
    size_t n = 0;
    for (; n < cap - 1; n++) {
        if (n && ((p + n) & 0xfff) == 0 && !plat_mem_readable(p + n, 1)) return 0;
        unsigned char ch = *(const unsigned char *)(uintptr_t)(p + n);
        if (!ch) break;
        if (ch < 32 || ch >= 127) return 0;
        out[n] = (char)ch;
    }
    out[n] = 0;
    return n >= 2 && n < cap - 1;
}

uint64_t romfs_mod_declines(uint64_t list, uint64_t path) {
    maybe_reload();
    if (!__atomic_load_n(&nfiles, __ATOMIC_RELAXED)) return list;
    // One entry, then the empty string that ends the list. Thread-local: the archive walks it right away on this thread.
    static __thread char one[1026];
    char raw[1024];
    if (!guest_string(path, raw, sizeof raw)) return list;
    if (!make_key(raw, one, sizeof one - 1) || !lookup(one, NULL, 0, NULL)) return list;
    one[strlen(one) + 1] = 0;
    if (__atomic_fetch_add(&claim_logs, 1, __ATOMIC_RELAXED) < 64) rt_log("romfs mods: %s from a mod", one);
    return (uint64_t)(uintptr_t)one;
}

int romfs_mod_file(const char *path, char *out, size_t cap) {
    maybe_reload();
    if (!__atomic_load_n(&nfiles, __ATOMIC_RELAXED) || !path) return 0;
    char key[1024], mount[64] = "";
    const char *colon = strchr(path, ':');
    if (colon && (size_t)(colon - path) < sizeof mount) {
        memcpy(mount, path, (size_t)(colon - path));
        mount[colon - path] = 0;
    }
    return make_key(path, key, sizeof key) && lookup(key, out, cap, *mount ? mount : NULL);
}

// ---- switching, for the F1 menu (runtime/mod_library.c) ------------------------------------------------------------------------------------------

// The ids of romfs-enabled.txt, in order, into ids (each 65 bytes); the count.
static int read_enabled(char (*ids)[65], int cap) {
    char path[4200];
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    int n = 0;
    for (char *p = text; p && *p && n < cap;) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        if (p[0] != '#' && valid_id(p, len)) {
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= strlen(ids[i]) == len && !memcmp(ids[i], p, len);
            if (!dup) {
                memcpy(ids[n], p, len);
                ids[n++][len] = 0;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    free(text);
    return n;
}

const char *romfs_mods_dir(void) { return mods_root() ? root : NULL; }

void romfs_mods_reload(void) {
    plat_mutex_lock(&check_lock);
    list_len_seen = -2;   // rebuilt from the list at the next check even when its bytes are the same
    plat_mutex_unlock(&check_lock);
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);
}

int romfs_mods_set(const char *id, int on) {
    if (!mods_root() || !id || !valid_id(id, strlen(id))) return 0;
    char ids[MOD_CAP][65];
    int n = read_enabled(ids, MOD_CAP);
    char text[LIST_CAP];
    size_t len = 0;
    if (on) len += (size_t)snprintf(text, sizeof text, "%s\n", id);   // a mod turned on goes first, so its files win
    for (int i = 0; i < n && len + 66 < sizeof text; i++)
        if (strcmp(ids[i], id)) len += (size_t)snprintf(text + len, sizeof text - len, "%s\n", ids[i]);
    plat_file_mkdir_p(root);
    char path[4200];
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
    int ok = plat_file_write_atomic(path, text, len);
    rt_log("romfs mods: %s turned %s from the menu%s", id, on ? "on" : "off", ok ? "" : " (the list could not be written)");
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);   // the next file lookup reads the new list
    return ok;
}

int romfs_mod_has_other_files(const char *id) {
    maybe_reload();
    plat_mutex_lock_shared(&lock);
    int other = 0;
    for (size_t i = 0; !other && i < nfiles; i++)
        other = strncmp(files[i].key, "bdat/", 5) && copy_of(&files[i], id);
    plat_mutex_unlock_shared(&lock);
    return other;
}

int romfs_mods_tables_changed(void) {
    maybe_reload();
    return stale_now();
}

static int stale_now(void) {
    plat_mutex_lock_shared(&lock);
    int changed = 0;
    for (int i = 0; !changed && i < nstarted; i++) {
        char sig[MOD_CAP * 66];
        table_inputs(started[i].key, sig, sizeof sig);
        changed = !started[i].path || strcmp(sig, started[i].path) != 0;
    }
    plat_mutex_unlock_shared(&lock);
    return changed;
}

// ---- merging ahead for the next start

static void premerge_run(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);   // the game plays meanwhile; bdat_merge_serve's helpers match it
    maybe_reload();
    char keys[MERGED_CAP][1024], mount[64];
    plat_mutex_lock_shared(&lock);
    int nk = 0;
    for (int i = 0; i < nstarted; i++)
        if (strlen(started[i].key) < sizeof keys[0]) snprintf(keys[nk++], sizeof keys[0], "%s", started[i].key);
    snprintf(mount, sizeof mount, "%s", start_mount);
    plat_mutex_unlock_shared(&lock);
    bdat_merge_next_clear();
    Merged done[MERGED_CAP];
    int nd = 0;
    uint64_t t0 = plat_now_ms();
    for (int k = 0; k < nk; k++) {
        BdatModFiles in[MOD_CAP];
        char *mem[MOD_CAP * 66], sig[MOD_CAP * 66];
        int nmem = 0, ok = 1, merge = 0;
        plat_mutex_lock_shared(&lock);
        table_inputs(keys[k], sig, sizeof sig);
        int nin = gather(keys[k], in, mem, &nmem, &ok, &merge);
        plat_mutex_unlock_shared(&lock);
        // as the next start would: merged where there is something to merge; else one mod's copy or the game's own
        if (ok && merge && mount[0]) bdat_merge_prepare(mount, keys[k], in, nin);
        free_gathered(in, nin, mem, nmem);
        done[nd].key = strdup(keys[k]);
        done[nd].path = strdup(sig);
        nd++;
    }
    rt_log("romfs mods: %d data table%s merged ahead for the next start, in %llu ms", nk, nk == 1 ? "" : "s",
           (unsigned long long)(plat_now_ms() - t0));
    plat_mutex_lock(&lock);
    Merged gone[MERGED_CAP];
    int ngone = nahead;
    memcpy(gone, ahead, sizeof gone);
    memcpy(ahead, done, sizeof done);
    nahead = nd;
    plat_mutex_unlock(&lock);
    for (int i = 0; i < ngone; i++) {
        free(gone[i].key);
        free(gone[i].path);
    }
    __atomic_store_n(&ahead_busy, 0, __ATOMIC_RELEASE);
}

int romfs_mods_premerge(void) {
    plat_mutex_lock_shared(&lock);
    int any = nstarted > 0 && start_mount[0];
    plat_mutex_unlock_shared(&lock);
    int idle = 0;
    if (!any || !__atomic_compare_exchange_n(&ahead_busy, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 0;
    // bdat_merge_serve keeps several hundred KB of paths on its stack
    if (!plat_thread_spawn(premerge_run, NULL, "bdat premerge", 8u << 20)) {
        __atomic_store_n(&ahead_busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

int romfs_mods_premerge_state(void) {
    if (__atomic_load_n(&ahead_busy, __ATOMIC_ACQUIRE)) return 1;
    maybe_reload();
    return ahead_state();
}

int romfs_mods_auto_merge(void) { return cfg_bool("XCX_BDAT_MERGE_AUTO"); }

void romfs_mods_set_auto_merge(int on) {
    cfg_set("XCX_BDAT_MERGE_AUTO", on ? "1" : "0");
    cfg_save();
    plat_mutex_lock(&check_lock);
    auto_pending = on;   // turned on with the list already changed: merged ahead at the next check
    plat_mutex_unlock(&check_lock);
}

static int ahead_state(void) {
    if (__atomic_load_n(&ahead_busy, __ATOMIC_ACQUIRE)) return 1;
    plat_mutex_lock_shared(&lock);
    int same = nahead > 0 && nahead >= nstarted;
    for (int i = 0; same && i < nahead; i++) {
        char sig[MOD_CAP * 66];
        table_inputs(ahead[i].key, sig, sizeof sig);
        same = ahead[i].path && !strcmp(sig, ahead[i].path);
    }
    plat_mutex_unlock_shared(&lock);
    return same ? 2 : 0;
}

int romfs_mods_priority(char (*ids)[65], int cap) {
    if (!mods_root() || cap <= 0) return 0;
    return read_enabled(ids, cap < MOD_CAP ? cap : MOD_CAP);
}

int romfs_mods_move(const char *id, int up) {
    if (!mods_root() || !id) return 0;
    char ids[MOD_CAP][65], tmp[65];
    int n = read_enabled(ids, MOD_CAP), i = 0;
    while (i < n && strcmp(ids[i], id)) i++;
    int j = up ? i - 1 : i + 1;
    if (i == n || j < 0 || j >= n) return 0;
    memcpy(tmp, ids[i], sizeof tmp);
    memcpy(ids[i], ids[j], sizeof tmp);
    memcpy(ids[j], tmp, sizeof tmp);
    char text[LIST_CAP], path[4200];
    size_t len = 0;
    for (int k = 0; k < n && len + 66 < sizeof text; k++) len += (size_t)snprintf(text + len, sizeof text - len, "%s\n", ids[k]);
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
    int ok = plat_file_write_atomic(path, text, len);
    rt_log("romfs mods: %s moved %s in the priority list from the menu%s", id, up ? "up" : "down",
           ok ? "" : " (the list could not be written)");
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);
    return ok;
}
