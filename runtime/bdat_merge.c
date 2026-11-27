// Merging the BDAT data tables of several game-file mods, so that mods which each ship a whole bdat/common.bdat can
// be on together. The rules, and why: docs/bdat-merge.md.
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
// Mod files come from anywhere, so every offset is checked before it is read.
#include "bdat_merge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODS 64
#define NONE UINT32_MAX

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
} Tab;

typedef struct {
    uint32_t count;
    Tab *t;
} File;

static void file_free(File *f) {
    for (uint32_t i = 0; f->t && i < f->count; i++) free(f->t[i].off);
    free(f->t);
    f->t = NULL;
}

static const uint8_t *cell(const Tab *t, uint32_t r, uint32_t c) { return t->p + t->ro + (size_t)r * t->rl + t->off[c]; }
static uint8_t ctype(const Tab *t, uint32_t c) { return t->cols[3 * c]; }
static uint32_t cname(const Tab *t, uint32_t c) { return rd32(t->p + t->so + rd16(t->cols + 3 * c + 1)); }
static uint32_t key(const Tab *t, uint32_t r) { return rd32(cell(t, r, 0)); }

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

static int read_file(const uint8_t *p, size_t n, File *f, char *why, size_t cap) {
    memset(f, 0, sizeof *f);
    if (n < 16 || n > 0xffffffffu || memcmp(p, "BDAT", 4) || rd32(p + 4) != 0x01001004 || rd32(p + 12) != n) {
        snprintf(why, cap, "not a BDAT file of the 2.0.0 layout");
        return 0;
    }
    f->count = rd32(p + 8);
    if (!f->count || 16 + 4ull * f->count > n || !(f->t = calloc(f->count, sizeof *f->t))) {
        snprintf(why, cap, "damaged table list");
        return 0;
    }
    for (uint32_t i = 0; i < f->count; i++) {
        uint32_t s = rd32(p + 16 + 4 * i), e = i + 1 < f->count ? rd32(p + 20 + 4 * i) : (uint32_t)n;
        if (s < 16 + 4 * f->count || e < s || e > n || !read_table(p + s, e - s, &f->t[i])) {
            snprintf(why, cap, "table %u is damaged", i);
            file_free(f);
            return 0;
        }
    }
    return 1;
}

// Debug string cells (type 11) are always alike: the developers' labels, which the game doesn't use and mod tools
// write back empty, are no change (docs/bdat-merge.md, "Debug names are not changes").
static int same_cell(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb, uint32_t c) {
    if (ctype(a, c) == 11) return 1;
    if (IS_STR(ctype(a, c))) {
        size_t na, nb;
        const char *x = cstr(a, ra, c, &na), *y = cstr(b, rb, c, &nb);
        return na == nb && !memcmp(x, y, na);
    }
    return !memcmp(cell(a, ra, c), cell(b, rb, c), TSIZE[ctype(a, c)]);
}

static int same_row(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb) {
    // Rows of the same bytes: every cell but the strings is alike, since a string cell holds only where its text is.
    // Mod tools keep the game's string order, so most rows of a mod's copy are the game's bytes (98.7% for two real
    // whole-file mods); then only the text cells are compared. Where a column's type differs, the cells are compared
    // as below.
    if (a->rl == b->rl && !memcmp(a->p + a->ro + (size_t)ra * a->rl, b->p + b->ro + (size_t)rb * b->rl, a->rl)) {
        uint32_t c = 0;
        for (; c < a->cc && ctype(a, c) == ctype(b, c); c++)
            if (ctype(a, c) == 7 && !same_cell(a, ra, b, rb, c)) return 0;
        if (c == a->cc) return 1;
    }
    for (uint32_t c = 0; c < a->cc; c++)
        if (ctype(a, c) != ctype(b, c) || !same_cell(a, ra, b, rb, c)) return 0;   // cells of another type differ
    return 1;
}

static int same_cols(const Tab *a, const Tab *b) {
    if (a->cc != b->cc) return 0;
    for (uint32_t c = 0; c < a->cc; c++)
        if (ctype(a, c) != ctype(b, c) || cname(a, c) != cname(b, c)) return 0;
    return 1;
}

// ---- sorted keys

typedef struct { uint32_t key, row; } Pair;

static int by_pair(const void *a, const void *b) {
    const Pair *x = a, *y = b;
    if (x->key != y->key) return (x->key > y->key) - (x->key < y->key);
    return (x->row > y->row) - (x->row < y->row);
}

// The table's (key, row) pairs sorted by key, NULL when out of memory; *unique says whether no key repeats.
static Pair *sorted_keys(const Tab *t, int *unique) {
    Pair *p = malloc((t->rc ? t->rc : 1) * sizeof *p);
    if (!p) return NULL;
    for (uint32_t r = 0; r < t->rc; r++) p[r] = (Pair){key(t, r), r};
    qsort(p, t->rc, sizeof *p, by_pair);
    *unique = 1;
    for (uint32_t r = 1; r < t->rc; r++)
        if (p[r].key == p[r - 1].key) *unique = 0;
    return p;
}

static uint32_t find_key(const Pair *p, uint32_t n, uint32_t k) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (p[mid].key < k) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && p[lo].key == k ? p[lo].row : NONE;
}

// A set of u32 values: open addressing, 0 stored apart.
typedef struct { uint32_t *v; uint32_t cap, n; int zero; } Set;

static int set_add(Set *s, uint32_t x) {
    if (!x) return s->zero = 1;
    if (2 * (s->n + 1) > s->cap) {
        Set t = {calloc(s->cap ? s->cap * 2 : 64, 4), s->cap ? s->cap * 2 : 64, 0, s->zero};
        if (!t.v) return 0;
        for (uint32_t i = 0; i < s->cap; i++)
            if (s->v[i]) set_add(&t, s->v[i]);
        free(s->v);
        *s = t;
    }
    uint32_t i = (x * 2654435761u) & (s->cap - 1);
    while (s->v[i] && s->v[i] != x) i = (i + 1) & (s->cap - 1);
    if (!s->v[i]) {
        s->v[i] = x;
        s->n++;
    }
    return 1;
}

