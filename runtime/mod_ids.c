// Numbers handed out to content mods (docs/mod-ids.md).
//
// A content mod adds things the game finds by number: a map region (its id is a FLD_FieldNameList row id and a byte
// of the world raster), a mod flag (runtime/mod_state.c), a survey reward's claimed index, a new row of a table other
// tables point at by row id (a place name's text, a name condition). Two authors who never heard of each other pick the
// same "free" numbers, and the data table merge would then take their rows for the same row. So a mod's mod.json names
// each such thing, with the number its own files use for it, and this file gives every name a number no other enabled
// mod has:
// - a name that has a number from an earlier start keeps it while no one else holds it, also when its mod was off in
//   between: numbers stay the same across starts, and turning one mod off never moves another's;
// - a new name gets the number its files use when that is free, else the first free one of its kind; mods higher in
//   priority are served first, so of two old mods that picked the same numbers, the lower one moves;
// - the numbers are kept in <profile>/mod-ids.txt; when a kind runs out, the oldest records of mods that are off give
//   theirs back.
// The data table merge (runtime/bdat_merge.c) then moves each mod's numbers in its tables before they are combined,
// and the mod state keys its flags and rewards by the numbers given.
#include "mod_ids.h"
#include "cfg.h"
#include "host.h"
#include "mod_state.h"
#include "platform/platform.h"
#include "romfs_mods.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_KINDS_NAME(k) bdat_id_kind_name(k)
static const uint32_t ID_RANGE[BDAT_ID_KINDS][2] = {
    {24, 255},           // regions past the game's 24 segment slots (ids 0-23); the world raster holds a byte
    {MOD_FLAG_FIRST, MOD_FLAG_NONE - 1},   // mod flags (mod_state.h MOD_IS_FLAG)
    {MOD_REWARD_FIRST, 0xFFFF},            // mod survey reward rows (a 16-bit flag_index)
    {0, 0xFFFF},         // rows: from the lowest number any mod's files use for that table (they come after the game's)
    {MOD_SITE_FIRST, MOD_SITE_LAST},       // FrontierNav sites: the FrontierNav manager's rows past the game's
};

static int id_kind_of(const char *s, size_t n) {
    for (int k = 0; k < BDAT_ID_KINDS; k++)
        if (strlen(ID_KINDS_NAME(k)) == n && !memcmp(ID_KINDS_NAME(k), s, n)) return k;
    return -1;
}

// ---- the records file

// One line of the records file into *r: "<kind> <table> <number> <mod> <name>", the name the rest after one space, as
// written. 0 when it doesn't read so or holds a number outside its kind's range (a hand edit, a damaged file).
static int parse_line(const char *line, ModIdRecord *r) {
    char kind[16], table[16], num[16], mod[64], *end;
    int at = 0;
    if (sscanf(line, "%15s %15s %15s %63s%n", kind, table, num, mod, &at) != 4 || line[at] != ' ' || !line[at + 1])
        return 0;
    at++;
    int k = id_kind_of(kind, strlen(kind));
    unsigned long long v = strtoull(num, &end, 10);
    if (k < 0 || num[0] < '0' || num[0] > '9' || *end || v < ID_RANGE[k][0] || v > ID_RANGE[k][1] ||
        strlen(line + at) >= sizeof r->name)
        return 0;
    memset(r, 0, sizeof *r);
    snprintf(r->mod, sizeof r->mod, "%s", mod);
    r->kind = (uint8_t)k;
    r->number = (uint32_t)v;
    r->table = k == BDAT_ID_ROW && strcmp(table, "-") ? (uint32_t)strtoul(table, NULL, 16) : 0;
    snprintf(r->name, sizeof r->name, "%s", line + at);
    return 1;
}

