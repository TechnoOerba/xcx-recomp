// The game side of the BDAT merge (bdat_merge.h, bdat_merge_serve): the game's own copy of a file read from its
// archive, the merged file kept in <profile>/bdat-merge/, and status lines for the F1 Mods tab.
//
// The archive: sts.arh holds 'arh2', the entry count, the alignment and a 0, then per entry the XXH64 (seed 0) of
// its path ("/bdat/common.bdat"), its stored size and its unpacked size; the entries' data follows each other in
// sts.ard, each starting at a multiple of the alignment. A stored entry that starts with 'xbc1' is packed: type (3 =
// zstd), unpacked size, packed size, 32 more bytes, then the data.
//
// The merged file is named after a SHA-256 of the game's copy and the mods' copies in order, so a set of inputs is
// merged once and a change to any of them (a mod update, another order) makes a new file. Older merges of the same
// game file are deleted. Next to the file, <file>.txt lists the mods, one line per event (bdat_merge.h) and, last,
// the summary, which a later start shows again when it reuses the file.
#include "bdat_merge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "fs.h"
#include "host.h"
#include "romfs_mods.h"
#include "sha256.h"
#include "platform/platform.h"

#define VERSION "bdat-merge 3"   // part of the cache name: a change to the merge rules makes new files
#define LOG_NOTES 20             // event lines in the log per merge; the report file has all of them
#define STATUS_CAP 48
#define STATUS_EVENTS 8          // left-out and caveat lines per merge in the Mods tab; the report has all

static PlatMutex lock = PLAT_MUTEX_INIT;     // one merge at a time; guards the archive index and the status lines
static uint8_t *arh;
static uint64_t arh_size;
static int arh_state;                        // 0 not read, 1 read, -1 missing
static char status[STATUS_CAP][320];
static int status_kind[STATUS_CAP];
static int nstatus;