static int set_has(const Set *s, uint32_t x) {
    if (!x) return s->zero;
    if (!s->cap) return 0;
    uint32_t i = (x * 2654435761u) & (s->cap - 1);
    while (s->v[i]) {
        if (s->v[i] == x) return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

// ---- one mod's change to one table

enum { SAME, CHANGES, OPAQUE };

typedef struct {
    uint8_t kind;
    const char *reason;        // OPAQUE
    uint32_t *pos;             // per mod row: the base row it is, or NONE (a row the mod adds)
    uint32_t *mpos;            // per base row: its mod row, or NONE (a row the mod removes)
    uint8_t *edited;           // per base row: some cell differs
    uint32_t ndeleted, ninserts;
    int layout;                // base rows at other numbers or in another order
} Change;

static void change_free(Change *c) {
    free(c->pos);
    free(c->mpos);
    free(c->edited);
    memset(c, 0, sizeof *c);
}

// Half or more of the cells but the key are equal: a row whose key was renamed.
static int similar(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb) {
    uint32_t n = a->cc - 1, eq = 0;
    for (uint32_t c = 1; c < a->cc; c++) eq += (uint32_t)same_cell(a, ra, b, rb, c);
    return n && eq * 2 >= n;
}

static int finish(Change *ch, const Tab *b, const Tab *m) {
    if (!(ch->mpos = malloc((b->rc ? b->rc : 1) * sizeof *ch->mpos)) || !(ch->edited = calloc(b->rc + 1, 1))) return 0;
    for (uint32_t r = 0; r < b->rc; r++) ch->mpos[r] = NONE;
    for (uint32_t i = 0; i < m->rc; i++) {
        uint32_t r = ch->pos[i];
        if (r == NONE) {
            ch->ninserts++;
            continue;
        }
        ch->mpos[r] = i;
        ch->edited[r] = !same_row(b, r, m, i);
        if (i != r) ch->layout = 1;
    }
    int any = ch->ninserts || ch->layout;
    for (uint32_t r = 0; r < b->rc; r++) {
        ch->ndeleted += ch->mpos[r] == NONE;
        any |= ch->edited[r];
    }
    if (!any && !ch->ndeleted) {
        change_free(ch);
        ch->kind = SAME;
    }
    return 1;
}

static int align_keyed(const Tab *b, const Pair *bk, const Tab *m, Change *ch) {
    if (!(ch->pos = malloc((m->rc ? m->rc : 1) * sizeof *ch->pos))) return 0;
    int order = 1;
    uint32_t last = NONE;
    for (uint32_t i = 0; i < m->rc; i++) {
        ch->pos[i] = find_key(bk, b->rc, key(m, i));
        if (ch->pos[i] != NONE) {
            if (last != NONE && ch->pos[i] <= last) order = 0;
            last = ch->pos[i];
        }
    }
    if (order) {
        // rows left over on both sides between the same matched rows: paired in order when similar (renamed keys)
        uint32_t m0 = 0, b0 = 0;   // the first mod and base rows after the last matched pair
        for (uint32_t i = 0; i <= m->rc; i++) {
            if (i < m->rc && ch->pos[i] == NONE) continue;
            uint32_t b1 = i < m->rc ? ch->pos[i] : b->rc, free_row = b0;
            for (uint32_t mi = m0; mi < i; mi++)
                for (uint32_t r = free_row; r < b1; r++)
                    if (similar(m, mi, b, r)) {
                        ch->pos[mi] = r;
                        free_row = r + 1;
                        break;
                    }
            m0 = i + 1;
            b0 = b1 + 1;
        }
    }
    return finish(ch, b, m);
}

static int align_positional(const Tab *b, const Pair *bk, const Tab *m, Change *ch) {
    uint32_t n = b->rc < m->rc ? b->rc : m->rc;
    for (uint32_t i = 0; bk && i < n; i++)
        if (key(m, i) != key(b, i) && find_key(bk, b->rc, key(m, i)) != NONE) {
            ch->kind = OPAQUE;
            ch->reason = "rows moved (it gives a row another row's key)";
            return 1;
        }
    if (!(ch->pos = malloc((m->rc ? m->rc : 1) * sizeof *ch->pos))) return 0;
    for (uint32_t i = 0; i < m->rc; i++) ch->pos[i] = i < n ? i : NONE;
    return finish(ch, b, m);
}

// bk: base's sorted keys (NULL when column 0 isn't a hash), bunique: no key repeats.
// The same columns, first row number and rows: two copies of a table alike.
static int same_table(const Tab *a, const Tab *b) {
    if (!same_cols(a, b) || a->base != b->base || a->rc != b->rc) return 0;
    for (uint32_t r = 0; r < a->rc; r++)
        if (!same_row(a, r, b, r)) return 0;
    return 1;
}

static int classify(const Tab *b, const Pair *bk, int bunique, const Tab *m, Change *ch) {
    memset(ch, 0, sizeof *ch);
    // the same bytes: the same table (a mod's file made from its JSON tables keeps the game's other tables as they are)
    if (b->len == m->len && !memcmp(b->p, m->p, b->len)) return 1;
    if (!same_cols(b, m)) {
        ch->kind = OPAQUE;
        ch->reason = "other columns";
        return 1;
    }
    if (m->base != b->base) {
        ch->kind = OPAQUE;
        ch->reason = "another first row number";
        return 1;
    }
    int same = m->rc == b->rc;
    for (uint32_t r = 0; same && r < b->rc; r++) same = same_row(b, r, m, r);
    if (same) return 1;
    ch->kind = CHANGES;
    if (bk && bunique) {
        int munique;
        Pair *mk = sorted_keys(m, &munique);
        if (!mk) return 0;
        free(mk);
        if (munique) return align_keyed(b, bk, m, ch);
    }
    return align_positional(b, bk, m, ch);
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

// Where an output cell comes from: src[t] (0 the base table, k + 1 mods[k]'s), at row r.
typedef struct { uint32_t r; uint8_t t; } Src;

// Table b's columns and base id with rows rows; cell (r, c) comes from from[r * cc + c]. extra is the string table's
// kept word.
static int write_table(Buf *out, const Tab *b, const Tab *const *src, const Src *from, uint32_t rows, uint32_t extra) {
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
            const Src *f = &from[(size_t)r * cc + c];
            const Tab *s = src[f->t];
            uint8_t *d = rb.p + (size_t)r * rl + b->off[c];
            if (IS_STR(ctype(b, c))) {
                size_t n;
                const char *str = cstr(s, f->r, c, &n);
                uint32_t o = intern(&in, &st, str, n);
                if (o == UINT32_MAX) goto done;
                wr32(d, o);
            } else {
                memcpy(d, cell(s, f->r, c), TSIZE[ctype(b, c)]);
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

// ---- events

typedef struct {
    BdatNote note;
    void *ctx;
    BdatSummary *s;
    const BdatInput *mods;
} Events;

static void event(Events *ev, int kind, int mod, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void event(Events *ev, int kind, int mod, const char *fmt, ...) {
    static const char *const names[] = {"priority", "left out", "caveat", "adds nothing", "partly alike"};
    if (kind == 0) ev->s->priority++;
    else if (kind == 1) ev->s->left_out++;
    else if (kind == 2) ev->s->caveats++;
    else if (kind == 3) ev->s->adds_nothing++;
    else ev->s->partly_alike++;
    if (!ev->note) return;
    char line[700];
    int n = snprintf(line, sizeof line, "%s: ", names[kind]);
    if (mod >= 0) n += snprintf(line + n, sizeof line - (size_t)n, "%s: ", ev->mods[mod].name);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;   // a long mod name: the line is cut, never overrun
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    ev->note(ev->ctx, line);
}
enum { PRIORITY, LEFT_OUT, CAVEAT, ADDS_NOTHING, PARTLY_ALIKE };

static void rowname(const Tab *t, uint32_t r, char *out, size_t cap) {
    if (ctype(t, 0) == 9) snprintf(out, cap, "row %u (key %08X)", t->base + r, key(t, r));
    else snprintf(out, cap, "row %u", t->base + r);
}

// ---- merging one table that several mods change

typedef struct { uint32_t row; uint8_t kind, mod; } Entry;   // kind 0: base row; 1: mods[mod]'s added row

typedef struct {
    const Tab *b;
    const Tab *m[MAX_MODS];        // the mods' copies of the table (NULL: the mod's file is left out)
    Change *ch[MAX_MODS];
    const Set *refs;               // per mod: the hash values its changes put in cells
    int n;
    Events *ev;
} Job;

static int grow(Entry **v, uint32_t *cap, uint32_t need) {
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap * 2 : 64;
    while (c < need) c *= 2;
    Entry *q = realloc(*v, c * sizeof *q);
    if (!q) return 0;
    *v = q;
    *cap = c;
    return 1;
}

static int found_by_zone(uint32_t name);

// Merges into out; 0 when out of memory.
static int merge_table(Job *j, int *P, int np, Buf *out, char *why, size_t why_cap) {
    const Tab *t = j->b;
    Events *ev = j->ev;
    char rn[64];
    int owner = -1;
    for (int w = 0; w < np; w++) {
        Change *c = j->ch[P[w]];
        if (!c->layout && !c->ndeleted) continue;
        if (owner < 0) owner = P[w];
        else if (j->m[P[w]]->rc == j->m[owner]->rc &&
                 !memcmp(c->pos, j->ch[owner]->pos, (size_t)j->m[owner]->rc * sizeof *c->pos))
            continue;   // the same layout as the owner's (docs/bdat-merge.md, "Row layout"): nothing of it is left out
        else
            event(ev, LEFT_OUT, P[w], "table %08X: its removed or reordered rows are not used; %s's row layout is "
                  "(rows it adds are kept, at the end)", t->name, ev->mods[owner].name);
    }
    int keyed = 0;
    if (ctype(t, 0) == 9) {
        Pair *bk = sorted_keys(t, &keyed);
        free(bk);
    }
    uint32_t cap = 0, n = 0;
    Entry *seq = NULL;
    uint8_t *present = calloc(t->rc + 1, 1);
    Src *from = NULL;
    uint8_t *drop = NULL;
    Set added = {0};
    int ok = 0;
    if (!present) goto oom;
    if (owner >= 0) {
        const Change *c = j->ch[owner];
        if (!grow(&seq, &cap, j->m[owner]->rc + 1)) goto oom;
        for (uint32_t i = 0; i < j->m[owner]->rc; i++)
            seq[n++] = c->pos[i] != NONE ? (Entry){c->pos[i], 0, 0} : (Entry){i, 1, (uint8_t)owner};
    } else {
        if (!grow(&seq, &cap, t->rc + 1)) goto oom;
        for (uint32_t r = 0; r < t->rc; r++) seq[n++] = (Entry){r, 0, 0};
    }
    uint32_t prefix = 0;
    while (prefix < n && !seq[prefix].kind && seq[prefix].row == prefix) prefix++;
    for (uint32_t i = 0; i < n; i++)
        if (!seq[i].kind) present[seq[i].row] = 1;
    // rows the layout's mod removes: kept for a higher mod's edits or another mod's reference, where no row moves
    if (owner >= 0)
        for (uint32_t r = 0; r < t->rc; r++) {
            if (j->ch[owner]->mpos[r] != NONE) continue;
            int higher[MAX_MODS], refd[MAX_MODS], nh = 0, nr = 0;
            for (int w = 0; w < np && P[w] < owner; w++) {
                Change *c = j->ch[P[w]];
                if (c->mpos[r] != NONE && c->edited[r]) higher[nh++] = P[w];
            }
            for (int k = 0; keyed && k < j->n; k++)
                if (k != owner && j->m[k] && set_has(&j->refs[k], key(t, r))) refd[nr++] = k;
            if (!nh && !nr) continue;
            rowname(t, r, rn, sizeof rn);
            if (n == r && prefix == r) {
                if (!grow(&seq, &cap, n + 1)) goto oom;
                seq[n++] = (Entry){r, 0, 0};
                present[r] = 1;
                prefix++;
                char who[400] = "";
                size_t wl = 0;
                for (int k = 0; k < j->n && wl < sizeof who - 70; k++) {
                    int in = 0;
                    for (int x = 0; x < nh; x++) in |= higher[x] == k;
                    for (int x = 0; x < nr; x++) in |= refd[x] == k;
                    if (in) wl += (size_t)snprintf(who + wl, sizeof who - wl, "%s%s", wl ? ", " : "", ev->mods[k].name);
                }
                event(ev, CAVEAT, owner, "table %08X: %s is kept, although it removes it, for %s", t->name, rn, who);
            } else {
                for (int x = 0; x < nh; x++)
                    event(ev, LEFT_OUT, higher[x], "table %08X: its edits to %s: %s removes the row, and putting it "
                          "back would renumber the rows after it", t->name, rn, ev->mods[owner].name);
                for (int x = 0; x < nr; x++) {
                    int dup = 0;
                    for (int y = 0; y < nh; y++) dup |= higher[y] == refd[x];
                    if (!dup)
                        event(ev, CAVEAT, refd[x], "table %08X: it refers to %s, which %s removes", t->name, rn,
                              ev->mods[owner].name);
                }
            }
        }
    // edits of lower mods to rows that are gone
    for (int w = 0; owner >= 0 && w < np; w++) {
        int k = P[w];
        if (k <= owner) continue;
        uint32_t lost = 0, first = NONE;
        for (uint32_t r = 0; r < t->rc; r++)
            if (j->ch[k]->mpos[r] != NONE && j->ch[k]->edited[r] && !present[r]) {
                lost++;
                if (first == NONE) first = r;
            }
        if (lost) {
            rowname(t, first, rn, sizeof rn);
            event(ev, LEFT_OUT, k, "table %08X: its edits to %u row%s that %s removes (%s%s)", t->name, lost,
                  lost > 1 ? "s" : "", ev->mods[owner].name, rn, lost > 1 ? ", ..." : "");
        }
    }
    // rows the other mods add, after the layout's: by key, the higher mod's; without keys, each list once
    for (uint32_t i = 0; keyed && i < n; i++)
        if (seq[i].kind && !set_add(&added, key(j->m[seq[i].mod], seq[i].row))) goto oom;
    int lists[MAX_MODS], nl = 0;
    for (int w = 0; w < np; w++) {
        int k = P[w];
        const Tab *m = j->m[k];
        const Change *c = j->ch[k];
        if (k == owner || !c->ninserts) continue;
        if (!keyed) {
            int dup = 0;
            for (int x = 0; x < nl && !dup; x++) {
                const Tab *o = j->m[lists[x]];
                const Change *oc = j->ch[lists[x]];
                if (oc->ninserts != c->ninserts) continue;
                dup = 1;
                for (uint32_t a = 0, b2 = 0; dup && a < m->rc; a++) {
                    if (c->pos[a] != NONE) continue;
                    while (oc->pos[b2] != NONE) b2++;
                    dup = same_row(m, a, o, b2++);
                }
            }
            if (dup) continue;
            lists[nl++] = k;
        }
        if (!grow(&seq, &cap, n + c->ninserts + 1)) goto oom;
        for (uint32_t i = 0; i < m->rc; i++) {
            if (c->pos[i] != NONE) continue;
            if (keyed) {
                uint32_t kv = key(m, i);
                if (set_has(&added, kv)) {
                    // added already, by a higher mod (or the layout's): the same row is kept once, silently
                    for (uint32_t x = 0; x < n; x++)
                        if (seq[x].kind && key(j->m[seq[x].mod], seq[x].row) == kv) {
                            if (!same_row(j->m[seq[x].mod], seq[x].row, m, i))
                                event(ev, PRIORITY, -1, "table %08X: added row (key %08X): %s's, not %s's", t->name,
                                      kv, ev->mods[seq[x].mod].name, ev->mods[k].name);
                            break;
                        }
                    continue;
                }
                if (!set_add(&added, kv)) goto oom;
            }
            seq[n++] = (Entry){i, 1, (uint8_t)k};
        }
    }
    // cells: the base row with the edits applied lowest priority first, or the added row
    uint32_t cc = t->cc;
    const Tab *src[MAX_MODS + 1];
    src[0] = t;
    for (int k = 0; k < j->n; k++) src[k + 1] = j->m[k];
    if (!(from = malloc(((size_t)n * cc + 1) * sizeof *from)) || !(drop = calloc(n + 1, 1))) goto oom;
    for (uint32_t i = 0; i < n; i++) {
        Src *f = from + (size_t)i * cc;
        if (seq[i].kind) {
            for (uint32_t c = 0; c < cc; c++) f[c] = (Src){seq[i].row, (uint8_t)(seq[i].mod + 1)};
            continue;
        }
        uint32_t r = seq[i].row;
        for (uint32_t c = 0; c < cc; c++) f[c] = (Src){r, 0};
        for (int w = np - 1; w >= 0; w--) {
            int k = P[w];
            const Change *ch = j->ch[k];
            uint32_t mr = ch->mpos[r];
            if (mr == NONE || !ch->edited[r]) continue;
            for (uint32_t c = 0; c < cc; c++) {
                if (same_cell(t, r, j->m[k], mr, c)) continue;
                if (f[c].t && !same_cell(src[f[c].t], f[c].r, j->m[k], mr, c)) {
                    rowname(t, r, rn, sizeof rn);
                    event(ev, PRIORITY, -1, "table %08X %s column %u: %s's value, not %s's", t->name, rn, c,
                          ev->mods[k].name, ev->mods[f[c].t - 1].name);
                }
                f[c] = (Src){mr, (uint8_t)(k + 1)};
            }
        }
    }
    // keys stay unique: the game's key, then the higher mod's
    if (keyed) {
        Pair *at = malloc((n + 1) * sizeof *at);
        if (!at) goto oom;
        int again = 1;
        // each round puts back the game's key of at least one renamed row, and the game's keys are unique, so it ends
        for (uint32_t round = 0; again && round <= n; round++) {
            again = 0;
            uint32_t na = 0;
            for (uint32_t i = 0; i < n; i++)
                if (!drop[i]) at[na++] = (Pair){rd32(cell(src[from[(size_t)i * cc].t], from[(size_t)i * cc].r, 0)), i};
            qsort(at, na, sizeof *at, by_pair);
            for (uint32_t x = 0; x + 1 < na; x++) {
                if (at[x].key != at[x + 1].key) continue;
                uint32_t a = at[x].row, b2 = at[x + 1].row;
                int ra = from[(size_t)a * cc].t ? from[(size_t)a * cc].t - 1 : -1;
                int rb = from[(size_t)b2 * cc].t ? from[(size_t)b2 * cc].t - 1 : -1;
                uint32_t lose = rb >= ra ? b2 : a;
                at[x + 1].row = lose == a ? b2 : a;   // the kept row meets the next one with this key
                if (seq[lose].kind) {
                    drop[lose] = 1;
                    event(ev, LEFT_OUT, seq[lose].mod, "table %08X: its added row (key %08X): another row has that key",
                          t->name, at[x].key);
                } else {
                    rowname(t, seq[lose].row, rn, sizeof rn);
                    event(ev, LEFT_OUT, from[(size_t)lose * cc].t - 1, "table %08X: its new key %08X for %s: another "
                          "row has that key", t->name, at[x].key, rn);
                    from[(size_t)lose * cc] = (Src){seq[lose].row, 0};
                    again = 1;
                }
            }
        }
        free(at);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (drop[i]) continue;
        seq[kept] = seq[i];
        memmove(from + (size_t)kept * cc, from + (size_t)i * cc, cc * sizeof *from);
        kept++;
    }
    n = kept;
    // row numbers against each mod's own copy
    for (int w = 0; w < np; w++) {
        int k = P[w];
        uint32_t moved = 0, at_i = 0, at_p = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p = !seq[i].kind ? j->ch[k]->mpos[seq[i].row] : seq[i].mod == k ? seq[i].row : NONE;
            if (p != NONE && p != i) {
                if (!moved++) {
                    at_i = i;
                    at_p = p;
                }
            }
        }
        if (moved && !found_by_zone(t->name))   // tables the game searches by their zone column: not by number
            event(ev, CAVEAT, k, "table %08X: %u of its rows have other row numbers than in its copy (first: row %u "
                  "is its row %u); what points at them by number may point elsewhere", t->name, moved, t->base + at_i,
                  t->base + at_p);
    }
    if (!write_table(out, t, src, from, n, j->m[P[0]]->extra)) {
        snprintf(why, why_cap, "table %08X could not be written", t->name);
        goto done;
    }
    ok = 1;
    goto done;
oom:
    snprintf(why, why_cap, "out of memory");
done:
    free(added.v);
    free(seq);
    free(present);
    free(from);
    free(drop);
    return ok;
}

// ---- bdat-toolset JSON tables
//
// Some mods ship only the tables they change, as bdat-toolset writes them with -f json, one file per table under
// bdat/<file>/<table>.json: {"schema": [{"name": n, "type": t}, ...], "rows": [{"$id": i, n: value, ...}, ...]}.
// A name is "<8 hex digits>" (the hash itself) or the plain name, which hashes with murmur3 (seed 0) to the game's.
// Hash cells are written the same way, strings as JSON strings, numbers as JSON numbers. Only whole tables are read
// (rows numbered one after another); such a table is the mod's copy of that table, and the game's copy (or the mod's
// full file) gives the rest. The reader is strict JSON (RFC 8259), with no NaN or Infinity and only valid UTF-8.

static uint32_t mm3(const uint8_t *d, size_t n) {
    uint32_t h = 0, k;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        k = rd32(d + i) * 0xcc9e2d51u;
        k = (k << 15 | k >> 17) * 0x1b873593u;
        h ^= k;
        h = (h << 13 | h >> 19) * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (n & 3) {
    case 3: k ^= (uint32_t)d[i + 2] << 16; /* fall through */
    case 2: k ^= (uint32_t)d[i + 1] << 8; /* fall through */
    case 1:
        k ^= d[i];
        k *= 0xcc9e2d51u;
        k = (k << 15 | k >> 17) * 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A name as bdat-toolset writes it: "<8 hex digits>" is the hash itself, anything else is hashed.
static uint32_t hashname(const char *s, size_t n) {
    if (n == 10 && s[0] == '<' && s[9] == '>') {
        uint32_t v = 0;
        int ok = 1;
        for (int i = 1; i < 9; i++) {
            int x = hexval((unsigned char)s[i]);
            ok &= x >= 0;
            v = v << 4 | (uint32_t)(x & 15);
        }
        if (ok) return v;
    }
    return mm3((const uint8_t *)s, n);
}

uint32_t bdat_merge_name_hash(const char *s, size_t n) { return hashname(s, n); }

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct JNode JNode;
struct JNode {
    uint8_t type, isint;
    int64_t i;            // J_NUM with isint
    double d;             // J_NUM
    char *s;              // J_STR: UTF-8, NUL-terminated (may hold NULs: len counts them)
    size_t len;           // J_STR length; J_ARR / J_OBJ item count
    JNode **items;        // J_ARR values; J_OBJ values
    char **keys;          // J_OBJ keys
    size_t *klen;
};

// The parsed JSON's memory: taken from big blocks and given back all at once when the table is made. A large table's
// hundreds of thousands of nodes, keys and strings were as many mallocs and frees, most of the time of reading it.
typedef struct Block Block;
struct Block {
    Block *next;
    size_t used, cap;
};
#define BLOCK_HEAD ((sizeof(Block) + 15) & ~(size_t)15)
typedef struct { Block *head; int nblocks; } Arena;

// n bytes (16-aligned, not zeroed) from a; NULL when out of memory. Blocks grow from 1 MiB to 32 MiB.
static void *aget(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    Block *b = a->head;
    if (!b || b->cap - b->used < n) {
        size_t cap = (size_t)1 << (20 + (a->nblocks < 5 ? a->nblocks : 5));
        if (cap < n) cap = n;
        if (!(b = malloc(BLOCK_HEAD + cap))) return NULL;
        *b = (Block){a->head, 0, cap};
        a->head = b;
        a->nblocks++;
    }
    void *p = (uint8_t *)b + BLOCK_HEAD + b->used;
    b->used += n;
    return p;
}

static void afree(Arena *a) {
    for (Block *b = a->head, *next; b; b = next) {
        next = b->next;
        free(b);
    }
    *a = (Arena){0};
}

typedef struct { const char *p, *end; int depth, bad; Arena *a; } JParse;

static void jws(JParse *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int put_utf8(Buf *b, uint32_t c) {
    uint8_t u[4];
    size_t n;
    if (c < 0x80) u[0] = (uint8_t)c, n = 1;
    else if (c < 0x800) u[0] = (uint8_t)(0xc0 | c >> 6), u[1] = (uint8_t)(0x80 | (c & 63)), n = 2;
    else if (c < 0x10000)
        u[0] = (uint8_t)(0xe0 | c >> 12), u[1] = (uint8_t)(0x80 | (c >> 6 & 63)), u[2] = (uint8_t)(0x80 | (c & 63)), n = 3;
    else
        u[0] = (uint8_t)(0xf0 | c >> 18), u[1] = (uint8_t)(0x80 | (c >> 12 & 63)), u[2] = (uint8_t)(0x80 | (c >> 6 & 63)),
        u[3] = (uint8_t)(0x80 | (c & 63)), n = 4;
    return put(b, u, n);
}

// A JSON string at j->p (on the opening quote) into *s (in j->a, NUL-terminated) and *len.
static int jstring(JParse *j, char **s, size_t *len) {
    // A string without escapes (nearly every string in a bdat-toolset table) is its bytes as they are. The Buf below
    // starts at 4 KiB, which for a table's hundreds of thousands of names and values made the parse most of a merge's
    // time.
    const char *q = j->p + 1;
    while (q < j->end && *q != '"' && *q != '\\' && (unsigned char)*q >= 0x20) q++;
    if (q < j->end && *q == '"') {
        size_t n = (size_t)(q - j->p - 1);
        char *o = aget(j->a, n + 1);
        if (!o) return 0;
        memcpy(o, j->p + 1, n);
        o[n] = 0;
        *s = o;
        *len = n;
        j->p = q + 1;
        return 1;
    }
    Buf b = {0};
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p;
        if (c < 0x20) goto bad;
        if (c != '\\') {
            if (!put(&b, j->p++, 1)) goto bad;
            continue;
        }
        if (++j->p >= j->end) goto bad;
        char e = *j->p++;
        const char *simple = strchr("\"\\/bfnrt", e);
        if (simple && e) {
            static const char out[] = "\"\\/\b\f\n\r\t";
            if (!put(&b, &out[simple - "\"\\/bfnrt"], 1)) goto bad;
            continue;
        }
        if (e != 'u' || j->end - j->p < 4) goto bad;
        uint32_t u = 0;
        for (int i = 0; i < 4; i++) {
            int x = hexval((unsigned char)j->p[i]);
            if (x < 0) goto bad;
            u = u << 4 | (uint32_t)x;
        }
        j->p += 4;
        if (u >= 0xdc00 && u < 0xe000) goto bad;   // half a surrogate pair is not text
        if (u >= 0xd800 && u < 0xdc00) {
            if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') goto bad;
            uint32_t lo = 0;
            for (int i = 0; i < 4; i++) {
                int x = hexval((unsigned char)j->p[2 + i]);
                if (x < 0) goto bad;
                lo = lo << 4 | (uint32_t)x;
            }
            if (lo < 0xdc00 || lo >= 0xe000) goto bad;
            j->p += 6;
            u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
        }
        if (!put_utf8(&b, u)) goto bad;
    }
    if (j->p >= j->end || !put(&b, "", 1) || !(*s = aget(j->a, b.n))) goto bad;
    j->p++;
    memcpy(*s, b.p, b.n);
    *len = b.n - 1;
    free(b.p);
    return 1;
bad:
    free(b.p);
    return 0;
}

static JNode *jvalue(JParse *j);

// n's lists in a, grown by doubling; the lists they replace stay in a until it is given back.
static int jadd(Arena *a, JNode *n, JNode *v, char *key, size_t klen) {
    if ((n->len & (n->len - 1)) == 0) {   // grow at 0, 1, 2, 4, 8 ...
        size_t cap = n->len ? n->len * 2 : 1;
        JNode **it = aget(a, cap * sizeof *it);
        if (!it) return 0;
        if (n->len) memcpy(it, n->items, n->len * sizeof *it);
        n->items = it;
        if (n->type == J_OBJ) {
            char **k = aget(a, cap * sizeof *k);
            size_t *kl = aget(a, cap * sizeof *kl);
            if (!k || !kl) return 0;
            if (n->len) {
                memcpy(k, n->keys, n->len * sizeof *k);
                memcpy(kl, n->klen, n->len * sizeof *kl);
            }
            n->keys = k;
            n->klen = kl;
        }
    }
    n->items[n->len] = v;
    if (n->type == J_OBJ) {
        n->keys[n->len] = key;
        n->klen[n->len] = klen;
    }
    n->len++;
    return 1;
}

static JNode *jvalue(JParse *j) {
    jws(j);
    if (j->p >= j->end || ++j->depth > 500) return NULL;
    JNode *n = aget(j->a, sizeof *n);
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    char c = *j->p;
    if (c == '{' || c == '[') {
        n->type = c == '{' ? J_OBJ : J_ARR;
        j->p++;
        jws(j);
        if (j->p < j->end && *j->p == (c == '{' ? '}' : ']')) {
            j->p++;
            j->depth--;
            return n;
        }
        for (;;) {
            char *key = NULL;
            size_t klen = 0;
            if (n->type == J_OBJ) {
                jws(j);
                if (j->p >= j->end || *j->p != '"' || !jstring(j, &key, &klen)) goto bad;
                jws(j);
                if (j->p >= j->end || *j->p != ':') goto bad;
                j->p++;
            }
            JNode *v = jvalue(j);
            if (!v || !jadd(j->a, n, v, key, klen)) goto bad;   // what was taken stays in j->a until it is given back
            jws(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->end && *j->p == (c == '{' ? '}' : ']')) {
                j->p++;
                break;
            }
            goto bad;
        }
    } else if (c == '"') {
        n->type = J_STR;
        if (!jstring(j, &n->s, &n->len)) goto bad;
    } else if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) {
        n->type = J_TRUE;
        j->p += 4;
    } else if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) {
        n->type = J_FALSE;
        j->p += 5;
    } else if (j->end - j->p >= 4 && !memcmp(j->p, "null", 4)) {
        n->type = J_NULL;
        j->p += 4;
    } else {
        // -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
        const char *s = j->p, *q = s;
        n->type = J_NUM;
        n->isint = 1;
        if (q < j->end && *q == '-') q++;
        if (q >= j->end || *q < '0' || *q > '9') goto bad;
        if (*q == '0') q++;
        else
            while (q < j->end && *q >= '0' && *q <= '9') q++;
        if (q < j->end && *q == '.') {
            n->isint = 0;
            if (++q >= j->end || *q < '0' || *q > '9') goto bad;
            while (q < j->end && *q >= '0' && *q <= '9') q++;
        }
        if (q < j->end && (*q == 'e' || *q == 'E')) {
            n->isint = 0;
            if (++q < j->end && (*q == '+' || *q == '-')) q++;
            if (q >= j->end || *q < '0' || *q > '9') goto bad;
            while (q < j->end && *q >= '0' && *q <= '9') q++;
        }
        char tmp[400];
        size_t len = (size_t)(q - s);
        if (len >= sizeof tmp) goto bad;
        memcpy(tmp, s, len);
        tmp[len] = 0;
        n->d = strtod(tmp, NULL);
        if (n->isint) {
            // an integer past 64 bits is still an integer, just out of every column's range
            int neg = tmp[0] == '-';
            uint64_t v = 0;
            int big = 0;
            for (const char *x = tmp + neg; *x; x++) {
                if (v > (UINT64_MAX - 9) / 10) big = 1;
                v = v * 10 + (uint64_t)(*x - '0');
            }
            if (big || v > (uint64_t)INT64_MAX) n->i = neg ? INT64_MIN : INT64_MAX, n->isint = 2;   // out of range
            else n->i = neg ? -(int64_t)v : (int64_t)v;
        }
        j->p = q;
    }
    j->depth--;
    return n;
bad:   // n and what it holds stay in j->a until it is given back
    return NULL;
}

// Valid UTF-8, without overlong forms or surrogates (as Python's decoder takes it).
static int utf8_ok(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!k || i + k > n) return 0;
        uint32_t u = k == 1 ? c : k == 2 ? c & 31u : k == 3 ? c & 15u : c & 7u;
        for (size_t x = 1; x < k; x++) {
            if ((p[i + x] & 0xc0) != 0x80) return 0;
            u = u << 6 | (p[i + x] & 63u);
        }
        if ((k == 2 && u < 0x80) || (k == 3 && u < 0x800) || (k == 4 && (u < 0x10000 || u > 0x10ffff)) ||
            (u >= 0xd800 && u < 0xe000))
            return 0;
        i += k;
    }
    return 1;
}

// The value of key in object o (the last one when a key repeats, as Python's json does), or NULL.
static const JNode *jget(const JNode *o, const char *key, size_t klen) {
    if (!o || o->type != J_OBJ) return NULL;
    for (size_t i = o->len; i-- > 0;)
        if (o->klen[i] == klen && !memcmp(o->keys[i], key, klen)) return o->items[i];
    return NULL;
}

static int resolve(const BdatInput *in, const char *s, size_t n, uint32_t *out, char *why, size_t cap);
static int fits(uint8_t ty, int64_t v);

// A JSON table as BDAT table bytes in out (the writer's layout), or 0 with the reason in why. in: the mod, whose
// numbers a '@kind:name' string in an integer cell stands for (resolve). A "table" string names the table instead of
// the file name (*named; '@kind:name' in it is resolved too).
static int json_table(const uint8_t *data, size_t size, const char *stem, const BdatInput *in, Buf *out, int *named,
                      char *why, size_t cap) {
    JNode *root = NULL;
    Arena arena = {0};   // the parsed JSON, given back at the end
    Buf raw = {0};
    Src *from = NULL;
    Tab t = {0};
    int ok = 0;
    uint32_t cc = 0, rc = 0;
    if (!utf8_ok(data, size)) goto notjson;
    JParse jp = {(const char *)data, (const char *)data + size, 0, 0, &arena};
    root = jvalue(&jp);
    jws(&jp);
    if (!root || jp.p != jp.end) goto notjson;
    const JNode *schema = jget(root, "schema", 6), *rows = jget(root, "rows", 4);
    if (!schema || schema->type != J_ARR || !rows || rows->type != J_ARR) {
        snprintf(why, cap, "no schema or rows");
        goto done;
    }
    cc = (uint32_t)schema->len;
    rc = (uint32_t)rows->len;
    if (!cc || !rc || cc > 4096 || rows->len > 0xffffffu) {
        snprintf(why, cap, "no columns or no rows");
        goto done;
    }
    // a table of the writer's layout with each string written where it is met: read_table then write_table give
    // the canonical bytes
    uint8_t types[4096];
    uint32_t names[4096], rl = 0;
    for (uint32_t c = 0; c < cc; c++) {
        const JNode *col = schema->items[c], *nm = jget(col, "name", 4), *ty = jget(col, "type", 4);
        if (!nm || nm->type != J_STR || !ty || ty->type != J_NUM || ty->isint != 1 || ty->i < 1 || ty->i > 13) {
            snprintf(why, cap, "column %u: no name or no known type", c);
            goto done;
        }
        types[c] = (uint8_t)ty->i;
        names[c] = hashname(nm->s, nm->len);
        rl += TSIZE[types[c]];
    }
    Buf st = {0}, rb = {0};
    uint8_t head[9] = {0};
    wr32(head + 1, hashname(stem, strlen(stem)));
    if (!put(&st, head, 9)) goto oom_st;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t h[4];
        wr32(h, names[c]);
        if (!put(&st, h, 4)) goto oom_st;
    }
    if (!put(&rb, NULL, (size_t)rl * rc)) goto oom_st;
    int64_t first = 0;
    for (uint32_t r = 0; r < rc; r++) {
        const JNode *row = rows->items[r], *id = jget(row, "$id", 3);
        if (!row || row->type != J_OBJ || !id || id->type != J_NUM || id->isint != 1) {
            snprintf(why, cap, "row %u: no $id", r);
            goto fail_st;
        }
        if (!r) first = id->i;
        if (id->i != first + r || id->i < 0 || id->i > 0xffffffffll) {
            snprintf(why, cap, "its rows are not numbered one after another (a table with only some rows is not read)");
            goto fail_st;
        }
        uint8_t *d = rb.p + (size_t)r * rl;
        for (uint32_t c = 0; c < cc; c++) {
            const JNode *nm = jget(schema->items[c], "name", 4), *v = jget(row, nm->s, nm->len);
            uint8_t ty = types[c];
            if (!v) {
                snprintf(why, cap, "row %lld: no value for column %u", (long long)id->i, c);
                goto fail_st;
            }
            int good = 1;
            if (IS_STR(ty)) {
                good = v->type == J_STR && !memchr(v->s, 0, v->len);
                if (good) {
                    if (st.n + v->len + 1 > 0xffffffffu) good = 0;
                    else {
                        wr32(d, (uint32_t)st.n);
                        if (!put(&st, v->s, v->len + 1)) goto oom_st;
                    }
                }
            } else if (ty == 8) {
                double x = v->type != J_NUM ? 0 : v->isint == 1 ? (double)v->i : v->d;
                good = v->type == J_NUM && x >= -3.4028234663852886e38 && x <= 3.4028234663852886e38;
                float f = (float)x;
                memcpy(d, &f, 4);
            } else if (ty == 9 && v->type == J_STR) {
                wr32(d, hashname(v->s, v->len));
            } else if (v->type == J_STR && v->len && v->s[0] == '@') {
                uint32_t x;
                char r[200];
                if (!resolve(in, v->s, v->len, &x, r, sizeof r)) {
                    snprintf(why, cap, "row %lld column %u: %s", (long long)id->i, c, r);
                    goto fail_st;
                }
                good = fits(ty, x);
                memcpy(d, &x, TSIZE[ty]);
            } else if (v->type != J_NUM || v->isint != 1) {
                good = 0;
            } else {
                int64_t x = v->i;
                int64_t lo = ty == 4 ? -128 : ty == 5 ? -32768 : ty == 6 ? INT32_MIN : 0;
                int64_t hi = ty == 4 ? 127 : ty == 5 ? 32767 : ty == 6 ? INT32_MAX : TSIZE[ty] == 1 ? 255 :
                             TSIZE[ty] == 2 ? 65535 : 0xffffffffll;
                good = x >= lo && x <= hi;
                uint32_t u = (uint32_t)x;
                memcpy(d, &u, TSIZE[ty]);
            }
            if (!good) {
                snprintf(why, cap, "row %lld column %u: not a value of its type", (long long)id->i, c);
                goto fail_st;
            }
            d += TSIZE[ty];
        }
    }
    const JNode *tn = jget(root, "table", 5);
    *named = tn != NULL;
    if (tn) {
        if (tn->type != J_STR || !tn->len || memchr(tn->s, 0, tn->len)) {
            snprintf(why, cap, "its \"table\" is not a name");
            goto fail_st;
        }
        const char *at = memchr(tn->s, '@', tn->len);
        uint32_t hash;
        if (at) {
            uint32_t x;
            char name[400];
            size_t pre = (size_t)(at - tn->s);
            if (!resolve(in, at, tn->len - pre, &x, why, cap)) goto fail_st;
            if (pre > 300) {
                snprintf(why, cap, "its \"table\" is not a name");
                goto fail_st;
            }
            int w = snprintf(name, sizeof name, "%.*s%u", (int)pre, tn->s, x);
            hash = hashname(name, (size_t)w);
        } else {
            hash = hashname(tn->s, tn->len);
        }
        wr32(st.p + 1, hash);
    }
    uint32_t co = 48, ro = co + 3 * cc;
    uint64_t so = (uint64_t)ro + (uint64_t)rl * rc;
    uint8_t h[48];
    memcpy(h, "BDAT", 4);
    uint32_t f[11] = {0x3004, cc, rc, (uint32_t)first, 0, co, ro, ro, rl, (uint32_t)so, (uint32_t)st.n};
    if (so + st.n > 0xffffffffu) goto oom_st;
    for (int i = 0; i < 11; i++) wr32(h + 4 + 4 * i, f[i]);
    if (!put(&raw, h, 48)) goto oom_st;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t col[3] = {types[c]};
        wr16(col + 1, (uint16_t)(9 + 4 * c));
        if (!put(&raw, col, 3)) goto oom_st;
    }
    if (!put(&raw, rb.p, rb.n) || !put(&raw, st.p, st.n)) goto oom_st;
    free(st.p);
    free(rb.p);
    if (!read_table(raw.p, (uint32_t)raw.n, &t)) {
        snprintf(why, cap, "no table can be made of it");
        goto done;
    }
    const Tab *src[1] = {&t};
    if (!(from = malloc(((size_t)rc * cc + 1) * sizeof *from))) goto oom;
    for (uint32_t r = 0; r < rc; r++)
        for (uint32_t c = 0; c < cc; c++) from[(size_t)r * cc + c] = (Src){r, 0};
    if (!write_table(out, &t, src, from, rc, 0)) goto oom;
    ok = 1;
    goto done;
fail_st:
    free(st.p);
    free(rb.p);
    goto done;
oom_st:
    free(st.p);
    free(rb.p);
oom:
    snprintf(why, cap, "out of memory");
    goto done;
notjson:
    snprintf(why, cap, "not valid JSON");
done:
    afree(&arena);
    free(raw.p);
    free(from);
    free(t.off);
    return ok;
}

static int by_stem(const void *a, const void *b) {
    return strcmp(((const BdatJson *)a)->stem, ((const BdatJson *)b)->stem);
}

// A table a mod adds (docs/mod-ids.md, "New tables"): its bytes, its own (own.p) or in the mod's file, read as t.
typedef struct { Buf own; Tab t; } Added;
typedef struct { Added *v; int n, cap; } Adds;

static void adds_free(Adds *a) {
    for (int i = 0; i < a->n; i++) {
        free(a->v[i].own.p);
        free(a->v[i].t.off);
    }
    free(a->v);
    *a = (Adds){0};
}

static int adds_find(const Adds *a, uint32_t name) {
    for (int i = 0; i < a->n; i++)
        if (a->v[i].t.name == name) return i;
    return -1;
}

// Takes x (its Buf, if any, is a's now); 0 when out of memory.
static int adds_put(Adds *a, Added x) {
    if (a->n == a->cap) {
        int cap = a->cap ? a->cap * 2 : 8;
        Added *v = realloc(a->v, (size_t)cap * sizeof *v);
        if (!v) return 0;
        a->v = v;
        a->cap = cap;
    }
    a->v[a->n++] = x;
    return 1;
}

// file (the mod's full copy, or the game's) read into *out with the mod's JSON tables in place of those tables. No new
// file is written: *tabs_out (one Buf per table, empty but for the JSON ones) holds those tables' bytes for as long as
// *out is used, and file_with makes the file when one is needed whole. A JSON table with a "table" name of its own
// that the file doesn't have goes to *added (a new table).
static int apply_json(const uint8_t *file, size_t size, const BdatInput *in, int k, Events *ev, File *out,
                      Buf **tabs_out, Adds *added, char *why, size_t cap) {
    File f;
    char r[300];
    if (!read_file(file, size, &f, r, sizeof r)) {
        snprintf(why, cap, "%s", r);
        return 0;
    }
    BdatJson *js = malloc((size_t)in->njson * sizeof *js);
    Buf *tabs = calloc(f.count, sizeof *tabs);
    int ok = 0;
    if (!js || !tabs) goto done;
    memcpy(js, in->json, (size_t)in->njson * sizeof *js);
    qsort(js, (size_t)in->njson, sizeof *js, by_stem);
    for (int x = 0; x < in->njson; x++) {
        Buf t = {0};
        int named = 0;
        if (!json_table(js[x].data, js[x].size, js[x].stem, in, &t, &named, r, sizeof r)) {
            event(ev, LEFT_OUT, k, "its table %s.json is not used (%s)", js[x].stem, r);
            free(t.p);
            continue;
        }
        uint32_t name = rd32(t.p + rd32(t.p + 40) + 1), j = 0;
        while (named && j < f.count && f.t[j].name != name) j++;
        if (named && j == f.count) {
            Added a = {t, {0}};
            if (adds_find(added, name) >= 0) {
                event(ev, LEFT_OUT, k, "its table %s.json is not used (another of its tables has that name)",
                      js[x].stem);
                free(t.p);
            } else if (!read_table(a.own.p, (uint32_t)a.own.n, &a.t) || !adds_put(added, a)) {
                free(a.t.off);
                free(t.p);
                goto done;
            }
            continue;
        }
        j = 0;
        // bdat-toolset names a table known only by its hash with the 8 hex digits, no brackets ("1A2B3C4D.json"):
        // such a name is the hash when the game's file has that table (else, as any name, its murmur3)
        int hex8 = !named && strlen(js[x].stem) == 8;
        uint32_t h = 0;
        for (int i = 0; hex8 && i < 8; i++) {
            int v = hexval((unsigned char)js[x].stem[i]);
            hex8 = v >= 0;
            h = h << 4 | (uint32_t)(v & 15);
        }
        for (uint32_t i = 0; hex8 && i < f.count; i++)
            if (f.t[i].name == h) {
                name = h;
                wr32(t.p + rd32(t.p + 40) + 1, h);   // the name is in the string table only
                break;
            }
        while (j < f.count && f.t[j].name != name) j++;
        if (j == f.count) {
            event(ev, LEFT_OUT, k, "its table %s.json is not used (the game's file has no such table)", js[x].stem);
            free(t.p);
            continue;
        }
        free(tabs[j].p);
        tabs[j] = t;
    }
    uint64_t total = 16 + 4 * (uint64_t)f.count;   // the file it would be must still fit a BDAT file's 32-bit sizes
    for (uint32_t j = 0; j < f.count; j++) total += tabs[j].p ? tabs[j].n : f.t[j].len;
    if (total > 0xffffffffu) goto done;
    for (uint32_t j = 0; j < f.count; j++) {
        Tab t;
        if (!tabs[j].p) continue;
        if (!read_table(tabs[j].p, (uint32_t)tabs[j].n, &t)) {
            snprintf(why, cap, "table %u is damaged", j);
            goto done;
        }
        free(f.t[j].off);
        f.t[j] = t;
    }
    ok = 1;
done:
    if (!ok && !why[0]) snprintf(why, cap, "out of memory");
    if (ok) {
        *out = f;
        *tabs_out = tabs;
    } else {
        for (uint32_t j = 0; tabs && j < f.count; j++) free(tabs[j].p);
        free(tabs);
        file_free(&f);
    }
    free(js);
    return ok;
}

// The hash values a mod's changes put in cells other than keys: what its added and edited rows refer to.
static int refs_of(const File *bf, const File *mf, const Change *ch, Set *out) {
    for (uint32_t j = 0; j < bf->count; j++) {
        const Change *c = &ch[j];
        if (c->kind != CHANGES) continue;
        const Tab *b = &bf->t[j], *m = &mf->t[j];
        for (uint32_t i = 0; i < m->rc; i++) {
            uint32_t r = c->pos[i];
            if (r != NONE && !c->edited[r]) continue;
            for (uint32_t col = 1; col < m->cc; col++)
                if (ctype(m, col) == 9 && (r == NONE || !same_cell(b, r, m, i, col)) && !set_add(out, rd32(cell(m, i, col))))
                    return 0;
        }
    }
    return 1;
}

static uint8_t *copy_of(const BdatInput *in, size_t *out_n) {
    uint8_t *p = malloc(in->size ? in->size : 1);
    if (p) {
        memcpy(p, in->data, in->size);
        *out_n = in->size;
    }
    return p;
}

// ---- numbers handed out (docs/mod-ids.md)
//
// A content mod adds things the game finds by number (a region id, a flag id, a survey reward's claimed index, a row
// id). runtime/mod_ids.c hands every number a mod.json names out once, so two mods never share one; here each mod's
// copy gets its numbers before anything is compared: '@kind:name' in its JSON tables, its own (local) numbers in the
// cells known to hold one of that kind (the census below), and the rows whose number is their row id put at that id.

#define FIELD_NAMES 0x97991FA4u   // FLD_FieldNameList: region id = row id
#define KIND_NAME(k) bdat_id_kind_name(k)
// The census of where a number of each kind lives (docs/mod-ids.md "Moving a number" lists the same; change both, and
// the merge's reference). Columns named zone or ZoneID hold a region id in any table; these pairs hold one under
// other names (the columns' names are not known: hashes as the game's files have them).
static const uint32_t REGION_USES[][2] = {
    {0xFC830E48u, 0x8C44DF98u},   // FLD_WeatherRefer
    {0x7A47835Eu, 0x8C44DF98u},   // MNU_ZoneNameConvert
    {0x12E0693Fu, 0x554A58C8u},   // MNU_MapZoneInfo
    {0x06FBB0CFu, 0x554A58C8u},   // the map's location rows (one per landmark, camp and FrontierNav site; name not known)
};
static const char *const REGION_TABLES[] = {"SEG_TerminalZone", "MNU_SegMapInfoZone", "SEG_ProbeListZone"};
#define REWARD_TABLE 0xFA12D19Bu   // SEG_ExploreReward
#define REWARD_COL 0x91C93FADu     // flag_index
#define VEIN 0xF63C8049u        // FnetVeinList: a FrontierNav site's number is its row id (base 0)
#define LOCATIONS 0xD12FB959u   // the location table: a location's id is its row id
#define LOC_MENU 0x06FBB0CFu    // the map's location rows: row for row with the location table
#define PROBES 0x8B4096AEu      // SEG_ProbeList: a probe hexagon's segment id is its row id
// Table families (in place of one table's name below): the hexagon tables (SEG_Terminal<zone>), the Segments tab's
// markers (MNU_SegMapInfo<zone>) and the probe lists (SEG_ProbeList, SEG_ProbeListZone<id>): the bare name, the
// game's zones, and Zone0..Zone255 (a mod region's own table).
#define FAM_HEX 0xFFFFFF01u
#define FAM_SEGMAP 0xFFFFFF02u
#define FAM_PROBE 0xFFFFFF03u
static const char *const FAM_PREFIX[3] = {"SEG_Terminal", "MNU_SegMapInfo", "SEG_ProbeList"};
static const char *const GAME_ZONES[] = {"1001", "1101", "1201", "1301", "1401", "1501", "1601", "1701", "2001", "2201",
                                         "5000"};
#define FAM_SIZE (1 + 11 + 256)
// (table or a family; column) pairs that hold a site number
static const uint32_t SITE_USES[][2] = {
    {VEIN, 0xBF16FE7Du}, {VEIN, 0x3791D641u}, {VEIN, 0xF14C0AACu}, {VEIN, 0x38E329FDu}, {VEIN, 0xD270368Au},
    {LOCATIONS, 0x655B3679u},   // a location's site number (F_spot)
    {FAM_PROBE, 0x073607AAu},   // a probe list row's site (SEG_ProbeList and the SEG_ProbeListZone<id> tables)
};
// A row of table [0] is pointed at by column [2] of table [1] (a table, or a family), in the rows whose column [3]
// (when not 0) holds [4].
static const uint32_t ROW_REFS[][5] = {
    {0x7A058ADAu, FIELD_NAMES, 0xBFCD306Du, 0, 0},   // FLD_NameCondition <- FLD_FieldNameList.nameCondition
    {LOCATIONS, LOCATIONS, 0x9759C6FAu, 0, 0},       // a location's own id
    {LOCATIONS, VEIN, 0xDBAF43F0u, 0, 0},            // a site's location
    {LOCATIONS, 0x097D1D32u, 0x70F37937u, 0, 0},     // map marks
    {LOCATIONS, FAM_HEX, 0x018D2A75u, 0, 0},         // a hexagon's field location
    {LOCATIONS, FAM_SEGMAP, 0x690F4E13u, 0, 0},      // Segments-tab markers
    {PROBES, FAM_HEX, 0x70BAC821u, 0x5BF33ED8u, 1},  // a probe hexagon's segment id (hexagon type 1)
};
// A row placed in table [0] takes the same row of table [1] with it.
static const uint32_t PARALLEL[][2] = {{LOCATIONS, LOC_MENU}};
// The row a gap row is a copy of (a number of a mod that is off), by row index; else the table's last row: for
// FLD_FieldNameList its row 20, a placeholder region no part of the world raster has; for the site table the game's
// unused site 47 (with no location and the gap's own number, below); for the location table and its menu rows the
// game's unused camp slot CAMP_1501_10 (no box, no name, no map row), so a gap is never a second copy of a real place;
// for the probe lists their unused row 6 (no site).
static const uint32_t FILLER[][2] = {{FIELD_NAMES, 19}, {VEIN, 47}, {LOCATIONS, 399}, {LOC_MENU, 399}, {PROBES, 6}};
#define VEIN_LOCATION 0xDBAF43F0u
#define VEIN_SITE 0xBF16FE7Du
// Columns of a gap row set to the gap's own row id ([2] = 1: a site's number, a location's own id) or to 0 (a site's
// location).
static const uint32_t GAP_SET[][3] = {{VEIN, VEIN_LOCATION, 0}, {VEIN, VEIN_SITE, 1}, {LOCATIONS, 0x9759C6FAu, 1}};
// Tables whose spare game rows content mods take over (they can't be handed out): the location table (D12FB959) and
// the map's location rows (06FBB0CF; names not known), FnetVeinList (the FrontierNav sites) and SEG_ProbeList.
static const uint32_t SLOT_TABLES[] = {LOCATIONS, LOC_MENU, VEIN, PROBES};
// Tables the game searches by a column (a region, or a location id for MNU_MapInfo), not by row number: a row they get
// at another number than in a mod's copy is no caveat.
static const uint32_t FOUND_BY_ZONE[] = {0xFC830E48u, 0xE6194796u, 0x12E0693Fu, 0x7A47835Eu, 0xFA12D19Bu, 0x097D1D32u};
#define NUM_OF(a) (sizeof(a) / sizeof((a)[0]))

static int in_list(const uint32_t *v, size_t n, uint32_t x) {
    for (size_t i = 0; i < n; i++)
        if (v[i] == x) return 1;
    return 0;
}

static int found_by_zone(uint32_t name) { return in_list(FOUND_BY_ZONE, NUM_OF(FOUND_BY_ZONE), name); }

static int int_type(uint8_t ty) { return ty && ty <= 13 && !IS_STR(ty) && ty != 8 && ty != 9; }
static int flag_type(uint8_t ty) { return ty == 2 || ty == 3 || ty == 13; }

static int fits(uint8_t ty, int64_t v) {
    if (ty == 4) return v >= -128 && v <= 127;
    if (ty == 5) return v >= -32768 && v <= 32767;
    if (ty == 6) return v >= INT32_MIN && v <= INT32_MAX;
    return v >= 0 && v <= (TSIZE[ty] == 1 ? 255 : TSIZE[ty] == 2 ? 65535 : 0xffffffffll);
}

static uint32_t col_of(const Tab *t, uint32_t h) {
    for (uint32_t c = 0; c < t->cc; c++)
        if (cname(t, c) == h) return c;
    return NONE;
}

// The families' name hashes, made once (the merge runs on several threads: one makes them, 804 hashes in some
// microseconds, while the others spin).
static uint32_t fam_names[3][FAM_SIZE];
static int fam_state;   // 0 not made, 1 being made, 2 made

static int in_family(uint32_t fam, uint32_t name) {
    if (fam < FAM_HEX || fam > FAM_PROBE) return 0;
    int st = __atomic_load_n(&fam_state, __ATOMIC_ACQUIRE);
    if (st != 2) {
        int zero = 0;
        if (__atomic_compare_exchange_n(&fam_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            for (int f = 0; f < 3; f++) {
                char b[64];
                int k = 0, n = snprintf(b, sizeof b, "%s", FAM_PREFIX[f]);
                fam_names[f][k++] = mm3((const uint8_t *)b, (size_t)n);
                for (size_t z = 0; z < NUM_OF(GAME_ZONES); z++) {
                    n = snprintf(b, sizeof b, "%s%s", FAM_PREFIX[f], GAME_ZONES[z]);
                    fam_names[f][k++] = mm3((const uint8_t *)b, (size_t)n);
                }
                for (int z = 0; z < 256; z++) {
                    n = snprintf(b, sizeof b, "%sZone%d", FAM_PREFIX[f], z);
                    fam_names[f][k++] = mm3((const uint8_t *)b, (size_t)n);
                }
            }
            __atomic_store_n(&fam_state, 2, __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&fam_state, __ATOMIC_ACQUIRE) != 2) {}
        }
    }
    const uint32_t *v = fam_names[fam - FAM_HEX];
    for (int i = 0; i < FAM_SIZE; i++)
        if (v[i] == name) return 1;
    return 0;
}

// Table name (a table's own) matches x (a table's name, or a family).
static int table_is(uint32_t x, uint32_t name) { return x == name || in_family(x, name); }

static int site_col(uint32_t table, uint32_t col) {
    for (size_t i = 0; i < NUM_OF(SITE_USES); i++)
        if (SITE_USES[i][1] == col && table_is(SITE_USES[i][0], table)) return 1;
    return 0;
}

static int64_t ival(const Tab *t, uint32_t r, uint32_t c) {
    const uint8_t *p = cell(t, r, c);
    switch (ctype(t, c)) {
    case 4: return (int8_t)p[0];
    case 5: return (int16_t)rd16(p);
    case 6: return (int32_t)rd32(p);
    case 3: return rd32(p);
    default: return TSIZE[ctype(t, c)] == 1 ? p[0] : rd16(p);
    }
}

static void set_ival(uint8_t *p, uint8_t ty, int64_t v) {
    uint32_t u = (uint32_t)v;
    memcpy(p, &u, TSIZE[ty]);
}

// '@kind:name' (s, n bytes) -> the number in's entry of that kind and name was given; 0 with the reason in why.
static int resolve(const BdatInput *in, const char *s, size_t n, uint32_t *out, char *why, size_t cap) {
    const char *colon = n > 1 ? memchr(s + 1, ':', n - 1) : NULL;
    if (colon) {
        size_t kn = (size_t)(colon - s - 1), nn = n - (size_t)(colon - s) - 1;
        for (int i = 0; in && i < in->nids; i++) {
            const BdatId *e = &in->ids[i];
            const char *k = KIND_NAME(e->kind);
            if (strlen(k) == kn && !memcmp(k, s + 1, kn) && strlen(e->name) == nn && !memcmp(e->name, colon + 1, nn)) {
                *out = e->number;
                return 1;
            }
        }
    }
    snprintf(why, cap, "it names %.*s, which its mod.json does not declare", (int)(n > 120 ? 120 : n), s);
    return 0;
}

// A mod's local numbers that it was given others for: region, flag, reward (local -> number, the first entry of a
// local wins), and rows: (table, column) holding a row id -> (local -> number).
typedef struct { uint32_t table, col, local, number, cond_col, cond_val; uint8_t kind; } Move;
typedef struct { Move *v; int n, cap; } Moves;

static int move_add(Moves *m, uint8_t kind, uint32_t table, uint32_t col, uint32_t local, uint32_t number,
                    uint32_t cond_col, uint32_t cond_val) {
    for (int i = 0; i < m->n; i++)
        if (m->v[i].kind == kind && m->v[i].table == table && m->v[i].col == col && m->v[i].local == local &&
            m->v[i].cond_col == cond_col && m->v[i].cond_val == cond_val)
            return 1;
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 32;
        Move *v = realloc(m->v, (size_t)cap * sizeof *v);
        if (!v) return 0;
        m->v = v;
        m->cap = cap;
    }
    m->v[m->n++] = (Move){table, col, local, number, cond_col, cond_val, kind};
    return 1;
}