int mod_ids_parse(const char *text, size_t len, ModIdRecord *recs, int cap, int *bad) {
    int n = 0;
    *bad = 0;
    const char *p = text, *end = text + len;
    while (p < end && n < cap) {
        const char *nl = memchr(p, '\n', (size_t)(end - p)), *e = nl ? nl : end;
        char line[400];
        size_t ln = (size_t)(e - p);
        if (ln && p[ln - 1] == '\r') ln--;
        if (ln && p[0] != '#') {
            if (ln < sizeof line) {
                memcpy(line, p, ln);
                line[ln] = 0;
            }
            if (ln < sizeof line && parse_line(line, &recs[n])) n++;
            else (*bad)++;
        }
        p = nl ? nl + 1 : end;
    }
    return n;
}

char *mod_ids_format(const ModIdRecord *recs, int n) {
    size_t cap = 256 + (size_t)n * 200, w = 0;
    char *o = malloc(cap);
    if (!o) return NULL;
    w += (size_t)snprintf(o + w, cap - w, "# Numbers handed out to content mods (docs/mod-ids.md): kind, table, number, "
                                          "mod, name.\n# A name keeps its number for as long as it is listed here.\n");
    for (int i = 0; i < n; i++) {
        char table[12] = "-";
        if (recs[i].kind == BDAT_ID_ROW) snprintf(table, sizeof table, "%08X", recs[i].table);
        w += (size_t)snprintf(o + w, cap - w, "%s %s %u %s %s\n", ID_KINDS_NAME(recs[i].kind), table, recs[i].number,
                              recs[i].mod, recs[i].name);
    }
    return o;
}

// ---- the rules

typedef struct { uint8_t *bits; } Taken;   // per (kind, table): numbers held, 65536 bits

static int same_key(const ModIdRecord *r, const char *mod, uint8_t kind, uint32_t table, const char *name) {
    return r->kind == kind && r->table == table && !strcmp(r->mod, mod) && !strcmp(r->name, name);
}