// ---- XXH64, seed 0 (the archive's path hash), after the xxHash specification
#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull
static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t round64(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
static uint64_t merge64(uint64_t h, uint64_t v) { return (h ^ round64(0, v)) * P1 + P4; }

static uint64_t xxh64(const uint8_t *d, size_t n) {
    size_t i = 0;
    uint64_t h;
    if (n >= 32) {
        uint64_t v[4] = {P1 + P2, P2, 0, (uint64_t)0 - P1};
        for (; i + 32 <= n; i += 32)
            for (int k = 0; k < 4; k++) v[k] = round64(v[k], rd64(d + i + 8 * k));
        h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
        for (int k = 0; k < 4; k++) h = merge64(h, v[k]);
    } else {
        h = P5;
    }
    h += n;
    for (; i + 8 <= n; i += 8) h = rotl(h ^ round64(0, rd64(d + i)), 27) * P1 + P4;
    if (i + 4 <= n) {
        h = rotl(h ^ (uint64_t)rd32(d + i) * P1, 23) * P2 + P3;
        i += 4;
    }
    for (; i < n; i++) h = rotl(h ^ d[i] * P5, 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    return h ^ (h >> 32);
}

// ---- the game's copy

// n bytes at off of RomFS file name (all of it for n == UINT64_MAX) into a new buffer; NULL on failure.
static uint8_t *rom_read(const char *mount, const char *name, uint64_t off, uint64_t n, uint64_t *got) {
    char path[512];
    uint64_t h;
    snprintf(path, sizeof path, "%s:/%s", mount, name);
    if (romfs_open(&h, path, FS_OPEN_READ)) return NULL;
    int64_t size = 0;
    uint8_t *p = NULL;
    if (!romfs_size(&size, h) && size >= 0 && off <= (uint64_t)size) {
        if (n == UINT64_MAX || n > (uint64_t)size - off) n = (uint64_t)size - off;
        uint64_t done = 0;
        if (n < (1ull << 31) && (p = malloc(n ? n : 1)) && romfs_read(&done, h, (int64_t)off, p, n) == 0 && done == n) {
            *got = n;
        } else {
            free(p);
            p = NULL;
        }
    }
    romfs_close(h);
    return p;
}

static uint8_t *game_copy(const char *mount, const char *key, size_t *n, char *why, size_t cap) {
    if (!arh_state) {
        arh = rom_read(mount, "sts.arh", 0, UINT64_MAX, &arh_size);
        arh_state = arh && arh_size >= 16 && !memcmp(arh, "arh2", 4) && 16 + 16ull * rd32(arh + 4) <= arh_size &&
                    rd32(arh + 8) && !(rd32(arh + 8) & (rd32(arh + 8) - 1)) ? 1 : -1;
    }
    if (arh_state < 0) {
        snprintf(why, cap, "the game's archive index sts.arh can't be read");
        return NULL;
    }
    char path[1100];
    int len = snprintf(path, sizeof path, "/%s", key);
    uint64_t want = xxh64((const uint8_t *)path, (size_t)len), off = 0, align = rd32(arh + 8);
    uint32_t count = rd32(arh + 4);
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *e = arh + 16 + 16ull * k;
        uint32_t stored = rd32(e + 8);
        if (rd64(e) != want) {
            off += (stored + align - 1) & ~(align - 1);
            continue;
        }
        uint64_t got = 0;
        uint8_t *p = rom_read(mount, "sts.ard", off, stored, &got);
        if (!p || got != stored) {
            free(p);
            snprintf(why, cap, "the game's %s can't be read from sts.ard", key);
            return NULL;
        }
        if (got < 48 || memcmp(p, "xbc1", 4)) {
            *n = got;
            return p;
        }
        uint32_t type = rd32(p + 4), un = rd32(p + 8), packed = rd32(p + 12);
        uint8_t *out = type == 3 && packed <= got - 48 && un < (1u << 30) ? malloc(un ? un : 1) : NULL;
        size_t r = out ? ZSTD_decompress(out, un, p + 48, packed) : 0;
        free(p);
        if (!out || ZSTD_isError(r) || r != un) {
            free(out);
            snprintf(why, cap, "the game's %s can't be unpacked (type %u)", key, type);
            return NULL;
        }
        *n = un;
        return out;
    }
    snprintf(why, cap, "the game has no %s", key);
    return NULL;
}

// ---- status lines and events

// kind 0: a summary line; 1: a change left out; 2: a caveat; 3: a mod that adds nothing. The oldest lines go first when the list is full.
static void add_status(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_status(int kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (nstatus == STATUS_CAP) {
        memmove(status, status + 1, sizeof status[0] * (STATUS_CAP - 1));
        memmove(status_kind, status_kind + 1, sizeof status_kind[0] * (STATUS_CAP - 1));
        nstatus--;
    }
    status_kind[nstatus] = kind;
    vsnprintf(status[nstatus++], sizeof status[0], fmt, ap);
    va_end(ap);
}

int bdat_merge_status(int i, char *out, size_t cap, int *kind) {
    plat_mutex_lock_shared(&lock);
    int ok = i >= 0 && i < nstatus;
    if (ok) {
        snprintf(out, cap, "%s", status[i]);
        if (kind) *kind = status_kind[i];
    }
    plat_mutex_unlock_shared(&lock);
    return ok;
}

typedef struct { char *text; size_t n, cap; int lines; const char *key; } Notes;

static void add_line(Notes *s, const char *line) {
    size_t len = strlen(line);
    if (s->n + len + 2 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        while (cap < s->n + len + 2) cap *= 2;
        char *t = realloc(s->text, cap);
        if (!t) return;
        s->text = t;
        s->cap = cap;
    }
    memcpy(s->text + s->n, line, len);
    s->n += len;
    s->text[s->n++] = '\n';
    s->text[s->n] = 0;
}

static void on_note(void *ctx, const char *line) {
    Notes *s = ctx;
    if (s->lines++ < LOG_NOTES) rt_log("bdat merge: %s: %s", s->key, line);
    else if (s->lines == LOG_NOTES + 1) rt_log("bdat merge: %s: more in the report file", s->key);
    add_line(s, line);
}

// The left-out and caveat lines of a report (one event per line), up to STATUS_EVENTS, into the status lines; then
// up to STATUS_EVENTS of its mods that add nothing, so those never push a left-out change out of view.
static void status_events(const char *key, const char *text) {
    int more = 0;
    for (int pass = 0; pass < 2; pass++) {
        int shown = 0;
        for (const char *p = text; p && *p;) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            int kind = !strncmp(p, "left out: ", 10) ? 1 : !strncmp(p, "caveat: ", 8) ? 2 :
                       !strncmp(p, "adds nothing: ", 14) ? 3 : 0;
            if (kind && (kind == 3) == pass) {
                if (shown < STATUS_EVENTS) {
                    add_status(kind, "%s: %.*s", key, (int)n, p);
                    shown++;
                } else {
                    more++;
                }
            }
            p = e ? e + 1 : NULL;
        }
    }
    if (more) add_status(0, "%s: %d more in the report", key, more);
}

// ---- the cache

typedef struct { const char *suffix; const char *keep; char dir[2100]; } Prune;

static int prune_entry(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Prune *p = ctx;
    size_t n = strlen(name), s = strlen(p->suffix);
    const char *tail = n > s ? name + n - s : NULL;
    int ours = kind == PLAT_FS_FILE && tail && (!strcmp(tail, p->suffix) ||
               (n > s + 4 && !strcmp(name + n - 4, ".txt") && !strncmp(name + n - 4 - s, p->suffix, s)));
    if (ours && strncmp(name, p->keep, strlen(p->keep))) {
        char path[2400];
        snprintf(path, sizeof path, "%s/%s", p->dir, name);
        plat_file_delete(path);   // may fail while the game still has it open; then the next merge deletes it
    }
    return 1;
}

static void hex(const uint8_t *d, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

// The status line of a merge: what was done and how much didn't go to plan.
static void summary_line(char *out, size_t cap, int n, const BdatSummary *s, char (*label)[160]) {
    if (s->file_mode == BDAT_FILE_COPIED && n == 1 && s->left_out)
        snprintf(out, cap, "%s's tables put in, %d left out (no other mod changes this file)", label[0], s->left_out);
    else if (s->file_mode == BDAT_FILE_COPIED && n == 1)
        snprintf(out, cap, "%s's tables put in (no other mod changes this file)", label[0]);
    else if (s->file_mode == BDAT_FILE_COPIED)
        snprintf(out, cap, "only %s's copy is used (the other mods' copies can't be)", label[s->file_mod]);
    else if (s->file_mode == BDAT_FILE_WHOLE)
        snprintf(out, cap, "%s's whole copy is used (it has another list of tables)", label[s->file_mod]);
    else {
        int w = snprintf(out, cap, "%d mods merged: %d table%s from several mods, %d settled by priority, %d change%s "
                         "left out, %d caveat%s", n, s->tables_merged, s->tables_merged == 1 ? "" : "s", s->priority,
                         s->left_out, s->left_out == 1 ? "" : "s", s->caveats, s->caveats == 1 ? "" : "s");
        if (s->adds_nothing && w > 0 && (size_t)w < cap)
            snprintf(out + w, cap - (size_t)w, ", %d mod%s adding nothing", s->adds_nothing,
                     s->adds_nothing == 1 ? "" : "s");
    }
}

// What a mod is called in events: the name in its library.ini (mods/<id>/library.ini, written by the mod managers;
// for a part of a mod, "<mod>: <part>"), else its id.
static void label_of(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "%s", id);
    const char *dir = romfs_mods_dir();
    char path[4200];
    if (!dir || snprintf(path, sizeof path, "%s/%s/library.ini", dir, id) >= (int)sizeof path) return;
    uint64_t n = 0;
    char *text = (char *)plat_file_read(path, &n);
    for (char *p = text; p && p < text + n;) {
        char *e = memchr(p, '\n', (size_t)(text + n - p)), *end = e ? e : text + n, *q = p;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (end - q > 4 && !strncmp(q, "name", 4)) {
            q += 4;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                char *v = end;
                while (v > q && (v[-1] == ' ' || v[-1] == '\t' || v[-1] == '\r')) v--;
                if (v > q) snprintf(out, cap, "%.*s", (int)(v - q), q);
                break;
            }
        }
        p = e ? e + 1 : NULL;
    }
    free(text);
}

