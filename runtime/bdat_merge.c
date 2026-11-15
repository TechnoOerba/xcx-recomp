// Merging the BDAT data tables of several game-file mods, so that mods which each ship a whole bdat/common.bdat can
// be on together (bdat_merge.h has the rules).
//
// The layout (version 4 with hashed names, as XCX DE 2.0.0 ships it), all little-endian:
//   file   'BDAT', 0x01001004, table count, file size, then one u32 offset per table; tables are 4-byte aligned
//   table  'BDAT', 0x3004, column count, row count, base id, 0, then offsets from the table start of the columns,
//          the hash index, the rows and the string table, the row length and the string table length
//   column 3 bytes: type (1 u8, 2 u16, 3 u32, 4 i8, 5 i16, 6 i32, 7 string, 8 float, 9 hash, 10 u8, 11 string,
//          12 u8, 13 u16) and the offset of the name's hash in the string table (u16)
//   index  one (hash, row) pair per distinct value of column 0 when that is a hash column, sorted, the first row of
//          a value kept
//   strings 0 (names are hashes), the table's name hash, one u32 kept as it is, one u32 per column name, then the
//          strings of the string cells, NUL-terminated, each distinct one once; an empty string points at the 0
// The game's own tables also hold a block of plain-text debug names before the columns. Mod tools drop it, and so
// does write_table, which only runs for tables merged from several mods; every other table keeps its bytes.
//
// Rows are matched by position: the mods made for 2.0.0 edit tables in place, and some append rows at the end. Mod
// files come from anywhere, so every offset is checked before it is read.
#include "bdat_merge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODS 64

static const uint8_t TSIZE[14] = {0, 1, 2, 4, 1, 2, 4, 4, 4, 4, 1, 4, 1, 2};
#define IS_STR(t) ((t) == 7 || (t) == 11)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

typedef struct {
    const uint8_t *p;           // the table's bytes, up to the next table
    uint32_t len;
    uint32_t cc, rc, base, ro, rl, so, sl, name, extra;
    const uint8_t *cols;        // cc columns of 3 bytes
    uint32_t *off;              // each column's offset in a row
    uint32_t *keys;             // column 0 values, sorted (built when a reorder check needs them)
} Tab;

typedef struct {
    uint32_t count;
    Tab *t;
} File;

static void file_free(File *f) {
    for (uint32_t i = 0; f->t && i < f->count; i++) {
        free(f->t[i].off);
        free(f->t[i].keys);
    }
    free(f->t);
    f->t = NULL;
}

static const uint8_t *cell(const Tab *t, uint32_t r, uint32_t c) { return t->p + t->ro + (size_t)r * t->rl + t->off[c]; }
static uint8_t ctype(const Tab *t, uint32_t c) { return t->cols[3 * c]; }
static uint32_t cname(const Tab *t, uint32_t c) { return rd32(t->p + t->so + rd16(t->cols + 3 * c + 1)); }

// The string a string cell points at (checked when the table was read), its length in *n.
static const char *cstr(const Tab *t, uint32_t r, uint32_t c, size_t *n) {
    const char *s = (const char *)t->p + t->so + rd32(cell(t, r, c));
    *n = strlen(s);
    return s;
}

static int read_table(const uint8_t *p, uint32_t len, Tab *t) {
    memset(t, 0, sizeof *t);
    if (len < 48 || memcmp(p, "BDAT", 4) || rd32(p + 4) != 0x3004 || rd32(p + 20)) return 0;
    t->p = p;
    t->len = len;
    t->cc = rd32(p + 8);
    t->rc = rd32(p + 12);
    t->base = rd32(p + 16);
    uint32_t co = rd32(p + 24);
    t->ro = rd32(p + 32);
    t->rl = rd32(p + 36);
    t->so = rd32(p + 40);
    t->sl = rd32(p + 44);
    if (!t->cc || t->cc > 4096 || (uint64_t)co + 3ull * t->cc > len || (uint64_t)t->ro + (uint64_t)t->rl * t->rc > len ||
        (uint64_t)t->so + t->sl > len || t->sl < 9 || p[t->so])
        return 0;
    t->cols = p + co;
    t->name = rd32(p + t->so + 1);
    t->extra = rd32(p + t->so + 5);
    t->off = malloc(t->cc * sizeof *t->off);
    if (!t->off) return 0;
    uint32_t o = 0;
    for (uint32_t c = 0; c < t->cc; c++) {
        uint8_t ty = ctype(t, c);
        if (!ty || ty > 13 || (uint32_t)rd16(t->cols + 3 * c + 1) + 4 > t->sl) return 0;
        t->off[c] = o;
        o += TSIZE[ty];
    }
    if (o != t->rl) return 0;
    const uint8_t *s = p + t->so;
    for (uint32_t c = 0; c < t->cc; c++)
        if (IS_STR(ctype(t, c)))
            for (uint32_t r = 0; r < t->rc; r++) {
                uint32_t v = rd32(cell(t, r, c));
                if (v >= t->sl || !memchr(s + v, 0, t->sl - v)) return 0;
            }
    return 1;
}