static void logf_(ModIdLog log, void *ctx, int kind, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void logf_(ModIdLog log, void *ctx, int kind, const char *fmt, ...) {
    if (!log) return;
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log(ctx, kind, line);
}

int mod_ids_assign(const ModManifest *mods, int n, ModIdRecord *recs, int *nrecs, int cap, uint32_t **numbers,
                   ModIdLog log, void *ctx) {
    // the (kind, table) pairs in use, each with its bit set of numbers held
    typedef struct { uint8_t kind; uint32_t table; uint8_t *bits; } Space;
    Space sp[256];
    int nsp = 0, ok = 0;
    for (int i = 0; i < n; i++)
        for (int d = 0; d < mods[i].n; d++) numbers[i][d] = UINT32_MAX;
#define SPACE(K, T, OUT)                                                                                              \
    do {                                                                                                              \
        OUT = NULL;                                                                                                   \
        for (int s_ = 0; s_ < nsp; s_++)                                                                              \
            if (sp[s_].kind == (K) && sp[s_].table == (T)) OUT = sp[s_].bits;                                         \
        if (!OUT && nsp < 256 && (OUT = calloc(65536 / 8, 1))) sp[nsp++] = (Space){(K), (T), OUT};                    \
        if (!OUT) goto done;                                                                                          \
    } while (0)
#define HAS(B, X) ((B)[(X) >> 3] >> ((X) & 7) & 1)
#define SET(B, X) ((B)[(X) >> 3] |= (uint8_t)(1u << ((X) & 7)))
#define CLR(B, X) ((B)[(X) >> 3] &= (uint8_t)~(1u << ((X) & 7)))
    // who holds a number: the first record of it
    int *holder = malloc((size_t)(cap ? cap : 1) * sizeof *holder);
    if (!holder) return 0;
    for (int r = 0; r < *nrecs; r++) {
        uint8_t *b = NULL;
        for (int s_ = 0; s_ < nsp; s_++)
            if (sp[s_].kind == recs[r].kind && sp[s_].table == recs[r].table) b = sp[s_].bits;
        if (!b && nsp < 256 && (b = calloc(65536 / 8, 1))) sp[nsp++] = (Space){recs[r].kind, recs[r].table, b};
        holder[r] = b && recs[r].number <= 0xFFFF && !HAS(b, recs[r].number);   // past 256 tables: holds nothing
        if (holder[r]) SET(b, recs[r].number);
    }
    // 1. a name with a record keeps its number (when that record is the number's holder)
    for (int i = 0; i < n; i++)
        for (int d = 0; d < mods[i].n; d++) {
            const ModIdDecl *e = &mods[i].decls[d];
            uint32_t table = e->kind == BDAT_ID_ROW ? e->table : 0;
            for (int r = 0; r < *nrecs; r++)
                if (holder[r] && same_key(&recs[r], mods[i].id, e->kind, table, e->name)) numbers[i][d] = recs[r].number;
        }
    // 2. the rest, highest priority first: its own number when free, else the first free one. Every name whose own
    // number is free keeps it first (pass 0, all mods), so a name that moves never pushes another name, of its own mod
    // or a lower one, off a number its files use (pass 1 hands out the rest)
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++)
        for (int d = 0; d < mods[i].n; d++) {
            const ModIdDecl *e = &mods[i].decls[d];
            if (numbers[i][d] != UINT32_MAX) continue;
            uint32_t table = e->kind == BDAT_ID_ROW ? e->table : 0, lo = ID_RANGE[e->kind][0], hi = ID_RANGE[e->kind][1];
            uint8_t *b;
            SPACE(e->kind, table, b);
            if (e->kind == BDAT_ID_ROW) {
                // rows come after the game's: from the lowest number the enabled mods' files use for that table (a
                // number under the game's last row is refused by the merge, which knows where that is)
                uint32_t low = UINT32_MAX;
                for (int i2 = 0; i2 < n; i2++)
                    for (int d2 = 0; d2 < mods[i2].n; d2++) {
                        const ModIdDecl *x = &mods[i2].decls[d2];
                        if (x->kind == BDAT_ID_ROW && x->table == table && x->has_local && x->local < low) low = x->local;
                    }
                if (low != UINT32_MAX) lo = low;
            }
            uint32_t num = e->has_local ? e->local : UINT32_MAX;
            int own_free = num != UINT32_MAX && num >= lo && num <= hi && !HAS(b, num);
            if (pass == 0 && !own_free) continue;
            if (!own_free) {
                num = UINT32_MAX;
                for (uint32_t x = lo; x <= hi && num == UINT32_MAX; x++)
                    if (!HAS(b, x)) num = x;
                if (num == UINT32_MAX) {
                    // run out: the oldest record of a mod that is off gives its number back
                    for (int r = 0; r < *nrecs && num == UINT32_MAX; r++) {
                        int on = 0;
                        for (int i2 = 0; i2 < n && !on; i2++) on = !strcmp(mods[i2].id, recs[r].mod);
                        if (on || recs[r].kind != e->kind || recs[r].table != table || !holder[r]) continue;
                        num = recs[r].number;
                        logf_(log, ctx, 1, "mod ids: %s's %s \"%s\" gives back %u (its mod is off)", recs[r].mod,
                              ID_KINDS_NAME(recs[r].kind), recs[r].name, num);
                        memmove(&recs[r], &recs[r + 1], (size_t)(*nrecs - r - 1) * sizeof *recs);
                        memmove(&holder[r], &holder[r + 1], (size_t)(*nrecs - r - 1) * sizeof *holder);
                        (*nrecs)--;
                        CLR(b, num);
                    }
                }
                if (num == UINT32_MAX) {
                    logf_(log, ctx, 2, "mod ids: %s: %s \"%s\" gets no number (none is free)", mods[i].id, ID_KINDS_NAME(e->kind),
                          e->name);
                    continue;
                }
                if (e->has_local)
                    logf_(log, ctx, 1, "mod ids: %s: %s \"%s\" moves from %u to %u", mods[i].id, ID_KINDS_NAME(e->kind),
                          e->name, e->local, num);
            }
            if (*nrecs >= cap) goto done;
            SET(b, num);
            numbers[i][d] = num;
            ModIdRecord rec = {{0}, e->kind, table, {0}, num};
            snprintf(rec.mod, sizeof rec.mod, "%s", mods[i].id);
            snprintf(rec.name, sizeof rec.name, "%s", e->name);
            holder[*nrecs] = 1;
            recs[(*nrecs)++] = rec;
        }
    ok = 1;
done:
    free(holder);
    for (int s = 0; s < nsp; s++) free(sp[s].bits);
    return ok;
#undef SPACE
#undef HAS
#undef SET
#undef CLR
}

