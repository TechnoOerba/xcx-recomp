// The map's placement files of several content mods combined (docs/mod-ids.md, "Placement files").
//
// gimmick/<zone>/arrangement_*.gimk say where each location box, FrontierNav sound, treasure, collection point, enemy
// spawn and so on of a zone is. Every one has the same layout: 'SPT ' (16 bytes), a 'GIMK' section (a 16-byte header:
// magic, u16 version, u16 count, u32 size = 16 + the records, u32) of 56-byte records, then a label section (the same
// header, its own magic) of fixed-size entries. A record's u32 at +52 is the index of its label; a record's u32 at +0
// is an instance id (0 in most files; numbered in the FrontierNav sound file). Several records may share a label (the
// boxes of one location), and a label is what the data tables find (a location row's key is the hash of its label).
//
// A content mod ships the whole file: the game's records and labels, some edited or moved, its own appended after the
// game's. So the unit two mods are compared by is the label with its records: a game label (by its position, which a
// mod keeps) one mod changes (its entry, or any of the game's records under it) is that mod's; two mods changing the
// same one: the higher keeps it whole and the lower's edits to it are left out (a line), never half of each. Records
// a mod only adds under a game label (a label can be a kind of record: every FrontierNav site's sound is under
// Se_FN_01) are additions, every mod's kept. Labels a mod appends are its own (two mods appending the very same entry
// with the same records: one copy). The game's records keep their places (a save may know a record by its place), but for ones a mod removes;
// records mods add come after them, and an added record whose instance id is another record's gets a free one.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdat_merge.h"

#define REC 56
#define REC_LABEL 52
#define MAX_MODS 64

typedef struct {
    const uint8_t *top;   // 'SPT ' header (16 bytes)
    const uint8_t *h1, *h2;   // the section headers (16 bytes each)
    const uint8_t *rec;   // nrec records of REC bytes
    const uint8_t *ent;   // nent label entries of esz bytes
    uint32_t nrec, nent, esz;
} Gimk;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int parse(const uint8_t *b, size_t n, Gimk *g) {
    memset(g, 0, sizeof *g);
    if (!b || n < 0x30 || memcmp(b, "SPT ", 4) || memcmp(b + 0x10, "GIMK", 4)) return 0;
    uint32_t cnt = rd16(b + 0x16), size = rd32(b + 0x18);
    if (size != 16 + (uint64_t)REC * cnt || 0x10 + (uint64_t)size + 16 > n) return 0;
    const uint8_t *s2 = b + 0x10 + size;
    uint32_t n2 = rd16(s2 + 6), size2 = rd32(s2 + 8);
    if ((uint64_t)(s2 - b) + size2 != n || size2 < 16 || (n2 && (size2 - 16) % n2) || (!n2 && size2 != 16)) return 0;
    g->top = b;
    g->h1 = b + 0x10;
    g->rec = b + 0x20;
    g->nrec = cnt;
    g->h2 = s2;
    g->ent = s2 + 16;
    g->nent = n2;
    g->esz = n2 ? (size2 - 16) / n2 : 0;
    for (uint32_t i = 0; i < cnt; i++)
        if (rd32(g->rec + (size_t)i * REC + REC_LABEL) >= n2) return 0;   // a record whose label isn't there
    return 1;
}

static const uint8_t *rec_at(const Gimk *g, uint32_t i) { return g->rec + (size_t)i * REC; }
static uint32_t label_of(const Gimk *g, uint32_t i) { return rd32(rec_at(g, i) + REC_LABEL); }
static const uint8_t *ent_at(const Gimk *g, uint32_t k) { return g->ent + (size_t)k * g->esz; }

// A record equal to another but for its label index.
static int rec_eq(const uint8_t *a, const uint8_t *b) {
    return !memcmp(a, b, REC_LABEL) && !memcmp(a + REC_LABEL + 4, b + REC_LABEL + 4, REC - REC_LABEL - 4);
}