static int read_file(const uint8_t *p, size_t n, File *f, char *why, size_t cap, const char *name) {
    memset(f, 0, sizeof *f);
    if (n < 16 || n > 0xffffffffu || memcmp(p, "BDAT", 4) || rd32(p + 4) != 0x01001004 || rd32(p + 12) != n) {
        snprintf(why, cap, "%s: not a BDAT file of the 2.0.0 layout", name);
        return 0;
    }
    f->count = rd32(p + 8);
    if (!f->count || 16 + 4ull * f->count > n || !(f->t = calloc(f->count, sizeof *f->t))) {
        snprintf(why, cap, "%s: damaged table list", name);
        return 0;
    }
    for (uint32_t i = 0; i < f->count; i++) {
        uint32_t s = rd32(p + 16 + 4 * i), e = i + 1 < f->count ? rd32(p + 20 + 4 * i) : (uint32_t)n;
        if (s < 16 + 4 * f->count || e < s || e > n || !read_table(p + s, e - s, &f->t[i])) {
            snprintf(why, cap, "%s: table %u is damaged", name, i);
            file_free(f);
            return 0;
        }
    }
    return 1;
}

static int same_cell(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb, uint32_t c) {
    if (IS_STR(ctype(a, c))) {
        size_t na, nb;
        const char *x = cstr(a, ra, c, &na), *y = cstr(b, rb, c, &nb);
        return na == nb && !memcmp(x, y, na);
    }
    return !memcmp(cell(a, ra, c), cell(b, rb, c), TSIZE[ctype(a, c)]);
}

static int same_row(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb) {
    for (uint32_t c = 0; c < a->cc; c++)
        if (!same_cell(a, ra, b, rb, c)) return 0;
    return 1;
}

static int same_cols(const Tab *a, const Tab *b) {
    if (a->cc != b->cc) return 0;
    for (uint32_t c = 0; c < a->cc; c++)
        if (ctype(a, c) != ctype(b, c) || cname(a, c) != cname(b, c)) return 0;
    return 1;
}

static int by_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int has_key(Tab *t, uint32_t k) {
    if (!t->keys) {
        if (!(t->keys = malloc((t->rc ? t->rc : 1) * sizeof *t->keys))) return 1;   // no memory: treat as moved
        for (uint32_t r = 0; r < t->rc; r++) t->keys[r] = rd32(cell(t, r, 0));
        qsort(t->keys, t->rc, sizeof *t->keys, by_u32);
    }
    return bsearch(&k, t->keys, t->rc, sizeof *t->keys, by_u32) != NULL;
}

enum { SAME, CELLS, WHOLE };

// How mod table m differs from base table b; *reason says why for WHOLE.
static int classify(Tab *b, const Tab *m, const char **reason) {
    if (!same_cols(b, m)) {
        *reason = "other columns";
        return WHOLE;
    }
    if (m->base != b->base || m->rc < b->rc) {
        *reason = "rows removed or renumbered";
        return WHOLE;
    }
    int keyed = ctype(b, 0) == 9, changed = m->rc != b->rc;
    for (uint32_t r = 0; r < b->rc; r++) {
        if (same_row(b, r, m, r)) continue;
        changed = 1;
        uint32_t k = rd32(cell(m, r, 0));
        if (keyed && k != rd32(cell(b, r, 0)) && has_key(b, k)) {   // the row has another row's key: rows moved
            *reason = "rows reordered";
            return WHOLE;
        }
    }
    return changed ? CELLS : SAME;
}