// ---- the enabled mods' hand-out

#define REC_CAP 16384
static PlatMutex ids_lock = PLAT_MUTEX_INIT;
static uint32_t ids_gen;            // romfs_mods_generation() the hand-out is for
static int ids_have;                // a hand-out was made
static ModManifest *ids_man;        // per enabled mod with a mod.json, priority order
static uint32_t **ids_nums;
static char (*ids_folder)[65];  // each one's folder (its id in romfs-enabled.txt, which mod.json's "id" may rename)
static int ids_nman;
static char (*ids_status)[200];
static int *ids_status_kind, ids_nstatus;

static void ids_on_log(void *ctx, int kind, const char *line) {
    (void)ctx;
    rt_log("%s", line);
    if (ids_nstatus < 512) {
        char (*s)[200] = realloc(ids_status, (size_t)(ids_nstatus + 1) * sizeof *ids_status);
        int *k = realloc(ids_status_kind, (size_t)(ids_nstatus + 1) * sizeof *ids_status_kind);
        if (s) ids_status = s;
        if (k) ids_status_kind = k;
        if (s && k) {
            const char *t = !strncmp(line, "mod ids: ", 9) ? line + 9 : line;
            snprintf(ids_status[ids_nstatus], sizeof ids_status[0], "%s", t);
            ids_status_kind[ids_nstatus++] = kind;
        }
    }
}

static void ids_forget(void) {
    for (int i = 0; i < ids_nman; i++) {
        mod_manifest_free(&ids_man[i]);
        free(ids_nums[i]);
    }
    free(ids_man);
    free(ids_nums);
    free(ids_folder);
    ids_folder = NULL;
    ids_man = NULL;
    ids_nums = NULL;
    ids_nman = 0;
    ids_nstatus = 0;
}