// bf: the game's file, whose tables say where a row's numbers start: a row whose number or own id is one of the game's
// rows moves nothing (place_rows leaves it out with a line); a row of another file's table can't be checked here.
static int moves_of(const BdatInput *in, const File *bf, Moves *m) {
    for (int i = 0; i < in->nids; i++) {
        const BdatId *e = &in->ids[i];
        if (!e->has_local || e->local == e->number) continue;
        if (e->kind != BDAT_ID_ROW) {
            if (!move_add(m, e->kind, 0, 0, e->local, e->number, 0, 0)) return 0;
            continue;
        }
        int game_row = 0;
        for (uint32_t j = 0; j < bf->count; j++)
            if (bf->t[j].name == e->table)
                game_row = e->number < bf->t[j].base + bf->t[j].rc || e->local < bf->t[j].base + bf->t[j].rc;
        if (game_row) continue;
        for (size_t r = 0; r < NUM_OF(ROW_REFS); r++)
            if (ROW_REFS[r][0] == e->table && !move_add(m, BDAT_ID_ROW, ROW_REFS[r][1], ROW_REFS[r][2], e->local,
                                                        e->number, ROW_REFS[r][3], ROW_REFS[r][4]))
                return 0;
        for (int r = 0; r < e->nrefs; r++)
            if (!move_add(m, BDAT_ID_ROW, e->refs[2 * r], e->refs[2 * r + 1], e->local, e->number, 0, 0)) return 0;
    }
    return 1;
}