// ---- writing a merged table

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static int put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) return 0;
        b->p = q;
        b->cap = cap;
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
    return 1;
}

// Distinct strings of the string table: open addressing on FNV-1a, offsets into the table being written.
typedef struct { uint32_t *slot; uint32_t cap; } Interns;

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

// The string's offset in st, added at the end the first time; UINT32_MAX when out of memory or room.
static uint32_t intern(Interns *in, Buf *st, const char *s, size_t n) {
    if (!n) return 0;
    uint32_t i = fnv(s, n) & (in->cap - 1);
    for (; in->slot[i]; i = (i + 1) & (in->cap - 1)) {
        const char *o = (const char *)st->p + in->slot[i];
        if (!strncmp(o, s, n) && !o[n]) return in->slot[i];
    }
    if (st->n + n + 1 > 0xffffffffu) return UINT32_MAX;
    uint32_t off = (uint32_t)st->n;
    if (!put(st, s, n) || !put(st, NULL, 1)) return UINT32_MAX;
    in->slot[i] = off;
    return off;
}

typedef struct { uint32_t key, row; } Pair;

static int by_pair(const void *a, const void *b) {
    const Pair *x = a, *y = b;
    if (x->key != y->key) return (x->key > y->key) - (x->key < y->key);
    return (x->row > y->row) - (x->row < y->row);
}

// Table b's columns and base id with rows rows; cell (r, c) comes from src[own[r * cc + c]] at row r. extra is the
// string table's kept word.
static int write_table(Buf *out, const Tab *b, const Tab *const *src, const uint8_t *own, uint32_t rows,
                       uint32_t extra) {
    uint32_t cc = b->cc, rl = b->rl;
    Buf st = {0}, rb = {0};
    Interns in = {0};
    Pair *pairs = NULL;
    int ok = 0;
    uint8_t head[9] = {0};
    wr32(head + 1, b->name);
    wr32(head + 5, extra);
    if (!put(&st, head, 9)) goto done;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t h[4];
        wr32(h, cname(b, c));
        if (!put(&st, h, 4)) goto done;
    }
    if (9 + 4ull * (cc - 1) > 0xffff) goto done;   // column name offsets are u16
    uint64_t strcols = 0;
    for (uint32_t c = 0; c < cc; c++) strcols += IS_STR(ctype(b, c));
    uint64_t want = (uint64_t)rows * strcols * 2 + 16;   // at most half full: every string cell may be distinct
    if (want > (1u << 30)) goto done;
    for (in.cap = 64; in.cap < want; in.cap *= 2) {}
    if (!(in.slot = calloc(in.cap, sizeof *in.slot)) || !put(&rb, NULL, (size_t)rl * rows)) goto done;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cc; c++) {
            const Tab *s = src[own[(size_t)r * cc + c]];
            uint8_t *d = rb.p + (size_t)r * rl + b->off[c];
            if (IS_STR(ctype(b, c))) {
                size_t n;
                const char *str = cstr(s, r, c, &n);
                uint32_t o = intern(&in, &st, str, n);
                if (o == UINT32_MAX) goto done;
                wr32(d, o);
            } else {
                memcpy(d, cell(s, r, c), TSIZE[ctype(b, c)]);
            }
        }
    uint32_t keys = 0;
    if (ctype(b, 0) == 9 && rows) {
        if (!(pairs = malloc(rows * sizeof *pairs))) goto done;
        for (uint32_t r = 0; r < rows; r++) pairs[r] = (Pair){rd32(rb.p + (size_t)r * rl + b->off[0]), r};
        qsort(pairs, rows, sizeof *pairs, by_pair);
        for (uint32_t r = 0; r < rows; r++)
            if (!keys || pairs[r].key != pairs[keys - 1].key) pairs[keys++] = pairs[r];
    }
    uint32_t co = 48, ho = co + 3 * cc, ro = ho + 8 * keys;
    uint64_t so = (uint64_t)ro + (uint64_t)rl * rows;
    if (so + st.n > 0xffffffffu) goto done;
    uint8_t h[48];
    memcpy(h, "BDAT", 4);
    uint32_t f[11] = {0x3004, cc, rows, b->base, 0, co, ho, ro, rl, (uint32_t)so, (uint32_t)st.n};
    for (int i = 0; i < 11; i++) wr32(h + 4 + 4 * i, f[i]);
    if (!put(out, h, 48)) goto done;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t col[3] = {ctype(b, c)};
        wr16(col + 1, (uint16_t)(9 + 4 * c));
        if (!put(out, col, 3)) goto done;
    }
    for (uint32_t k = 0; k < keys; k++) {
        uint8_t e[8];
        wr32(e, pairs[k].key);
        wr32(e + 4, pairs[k].row);
        if (!put(out, e, 8)) goto done;
    }
    ok = put(out, rb.p, rb.n) && put(out, st.p, st.n) && put(out, NULL, (4 - (out->n & 3)) & 3);