// Made again when the mods changed (romfs_mods.c rebuilt its map: the enabled list changed, or a mod was updated in
// place, which bumps its generation): their mod.json files, the records, the hand-out, the records written back.
// Otherwise nothing is read: the callers ask on every data table open and for every declaration.
static void ids_refresh_locked(void) {
    uint32_t g = romfs_mods_generation();
    if (ids_have && g == ids_gen) return;
    ids_have = 1;
    ids_gen = g;
    ids_forget();
    static char ids[ROMFS_MODS_MAX][65];
    int n = romfs_mods_enabled(ids, ROMFS_MODS_MAX);
    ids_man = calloc((size_t)(n ? n : 1), sizeof *ids_man);
    ids_nums = calloc((size_t)(n ? n : 1), sizeof *ids_nums);
    ids_folder = calloc((size_t)(n ? n : 1), sizeof *ids_folder);
    if (!ids_man || !ids_nums || !ids_folder) return;
    for (int i = 0; i < n; i++) {
        ModManifest m;
        if (mod_manifest_read(ids[i], &m) != 1) continue;
        int dup = 0;
        for (int j = 0; j < ids_nman && !dup; j++) dup = !strcmp(ids_man[j].id, m.id);
        if (dup || !(ids_nums[ids_nman] = malloc((size_t)(m.n ? m.n : 1) * sizeof **ids_nums))) {
            mod_manifest_free(&m);
            continue;
        }
        snprintf(ids_folder[ids_nman], sizeof ids_folder[0], "%s", ids[i]);
        ids_man[ids_nman++] = m;
    }
    if (!ids_nman) return;
    const RtConfig *cfg = rt_config();
    char path[1200] = "";
    if (cfg && cfg->profile) snprintf(path, sizeof path, "%s/mod-ids.txt", cfg->profile);
    ModIdRecord *recs = malloc(REC_CAP * sizeof *recs);
    if (!recs) return;
    uint64_t size = 0;
    char *text = path[0] ? (char *)plat_file_read(path, &size) : NULL;
    int bad = 0, nrecs = text ? mod_ids_parse(text, (size_t)size, recs, REC_CAP, &bad) : 0;
    if (!text && path[0] && plat_file_exists(path)) {
        // there but not readable (locked, an I/O error): numbers from no records this run, and the file never written
        // over, so the numbers it keeps come back next time
        rt_log("mod ids: %s can't be read; this run hands out numbers without it and leaves it as it is", path);
        path[0] = 0;
    } else if (bad && path[0]) {
        // lines it can't read (a hand edit, a damaged file): kept as <file>.bad before the file is written again
        char badp[1300];
        snprintf(badp, sizeof badp, "%s.bad", path);
        rt_log("mod ids: %s: %d line(s) not read as records; the file as it was is kept as %s", path, bad, badp);
        if (!plat_file_write_atomic(badp, text, (size_t)size)) path[0] = 0;
    }
    if (nrecs == REC_CAP) {   // a file cut short here: never written back over (its later records would be lost)
        rt_log("mod ids: %s has %d records or more; only those are used, and the file is left as it is", path, REC_CAP);
        path[0] = 0;
    }
    char *before = mod_ids_format(recs, nrecs);
    int read = nrecs, wrote = 0;
    if (!mod_ids_assign(ids_man, ids_nman, recs, &nrecs, REC_CAP, ids_nums, ids_on_log, NULL)) {
        // out of memory or room (REC_CAP records): the names handed out so far keep their numbers this run, the rest
        // get none (each said by the lines above as "gets no number" only when a range ran out)
        rt_log("mod ids: the hand-out stopped early (out of memory, or more than %d records in %s); some names got no "
               "number this run and the records are not written", REC_CAP, path[0] ? path : "(no profile)");
    } else {
        char *after = mod_ids_format(recs, nrecs);
        if (after && path[0] && (!before || strcmp(before, after))) {
            wrote = plat_file_write_atomic(path, after, strlen(after)) != 0;
            if (!wrote) rt_log("mod ids: %s can't be written; the numbers given this run may differ next time", path);
        }
        free(after);
    }
    rt_log("mod ids: %d mod(s) with a mod.json; %s: %d record(s) read, %d now%s", ids_nman, path[0] ? path : "(no profile)",
           read, nrecs, wrote ? ", written" : "");
    free(before);
    free(text);
    free(recs);
}

static void ids_refresh(void) {
    plat_mutex_lock(&ids_lock);
    ids_refresh_locked();
    plat_mutex_unlock(&ids_lock);
}

void mod_ids_refresh(void) { ids_refresh(); }

// By mod.json's id (the mod state's name for a mod) or by its folder (the mods list's and the merge's).
static int ids_find(const char *mod, int folder) {
    for (int i = 0; i < ids_nman; i++)
        if (!strcmp(folder ? ids_folder[i] : ids_man[i].id, mod)) return i;
    return -1;
}

int mod_ids_number(const char *mod, int kind, uint32_t table, const char *name, uint32_t *out) {
    ids_refresh();
    plat_mutex_lock(&ids_lock);
    int i = ids_find(mod, 0), ok = 0;
    for (int d = 0; i >= 0 && d < ids_man[i].n && !ok; d++) {
        const ModIdDecl *e = &ids_man[i].decls[d];
        if (e->kind == kind && (kind != BDAT_ID_ROW || e->table == table) && !strcmp(e->name, name) &&
            ids_nums[i][d] != UINT32_MAX) {
            *out = ids_nums[i][d];
            ok = 1;
        }
    }
    plat_mutex_unlock(&ids_lock);
    return ok;
}