// The number local v (in row r of table t, column col) gets, by a move of that kind; a row move only in its column
// (of its table, or any table) and in rows that hold its condition.
static int moved(const Moves *m, uint8_t kind, const Tab *t, uint32_t r, uint32_t col, int64_t v, uint32_t *out) {
    if (v < 0 || v > 0xffffffffll) return 0;
    for (int i = 0; i < m->n; i++) {
        const Move *x = &m->v[i];
        if (x->kind != kind || x->local != (uint32_t)v) continue;
        if (kind == BDAT_ID_ROW) {
            if (x->col != col || !table_is(x->table, t->name)) continue;
            if (x->cond_col) {
                uint32_t ci = col_of(t, x->cond_col);
                if (ci == NONE || ival(t, r, ci) != (int64_t)x->cond_val) continue;
            }
        }
        *out = x->number;
        return 1;
    }
    return 0;
}

#define H_ZONE 0x89194B91u      // murmur3 of "zone"
#define H_ZONE_ID 0x4F89BA86u   // of "ZoneID"

static int region_col(uint32_t table, uint32_t col) {
    if (col == H_ZONE || col == H_ZONE_ID) return 1;
    for (size_t i = 0; i < NUM_OF(REGION_USES); i++)
        if (REGION_USES[i][0] == table && REGION_USES[i][1] == col) return 1;
    return 0;
}