done:
    free(st.p);
    free(rb.p);
    free(in.slot);
    free(pairs);
    return ok;
}

// ---- merging

static void notef(BdatNote note, void *ctx, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void notef(BdatNote note, void *ctx, const char *fmt, ...) {
    if (!note) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    note(ctx, line);
}

uint8_t *bdat_merge(const BdatInput *base, const BdatInput *mods, int n, size_t *out_n, BdatSummary *sum,
                    BdatNote note, void *ctx, char *why, size_t why_cap) {
    BdatSummary s = {0};
    File bf = {0}, mf[MAX_MODS] = {{0}};
    uint8_t *kind = NULL;
    const char **reason = NULL;
    Buf body = {0};
    uint32_t *offs = NULL;
    uint8_t *result = NULL;
    if (n < 1 || n > MAX_MODS) {
        snprintf(why, why_cap, "%d mods (1 to %d can be merged)", n, MAX_MODS);
        return NULL;
    }
    if (!read_file(base->data, base->size, &bf, why, why_cap, base->name)) return NULL;
    for (int k = 0; k < n; k++) {
        if (!read_file(mods[k].data, mods[k].size, &mf[k], why, why_cap, mods[k].name)) goto done;
        int same = mf[k].count == bf.count;
        for (uint32_t j = 0; same && j < bf.count; j++) same = mf[k].t[j].name == bf.t[j].name;
        if (!same) {
            snprintf(why, why_cap, "%s: its list of tables differs from the game's", mods[k].name);
            goto done;
        }
    }
    kind = malloc((size_t)bf.count * n);
    reason = calloc((size_t)bf.count * n, sizeof *reason);
    offs = malloc(bf.count * sizeof *offs);
    if (!kind || !reason || !offs) {
        snprintf(why, why_cap, "out of memory");
        goto done;
    }
    for (uint32_t j = 0; j < bf.count; j++)
        for (int k = 0; k < n; k++)
            kind[(size_t)j * n + k] = (uint8_t)classify(&bf.t[j], &mf[k].t[j], &reason[(size_t)j * n + k]);
    for (uint32_t j = 0; j < bf.count; j++) {
        Tab *b = &bf.t[j];
        const uint8_t *kj = kind + (size_t)j * n;
        int who[MAX_MODS], nw = 0;
        for (int k = 0; k < n; k++)
            if (kj[k] != SAME) who[nw++] = k;
        offs[j] = (uint32_t)body.n;
        const Tab *copy = NULL;
        if (!nw) copy = b;
        else if (nw == 1) copy = &mf[who[0]].t[j];
        if (nw) s.tables_changed++;
        if (!copy) {
            // several mods: any change that isn't cell edits, or two different sets of appended rows, means the top
            // mod's whole table
            const char *lost = NULL, *by = NULL;
            int app = -1;
            for (int w = 0; w < nw && !lost; w++) {
                int k = who[w];
                const Tab *m = &mf[k].t[j];
                if (kj[k] == WHOLE) {
                    lost = reason[(size_t)j * n + k];
                    by = mods[k].name;
                } else if (m->rc > b->rc) {
                    if (app < 0) {
                        app = k;
                        continue;
                    }
                    const Tab *a = &mf[app].t[j];
                    int same = a->rc == m->rc;
                    for (uint32_t r = b->rc; same && r < m->rc; r++) same = same_row(a, r, m, r);
                    if (!same) {
                        lost = "add different rows";
                        by = NULL;
                    }
                }
            }
            if (lost) {
                copy = &mf[who[0]].t[j];
                s.tables_whole++;
                char losers[400] = "";
                size_t ln = 0;
                for (int w = 1; w < nw && ln < sizeof losers - 80; w++)
                    ln += (size_t)snprintf(losers + ln, sizeof losers - ln, "%s%s", w > 1 ? ", " : "",
                                           mods[who[w]].name);
                if (by)
                    notef(note, ctx, "table %08X: %s's whole table is used (%s: %s); left out: %s", b->name,
                          mods[who[0]].name, by, lost, losers);
                else
                    notef(note, ctx, "table %08X: %s's whole table is used (two mods %s); left out: %s", b->name,
                          mods[who[0]].name, lost, losers);
            } else {
                uint32_t rows = app >= 0 ? mf[app].t[j].rc : b->rc;
                uint8_t *own = calloc((size_t)rows * b->cc, 1);   // 0 = base, k + 1 = mods[k]
                const Tab *src[MAX_MODS + 1];
                src[0] = b;
                for (int k = 0; k < n; k++) src[k + 1] = &mf[k].t[j];
                if (!own) {
                    snprintf(why, why_cap, "out of memory");
                    goto done;
                }
                if (app >= 0) memset(own + (size_t)b->rc * b->cc, app + 1, (size_t)(rows - b->rc) * b->cc);
                for (int w = nw - 1; w >= 0; w--) {   // lowest priority first, so the top mod is applied last
                    int k = who[w];
                    const Tab *m = &mf[k].t[j];
                    for (uint32_t r = 0; r < b->rc; r++)
                        for (uint32_t c = 0; c < b->cc; c++) {
                            if (same_cell(b, r, m, r, c)) continue;
                            uint8_t *o = own + (size_t)r * b->cc + c;
                            if (*o && !same_cell(src[*o], r, m, r, c)) {
                                s.cells_overridden++;
                                notef(note, ctx, "table %08X row %u column %u: %s's value, not %s's", b->name,
                                      b->base + r, c, mods[k].name, mods[*o - 1].name);
                            }
                            *o = (uint8_t)(k + 1);
                        }
                }
                int ok = write_table(&body, b, src, own, rows, mf[who[0]].t[j].extra);
                free(own);
                if (!ok) {
                    snprintf(why, why_cap, "table %08X could not be written", b->name);
                    goto done;
                }
                s.tables_merged++;
                continue;
            }
        }
        if (!put(&body, copy->p, copy->len)) {
            snprintf(why, why_cap, "out of memory");
            goto done;
        }
    }
    size_t head = 16 + 4 * (size_t)bf.count, total = head + body.n;
    if (total > 0xffffffffu || !(result = malloc(total))) {
        snprintf(why, why_cap, "the merged file is too big");
        goto done;
    }
    memcpy(result, "BDAT", 4);
    wr32(result + 4, 0x01001004);
    wr32(result + 8, bf.count);
    wr32(result + 12, (uint32_t)total);
    for (uint32_t j = 0; j < bf.count; j++) wr32(result + 16 + 4 * j, (uint32_t)(head + offs[j]));
    memcpy(result + head, body.p, body.n);
    *out_n = total;
    if (sum) *sum = s;
done:
    file_free(&bf);
    for (int k = 0; k < n; k++) file_free(&mf[k]);
    free(kind);
    free(reason);
    free(offs);
    free(body.p);
    return result;
}

int bdat_rewrite_check(const uint8_t *data, size_t size) {
    File f;
    char why[200];
    if (!read_file(data, size, &f, why, sizeof why, "file")) return -1;
    int bad = 0;
    for (uint32_t j = 0; j < f.count; j++) {
        Tab *t = &f.t[j];
        uint8_t *own = calloc((size_t)t->rc * t->cc + 1, 1);
        const Tab *src[1] = {t};
        Buf out = {0};
        if (!own || !write_table(&out, t, src, own, t->rc, t->extra) || out.n != t->len || memcmp(out.p, t->p, t->len))
            bad++;
        free(own);
        free(out.p);
    }
    file_free(&f);
    return bad;
}