// Label k of a and label l of b: the same entry and the same records in the same order.
static int group_eq(const Gimk *a, uint32_t k, const Gimk *b, uint32_t l) {
    if (memcmp(ent_at(a, k), ent_at(b, l), a->esz)) return 0;
    uint32_t i = 0, j = 0;
    for (;;) {
        while (i < a->nrec && label_of(a, i) != k) i++;
        while (j < b->nrec && label_of(b, j) != l) j++;
        if (i == a->nrec || j == b->nrec) return i == a->nrec && j == b->nrec;
        if (!rec_eq(rec_at(a, i), rec_at(b, j))) return 0;
        i++;
        j++;
    }
}

// Mod m changes game label k: its entry, or the game's records under it (each, in order: one edited or missing). Records
// after the game's under k are additions, not changes.
static int changes(const Gimk *g, uint32_t k, const Gimk *m) {
    if (memcmp(ent_at(g, k), ent_at(m, k), g->esz)) return 1;
    uint32_t i = 0, j = 0;
    for (;;) {
        while (i < g->nrec && label_of(g, i) != k) i++;
        if (i == g->nrec) return 0;
        while (j < m->nrec && label_of(m, j) != k) j++;
        if (j == m->nrec || !rec_eq(rec_at(g, i), rec_at(m, j))) return 1;
        i++;
        j++;
    }
}

// An entry that is a name (16 printable bytes, zero-padded): a label two mods meaning the same thing share.
static int named(const uint8_t *e, uint32_t esz) {
    if (esz != 16 || e[0] < 0x20 || e[0] >= 0x7f) return 0;
    size_t n = 0;
    while (n < 16 && e[n] >= 0x20 && e[n] < 0x7f) n++;
    for (size_t i = n; i < 16; i++)
        if (e[i]) return 0;
    return 1;
}

static int name_of(const uint8_t *e, uint32_t esz, char *out, size_t cap) {
    size_t n = 0;
    while (n < esz && n < 16 && e[n] >= 0x20 && e[n] < 0x7f) n++;
    if (n < 2 || (n < 16 && n < esz && e[n])) return snprintf(out, cap, "label");   // no name in the entry
    return snprintf(out, cap, "label \"%.*s\"", (int)n, (const char *)e);
}

typedef struct { uint8_t *p; size_t n, cap; } Out;

static int put(Out *o, const void *p, size_t n) {
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->n + n) cap *= 2;
        uint8_t *q = realloc(o->p, cap);
        if (!q) return 0;
        o->p = q;
        o->cap = cap;
    }
    memcpy(o->p + o->n, p, n);
    o->n += n;
    return 1;
}