// The number a cell of a mod's (local number v, row r, column c of t) gets instead, if any.
static int new_value(const Moves *m, const Tab *t, uint32_t r, uint32_t c, int64_t v, uint32_t *out) {
    uint32_t h = cname(t, c);
    if (region_col(t->name, h) && moved(m, BDAT_ID_REGION, t, r, h, v, out)) return 1;
    if (site_col(t->name, h) && moved(m, BDAT_ID_SITE, t, r, h, v, out)) return 1;
    if (moved(m, BDAT_ID_ROW, t, r, h, v, out)) return 1;
    if (t->name == REWARD_TABLE && h == REWARD_COL && moved(m, BDAT_ID_REWARD, t, r, h, v, out)) return 1;
    return flag_type(ctype(t, c)) && moved(m, BDAT_ID_FLAG, t, r, h, v, out);
}

// The bytes a table is read from, its own (a copy the first time), so its cells can be set in place.
static int own_bytes(Buf *b, Tab *t) {
    if (b->p && b->p == t->p) return 1;
    Buf n = {0};
    Tab nt;
    if (!put(&n, t->p, t->len)) return 0;
    if (!read_table(n.p, (uint32_t)n.n, &nt)) {
        free(n.p);
        return 0;
    }
    free(t->off);
    free(b->p);
    *b = n;
    *t = nt;
    return 1;
}

// Table t (own bytes in b) with its local numbers moved where g (the game's, or NULL for a new table) doesn't have
// the same value at the same place. *unfit counts numbers that don't fit their column; 0 when out of memory.
static int move_cells(const Moves *m, Buf *b, Tab *t, const Tab *g, int *unfit) {
    int same = g && same_cols(g, t);
    uint8_t cand[4096];   // columns whose cells may hold a number to move (read_table: at most 4096)
    int any = 0;
    for (uint32_t c = 0; c < t->cc; c++) {
        uint32_t h = cname(t, c);
        uint8_t ty = ctype(t, c);
        cand[c] = int_type(ty) && (region_col(t->name, h) || flag_type(ty) || site_col(t->name, h) ||
                                   (t->name == REWARD_TABLE && h == REWARD_COL));
        for (int i = 0; !cand[c] && int_type(ty) && i < m->n; i++)
            cand[c] = m->v[i].kind == BDAT_ID_ROW && m->v[i].col == h && table_is(m->v[i].table, t->name);
        any |= cand[c];
    }
    if (!any) return 1;
    for (uint32_t r = 0; r < t->rc; r++) {
        if (same && r < g->rc && !memcmp(g->p + g->ro + (size_t)r * g->rl, t->p + t->ro + (size_t)r * t->rl, t->rl))
            continue;   // the game's row byte for byte: no number of the mod's in it
        for (uint32_t c = 0; c < t->cc; c++) {
            uint8_t ty = ctype(t, c);
            if (!cand[c]) continue;
            if (same && r < g->rc && !memcmp(cell(g, r, c), cell(t, r, c), TSIZE[ty])) continue;
            uint32_t nv;
            if (!new_value(m, t, r, c, ival(t, r, c), &nv)) continue;
            if (!fits(ty, nv)) {
                (*unfit)++;
                continue;
            }
            if (!own_bytes(b, t)) return 0;
            set_ival(b->p + t->ro + (size_t)r * t->rl + t->off[c], ty, nv);
        }
    }
    return 1;
}

static uint32_t name_with(const char *tmpl, uint32_t id) {
    char s[64];
    int n = snprintf(s, sizeof s, "%s%u", tmpl, id);
    return mm3((const uint8_t *)s, (size_t)n);
}

static int by_added_name(const void *a, const void *b) {
    uint32_t x = (*(Added *const *)a)->t.name, y = (*(Added *const *)b)->t.name;
    return (x > y) - (x < y);
}

// ---- each mod's own work, possibly at the same time (bdat_merge_set_parallel)

static BdatParallelFor parallel;

void bdat_merge_set_parallel(BdatParallelFor f) { parallel = f; }

static void each(int n, void (*fn)(void *ctx, int i), void *ctx) {
    if (parallel && n > 1) {
        parallel(n, fn, ctx);
        return;
    }
    for (int i = 0; i < n; i++) fn(ctx, i);
}

// The events of one mod's work, kept with it and passed on in mod order afterwards, so the report reads the same
// however the work was spread: its lines one after another, each with its NUL, and its counts.
typedef struct { Buf lines; BdatSummary s; int oom; } Kept;

static void keep_line(void *ctx, const char *line) {
    Kept *k = ctx;
    if (!put(&k->lines, line, strlen(line) + 1)) k->oom = 1;
}

static void pass_on(Events *ev, const Kept *k) {
    for (size_t o = 0; ev->note && o < k->lines.n; o += strlen((const char *)k->lines.p + o) + 1)
        ev->note(ev->ctx, (const char *)k->lines.p + o);
    ev->s->priority += k->s.priority;
    ev->s->left_out += k->s.left_out;
    ev->s->caveats += k->s.caveats;
    ev->s->adds_nothing += k->s.adds_nothing;
    ev->s->partly_alike += k->s.partly_alike;
}

// A mod's file as it is merged: its JSON tables put in, then read; its new tables apart; its numbers put in.
// state: what came of it.
enum { PREP_SAME_TABLES, PREP_UNREADABLE, PREP_OTHER_TABLES };
typedef struct {
    const BdatInput *base, *mods;
    const File *bf;
    File *mf;
    Buf *tabs[MAX_MODS];     // per table: bytes of its own (JSON tables, tables with numbers moved), which mf[k] uses
    uint32_t ntabs[MAX_MODS];
    Adds added[MAX_MODS];    // its new tables
    int state[MAX_MODS];
    char why[MAX_MODS][300];
    Kept kept[MAX_MODS];
} Prep;

// Mod k's table j replaced by the bytes in nb (taken, freed on failure); 0 when out of memory.
static int set_table(Prep *p, int k, uint32_t j, Buf nb) {
    File *f = &p->mf[k];
    Tab t;
    if (!p->tabs[k]) {
        if (!(p->tabs[k] = calloc(f->count, sizeof *p->tabs[k]))) {
            free(nb.p);
            return 0;
        }
        p->ntabs[k] = f->count;
    }
    if (!read_table(nb.p, (uint32_t)nb.n, &t)) {
        free(t.off);
        free(nb.p);
        return 0;
    }
    free(f->t[j].off);
    free(p->tabs[k][j].p);
    p->tabs[k][j] = nb;
    f->t[j] = t;
    return 1;
}