BdatId *mod_ids_of(const char *mod, int *n) {
    ids_refresh();
    plat_mutex_lock(&ids_lock);
    BdatId *out = NULL;
    *n = 0;
    int i = ids_find(mod, 1);
    if (i >= 0 && ids_man[i].n) {
        // one block: the entries, their refs, their names
        size_t refs = 0, names = 0;
        for (int d = 0; d < ids_man[i].n; d++) {
            refs += (size_t)ids_man[i].decls[d].nrefs * 2;
            names += strlen(ids_man[i].decls[d].name) + 1;
        }
        size_t at_refs = (size_t)ids_man[i].n * sizeof *out, at_names = at_refs + refs * sizeof(uint32_t);
        uint8_t *blk = malloc(at_names + names);
        if (blk) {
            out = (BdatId *)blk;
            uint32_t *rp = (uint32_t *)(blk + at_refs);
            char *np = (char *)blk + at_names;
            for (int d = 0; d < ids_man[i].n; d++) {
                const ModIdDecl *e = &ids_man[i].decls[d];
                if (ids_nums[i][d] == UINT32_MAX) continue;   // no number: its names stay undeclared to the merge
                size_t ln = strlen(e->name) + 1;
                memcpy(np, e->name, ln);
                memcpy(rp, e->refs, (size_t)e->nrefs * 2 * sizeof *rp);
                out[(*n)++] = (BdatId){e->kind, e->has_local, np, e->local, ids_nums[i][d], e->table, rp, e->nrefs};
                np += ln;
                rp += e->nrefs * 2;
            }
            if (!*n) {
                free(blk);
                out = NULL;
            }
        }
    }
    plat_mutex_unlock(&ids_lock);
    return out;
}

int mod_ids_moved(const char *mod) {
    ids_refresh();
    plat_mutex_lock(&ids_lock);
    int i = ids_find(mod, 1), moved = 0;
    for (int d = 0; i >= 0 && d < ids_man[i].n; d++)
        moved |= ids_man[i].decls[d].has_local && ids_nums[i][d] != ids_man[i].decls[d].local;
    plat_mutex_unlock(&ids_lock);
    return moved;
}

int mod_ids_status(int i, char *out, size_t cap, int *kind) {
    plat_mutex_lock(&ids_lock);
    // first one line per mod with what it was given, then the moves and the names that got none
    int ok = 0;
    if (i < ids_nman) {
        int c[BDAT_ID_KINDS] = {0}, moved = 0;
        char regions[120] = "";
        size_t w = 0;
        for (int d = 0; d < ids_man[i].n; d++) {
            const ModIdDecl *e = &ids_man[i].decls[d];
            if (ids_nums[i][d] == UINT32_MAX) continue;
            c[e->kind]++;
            moved += e->has_local && ids_nums[i][d] != e->local;
            if (e->kind == BDAT_ID_REGION && w < sizeof regions - 8)
                w += (size_t)snprintf(regions + w, sizeof regions - w, "%s%u", w ? ", " : "", ids_nums[i][d]);
        }
        snprintf(out, cap, "%s: region%s %s; %d flag(s), %d reward(s), %d row(s), %d FrontierNav site(s); %d moved "
                 "from its own number(s)", ids_man[i].id, c[BDAT_ID_REGION] == 1 ? "" : "s",
                 c[BDAT_ID_REGION] ? regions : "none", c[BDAT_ID_FLAG], c[BDAT_ID_REWARD], c[BDAT_ID_ROW],
                 c[BDAT_ID_SITE], moved);
        *kind = moved ? 1 : 0;
        ok = 1;
    } else if (i - ids_nman < ids_nstatus) {
        snprintf(out, cap, "%s", ids_status[i - ids_nman]);
        *kind = ids_status_kind[i - ids_nman];
        ok = 1;
    }
    plat_mutex_unlock(&ids_lock);
    return ok;
}