static void say(BdatNote note, void *ctx, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(BdatNote note, void *ctx, const char *fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (note) note(ctx, line);
}

uint8_t *bdat_gimk_combine(const BdatInput *game, const BdatInput *mods, int n, size_t *out_n, BdatNote note, void *ctx,
                           char *why, size_t cap) {
    Gimk g, m[MAX_MODS];
    int used[MAX_MODS] = {0}, nused = 0;
    if (!parse(game->data, game->size, &g)) {
        snprintf(why, cap, "the game's file is not a placement file this reads");
        return NULL;
    }
    if (n > MAX_MODS) {
        snprintf(why, cap, "more than %d mods", MAX_MODS);
        return NULL;
    }
    for (int k = 0; k < n; k++) {
        if (!parse(mods[k].data, mods[k].size, &m[k]) || m[k].esz != g.esz || m[k].nent < g.nent ||
            memcmp(m[k].h1, g.h1, 6) || memcmp(m[k].h2, g.h2, 6)) {
            say(note, ctx, "left out: %s: its file is not the game's layout (or has fewer labels); not used", mods[k].name);
            continue;
        }
        used[k] = 1;
        nused++;
    }
    if (!nused || !used[0]) {   // the top mod's own copy can't be read as one: it is used whole, as without a merge
        snprintf(why, cap, "%s", nused ? "the top mod's file is not the game's layout" : "no mod's file can be used");
        return NULL;
    }
    // owner of each game label: the highest mod that changes it; the others' changes to it are left out
    int *owner = malloc((size_t)(g.nent ? g.nent : 1) * sizeof *owner);
    int *src_mod = NULL;          // per added label: the mod and its label it came from
    uint32_t *src_lab = NULL, src_cap = 0;
    Out recs = {0}, ents = {0}, o = {0};
    if (!owner) goto oom;
    uint32_t changed[MAX_MODS] = {0}, lost[MAX_MODS] = {0}, added[MAX_MODS] = {0}, same[MAX_MODS] = {0},
             more[MAX_MODS] = {0};
    for (uint32_t k = 0; k < g.nent; k++) {
        owner[k] = -1;
        for (int x = 0; x < n; x++) {
            if (!used[x] || !changes(&g, k, &m[x])) continue;
            if (owner[k] < 0) {
                owner[k] = x;
                changed[x]++;
            } else {
                lost[x]++;
                char nm[40];
                name_of(ent_at(&g, k), g.esz, nm, sizeof nm);
                say(note, ctx, "left out: %s: its changes to %s %u: %s changes it too", mods[x].name, nm, k,
                    mods[owner[k]].name);
            }
        }
    }
    // the game's records in their places: an owned label's records are its owner's, in order (fewer: the rest are
    // dropped; more: the rest come after the game's records)
    for (uint32_t k = 0; k < g.nent; k++)
        if (!put(&ents, owner[k] >= 0 ? ent_at(&m[owner[k]], k) : ent_at(&g, k), g.esz)) goto oom;
    uint32_t *next = calloc((size_t)(g.nent ? g.nent : 1), sizeof *next);   // per owned label: its owner's next record
    if (!next) goto oom;
    for (uint32_t i = 0; i < g.nrec; i++) {
        uint32_t k = label_of(&g, i);
        if (owner[k] < 0) {
            if (!put(&recs, rec_at(&g, i), REC)) { free(next); goto oom; }
            continue;
        }
        const Gimk *w = &m[owner[k]];
        while (next[k] < w->nrec && label_of(w, next[k]) != k) next[k]++;
        if (next[k] == w->nrec) continue;   // its owner has fewer records for it: this one is dropped
        if (!put(&recs, rec_at(w, next[k]), REC)) { free(next); goto oom; }
        next[k]++;
    }
    uint32_t inplace = (uint32_t)(recs.n / REC);   // the records in the game's records' places
    for (uint32_t k = 0; k < g.nent; k++) {   // an owner's further records for a game label
        if (owner[k] < 0) continue;
        const Gimk *w = &m[owner[k]];
        for (uint32_t i = next[k]; i < w->nrec; i++)
            if (label_of(w, i) == k && !put(&recs, rec_at(w, i), REC)) { free(next); goto oom; }
    }
    // records mods add under a game label, past its number of the game's records: after the game's records, every
    // mod's (an owner's are in its group), also of a mod whose edits to the game's records there were left out
    for (uint32_t k = 0; k < g.nent; k++) {
        uint32_t ng = 0;
        for (uint32_t i = 0; i < g.nrec; i++) ng += label_of(&g, i) == k;
        for (int x = 0; x < n; x++) {
            if (!used[x] || owner[k] == x) continue;
            for (uint32_t i = 0, seen = 0; i < m[x].nrec; i++) {
                if (label_of(&m[x], i) != k || seen++ < ng) continue;
                if (!put(&recs, rec_at(&m[x], i), REC)) { free(next); goto oom; }
                more[x]++;
            }
        }
    }
    free(next);
    // the labels mods add, highest priority first: one copy of an entry two mods add alike; two different groups under
    // the same entry: the higher one's
    uint32_t nent = g.nent;
    for (int x = 0; x < n; x++) {
        if (!used[x]) continue;
        for (uint32_t l = g.nent; l < m[x].nent; l++) {
            // an entry already added: the very same label and records once; a name (a 16-byte printable entry: a
            // location's, a box's) with other records: the first one's; an entry that isn't a name (an enemy spawn
            // group) with other records is another label, kept
            int dup = -1;
            const uint8_t *e = ent_at(&m[x], l);
            for (uint32_t q = g.nent; q < nent && dup < 0; q++)
                if (!memcmp(ents.p + (size_t)q * g.esz, e, g.esz) &&
                    (named(e, g.esz) || group_eq(&m[src_mod[q - g.nent]], src_lab[q - g.nent], &m[x], l)))
                    dup = (int)q;
            if (dup >= 0) {
                int src = src_mod[dup - (int)g.nent];
                char nm[40];
                name_of(e, g.esz, nm, sizeof nm);
                if (group_eq(&m[src], src_lab[dup - (int)g.nent], &m[x], l)) {
                    same[x]++;
                } else {
                    lost[x]++;
                    say(note, ctx, "left out: %s: its added %s: %s adds it too, with other records", mods[x].name, nm,
                        src == x ? "it" : mods[src].name);
                }
                continue;
            }
            if (nent - g.nent == src_cap) {
                uint32_t c2 = src_cap ? src_cap * 2 : 64;
                int *a = realloc(src_mod, c2 * sizeof *a);
                if (a) src_mod = a;
                uint32_t *b = a ? realloc(src_lab, c2 * sizeof *b) : NULL;
                if (!a || !b) goto oom;
                src_lab = b;
                src_cap = c2;
            }
            src_mod[nent - g.nent] = x;
            src_lab[nent - g.nent] = l;
            if (!put(&ents, e, g.esz)) goto oom;
            for (uint32_t i = 0; i < m[x].nrec; i++) {
                if (label_of(&m[x], i) != l) continue;
                uint8_t r[REC];
                memcpy(r, rec_at(&m[x], i), REC);
                wr32(r + REC_LABEL, nent);
                if (!put(&recs, r, REC)) goto oom;
            }
            nent++;
            added[x]++;
        }
    }
    // instance ids: where the game numbers its records (every id non-zero and its own), a record after the game's
    // whose id is already taken gets the next free one
    {
        int numbered = g.nrec > 0;
        uint32_t top = 0;
        for (uint32_t i = 0; i < g.nrec && numbered; i++) {
            uint32_t id = rd32(rec_at(&g, i));
            numbered = id != 0;
            for (uint32_t j = 0; j < i && numbered; j++) numbered = rd32(rec_at(&g, j)) != id;
            if (id > top) top = id;
        }
        uint32_t nr = (uint32_t)(recs.n / REC), renumbered = 0;
        for (uint32_t i = 0; numbered && i < nr; i++) {
            uint32_t id = rd32(recs.p + (size_t)i * REC);
            if (id > top) top = id;
        }
        for (uint32_t i = inplace; numbered && i < nr; i++) {
            uint8_t *r = recs.p + (size_t)i * REC;
            uint32_t id = rd32(r);
            int taken = id == 0;
            for (uint32_t j = 0; j < i && !taken; j++) taken = rd32(recs.p + (size_t)j * REC) == id;
            if (taken) {
                wr32(r, ++top);
                renumbered++;
            }
        }
        if (renumbered) say(note, ctx, "gimmick: %u added record(s) given instance ids of their own", renumbered);
    }
    if (recs.n / REC > 0xffff || nent > 0xffff) {
        free(owner);
    free(src_mod);
    free(src_lab);
        free(recs.p);
        free(ents.p);
        snprintf(why, cap, "more than 65535 records or labels");
        return NULL;
    }
    for (int x = 0; x < n; x++)
        if (used[x])
            say(note, ctx, "gimmick: %s: %u label(s) changed, %u added, %u record(s) added to the game's labels, %u left out%s",
                mods[x].name, changed[x], added[x], more[x], lost[x], same[x] ? " (some added as another mod adds them)" : "");
    {
        uint8_t h[16];
        if (!put(&o, g.top, 16)) goto oom;
        memcpy(h, g.h1, 16);
        wr16(h + 6, (uint32_t)(recs.n / REC));
        wr32(h + 8, (uint32_t)(16 + recs.n));
        if (!put(&o, h, 16) || (recs.n && !put(&o, recs.p, recs.n))) goto oom;
        memcpy(h, g.h2, 16);
        wr16(h + 6, nent);
        wr32(h + 8, (uint32_t)(16 + ents.n));
        if (!put(&o, h, 16) || (ents.n && !put(&o, ents.p, ents.n))) goto oom;
    }
    free(owner);
    free(src_mod);
    free(src_lab);
    free(recs.p);
    free(ents.p);
    *out_n = o.n;
    return o.p;
oom:
    free(owner);
    free(src_mod);
    free(src_lab);
    free(recs.p);
    free(ents.p);
    free(o.p);
    snprintf(why, cap, "out of memory");
    return NULL;
}