// A copy whose list is the game's with new tables among them: the new ones go to the mod's new tables (after its
// JSON ones of the same name, which come first). 1 when split, 0 when the list is another one, -1 out of memory.
static int split_added(Prep *p, int k) {
    File *f = &p->mf[k];
    const File *g = p->bf;
    uint32_t i = 0;
    for (uint32_t x = 0; x < f->count; x++) {
        uint32_t nm = f->t[x].name;
        if (i < g->count && nm == g->t[i].name) {
            i++;
            continue;
        }
        for (uint32_t y = 0; y < g->count; y++)
            if (g->t[y].name == nm) return 0;
        for (uint32_t y = 0; y < x; y++)
            if (f->t[y].name == nm) return 0;
    }
    if (i != g->count) return 0;
    Tab *keep = calloc(g->count, sizeof *keep);
    Buf *own = p->tabs[k] ? calloc(g->count, sizeof *own) : NULL;
    if (!keep || (p->tabs[k] && !own)) {
        free(keep);
        free(own);
        return -1;
    }
    i = 0;
    for (uint32_t x = 0; x < f->count; x++) {
        Buf b = p->tabs[k] ? p->tabs[k][x] : (Buf){0};
        if (i < g->count && f->t[x].name == g->t[i].name) {
            keep[i] = f->t[x];
            if (own) own[i] = b;
            i++;
            continue;
        }
        if (adds_find(&p->added[k], f->t[x].name) >= 0 || !adds_put(&p->added[k], (Added){b, f->t[x]})) {
            free(b.p);
            free(f->t[x].off);
        }
    }
    free(f->t);
    free(p->tabs[k]);
    f->t = keep;
    f->count = g->count;
    p->tabs[k] = own;
    p->ntabs[k] = own ? g->count : 0;
    return 1;
}

// Mod k's local numbers -> the numbers it was given, in the cells known to hold them, and its new tables named after a
// region renamed with it (docs/mod-ids.md, "Moving a number"). 0 when out of memory.
static int move_locals(Prep *p, int k, Events *ev) {
    const BdatInput *in = &p->mods[k];
    Moves m = {0};
    int ok = 0, unfit = 0;
    Added **order = NULL;
    uint32_t *keys = NULL;
    if (!moves_of(in, p->bf, &m)) goto done;
    if (!m.n) {
        ok = 1;
        goto done;
    }
    File *f = &p->mf[k];
    if (!p->tabs[k]) {
        if (!(p->tabs[k] = calloc(f->count, sizeof *p->tabs[k]))) goto done;
        p->ntabs[k] = f->count;
    }
    for (uint32_t j = 0; j < f->count; j++)
        if (!move_cells(&m, &p->tabs[k][j], &f->t[j], &p->bf->t[j], &unfit)) goto done;
    Adds *a = &p->added[k];
    if (a->n && (!(order = malloc((size_t)a->n * sizeof *order)) || !(keys = malloc((size_t)a->n * sizeof *keys))))
        goto done;
    for (int i = 0; i < a->n; i++) order[i] = &a->v[i];
    if (a->n) qsort(order, (size_t)a->n, sizeof *order, by_added_name);
    for (int i = 0; i < a->n; i++)
        if (!move_cells(&m, &order[i]->own, &order[i]->t, NULL, &unfit)) goto done;
    // tables named after a region: renamed with it (a name matches at most one template and local number)
    for (int i = 0; i < a->n; i++) {
        Added *x = order[i];
        uint32_t h = x->t.name;
        keys[i] = h;
        for (int q = 0; q < m.n; q++) {
            if (m.v[q].kind != BDAT_ID_REGION) continue;
            for (size_t t = 0; t < NUM_OF(REGION_TABLES); t++)
                if (h == name_with(REGION_TABLES[t], m.v[q].local)) {
                    if (!own_bytes(&x->own, &x->t)) goto done;
                    uint32_t nm = name_with(REGION_TABLES[t], m.v[q].number);
                    wr32(x->own.p + x->t.so + 1, nm);
                    x->t.name = nm;
                }
        }
    }
    if (unfit)
        event(ev, LEFT_OUT, k, "%d cell%s keep%s its own number: the number given doesn't fit the column", unfit,
              unfit > 1 ? "s" : "", unfit > 1 ? "" : "s");
    // two of its tables with one name once renamed (in order of the old names): the later one is left out
    for (int i = 0; i < a->n; i++) {
        Added *x = order[i];
        int clash = 0;
        for (int q = 0; q < i && !clash; q++) clash = order[q] && order[q]->t.name == x->t.name;
        if (clash) {
            event(ev, LEFT_OUT, k, "its table %08X: renamed with its region, it would be another of its tables", keys[i]);
            free(x->own.p);
            free(x->t.off);
            x->own = (Buf){0};
            x->t = (Tab){0};
            order[i] = NULL;
        }
    }
    // drop the left-out ones
    int w = 0;
    for (int i = 0; i < a->n; i++)
        if (a->v[i].t.p) a->v[w++] = a->v[i];
    a->n = w;
    ok = 1;
done:
    free(m.v);
    free(order);
    free(keys);
    return ok;
}

static void prepare(void *ctx, int k) {
    Prep *p = ctx;
    char *r = p->why[k];
    Events ev = {keep_line, &p->kept[k], &p->kept[k].s, p->mods};
    const BdatInput *m = &p->mods[k];
    p->state[k] = PREP_UNREADABLE;
    if (m->njson) {
        // the mod's JSON tables in its full copy, or in the game's when it has none
        r[0] = 0;
        if (!apply_json(m->data ? m->data : p->base->data, m->data ? m->size : p->base->size, m, k, &ev, &p->mf[k],
                        &p->tabs[k], &p->added[k], r, sizeof p->why[k]))
            return;
        p->ntabs[k] = p->mf[k].count;
    } else if (!m->data) {
        snprintf(r, sizeof p->why[k], "none given");
        return;
    } else if (!read_file(m->data, m->size, &p->mf[k], r, sizeof p->why[k])) {
        return;
    }
    int same = p->mf[k].count == p->bf->count;
    for (uint32_t j = 0; same && j < p->bf->count; j++) same = p->mf[k].t[j].name == p->bf->t[j].name;
    if (!same) {
        int s = split_added(p, k);
        if (s < 0) {
            p->kept[k].oom = 1;
            return;
        }
        same = s;
    }
    if (same && !move_locals(p, k, &ev)) p->kept[k].oom = 1;
    p->state[k] = same ? PREP_SAME_TABLES : PREP_OTHER_TABLES;
}

// The file of f's tables as they are, then the new tables news[0..nn) (a mod's copy used whole).
static uint8_t *file_with(const File *f, Added *const *news, int nn, size_t *out_n) {
    Buf out = {0};
    uint32_t count = f->count + (uint32_t)nn;
    size_t head = 16 + 4 * (size_t)count;
    if (!put(&out, NULL, head)) return NULL;
    memcpy(out.p, "BDAT", 4);
    wr32(out.p + 4, 0x01001004);
    wr32(out.p + 8, count);
    for (uint32_t j = 0; j < count; j++) {
        const Tab *t = j < f->count ? &f->t[j] : &news[j - f->count]->t;
        wr32(out.p + 16 + 4 * j, (uint32_t)out.n);
        if (!put(&out, t->p, t->len)) {
            free(out.p);
            return NULL;
        }
    }
    if (out.n > 0xffffffffu) {
        free(out.p);
        return NULL;
    }
    wr32(out.p + 12, (uint32_t)out.n);
    *out_n = out.n;
    return out.p;
}

// Mod k's copy as the result, when it is used whole: its own file, or for a mod with JSON tables the file of them put in.
static uint8_t *whole_copy(const Prep *p, int k, size_t *out_n) {
    return p->tabs[k] ? file_with(&p->mf[k], NULL, 0, out_n) : copy_of(&p->mods[k], out_n);
}

static int by_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

// A value each of the game's rows has its own of (a flag, a site number), that rows two mods add both use: the two
// mods may clash there (a caveat; the numbers mod.json names are handed out before this, so these are the others).
// Not for tables of fewer than 8 rows (a column of a few distinct values says little) or column 0 (the key: the merge
// keeps keys unique itself).
static int shared_values(const File *bf, const File *mf, const int *live, int nlive, Events *ev) {
    int64_t *vals = NULL;
    uint32_t cap = 0;
    typedef struct { int64_t v; int k; } Seen;
    Seen *seen = NULL, *told = NULL;
    uint32_t scap = 0;
    int ok = 0;
    for (uint32_t j = 0; j < bf->count; j++) {
        const Tab *g = &bf->t[j];
        if (g->rc < 8) continue;
        int first = -1, two = 0;
        uint32_t nadd = 0;
        for (int x = 0; x < nlive; x++) {
            const Tab *t = &mf[live[x]].t[j];
            if (!same_cols(t, g) || t->base != g->base || t->rc <= g->rc) continue;
            nadd += t->rc - g->rc;
            if (first < 0) first = live[x];
            else if (first != live[x]) two = 1;
        }
        if (!two) continue;
        if (g->rc > cap) {
            int64_t *q = realloc(vals, (size_t)g->rc * sizeof *q);
            if (!q) goto done;
            vals = q;
            cap = g->rc;
        }
        if (nadd > scap) {
            Seen *q = realloc(seen, (size_t)nadd * sizeof *q), *q2;
            if (!q) goto done;
            seen = q;
            if (!(q2 = realloc(told, (size_t)nadd * sizeof *q2))) goto done;
            told = q2;
            scap = nadd;
        }
        for (uint32_t c = 1; c < g->cc; c++) {
            if (!int_type(ctype(g, c))) continue;
            for (uint32_t r = 0; r < g->rc; r++) vals[r] = ival(g, r, c);
            qsort(vals, g->rc, sizeof *vals, by_i64);
            int uniq = 1;
            for (uint32_t r = 1; uniq && r < g->rc; r++) uniq = vals[r] != vals[r - 1];
            if (!uniq) continue;
            uint32_t ns = 0, nt = 0;
            for (int x = 0; x < nlive; x++) {
                int k = live[x];
                const Tab *t = &mf[k].t[j];
                if (!same_cols(t, g) || t->base != g->base) continue;
                for (uint32_t r = g->rc; r < t->rc; r++) {
                    int64_t v = ival(t, r, c);
                    int w = -1;
                    for (uint32_t s = 0; s < ns && w < 0; s++)
                        if (seen[s].v == v) w = seen[s].k;
                    if (w < 0) {
                        seen[ns++] = (Seen){v, k};
                        continue;
                    }
                    if (w == k) continue;
                    int again = 0;
                    for (uint32_t s = 0; s < nt && !again; s++) again = told[s].v == v && told[s].k == k;
                    if (again) continue;
                    told[nt++] = (Seen){v, k};
                    event(ev, CAVEAT, k, "table %08X column %u: a row it adds has %lld, as one of %s's does (each of "
                          "the game's rows has a value of its own there)", g->name, c, (long long)v, ev->mods[w].name);
                }
            }
        }
    }
    ok = 1;
done:
    free(vals);
    free(seen);
    free(told);
    return ok;
}

// Spare game rows (FrontierNav sites, camp slots) two content mods both take: the lower mod's edits to that row are
// left out whole, so no site is half one mod's and half the other's.
static int take_slots(Prep *p, const int *live, int nlive, Events *ev) {
    const File *bf = p->bf;
    int ok = 0;
    int *owner = NULL;
    Src *from = NULL;
    for (uint32_t j = 0; j < bf->count; j++) {
        const Tab *g = &bf->t[j];
        if (!in_list(SLOT_TABLES, NUM_OF(SLOT_TABLES), g->name)) continue;
        free(owner);
        if (!(owner = malloc((g->rc + 1) * sizeof *owner))) goto done;
        for (uint32_t r = 0; r < g->rc; r++) owner[r] = -1;
        for (int x = 0; x < nlive; x++) {
            int k = live[x];
            const Tab *t = &p->mf[k].t[j];
            if (!p->mods[k].nids || !same_cols(t, g) || t->base != g->base) continue;
            uint32_t n = t->rc < g->rc ? t->rc : g->rc, back = 0;
            for (uint32_t r = 0; r < n; r++) {
                if (same_row(g, r, t, r)) continue;
                if (owner[r] >= 0) back++;
                else owner[r] = k;
            }
            if (!back) continue;
            free(from);
            if (!(from = malloc(((size_t)t->rc * t->cc + 1) * sizeof *from))) goto done;
            for (uint32_t r = 0; r < t->rc; r++) {
                int mine = r < n && !same_row(g, r, t, r) && owner[r] != k;
                for (uint32_t c = 0; c < t->cc; c++) from[(size_t)r * t->cc + c] = (Src){r, (uint8_t)(mine ? 0 : 1)};
                if (mine && !same_row(&p->mf[owner[r]].t[j], r, t, r)) {   // the same edit as its owner's: nothing lost
                    char rn[80];
                    rowname(g, r, rn, sizeof rn);
                    event(ev, LEFT_OUT, k, "table %08X %s: %s takes this row too; its edits to it are not used", g->name,
                          rn, p->mods[owner[r]].name);
                }
            }
            const Tab *src[2] = {g, t};
            Buf nb = {0};
            if (!write_table(&nb, t, src, from, t->rc, t->extra) || !set_table(p, k, j, nb)) goto done;
        }
    }
    ok = 1;
done:
    free(owner);
    free(from);
    return ok;
}

// A game site's four network links (FnetVeinList) as a set: a site a mod adds links to a site of the game's, and the
// game's links go both ways, so the mod edits the game site's row too, putting its site in a free link column; two
// such mods would both take the same column. Here every mod's copy of a game site whose own links it keeps gets the
// game's links in their columns and every mod's added links in the free ones, higher priority first (beyond four: a
// "left out" line), so they agree there and the rest of their edits to that row merge as usual. Before take_slots.
static const uint32_t VEIN_LINKS[4] = {0x3791D641u, 0xF14C0AACu, 0x38E329FDu, 0xD270368Au};

static int link_slots(Prep *p, const int *live, int nlive, Events *ev) {
    const File *bf = p->bf;
    uint32_t j = 0;
    while (j < bf->count && bf->t[j].name != VEIN) j++;
    if (j == bf->count) return 1;
    const Tab *g = &bf->t[j];
    uint32_t lc[4];
    for (int i = 0; i < 4; i++)
        if ((lc[i] = col_of(g, VEIN_LINKS[i])) == NONE) return 1;
    for (uint32_t r = 0; r < g->rc; r++) {
        int64_t gl[4], uni[4];
        int keeps[MAX_MODS] = {0}, any = 0;
        for (int i = 0; i < 4; i++) uni[i] = gl[i] = ival(g, r, lc[i]);
        for (int x = 0; x < nlive; x++) {
            int k = live[x];
            const Tab *t = &p->mf[k].t[j];
            if (!same_cols(t, g) || t->base != g->base || r >= t->rc) continue;
            int64_t ml[4];
            for (int i = 0; i < 4; i++) ml[i] = ival(t, r, lc[i]);
            int keep = 1;   // every link the game's row has, it has too (in any column)
            for (int i = 0; i < 4 && keep; i++) {
                int found = !gl[i];
                for (int q = 0; q < 4 && !found; q++) found = ml[q] == gl[i];
                keep = found;
            }
            if (!keep) continue;
            keeps[x] = 1;
            for (int i = 0; i < 4; i++) {
                int64_t v = ml[i];
                int have = !v;
                for (int q = 0; q < 4 && !have; q++) have = uni[q] == v;
                if (have) continue;
                int at = -1;
                for (int q = 0; q < 4 && at < 0; q++)
                    if (!uni[q]) at = q;
                if (at < 0) {
                    event(ev, LEFT_OUT, k, "table %08X row %u: its link to site %lld: the site's four links are full",
                          VEIN, r + g->base, (long long)v);
                    continue;
                }
                uni[at] = v;
                any = 1;
            }
        }
        if (!any) continue;
        for (int x = 0; x < nlive; x++) {
            int k = live[x];
            const Tab *t = &p->mf[k].t[j];
            if (!keeps[x]) continue;
            int differs = 0;
            for (int i = 0; i < 4; i++) differs |= ival(t, r, lc[i]) != gl[i];
            if (!differs) continue;   // a copy that doesn't touch this site's links stays the game's
            Buf nb = {0};
            Tab nt;
            if (!put(&nb, t->p, t->len) || !read_table(nb.p, (uint32_t)nb.n, &nt)) {
                free(nb.p);
                free(nt.off);
                return 0;
            }
            for (int i = 0; i < 4; i++)
                set_ival(nb.p + nt.ro + (size_t)r * nt.rl + nt.off[lc[i]], ctype(&nt, lc[i]), uni[i]);
            free(nt.off);
            if (!set_table(p, k, j, nb)) return 0;
        }
    }
    return 1;
}