// The file name of a host path without its folders and .json.
static void stem_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *f = slash && (!back || slash > back) ? slash + 1 : back ? back + 1 : path;
    size_t n = strlen(f);
    if (n > 5 && !strcmp(f + n - 5, ".json")) n -= 5;
    if (n >= cap) n = cap - 1;
    memcpy(out, f, n);
    out[n] = 0;
}

// The game's own copy as <profile>/bdat-merge/game-<key with / as _>, its path in out; 0 when it can't be written.
static int serve_game_copy(const char *profile, const char *key, const uint8_t *game, size_t n, char *out, size_t cap) {
    char dir[2100], file[2400];
    if (!game) return 0;
    snprintf(dir, sizeof dir, "%s/bdat-merge", profile);
    int len = snprintf(file, sizeof file, "%s/game-", dir);
    for (const char *c = key; *c && len < (int)sizeof file - 1; c++) file[len++] = *c == '/' || *c == '\\' ? '_' : *c;
    file[len] = 0;
    if ((size_t)len >= cap) return 0;
    plat_file_mkdir_p(dir);
    if (plat_file_size(file) != (int64_t)n && !plat_file_write_atomic(file, game, n)) return 0;
    snprintf(out, cap, "%s", file);
    return 1;
}

int bdat_merge_serve(const char *mount, const char *key, const BdatModFiles *mods, int n, char *out, size_t cap) {
    if (n < 1 || n > 64 || !mount || !key) return 0;
    const RtConfig *cfg = rt_config();
    if (!cfg || !cfg->profile) return 0;
    plat_mutex_lock(&lock);
    int ok = 0, full = -1;
    char why[400] = "";
    uint8_t *game = NULL, *merged = NULL;
    uint8_t *data[64] = {0};
    uint64_t size[64] = {0};
    BdatJson *json[64] = {0};
    int njson[64] = {0};
    char stems[64][64][128], label[64][160];
    for (int k = 0; k < n; k++) label_of(mods[k].id, label[k], sizeof label[k]);
    size_t game_n = 0, merged_n = 0;
    Notes notes = {.key = key};
    uint64_t t0 = plat_now_ms();
    char head[600];
    size_t hn = (size_t)snprintf(head, sizeof head, "%s merged from, highest priority first:", key);
    for (int k = 0; k < n && hn < sizeof head - 70; k++)
        hn += (size_t)snprintf(head + hn, sizeof head - hn, "%s %s", k ? ";" : "", label[k]);
    add_line(&notes, head);
    for (int k = 0; k < n; k++) {
        if (mods[k].full) {
            if (full < 0) full = k;
            if (!(data[k] = plat_file_read(mods[k].full, &size[k]))) {
                snprintf(why, sizeof why, "%s's copy can't be read", label[k]);
                goto done;
            }
        }
        if (mods[k].njson && !(json[k] = calloc((size_t)mods[k].njson, sizeof *json[k]))) goto done;
        for (int x = 0; x < mods[k].njson && x < 64; x++) {
            uint64_t sz = 0;
            uint8_t *p = plat_file_read(mods[k].json[x], &sz);
            stem_of(mods[k].json[x], stems[k][x], sizeof stems[k][x]);
            if (!p) {
                char line[400];
                snprintf(line, sizeof line, "left out: %s: its table %s.json can't be read", label[k], stems[k][x]);
                on_note(&notes, line);
                continue;
            }
            json[k][njson[k]++] = (BdatJson){stems[k][x], p, (size_t)sz};
        }
    }
    if (!(game = game_copy(mount, key, &game_n, why, sizeof why))) goto done;

    // the file name: <first 16 bytes of SHA-256(version, game copy, per mod its id, full copy and JSON tables)>-<key
    // with / as _>
    Sha256 s;
    uint8_t d[32];
    sha256_init(&s);
    sha256_update(&s, VERSION, sizeof VERSION);
    sha256(game, game_n, d);
    sha256_update(&s, d, 32);
    for (int k = 0; k < n; k++) {
        sha256_update(&s, mods[k].id, strlen(mods[k].id) + 1);
        sha256(data[k] ? data[k] : (const uint8_t *)"", data[k] ? size[k] : 0, d);
        sha256_update(&s, data[k] ? "f" : "-", 1);
        sha256_update(&s, d, 32);
        for (int x = 0; x < njson[k]; x++) {
            sha256_update(&s, json[k][x].stem, strlen(json[k][x].stem) + 1);
            sha256(json[k][x].data, json[k][x].size, d);
            sha256_update(&s, d, 32);
        }
    }
    sha256_final(&s, d);
    Prune pr;
    char name[300], flat[256], file[2400], report[2500];
    snprintf(flat, sizeof flat, "%s", key);
    for (char *c = flat; *c; c++)
        if (*c == '/' || *c == '\\') *c = '_';
    hex(d, 16, name);
    name[32] = '-';
    snprintf(name + 33, sizeof name - 33, "%s", flat);
    snprintf(pr.dir, sizeof pr.dir, "%s/bdat-merge", cfg->profile);
    snprintf(file, sizeof file, "%s/%s", pr.dir, name);
    snprintf(report, sizeof report, "%s.txt", file);
    if (strlen(file) >= cap) goto done;
    if (plat_file_size(file) > 0) {
        // made before: its report's last line is the summary, the lines before it the events
        uint64_t rn = 0;
        char *text = (char *)plat_file_read(report, &rn);
        char *last = NULL;
        if (text) {
            char *t = realloc(text, rn + 1);
            if (t) {
                text = t;
                text[rn] = 0;
                while (rn && (text[rn - 1] == '\n' || text[rn - 1] == '\r')) text[--rn] = 0;
                last = strrchr(text, '\n');
            }
        }
        snprintf(out, cap, "%s", file);
        ok = 1;
        rt_log("bdat merge: %s: the merge made before (%s)", key, file);
        add_status(0, "%s: %s (as before)", key, last ? last + 1 : "merged");
        if (last) {
            *last = 0;
            status_events(key, text);
        }
        free(text);
        goto done;
    }

    BdatInput base = {game, game_n, "game", NULL, 0}, in[64];
    for (int k = 0; k < n; k++) in[k] = (BdatInput){data[k], (size_t)size[k], label[k], json[k], njson[k]};
    BdatSummary sum;
    if (!(merged = bdat_merge(&base, in, n, &merged_n, &sum, on_note, &notes, why, sizeof why))) goto done;
    plat_file_mkdir_p(pr.dir);
    if (!plat_file_write_atomic(file, merged, merged_n)) {
        snprintf(why, sizeof why, "%s can't be written", file);
        goto done;
    }
    char line[400];
    summary_line(line, sizeof line, n, &sum, label);
    add_line(&notes, line);
    plat_file_write_atomic(report, notes.text, notes.n);
    pr.suffix = flat;
    pr.keep = name;
    plat_fs_list(pr.dir, prune_entry, &pr);
    snprintf(out, cap, "%s", file);
    ok = 1;
    rt_log("bdat merge: %s: %s, in %llu ms (report %s)", key, line, (unsigned long long)(plat_now_ms() - t0), report);
    add_status(0, "%s: %s", key, line);
    status_events(key, strchr(notes.text, '\n') ? strchr(notes.text, '\n') + 1 : "");
done:
    if (!ok && full >= 0) {
        rt_log("bdat merge: %s: not merged (%s); %s's whole file is used", key, why[0] ? why : "no room for the path",
               label[full]);
        add_status(1, "%s: not merged (%s); %s's whole file is used", key, why, label[full]);
    } else if (!ok) {
        // only JSON tables, and they can't be put in: the game's own copy, so the game still finds the file
        ok = serve_game_copy(cfg->profile, key, game, game_n, out, cap);
        rt_log("bdat merge: %s: not merged (%s); %s", key, why[0] ? why : "no room for the path",
               ok ? "the game's own copy is used" : "the game's own copy can't be served either");
        add_status(1, "%s: not merged (%s); %s", key, why, ok ? "the game's own copy is used" : "nothing can be served");
    }
    plat_mutex_unlock(&lock);
    for (int k = 0; k < n; k++) {
        free(data[k]);
        for (int x = 0; x < njson[k]; x++) free((void *)json[k][x].data);
        free(json[k]);
    }
    free(game);
    free(merged);
    free(notes.text);
    return ok;
}