// Rows whose number is their row id (a region's FLD_FieldNameList row, new rows a mod.json names) put at those ids:
// each such table grows by gap rows up to the highest one, in the base and in every mod's copy, so different mods'
// rows are edits of different rows and combine. bf's grown tables get bytes of their own in bown.
typedef struct { uint32_t j, number; int k; uint32_t row; } Owner;

static int place_rows(Prep *p, File *bf, Buf *bown, const int *live, int nlive, Events *ev) {
    int ok = 0, nall = 0, capall = 0;
    Owner *own = NULL, *all = NULL;
    Src *from = NULL;
    uint32_t *keys = NULL;
    Buf fill = {0};
    Tab ft = {0};
    // the owners: every live mod's entries in order, the first to a number of a table keeps it
    for (int x = 0; x < nlive; x++) {
        int k = live[x];
        const BdatInput *in = &p->mods[k];
        for (int i = 0; i < in->nids; i++) {
            const BdatId *e = &in->ids[i];
            uint32_t h, j = 0;
            if (e->kind == BDAT_ID_REGION && e->has_local) h = FIELD_NAMES;
            else if (e->kind == BDAT_ID_SITE && e->has_local) h = VEIN;
            else if (e->kind == BDAT_ID_ROW && e->has_local) h = e->table;
            else continue;
            while (j < bf->count && bf->t[j].name != h) j++;
            if (j == bf->count) continue;   // a table of another file (mod_ids says when no file has it)
            const Tab *g = &bf->t[j], *t = &p->mf[k].t[j];
            uint32_t end = g->base + g->rc;
            if (e->number < end || e->local < end || !same_cols(t, g) || t->base != g->base ||
                e->local - t->base >= t->rc) {
                event(ev, LEFT_OUT, k, "its %s \"%s\" (row %u of table %08X): not a row it adds there",
                      KIND_NAME(e->kind), e->name, e->local, h);
                continue;
            }
            int dup = 0;
            for (int q = 0; q < nall && !dup; q++) dup = all[q].j == j && all[q].number == e->number;
            if (dup) continue;
            if (nall == capall) {
                capall = capall ? capall * 2 : 16;
                Owner *q = realloc(all, (size_t)capall * sizeof *q);
                if (!q) goto done;
                all = q;
            }
            all[nall++] = (Owner){j, e->number, k, e->local - t->base};
            // its row of a parallel table, when it has one, goes with it
            for (size_t q = 0; q < NUM_OF(PARALLEL); q++) {
                if (PARALLEL[q][0] != h) continue;
                uint32_t j2 = 0;
                while (j2 < bf->count && bf->t[j2].name != PARALLEL[q][1]) j2++;
                if (j2 == bf->count) continue;
                const Tab *g2 = &bf->t[j2], *t2 = &p->mf[k].t[j2];
                uint32_t end2 = g2->base + g2->rc;
                if (!same_cols(t2, g2) || t2->base != g2->base || e->local < end2 || e->number < end2 ||
                    e->local - t2->base >= t2->rc)
                    continue;
                int dup2 = 0;
                for (int w = 0; w < nall && !dup2; w++) dup2 = all[w].j == j2 && all[w].number == e->number;
                if (dup2) {
                    event(ev, LEFT_OUT, k, "its %s \"%s\": its row %u of table %08X is another mod's number there",
                          KIND_NAME(e->kind), e->name, e->local, PARALLEL[q][1]);
                    continue;
                }
                if (nall == capall) {
                    capall *= 2;
                    Owner *w = realloc(all, (size_t)capall * sizeof *w);
                    if (!w) goto done;
                    all = w;
                }
                all[nall++] = (Owner){j2, e->number, k, e->local - t2->base};
            }
        }
    }
    for (uint32_t j = 0; j < bf->count; j++) {
        int nown = 0;
        free(own);
        if (!(own = malloc((size_t)(nall ? nall : 1) * sizeof *own))) goto done;
        for (int q = 0; q < nall; q++)
            if (all[q].j == j) own[nown++] = all[q];
        if (!nown) continue;
        const Tab *g = &bf->t[j];
        uint32_t end = g->base + g->rc;
        uint32_t top = 0;
        for (int q = 0; q < nown; q++)
            if (own[q].number - end + 1 > top) top = own[q].number - end + 1;
        int keyed = ctype(g, 0) == 9;
        // A gap row is a copy of a row the game has (FILLER: a placeholder where the game has one, else the last row).
        // Only the key is made up. Nothing points at a gap row: the mod whose number it is is off, and the numbers it
        // was given are no other mod's.
        uint32_t tmpl = g->rc - 1;
        for (size_t q = 0; q < NUM_OF(FILLER); q++)
            if (FILLER[q][0] == g->name && FILLER[q][1] < g->rc) tmpl = FILLER[q][1];
        free(keys);
        if (!(keys = malloc((size_t)top * sizeof *keys))) goto done;
        // the gap rows: copies of the template row, each with a key of its own (the owner's, unless another row has it)
        for (uint32_t o = 0; keyed && o < top; o++) {
            int w = -1;
            for (int q = 0; q < nown && w < 0; q++)
                if (own[q].number == end + o) w = q;
            uint32_t key = 0;
            int have = 0;
            if (w >= 0) {
                key = rd32(cell(&p->mf[own[w].k].t[j], own[w].row, 0));
                have = 1;
                for (uint32_t r = 0; have && r < g->rc; r++) have = rd32(cell(g, r, 0)) != key;
                for (uint32_t r = 0; have && r < o; r++) have = keys[r] != key;
            }
            if (!have) {
                char s[64];
                int n = snprintf(s, sizeof s, "mod-ids %08X %u", g->name, end + o);
                uint32_t syn = mm3((const uint8_t *)s, (size_t)n);
                if (w >= 0)
                    event(ev, CAVEAT, own[w].k, "table %08X: its row %u gets key %08X: its own key is another row's",
                          g->name, end + o, syn);
                key = syn;
            }
            keys[o] = key;
        }
        free(from);
        if (!(from = malloc(((size_t)top * g->cc + 1) * sizeof *from))) goto done;
        for (uint32_t r = 0; r < top; r++)
            for (uint32_t c = 0; c < g->cc; c++) from[(size_t)r * g->cc + c] = (Src){tmpl, 0};
        const Tab *gsrc[1] = {g};
        free(fill.p);
        free(ft.off);
        fill = (Buf){0};
        ft = (Tab){0};
        if (!write_table(&fill, g, gsrc, from, top, g->extra) || !read_table(fill.p, (uint32_t)fill.n, &ft)) goto done;
        for (uint32_t o = 0; keyed && o < top; o++) wr32(fill.p + ft.ro + (size_t)o * ft.rl + ft.off[0], keys[o]);
        for (size_t q = 0; q < NUM_OF(GAP_SET); q++) {   // a gap's own number, or no location
            uint32_t ci = GAP_SET[q][0] == g->name ? col_of(&ft, GAP_SET[q][1]) : NONE;
            for (uint32_t o = 0; ci != NONE && o < top; o++) {
                int64_t v = GAP_SET[q][2] ? (int64_t)(end + o) : 0;
                if (fits(ctype(&ft, ci), v)) set_ival(fill.p + ft.ro + (size_t)o * ft.rl + ft.off[ci], ctype(&ft, ci), v);
            }
        }
        // the base: the game's rows, then the gap rows
        uint32_t rows = g->rc + top;
        free(from);
        if (!(from = malloc(((size_t)rows * g->cc + 1) * sizeof *from))) goto done;
        for (uint32_t r = 0; r < rows; r++)
            for (uint32_t c = 0; c < g->cc; c++)
                from[(size_t)r * g->cc + c] = r < g->rc ? (Src){r, 0} : (Src){r - g->rc, 1};
        const Tab *bsrc[2] = {g, &ft};
        Buf nb = {0};
        Tab nt;
        if (!write_table(&nb, g, bsrc, from, rows, g->extra) || !read_table(nb.p, (uint32_t)nb.n, &nt)) {
            free(nb.p);
            goto done;
        }
        // every live mod's copy: its rows but those it owns, with the gap rows (its own ones in place) after the
        // game's number of rows
        for (int x = 0; x < nlive; x++) {
            int k = live[x];
            const Tab *t = &p->mf[k].t[j];
            if (!same_cols(t, g) || t->base != g->base) continue;
            uint32_t mine = 0;
            for (int q = 0; q < nown; q++) mine += own[q].k == k;
            uint32_t mrows = t->rc - mine + top;
            Src *mf2 = malloc(((size_t)mrows * t->cc + 1) * sizeof *mf2);
            if (!mf2) {
                free(nb.p);
                free(nt.off);
                goto done;
            }
            uint32_t at = 0, kept = 0;
            for (uint32_t i = 0; i <= t->rc; i++) {
                if (kept == g->rc || (i == t->rc && kept < g->rc)) {
                    // the gap rows go here (once): after the first g->rc rows kept, or at the end of a shorter copy
                    if (kept != NONE) {
                        for (uint32_t o = 0; o < top; o++) {
                            int w = -1;
                            for (int q = 0; q < nown && w < 0; q++)
                                if (own[q].number == end + o && own[q].k == k) w = q;
                            for (uint32_t c = 0; c < t->cc; c++)
                                mf2[(size_t)at * t->cc + c] = w >= 0 && !(keyed && c == 0) ? (Src){own[w].row, 0}
                                                                                         : (Src){o, 1};
                            at++;
                        }
                        kept = NONE;
                    }
                }
                if (i == t->rc) break;
                int owned = 0;
                for (int q = 0; q < nown && !owned; q++) owned = own[q].k == k && own[q].row == i;
                if (owned) continue;
                for (uint32_t c = 0; c < t->cc; c++) mf2[(size_t)at * t->cc + c] = (Src){i, 0};
                at++;
                if (kept != NONE) kept++;
            }
            const Tab *msrc[2] = {t, &ft};
            Buf mb = {0};
            int w_ok = write_table(&mb, t, msrc, mf2, at, t->extra);
            free(mf2);
            if (!w_ok || !set_table(p, k, j, mb)) {
                free(nb.p);
                free(nt.off);
                goto done;
            }
        }
        free(bf->t[j].off);
        free(bown[j].p);
        bown[j] = nb;
        bf->t[j] = nt;
    }
    ok = 1;
done:
    free(all);
    free(own);
    free(from);
    free(keys);
    free(fill.p);
    free(ft.off);
    return ok;
}

// A live mod's change to every table, and what its changes refer to.
typedef struct {
    const File *bf, *mf;
    Pair *const *bkeys;
    const int *bunique, *live;
    Change **ch;
    Set *refs;
    int oom[MAX_MODS];
} Compare;

static void compare(void *ctx, int x) {
    Compare *c = ctx;
    int k = c->live[x];
    uint32_t count = c->bf->count;
    if (!(c->ch[k] = calloc(count, sizeof *c->ch[k]))) {
        c->oom[x] = 1;
        return;
    }
    for (uint32_t j = 0; j < count; j++)
        if (!classify(&c->bf->t[j], c->bkeys[j], c->bunique[j], &c->mf[k].t[j], &c->ch[k][j])) {
            c->oom[x] = 1;
            return;
        }
    if (!refs_of(c->bf, &c->mf[k], c->ch[k], &c->refs[k])) c->oom[x] = 1;
}

uint8_t *bdat_merge(const BdatInput *base, const BdatInput *mods, int n, size_t *out_n, BdatSummary *sum,
                    BdatNote note, void *ctx, char *why, size_t why_cap) {
    BdatSummary s = {0};
    s.file_mod = -1;
    Events ev = {note, ctx, &s, mods};
    File bf = {0}, mf[MAX_MODS] = {{0}};
    Buf *bown = NULL;
    int live[MAX_MODS], nlive = 0;
    Change *ch[MAX_MODS] = {0};
    Set refs[MAX_MODS] = {{0}};
    Buf body = {0};
    uint32_t *offs = NULL;
    uint8_t *result = NULL;
    Pair **bkeys = NULL;
    int *bunique = NULL;
    Prep *prep = NULL;
    Added **news = NULL;
    int nnews = 0;
    char r[300];
    if (n < 1 || n > MAX_MODS) {
        snprintf(why, why_cap, "%d mods (1 to %d can be merged)", n, MAX_MODS);
        return NULL;
    }
    if (!read_file(base->data, base->size, &bf, r, sizeof r)) {
        snprintf(why, why_cap, "the game's copy: %s", r);
        return NULL;
    }
    // every mod's file made and read (at the same time when there are threads), then taken in priority order as if
    // one after another: a mod whose copy is used whole ends it, and the mods after it say nothing more
    if (!(prep = calloc(1, sizeof *prep))) goto oom;
    *prep = (Prep){.base = base, .mods = mods, .bf = &bf, .mf = mf};
    each(n, prepare, prep);
    for (int k = 0; k < n; k++) {
        pass_on(&ev, &prep->kept[k]);
        if (prep->kept[k].oom) goto oom;
        if (prep->state[k] == PREP_UNREADABLE) {
            event(&ev, LEFT_OUT, k, "its copy can't be read (%s)", prep->why[k]);
            continue;
        }
        if (prep->state[k] == PREP_SAME_TABLES) {
            live[nlive++] = k;
            continue;
        }
        if (!nlive) {
            // the top mod that can be read has another list of tables: its whole file, the others left out
            for (int x = k + 1; x < n; x++)
                event(&ev, LEFT_OUT, x, "its changes to this file are not used: %s's copy has another list of tables "
                      "and is used whole", mods[k].name);
            s.file_mode = BDAT_FILE_WHOLE;
            s.file_mod = k;
            if (!(result = whole_copy(prep, k, out_n))) snprintf(why, why_cap, "out of memory");
            goto done;
        }
        file_free(&mf[k]);
        event(&ev, LEFT_OUT, k, "its changes to this file are not used: its copy has another list of tables");
    }
    if (!nlive) {
        snprintf(why, why_cap, "no mod's copy can be read");
        goto done;
    }
    // new tables: each name once, the highest mod's (a mod's own names are distinct: apply_json, split_added and
    // move_locals leave a second one out)
    {
        int total = 0;
        for (int x = 0; x < nlive; x++) total += prep->added[live[x]].n;
        if (!(news = malloc((size_t)(total ? total : 1) * sizeof *news))) goto oom;
    }
    for (int x = 0; x < nlive; x++) {
        Adds *a = &prep->added[live[x]];
        Added **order = malloc((size_t)(a->n ? a->n : 1) * sizeof *order);
        if (!order) goto oom;
        for (int i = 0; i < a->n; i++) order[i] = &a->v[i];
        qsort(order, (size_t)a->n, sizeof *order, by_added_name);   // the lines in order of name, as the reference
        for (int i = 0; i < a->n; i++) {
            int w = -1;
            for (int q = 0; q < nnews && w < 0; q++)
                if (news[q]->t.name == order[i]->t.name) w = q;
            if (w >= 0) {
                int owner = -1;
                for (int y = 0; y < x && owner < 0; y++)
                    if (adds_find(&prep->added[live[y]], order[i]->t.name) >= 0) owner = live[y];
                event(&ev, LEFT_OUT, live[x], "its new table %08X is not used: %s's is", order[i]->t.name,
                      mods[owner].name);
            } else {
                news[nnews++] = order[i];
            }
        }
        free(order);
    }
    qsort(news, (size_t)nnews, sizeof *news, by_added_name);
    int reworked = nnews > 0;
    for (int x = 0; x < nlive; x++) reworked |= mods[live[x]].nids > 0;
    if (nlive > 1 && !shared_values(&bf, mf, live, nlive, &ev)) goto oom;
    if (reworked) {
        if (!(bown = calloc(bf.count, sizeof *bown))) goto oom;
        if (!link_slots(prep, live, nlive, &ev) || !take_slots(prep, live, nlive, &ev) ||
            !place_rows(prep, &bf, bown, live, nlive, &ev))
            goto oom;
    }
    if (nlive == 1) {
        s.file_mode = BDAT_FILE_COPIED;
        s.file_mod = live[0];
        result = reworked ? file_with(&mf[live[0]], news, nnews, out_n) : whole_copy(prep, live[0], out_n);
        if (!result) snprintf(why, why_cap, "out of memory");
        goto done;
    }
    // every mod's change to every table, and what its changes refer to
    bkeys = calloc(bf.count, sizeof *bkeys);
    bunique = calloc(bf.count, sizeof *bunique);
    if (!bkeys || !bunique) goto oom;
    for (uint32_t j = 0; j < bf.count; j++)
        if (ctype(&bf.t[j], 0) == 9 && !(bkeys[j] = sorted_keys(&bf.t[j], &bunique[j]))) goto oom;
    {
        Compare c = {.bf = &bf, .mf = mf, .bkeys = bkeys, .bunique = bunique, .live = live, .ch = ch, .refs = refs};
        each(nlive, compare, &c);
        for (int x = 0; x < nlive; x++)
            if (c.oom[x]) goto oom;
    }
    // changed tables a higher mod has alike: all of them, the mod adds nothing; some, partly alike
    // (docs/bdat-merge.md, "The report")
    for (int x = 1; x < nlive; x++) {
        int k = live[x], changed = 0, alike = 0, used[MAX_MODS] = {0};
        for (uint32_t j = 0; j < bf.count; j++) {
            if (ch[k][j].kind == SAME) continue;
            changed++;
            int h = -1;
            for (int y = 0; h < 0 && y < x; y++)
                if (same_table(&mf[live[y]].t[j], &mf[k].t[j])) h = y;
            if (h >= 0) {
                alike++;
                used[h] = 1;
            }
        }
        if (!alike) continue;
        char who[400];
        size_t w = 0;
        who[0] = 0;
        for (int y = 0; y < x; y++)
            if (used[y] && w < sizeof who)
                w += (size_t)snprintf(who + w, sizeof who - w, "%s%s", w ? ", " : "", mods[live[y]].name);
        if (alike == changed) event(&ev, ADDS_NOTHING, k, "every table it changes is the same in %s", who);
        else event(&ev, PARTLY_ALIKE, k, "%d of the %d tables it changes %s the same in %s", alike, changed,
                   alike == 1 ? "is" : "are", who);
    }
    s.file_mode = BDAT_FILE_MERGED;
    uint32_t count = bf.count + (uint32_t)nnews;
    if (!(offs = malloc(count * sizeof *offs))) goto oom;
    for (uint32_t j = 0; j < bf.count; j++) {
        const Tab *b = &bf.t[j];
        int touch[MAX_MODS], nt = 0;
        for (int x = 0; x < nlive; x++)
            if (ch[live[x]][j].kind != SAME) touch[nt++] = live[x];
        offs[j] = (uint32_t)body.n;
        const Tab *copy = NULL;
        if (!nt) copy = b;
        else if (nt == 1) copy = &mf[touch[0]].t[j];
        else if (ch[touch[0]][j].kind == OPAQUE) {
            copy = &mf[touch[0]].t[j];
            for (int x = 1; x < nt; x++)
                event(&ev, LEFT_OUT, touch[x], "table %08X: its changes are not used; %s's whole table is (%s)",
                      b->name, mods[touch[0]].name, ch[touch[0]][j].reason);
            s.tables_whole++;
        }
        if (nt) s.tables_changed++;
        if (!copy) {
            int P[MAX_MODS], np = 0;
            for (int x = 0; x < nt; x++) {
                if (ch[touch[x]][j].kind == OPAQUE)
                    event(&ev, LEFT_OUT, touch[x], "table %08X: its change is not used (%s, and %s changes the table "
                          "too)", b->name, ch[touch[x]][j].reason, mods[touch[0]].name);
                else P[np++] = touch[x];
            }
            if (np == 1) {
                copy = &mf[P[0]].t[j];
            } else {
                Job job = {.b = b, .refs = refs, .n = n, .ev = &ev};
                for (int k = 0; k < n; k++) {
                    job.m[k] = mf[k].t ? &mf[k].t[j] : NULL;
                    job.ch[k] = ch[k] ? &ch[k][j] : NULL;
                }
                if (!merge_table(&job, P, np, &body, why, why_cap)) goto done;
                s.tables_merged++;
                continue;
            }
        }
        if (!put(&body, copy->p, copy->len) || !put(&body, NULL, (4 - (body.n & 3)) & 3)) goto oom;   // tables 4-aligned
    }
    for (int i = 0; i < nnews; i++) {   // the new tables, by name
        offs[bf.count + (uint32_t)i] = (uint32_t)body.n;
        if (!put(&body, news[i]->t.p, news[i]->t.len) || !put(&body, NULL, (4 - (body.n & 3)) & 3)) goto oom;
    }
    size_t head = 16 + 4 * (size_t)count, total = head + body.n;
    if (total > 0xffffffffu || !(result = malloc(total))) {
        snprintf(why, why_cap, "the merged file is too big");
        goto done;
    }
    memcpy(result, "BDAT", 4);
    wr32(result + 4, 0x01001004);
    wr32(result + 8, count);
    wr32(result + 12, (uint32_t)total);
    for (uint32_t j = 0; j < count; j++) wr32(result + 16 + 4 * j, (uint32_t)(head + offs[j]));
    memcpy(result + head, body.p, body.n);
    *out_n = total;
    goto done;
oom:
    snprintf(why, why_cap, "out of memory");
done:
    if (bkeys)
        for (uint32_t j = 0; j < bf.count; j++) free(bkeys[j]);
    free(bkeys);
    free(bunique);
    for (int k = 0; k < n; k++) {
        if (ch[k])
            for (uint32_t j = 0; j < bf.count; j++) change_free(&ch[k][j]);
        free(ch[k]);
        free(refs[k].v);
        file_free(&mf[k]);
        if (prep) {
            free(prep->kept[k].lines.p);
            for (uint32_t j = 0; prep->tabs[k] && j < prep->ntabs[k]; j++) free(prep->tabs[k][j].p);
            free(prep->tabs[k]);
            adds_free(&prep->added[k]);
        }
    }
    free(prep);
    free(news);
    if (bown)
        for (uint32_t j = 0; j < bf.count; j++) free(bown[j].p);
    free(bown);
    file_free(&bf);
    free(offs);
    free(body.p);
    if (sum) *sum = s;
    return result;
}

// ---- the ids file of the check command line (tests/bdat_merge_check.c)

BdatId *bdat_merge_ids_from_json(const uint8_t *data, size_t size, const char *mod, int *n) {
    Arena arena = {0};
    JParse jp = {(const char *)data, (const char *)data + size, 0, 0, &arena};
    BdatId *out = NULL;
    *n = 0;
    JNode *root = utf8_ok(data, size) ? jvalue(&jp) : NULL;
    const JNode *list = root ? jget(root, mod, strlen(mod)) : NULL;
    if (list && list->type == J_ARR && list->len) {
        // one block: the entries, then the refs, then the names
        size_t refs = 0, names = 0;
        for (size_t i = 0; i < list->len; i++) {
            const JNode *e = list->items[i], *rf = jget(e, "refs", 4), *nm = jget(e, "name", 4);
            refs += rf && rf->type == J_ARR ? rf->len : 0;
            names += nm && nm->type == J_STR ? nm->len + 1 : 1;
        }
        size_t at_refs = list->len * sizeof *out, at_names = at_refs + 2 * refs * sizeof(uint32_t);
        uint8_t *blk = calloc(1, at_names + names);
        if (blk) {
            out = (BdatId *)blk;
            uint32_t *rp = (uint32_t *)(blk + at_refs);
            char *np = (char *)blk + at_names;
            for (size_t i = 0; i < list->len; i++) {
                const JNode *e = list->items[i], *v;
                BdatId *d = &out[(*n)++];
                if ((v = jget(e, "kind", 4)) && v->type == J_STR)
                    for (int k = 0; k < BDAT_ID_KINDS; k++)
                        if (strlen(KIND_NAME(k)) == v->len && !memcmp(KIND_NAME(k), v->s, v->len)) d->kind = (uint8_t)k;
                d->name = np;
                if ((v = jget(e, "name", 4)) && v->type == J_STR) {
                    memcpy(np, v->s, v->len);
                    np += v->len;
                }
                *np++ = 0;
                if ((v = jget(e, "local", 5)) && v->type == J_NUM && v->isint) {
                    d->has_local = 1;
                    d->local = (uint32_t)v->i;
                }
                if ((v = jget(e, "number", 6)) && v->type == J_NUM && v->isint) d->number = (uint32_t)v->i;
                if ((v = jget(e, "table", 5)) && v->type == J_STR) d->table = hashname(v->s, v->len);
                if ((v = jget(e, "refs", 4)) && v->type == J_ARR) {
                    d->refs = rp;
                    for (size_t q = 0; q < v->len; q++) {
                        const JNode *s = v->items[q];
                        const char *dot = NULL;
                        for (size_t c = 0; s->type == J_STR && c < s->len; c++)
                            if (s->s[c] == '.') dot = s->s + c;
                        if (!dot) continue;
                        *rp++ = hashname(s->s, (size_t)(dot - s->s));
                        *rp++ = hashname(dot + 1, s->len - (size_t)(dot - s->s) - 1);
                        d->nrefs++;
                    }
                }
            }
        }
    }
    afree(&arena);
    return out;
}

int bdat_rewrite_check(const uint8_t *data, size_t size) {
    File f;
    char why[200];
    if (!read_file(data, size, &f, why, sizeof why)) return -1;
    int bad = 0;
    for (uint32_t j = 0; j < f.count; j++) {
        Tab *t = &f.t[j];
        Src *from = malloc(((size_t)t->rc * t->cc + 1) * sizeof *from);
        const Tab *src[1] = {t};
        Buf out = {0};
        for (uint32_t r = 0; from && r < t->rc; r++)
            for (uint32_t c = 0; c < t->cc; c++) from[(size_t)r * t->cc + c] = (Src){r, 0};
        if (!from || !write_table(&out, t, src, from, t->rc, t->extra) || out.n != t->len || memcmp(out.p, t->p, t->len))
            bad++;
        free(from);
        free(out.p);
    }
    file_free(&f);
    return bad;
}

// ---- the world raster (docs/mod-ids.md, "The world raster")
//
// zone/zone0001.atr says which map region each 10 m cell of Mira is: a 32-byte header, then one byte per cell (the
// region id). A region mod paints its land with its region's number; two of them ship the whole file each, so as a
// whole file only the higher one's land would have its region. Combined, every mod's painted cells (the bytes that
// differ from the game's) are put on the game's raster, lowest priority first, so a higher mod's cell wins, each
// painted with the region number its mod was given.
uint8_t *bdat_raster_combine(const BdatInput *game, const BdatInput *mods, int n, size_t *out_n, BdatNote note,
                             void *ctx, char *why, size_t cap) {
    const size_t head = 32;
    if (game->size <= head || memcmp(game->data, "zatr", 4)) {
        snprintf(why, cap, "the game's raster is not one this reads");
        return NULL;
    }
    uint8_t *out = malloc(game->size), *owner = calloc(game->size, 1);
    uint32_t won[MAX_MODS] = {0}, lost[MAX_MODS] = {0};
    int used[MAX_MODS] = {0}, nused = 0;
    if (!out || !owner || n > MAX_MODS) {
        free(out);
        free(owner);
        snprintf(why, cap, "out of memory");
        return NULL;
    }
    memcpy(out, game->data, game->size);
    char line[400];
    for (int k = 0; k < n; k++)
        if (!mods[k].data || mods[k].size != game->size || memcmp(mods[k].data, game->data, head)) {
            snprintf(line, sizeof line, "left out: %s: its raster is not the game's size or layout; not used",
                     mods[k].name);
            if (note) note(ctx, line);
        } else {
            used[k] = 1;
            nused++;
        }
    for (int k = n - 1; k >= 0; k--) {   // lowest priority first: a higher mod's cell wins
        if (!used[k]) continue;
        uint8_t map[256];
        for (int v = 0; v < 256; v++) map[v] = (uint8_t)v;
        for (int i = 0; i < mods[k].nids; i++) {
            const BdatId *e = &mods[k].ids[i];
            if (e->kind == BDAT_ID_REGION && e->has_local && e->local < 256 && e->number < 256)
                map[e->local] = (uint8_t)e->number;
        }
        const uint8_t *m = mods[k].data, *g = game->data;
        for (size_t i = head; i < game->size; i++) {
            if (m[i] == g[i]) continue;
            uint8_t v = map[m[i]];
            if (owner[i] && out[i] != v) lost[owner[i] - 1]++;
            out[i] = v;
            owner[i] = (uint8_t)(k + 1);
        }
    }
    for (size_t i = head; i < game->size; i++)
        if (owner[i]) won[owner[i] - 1]++;
    for (int k = 0; k < n; k++) {
        if (!used[k]) continue;
        snprintf(line, sizeof line, "raster: %s: %u cell(s) painted", mods[k].name, won[k]);
        if (note) note(ctx, line);
        if (lost[k]) {
            snprintf(line, sizeof line, "left out: %s: %u of its raster cells: a mod higher in priority paints them "
                     "too", mods[k].name, lost[k]);
            if (note) note(ctx, line);
        }
    }
    free(owner);
    if (!nused) {
        free(out);
        snprintf(why, cap, "no mod's raster can be used");
        return NULL;
    }
    *out_n = game->size;
    return out;
}
