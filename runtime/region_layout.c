// Panel rows for mod regions, made from the loaded menu layout (docs/regions.md, "Panel rows").
//
// Region Intel (on the Segments, Map and FrontierNav tabs) and Map Shortcuts build one row widget for each row node
// "mc_List01", "mc_List02", ... their menu layout has, and the game's layouts have exactly as many rows as the game
// fills. runtime/regions.c gives every mod region a row after the game's (the panels' lists are moved past their
// panel's end with room for more), so a layout needs a row node per mod region. Instead of each mod shipping an edited
// layout (two mods would then clash on the same file), the rows are added here when the game opens one of those
// layouts and mod regions exist: the file the game would have read (a mod's copy, else the game's own; a mod's copy
// that brings region rows of its own is set aside for the game's) is read, rows are added to it, and the result is
// served from <profile>/region-layouts/ in its place. With no mod region nothing is served and the game reads its file
// as it is.
//
// The layouts are LAGP files (version 10003): a header with section offsets, a section of placement nodes (64-byte
// entries, each pointing at small arrays of position and scale keys) and a section of components (movie clips such as
// mc_Top and its rows, each with frames, a display list, rects and its own blocks, and a component tree). The fields
// used here were read off the game's three layouts; what the rest of the format means is not known, and those bytes
// are copied as they are. Every offset inside a section is relative to the section's start, so grown tables can be
// written past the end of the file and repointed while the original bytes stay in place. One added row:
// - the new row component mc_List(N+1) is a copy of row N's block and of its child components (the FN tab's icons,
//   Map Shortcuts' mc_Junk), placed one row step further on (the step between rows N-1 and N);
// - a special last row (Region Intel's Mira, whose placement node carries extra tracks that hide it in the Map tab's
//   mission view, and whose frame flags differ) stays last: the row that stops being last gets an ordinary placement
//   node and frame flags, and the new row takes the special ones;
// - mc_Top gets one more display item, frame and tree slot; the window and its blur grow by one row through their
//   open-animation scale keys (their shown size is the rect's size times the key), moved by half the growth so the top
//   edge stays;
// - per layout: Map Shortcuts' panel moves up one row (it already reaches the screen's bottom), and since its window's
//   header band is part of the window's picture and stretches with it, its rows also move down by the band's growth
//   and the window and the move up grow by as much (RegionLayoutKind.band); the FN tab's panel is made a little wider
//   once (room for a longer region name: the window, the icon columns and their header).
// A panel too tall for the screen is then scaled down as a whole (fit), and Map Shortcuts' cursor rects grow to make
// up for the part of the cursor's picture that keeps its size (cursor_grow).
// Nothing else in the file changes; the open and close animations need no change (seen in game).
#include "region_layout.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *d; size_t n, cap; int bad; } Buf;

static uint32_t rd32(const Buf *b, size_t o) {
    uint32_t v = 0;
    if (o + 4 <= b->n) memcpy(&v, b->d + o, 4);
    return v;
}
static uint16_t rd16(const Buf *b, size_t o) {
    uint16_t v = 0;
    if (o + 2 <= b->n) memcpy(&v, b->d + o, 2);
    return v;
}
static double rdf(const Buf *b, size_t o) {
    float v = 0;
    if (o + 4 <= b->n) memcpy(&v, b->d + o, 4);
    return v;
}
static void wr32(Buf *b, size_t o, uint32_t v) {
    if (o + 4 <= b->n) memcpy(b->d + o, &v, 4);
    else b->bad = 1;
}
static void wrf(Buf *b, size_t o, double v) {
    float f = (float)v;
    if (o + 4 <= b->n) memcpy(b->d + o, &f, 4);
    else b->bad = 1;
}
static int in_range(const Buf *b, size_t o, size_t len) { return o <= b->n && len <= b->n - o; }

// Appends len bytes (src, or zeros for NULL) after padding the file to align; the offset written at, 0 on failure.
static size_t append(Buf *b, const void *src, size_t len, size_t align) {
    size_t at = (b->n + align - 1) / align * align;
    if (at + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < at + len) cap *= 2;
        uint8_t *d = realloc(b->d, cap);
        if (!d) { b->bad = 1; return 0; }
        b->d = d;
        b->cap = cap;
    }
    memset(b->d + b->n, 0, at - b->n);
    if (src) memmove(b->d + at, src, len);
    else memset(b->d + at, 0, len);
    b->n = at + len;
    return at;
}

static const char *cstr(const Buf *b, size_t o) {
    if (o >= b->n || !memchr(b->d + o, 0, b->n - o)) return "";
    return (const char *)b->d + o;
}

#define MAXC 512     // components, display items, frames, rects, nodes, tree slots a layout may have
typedef struct { uint32_t e[7]; } Comp;
typedef struct { uint32_t kind; uint16_t item, ref; uint32_t inst; } Item;

typedef struct {
    Buf b;
    size_t u4, u13;
} Lagp;

static int lagp_ok(const Buf *b) {
    return b->n >= 0x40 && !memcmp(b->d, "LAGP", 4) && rd32(b, 4) == 10003 && rd32(b, 0x18) && rd32(b, 0x3c) &&
           rd32(b, 0x18) < b->n && rd32(b, 0x3c) < b->n;
}

// How many keys (8 bytes each) the array at a placement node's field `at` holds (2 = position, 3 = scale). The one
// rule for every key array read here: an array this code appended has the count it recorded (recorded, nonzero);
// any other runs up to the node's next array (a node's arrays lie in order), and a node's last array holds one key.
static uint32_t keys_of(const uint8_t node[64], uint16_t recorded, int at) {
    if (recorded) return recorded;
    uint32_t f[8], end = 0;
    memcpy(f, node, 32);
    for (int j = at + 1; j < 8; j++)
        if (f[j] > f[at] && (!end || f[j] < end)) end = f[j];
    return end ? (end - f[at]) / 8 : 1;
}

// The (x, y) position keys of node i: their file offset and how many (keys_of; keys[i] = the count recorded for a
// position array this code wrote).
static size_t node_xy(const Lagp *L, const uint8_t (*nodes)[64], const uint16_t *keys, uint32_t i, uint32_t *n) {
    uint32_t f2;
    memcpy(&f2, nodes[i] + 8, 4);
    *n = keys_of(nodes[i], keys[i], 2);
    return L->u4 + f2;
}

static int comps_of(const Lagp *L, Comp *out) {
    uint32_t off = rd32(&L->b, L->u13), cnt = rd32(&L->b, L->u13 + 4);
    if (cnt > MAXC || !in_range(&L->b, L->u13 + off, (size_t)cnt * 28)) return -1;
    for (uint32_t i = 0; i < cnt; i++) memcpy(out[i].e, L->b.d + L->u13 + off + 28 * i, 28);
    return (int)cnt;
}
static const char *comp_name(const Lagp *L, const Comp *c) { return cstr(&L->b, L->u13 + c->e[6]); }

// A component's display items and its three u16 lists.
typedef struct { int n; Item it[MAXC]; int nl[3]; uint16_t l[3][MAXC]; } Display;
static int display_of(const Lagp *L, const Comp *c, Display *D) {
    size_t b = L->u13 + c->e[3];
    if (!in_range(&L->b, b, 32)) return 0;
    uint32_t h[8];
    memcpy(h, L->b.d + b, 32);
    if (h[0] > MAXC || !in_range(&L->b, b + h[1], (size_t)h[0] * 12)) return 0;
    D->n = (int)h[0];
    for (uint32_t j = 0; j < h[0]; j++) {
        size_t o = b + h[1] + 12 * j;
        D->it[j].kind = rd32(&L->b, o);
        D->it[j].item = rd16(&L->b, o + 4);
        D->it[j].ref = rd16(&L->b, o + 6);
        D->it[j].inst = rd32(&L->b, o + 8);
    }
    for (int k = 0; k < 3; k++) {
        uint32_t cnt = h[2 + 2 * k], at = h[3 + 2 * k];
        if (cnt >= MAXC || (cnt && !in_range(&L->b, b + at, (size_t)cnt * 2))) return 0;
        D->nl[k] = (int)cnt;
        for (uint32_t j = 0; j < cnt; j++) D->l[k][j] = rd16(&L->b, b + at + 2 * j);
    }
    return 1;
}

static int frames_of(const Lagp *L, const Comp *c, uint32_t (*fr)[2]) {
    size_t b = L->u13 + c->e[1];
    uint32_t off = rd32(&L->b, b), cnt = rd32(&L->b, b + 4);
    if (cnt >= MAXC || !in_range(&L->b, b + off, (size_t)cnt * 8)) return -1;
    for (uint32_t j = 0; j < cnt; j++) {
        fr[j][0] = rd32(&L->b, b + off + 8 * j);
        fr[j][1] = rd32(&L->b, b + off + 8 * j + 4);
    }
    return (int)cnt;
}

typedef struct { uint32_t name, node; double x, y, w, h; } Rect;
static int rects_of(const Lagp *L, const Comp *c, Rect *r) {
    size_t b = L->u13 + c->e[4];
    uint32_t cnt = rd32(&L->b, b), off = rd32(&L->b, b + 4);
    if (cnt > MAXC || !in_range(&L->b, b + off, (size_t)cnt * 32)) return -1;
    for (uint32_t j = 0; j < cnt; j++) {
        size_t o = b + off + 32 * j;
        r[j] = (Rect){rd32(&L->b, o), rd32(&L->b, o + 4), rdf(&L->b, o + 8), rdf(&L->b, o + 12), rdf(&L->b, o + 16),
                      rdf(&L->b, o + 20)};
    }
    return (int)cnt;
}

static int find_comp(const Lagp *L, const Comp *c, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(comp_name(L, &c[i]), name)) return i;
    return -1;
}

static int row_number(const char *name) {
    if (strncmp(name, "mc_List", 7) || !name[7]) return -1;
    int v = 0;
    for (const char *p = name + 7; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

// Rows (mc_List components) a layout has; -1 when it isn't a layout this reads.
int region_layout_rows(const uint8_t *data, size_t n) {
    Lagp L = {{(uint8_t *)data, n, n, 0}, 0, 0};
    if (!lagp_ok(&L.b)) return -1;
    L.u4 = rd32(&L.b, 0x18);
    L.u13 = rd32(&L.b, 0x3c);
    Comp *c = malloc(MAXC * sizeof *c);
    if (!c) return -1;
    int nc = comps_of(&L, c), rows = 0;
    for (int i = 0; i < nc; i++) rows += row_number(comp_name(&L, &c[i])) > 0;
    free(c);
    return nc < 0 ? -1 : rows;
}

// Region Intel's divider lines: rect_LineNN rects in mc_Top's rect table.
int region_layout_lines(const uint8_t *data, size_t n) {
    Lagp L = {{(uint8_t *)data, n, n, 0}, 0, 0};
    if (!lagp_ok(&L.b)) return -1;
    L.u4 = rd32(&L.b, 0x18);
    L.u13 = rd32(&L.b, 0x3c);
    Comp *c = malloc(MAXC * sizeof *c);
    Rect *r = malloc(MAXC * sizeof *r);
    int lines = -1;
    if (c && r) {
        int nc = comps_of(&L, c), ti = nc < 0 ? -1 : find_comp(&L, c, nc, "mc_Top"), nr = ti < 0 ? -1 : rects_of(&L, &c[ti], r);
        if (nr >= 0) {
            lines = 0;
            for (int i = 0; i < nr; i++) lines += !strncmp(cstr(&L.b, L.u13 + r[i].name), "rect_Line", 9);
        }
    }
    free(c);
    free(r);
    return lines;
}

typedef struct {
    Comp c[MAXC];
    Display top, kid;
    uint32_t fr[MAXC][2], fk[MAXC][2];
    Rect r[MAXC];
    uint8_t nodes[MAXC + 4][64];
    uint16_t keys[MAXC + 4];
    uint32_t ti[MAXC + 2][8];
    uint16_t idx[MAXC + 2];
} Work;

// One more row (see the top of the file); 0 when the layout doesn't have the expected shape. A row can fail after it
// changed bytes in place (the window's keys, the node table's place): region_layout_grow puts the file back then.
static int add_row(Lagp *L, Work *W, int move_up, double widen, const char *const *widen_also, double band) {
    Buf *b = &L->b;
    uint32_t noff = rd32(b, L->u4 + 4), nn = rd32(b, L->u4 + 8);
    if (nn + 4 > MAXC || !in_range(b, L->u4 + noff, (size_t)nn * 64)) return 0;
    for (uint32_t i = 0; i < nn; i++) memcpy(W->nodes[i], b->d + L->u4 + noff + 64 * i, 64);
    int nc = comps_of(L, W->c);
    if (nc < 3) return 0;
    int top_i = find_comp(L, W->c, nc, "mc_Top");
    if (top_i < 1) return 0;
    Comp top = W->c[top_i];
    if (!display_of(L, &top, &W->top)) return 0;
    int nf = frames_of(L, &top, W->fr), nr = rects_of(L, &top, W->r);
    if (nf < 0 || nr < 0 || nf != W->top.n) return 0;
    // the rows by number (names sort as numbers: two digits), the last two
    int last, prev, third, nrows = 0, lastn;
    for (int i = 0; i < nc; i++) {
        int v = row_number(comp_name(L, &W->c[i]));
        if (v > 0) nrows++;
    }
    if (nrows < 2) return 0;
    {   // last, prev, third = the rows with the three highest numbers (first component of that name)
        int best[3] = {-1, -1, -1}, bi[3] = {-1, -1, -1};
        for (int i = 0; i < nc; i++) {
            int v = row_number(comp_name(L, &W->c[i]));
            if (v <= 0 || v == best[0] || v == best[1] || v == best[2]) continue;
            for (int k = 0; k < 3; k++)
                if (v > best[k]) {
                    for (int m = 2; m > k; m--) { best[m] = best[m - 1]; bi[m] = bi[m - 1]; }
                    best[k] = v;
                    bi[k] = i;
                    break;
                }
        }
        last = bi[0], prev = bi[1], third = bi[2];
        lastn = best[0];
    }
    if (lastn >= 99) return 0;
    char new_name[16];
    snprintf(new_name, sizeof new_name, "mc_List%02d", lastn + 1);
    int kids[MAXC], nk = 0;
    for (int i = last + 1; i < nc; i++) {
        const char *s = comp_name(L, &W->c[i]);
        if (!strncmp(s, "mc_List", 7) || !strcmp(s, "top") || !strcmp(s, "mc_Top")) break;
        kids[nk++] = i;
    }
    // component -> its placement node, from mc_Top's display items and frames (the last item naming it)
    int node_of[MAXC];
    for (int i = 0; i < MAXC; i++) node_of[i] = -1;
    for (int j = 0; j < W->top.n; j++)
        if (W->top.it[j].kind == 0 && W->top.it[j].ref < MAXC) node_of[W->top.it[j].ref] = (int)(W->fr[j][1] >> 16);
    int nl = node_of[last], np = node_of[prev];
    if (nl < 0 || np < 0 || (uint32_t)nl >= nn || (uint32_t)np >= nn) return 0;
    uint32_t k;
    size_t o = node_xy(L, W->nodes, W->keys, (uint32_t)(nl), &k);
    double yl = rdf(b, o + 4);
    o = node_xy(L, W->nodes, W->keys, (uint32_t)(np), &k);
    double xp = rdf(b, o), yp = rdf(b, o + 4);
    double row = yl - yp;
    int special = memcmp(W->nodes[nl], W->nodes[np], 4) != 0, plain_node = -1;
    uint32_t count = nn;
    if (special && nrows > 2) {
        int n3 = third >= 0 ? node_of[third] : -1;
        if (n3 < 0 || (uint32_t)n3 >= nn) return 0;
        o = node_xy(L, W->nodes, W->keys, (uint32_t)(n3), &k);
        row = yp - rdf(b, o + 4);
        memcpy(W->nodes[count], W->nodes[np], 64);
        float xy[2] = {(float)xp, (float)(yp + row)};
        size_t at = append(b, xy, 8, 4);
        uint32_t rel = (uint32_t)(at - L->u4);
        memcpy(W->nodes[count] + 8, &rel, 4);
        W->keys[count] = 1;
        plain_node = (int)count++;
    }
    // the new row's placement node: every key of the last row's position track, one row further on
    memcpy(W->nodes[count], W->nodes[nl], 64);
    o = node_xy(L, W->nodes, W->keys, (uint32_t)(nl), &k);
    if (k > 64 || !in_range(b, o, (size_t)k * 8)) return 0;
    float keys[128];
    for (uint32_t j = 0; j < k; j++) {
        keys[2 * j] = (float)rdf(b, o + 8 * j);
        keys[2 * j + 1] = (float)(rdf(b, o + 8 * j + 4) + row);
    }
    size_t at = append(b, keys, 8 * (size_t)k, 4);
    uint32_t rel = (uint32_t)(at - L->u4);
    memcpy(W->nodes[count] + 8, &rel, 4);
    W->keys[count] = (uint16_t)k;
    int row_node = (int)count++;
    // A window whose header band is part of its picture (Map Shortcuts') stretches the band with its height: the band
    // grows by band * the scale added. The rows move down by that growth (d) and the window grows by it too; d solves
    // d = band * (row + d) / h, h the window rect's height.
    double d = 0;
    if (band > 0) {
        for (int i = 0; i < nr; i++)
            if (!strcmp(cstr(b, L->u13 + W->r[i].name), "rect_Window") && W->r[i].h > band)
                d = band * fabs(row) / (W->r[i].h - band);
        uint8_t moved[MAXC + 4] = {0};
        for (int i = 0; i < nc; i++)
            if (row_number(comp_name(L, &W->c[i])) > 0 && node_of[i] >= 0 && (uint32_t)node_of[i] < count)
                moved[node_of[i]] = 1;
        moved[row_node] = 1;
        if (plain_node >= 0) moved[plain_node] = 1;
        for (uint32_t nd = 0; nd < count; nd++) {
            if (!moved[nd]) continue;
            o = node_xy(L, W->nodes, W->keys, nd, &k);
            if (k > 64 || !in_range(b, o, 8 * (size_t)k)) return 0;
            for (uint32_t j = 0; j < k; j++) wrf(b, o + 8 * j + 4, rdf(b, o + 8 * j + 4) + (row >= 0 ? d : -d));
        }
    }
    double grow = row >= 0 ? row + d : row - d;   // how much taller the panel gets
    // (divider lines, below, don't take d: only Map Shortcuts has a band, and it has no lines)
    // Region Intel's divider lines: rects rect_Line01.. in mc_Top, each just above its row, the last one (above the
    // special row) on a node like that row's. One more line, the same way as the row: the line that stops being last
    // gets an ordinary node where it is, and the new line takes the special one, one row further on.
    int line_last = -1, line_prev = -1, line_plain = -1, line_node = -1;
    {
        char ln[24], lp[24];
        snprintf(ln, sizeof ln, "rect_Line%02d", lastn);
        snprintf(lp, sizeof lp, "rect_Line%02d", lastn - 1);
        for (int i = 0; i < nr; i++) {
            const char *s = cstr(b, L->u13 + W->r[i].name);
            if (!strcmp(s, ln)) line_last = i;
            if (!strcmp(s, lp)) line_prev = i;
        }
    }
    if (line_last >= 0 && line_prev >= 0) {
        uint32_t al = W->r[line_last].node, ap = W->r[line_prev].node;
        if (al >= nn || ap >= nn) return 0;
        if (memcmp(W->nodes[al], W->nodes[ap], 4)) {
            o = node_xy(L, W->nodes, W->keys, ap, &k);
            if (!in_range(b, o, 8)) return 0;
            float xy[2] = {(float)rdf(b, o), (float)(rdf(b, o + 4) + row)};
            memcpy(W->nodes[count], W->nodes[ap], 64);
            rel = (uint32_t)(append(b, xy, 8, 4) - L->u4);
            memcpy(W->nodes[count] + 8, &rel, 4);
            W->keys[count] = 1;
            line_plain = (int)count++;
        }
        memcpy(W->nodes[count], W->nodes[al], 64);
        o = node_xy(L, W->nodes, W->keys, al, &k);
        if (k > 64 || !in_range(b, o, (size_t)k * 8)) return 0;
        for (uint32_t j = 0; j < k; j++) {
            keys[2 * j] = (float)rdf(b, o + 8 * j);
            keys[2 * j + 1] = (float)(rdf(b, o + 8 * j + 4) + row);
        }
        rel = (uint32_t)(append(b, keys, 8 * (size_t)k, 4) - L->u4);
        memcpy(W->nodes[count] + 8, &rel, 4);
        W->keys[count] = (uint16_t)k;
        line_node = (int)count++;
    }
    at = append(b, W->nodes, 64 * (size_t)count, 16);
    wr32(b, L->u4 + 4, (uint32_t)(at - L->u4));
    wr32(b, L->u4 + 8, count);

    // window and blur one row taller (scale keys), top edge kept
    for (int i = 0; i < nr; i++) {
        const char *s = cstr(b, L->u13 + W->r[i].name);
        if (strcmp(s, "rect_Window") && strcmp(s, "rect_Blur")) continue;
        if (W->r[i].node >= count || W->r[i].w == 0 || W->r[i].h == 0) return 0;
        uint32_t f[12], nd = W->r[i].node;
        memcpy(f, W->nodes[nd], 48);
        uint32_t ns = keys_of(W->nodes[nd], 0, 3), np = keys_of(W->nodes[nd], W->keys[nd], 2);
        if (!f[3] || ns > 64 || np > 64 || !in_range(b, L->u4 + f[3], 8 * (size_t)ns) ||
            !in_range(b, L->u4 + f[2], 8 * (size_t)np))
            return 0;
        for (uint32_t j = 0; j < ns; j++) {
            size_t so = L->u4 + f[3] + 8 * j;
            wrf(b, so, rdf(b, so) + widen / W->r[i].w);
            wrf(b, so + 4, rdf(b, so + 4) + fabs(grow) / W->r[i].h);
        }
        for (uint32_t j = 0; j < np; j++) {
            size_t po = L->u4 + f[2] + 8 * j;
            wrf(b, po, rdf(b, po) + widen / 2);
            wrf(b, po + 4, rdf(b, po + 4) + grow / 2);
        }
    }
    if (widen != 0) {   // the rows' child columns (shared placement nodes) and named rects move right by widen
        if (!display_of(L, &W->c[last], &W->kid)) return 0;
        int nfk = frames_of(L, &W->c[last], W->fk);
        if (nfk != W->kid.n) return 0;
        uint8_t shift[MAXC + 2] = {0};
        for (int j = 0; j < W->kid.n; j++)
            if (W->kid.it[j].kind == 0 && (W->fk[j][1] >> 16) < count) shift[W->fk[j][1] >> 16] = 1;
        for (int i = 0; i < nr; i++)
            for (int a = 0; widen_also && widen_also[a]; a++)
                if (!strcmp(cstr(b, L->u13 + W->r[i].name), widen_also[a]) && W->r[i].node < count)
                    shift[W->r[i].node] = 1;
        for (uint32_t nd = 0; nd < count; nd++) {
            if (!shift[nd]) continue;
            o = node_xy(L, W->nodes, W->keys, (uint32_t)(nd), &k);
            if (k > 64 || !in_range(b, o, 8 * (size_t)k)) return 0;
            for (uint32_t j = 0; j < k; j++) wrf(b, o + 8 * j, rdf(b, o + 8 * j) + widen);
        }
    }
    if (move_up) {   // the whole panel one row up (`top` places mc_Top)
        int ti = -1;
        for (int i = 0; i < nc; i++) if (!strcmp(comp_name(L, &W->c[i]), "top")) ti = i;
        if (ti < 0 || frames_of(L, &W->c[ti], W->fk) < 1 || (W->fk[0][1] >> 16) >= count) return 0;
        o = node_xy(L, W->nodes, W->keys, (uint32_t)(W->fk[0][1] >> 16), &k);
        if (k > 64 || !in_range(b, o, 8 * (size_t)k)) return 0;
        for (uint32_t j = 0; j < k; j++) wrf(b, o + 8 * j + 4, rdf(b, o + 8 * j + 4) - grow);
    }

    // clone the row and its children: a component's blocks run from its block (entry 5) to the next component's
    uint32_t hdr13[4] = {rd32(b, L->u13), rd32(b, L->u13 + 4), rd32(b, L->u13 + 8), rd32(b, L->u13 + 12)};
    Comp made[MAXC];
    int nmade = 0;
    for (int m = 0; m <= nk; m++) {
        int ci = m == 0 ? last : kids[m - 1];
        Comp e = W->c[ci];
        uint32_t begin = e.e[5], end = 0;
        for (int j = 0; j < nc; j++)   // the next block start after begin
            if (W->c[j].e[5] > begin && (!end || W->c[j].e[5] < end)) end = W->c[j].e[5];
        if (!end) {   // a row appended earlier: the next thing written after it, else the end of the file
            uint32_t lim = (uint32_t)(b->n - L->u13);
            uint32_t cand[4] = {hdr13[0], hdr13[2], hdr13[3], lim};
            for (int j = 0; j < nc; j++)
                for (int f = 1; f < 7; f++)
                    if (j != ci && W->c[j].e[f] > begin && (!end || W->c[j].e[f] < end)) end = W->c[j].e[f];
            for (int q = 0; q < 4; q++) if (cand[q] > begin && (!end || cand[q] < end)) end = cand[q];
        }
        if (!begin || end <= begin || !in_range(b, L->u13 + begin, end - begin)) return 0;
        size_t blk = append(b, NULL, end - begin, 4);
        if (b->bad) return 0;
        memmove(b->d + blk, b->d + L->u13 + begin, end - begin);
        uint32_t d = (uint32_t)(blk - L->u13) - begin;
        for (int f = 1; f <= 5; f++) if (e.e[f]) e.e[f] += d;
        if (m == 0) e.e[6] = (uint32_t)(append(b, new_name, strlen(new_name) + 1, 1) - L->u13);
        made[nmade++] = e;
    }
    int li = nc;
    {   // the copy's display list: its children -> the new ones
        size_t db = L->u13 + made[0].e[3];
        uint32_t h0 = rd32(b, db), h1 = rd32(b, db + 4);
        if (h0 > MAXC || !in_range(b, db + h1, 12 * (size_t)h0)) return 0;
        for (uint32_t j = 0; j < h0; j++) {
            size_t io = db + h1 + 12 * j;
            if (rd32(b, io)) continue;
            uint16_t ref = rd16(b, io + 6);
            if (!in_range(b, io + 6, 2)) continue;
            for (int q = 0; q < nk; q++)
                if (ref == kids[q]) {
                    uint16_t nr16 = (uint16_t)(li + 1 + q);
                    memcpy(b->d + io + 6, &nr16, 2);
                }
        }
    }

    // mc_Top: one more row instance (display item, instance list, frame)
    Display *T = &W->top;
    int i_last = -1, i_prev = -1, n_inst = 0;
    for (int j = 0; j < T->n; j++) {
        if (T->it[j].kind) continue;
        n_inst++;
        if (i_last < 0 && T->it[j].ref == last) i_last = j;
        if (i_prev < 0 && T->it[j].ref == prev) i_prev = j;
    }
    if (i_last < 0 || i_prev < 0 || T->n + 1 >= MAXC || T->nl[0] + 1 >= MAXC) return 0;
    T->it[T->n] = (Item){0, (uint16_t)T->n, (uint16_t)li, (uint32_t)n_inst};
    T->n++;
    T->l[0][T->nl[0]++] = (uint16_t)(T->n - 1);
    int i_ll = -1, i_lp = -1;
    if (line_node >= 0) {   // and one more line instance (rects are mc_Top's kind 1 items, in its second list)
        for (int j = 0; j < T->n; j++) {
            if (T->it[j].kind != 1) continue;
            if (T->it[j].ref == line_last) i_ll = j;
            if (T->it[j].ref == line_prev) i_lp = j;
        }
        if (i_ll < 0 || i_lp < 0 || T->n + 1 >= MAXC || T->nl[1] + 1 >= MAXC) return 0;
        T->it[T->n] = (Item){1, (uint16_t)T->n, (uint16_t)nr, 0};
        T->n++;
        T->l[1][T->nl[1]++] = (uint16_t)(T->n - 1);
    }
    {
        size_t body = 12 * (size_t)T->n, lsz[3] = {2 * (size_t)T->nl[0], 2 * (size_t)T->nl[1], 2 * (size_t)T->nl[2]};
        uint32_t l_off[3] = {(uint32_t)(64 + body), 0, 0};
        l_off[1] = l_off[0] + (uint32_t)lsz[0];
        l_off[2] = l_off[1] + (uint32_t)lsz[1];
        size_t total = 64 + body + lsz[0] + lsz[1] + lsz[2];
        uint8_t *blob = calloc(1, total);
        if (!blob) return 0;
        uint32_t h[8] = {(uint32_t)T->n, 64, (uint32_t)T->nl[0], l_off[0], (uint32_t)T->nl[1], l_off[1],
                         (uint32_t)T->nl[2], T->nl[2] ? l_off[2] : 0};
        memcpy(blob, h, 32);
        for (int j = 0; j < T->n; j++) {
            uint8_t *p = blob + 64 + 12 * j;
            memcpy(p, &T->it[j].kind, 4);
            memcpy(p + 4, &T->it[j].item, 2);
            memcpy(p + 6, &T->it[j].ref, 2);
            memcpy(p + 8, &T->it[j].inst, 4);
        }
        for (int q = 0; q < 3; q++) memcpy(blob + l_off[q], T->l[q], lsz[q]);
        top.e[3] = (uint32_t)(append(b, blob, total, 4) - L->u13);
        free(blob);
    }
    uint32_t flag_new = W->fr[i_last][0];
    W->fr[i_last][0] = W->fr[i_prev][0];   // the old last row becomes an ordinary one (the same when it was)
    if (special && nrows > 2) W->fr[i_last][1] = (uint32_t)plain_node << 16 | (W->fr[i_prev][1] & 0xffff);
    W->fr[nf][0] = flag_new;
    W->fr[nf][1] = (uint32_t)row_node << 16 | 0xffff;   // low half as the rows' own frames have it (not traced)
    nf++;
    if (line_node >= 0) {
        uint32_t line_flags = W->fr[i_ll][0], low = W->fr[i_ll][1] & 0xffff;
        W->fr[i_ll][0] = W->fr[i_lp][0];
        if (line_plain >= 0) W->fr[i_ll][1] = (uint32_t)line_plain << 16 | (W->fr[i_lp][1] & 0xffff);
        W->fr[nf][0] = line_flags;
        W->fr[nf][1] = (uint32_t)line_node << 16 | low;
        nf++;
    }
    {
        uint32_t hdr[2] = {8, (uint32_t)nf};
        size_t fo = append(b, hdr, 8, 4);
        append(b, W->fr, 8 * (size_t)nf, 1);
        top.e[1] = (uint32_t)(fo - L->u13);
    }
    if (line_node >= 0) {   // mc_Top's rect table with the new line: the last line's entry, on its node, named for the row
        size_t rb = L->u13 + top.e[4];
        uint32_t rc = rd32(b, rb), ro = rd32(b, rb + 4);
        if (rc != (uint32_t)nr || !in_range(b, rb + ro, 32 * (size_t)rc)) return 0;
        uint8_t *tab = malloc(8 + 32 * ((size_t)rc + 1));
        if (!tab) return 0;
        uint32_t h[2] = {rc + 1, 8};
        memcpy(tab, h, 8);
        memcpy(tab + 8, b->d + rb + ro, 32 * (size_t)rc);
        memcpy(tab + 8 + 32 * (size_t)rc, b->d + rb + ro + 32 * (size_t)line_last, 32);
        char nm[24];
        snprintf(nm, sizeof nm, "rect_Line%02d", lastn + 1);
        uint32_t name = (uint32_t)(append(b, nm, strlen(nm) + 1, 1) - L->u13), nd = (uint32_t)line_node;
        memcpy(tab + 8 + 32 * (size_t)rc, &name, 4);
        memcpy(tab + 8 + 32 * (size_t)rc + 4, &nd, 4);
        if (line_plain >= 0) {
            uint32_t pn = (uint32_t)line_plain;
            memcpy(tab + 8 + 32 * (size_t)line_last + 4, &pn, 4);
        }
        top.e[4] = (uint32_t)(append(b, tab, 8 + 32 * ((size_t)rc + 1), 4) - L->u13);
        free(tab);
    }
    if (nc + nmade > MAXC) return 0;
    W->c[top_i] = top;
    for (int m = 0; m < nmade; m++) W->c[nc + m] = made[m];
    int ncomp = nc + nmade;
    {
        size_t cb = append(b, W->c, 28 * (size_t)ncomp, 4);
        wr32(b, L->u13, (uint32_t)(cb - L->u13));
        wr32(b, L->u13 + 4, (uint32_t)ncomp);
    }

    // the component tree: item i = component i + 1 {children, first child slot, descendants, parent, ...}; slots = the
    // index list (component per slot). mc_Top's rows are contiguous slots, so the new row's slot shifts every later
    // slot by one; the new row's children go last (before `top`).
    size_t tb0 = L->u13 + rd32(b, L->u13 + 8), ib0 = L->u13 + rd32(b, L->u13 + 12);
    uint32_t th[4] = {rd32(b, tb0), rd32(b, tb0 + 4), rd32(b, tb0 + 8), rd32(b, tb0 + 12)};
    uint32_t nt = th[2];
    if (nt + 2 + (uint32_t)nk > MAXC || !in_range(b, tb0 + 32, (size_t)nt * 32) ||
        !in_range(b, ib0, ((size_t)nt + 1) * 2))
        return 0;
    for (uint32_t j = 0; j < nt; j++) memcpy(W->ti[j], b->d + tb0 + 32 + 32 * j, 32);
    uint16_t old_idx[MAXC + 2];
    for (uint32_t j = 0; j <= nt; j++) old_idx[j] = rd16(b, ib0 + 2 * j);
    if (top_i - 1 >= (int)nt) return 0;
    uint32_t first = W->ti[top_i - 1][1], cnt = W->ti[top_i - 1][0], new_slot = first + cnt;
    if (new_slot > nt) return 0;
    for (uint32_t j = 0; j < nt; j++) if (W->ti[j][1] >= new_slot) W->ti[j][1]++;
    W->ti[top_i - 1][0] += 1;
    W->ti[top_i - 1][2] += 1 + (uint32_t)nk;
    uint32_t kid_slot = nt + 1, ntn = nt;
    // fields 4-7 of a tree item are 0 in the rows' own items (not traced further)
    uint32_t row_item[8] = {(uint32_t)nk, kid_slot, (uint32_t)nk, (uint32_t)top_i, 0, 0, 0, 0};
    memcpy(W->ti[ntn++], row_item, 32);
    for (int q = 0; q < nk; q++) {
        uint32_t kid_item[8] = {0, kid_slot + (uint32_t)nk, 0, (uint32_t)li, 0, 0, 0, 0};
        memcpy(W->ti[ntn++], kid_item, 32);
    }
    th[2] += 1 + (uint32_t)nk;
    uint32_t ni = 0;
    for (uint32_t j = 0; j < new_slot; j++) W->idx[ni++] = old_idx[j];
    W->idx[ni++] = (uint16_t)li;
    for (uint32_t j = new_slot; j < nt; j++) W->idx[ni++] = old_idx[j];   // all but the last slot
    for (int q = 0; q < nk; q++) W->idx[ni++] = (uint16_t)(li + 1 + q);
    W->idx[ni++] = old_idx[nt];
    {
        size_t tb = append(b, th, 16, 4);
        append(b, NULL, 16, 1);
        append(b, W->ti, 32 * (size_t)ntn, 1);
        size_t ib = append(b, W->idx, 2 * (size_t)ni, 4);
        wr32(b, L->u13 + 8, (uint32_t)(tb - L->u13));
        wr32(b, L->u13 + 12, (uint32_t)(ib - L->u13));
    }
    return !b->bad;
}

// ---- fitting a tall panel on the screen
// A panel with many rows would run off the screen: the window's top or bottom edge past the band the game's own panels
// keep to (screen-width units, y down: under the tab row, above the button guide). Its root node (`top`, which places
// mc_Top: position and scale keys, the first key the resting one) is then scaled down as a whole, rows and text alike,
// and moved so that the window's left edge stays and the window lies in the band (its top edge where it was when that
// fits). Returns the scale (1: the panel fits as it is), or 0 when the layout doesn't have the expected shape.
#define BAND_TOP 0.05
#define BAND_BOTTOM 0.525

typedef struct { Comp c[MAXC]; Rect r[MAXC]; uint32_t fr[MAXC][2]; } FitWork;
static double fit(Lagp *L, FitWork *F, int apply);

double region_layout_fit(uint8_t *data, size_t n, int apply) {
    Lagp L = {{data, n, n, 0}, 0, 0};
    if (!lagp_ok(&L.b)) return 0;
    L.u4 = rd32(&L.b, 0x18);
    L.u13 = rd32(&L.b, 0x3c);
    FitWork *F = malloc(sizeof *F);
    if (!F) return 0;
    double s = fit(&L, F, apply);
    free(F);
    return s;
}

static double fit(Lagp *L, FitWork *F, int apply) {
    Comp *c = F->c;
    Rect *r = F->r;
    uint32_t (*fr)[2] = F->fr;
    int nc = comps_of(L, c), ti = -1, mi = -1;
    if (nc < 0) return 0;
    for (int i = 0; i < nc; i++) {
        if (!strcmp(comp_name(L, &c[i]), "top")) ti = i;
        if (mi < 0 && !strcmp(comp_name(L, &c[i]), "mc_Top")) mi = i;
    }
    if (ti < 0 || mi < 0 || frames_of(L, &c[ti], fr) < 1) return 0;
    uint32_t nn = rd32(&L->b, L->u4 + 8), noff = rd32(&L->b, L->u4 + 4), tn = fr[0][1] >> 16;
    int nr = rects_of(L, &c[mi], r), wi = -1;
    for (int i = 0; i < nr; i++) if (!strcmp(cstr(&L->b, L->u13 + r[i].name), "rect_Window")) wi = i;
    if (wi < 0 || tn >= nn || r[wi].node >= nn || !in_range(&L->b, L->u4 + noff, (size_t)nn * 64)) return 0;
    uint32_t tf[12], wf[12];
    const uint8_t *tnode = L->b.d + L->u4 + noff + 64 * tn;
    memcpy(tf, tnode, 48);
    memcpy(wf, L->b.d + L->u4 + noff + 64 * r[wi].node, 48);
    size_t tp = L->u4 + tf[2], ts = tf[3] ? L->u4 + tf[3] : 0, wp = L->u4 + wf[2], ws = wf[3] ? L->u4 + wf[3] : 0;
    uint32_t tpn = keys_of(tnode, 0, 2), tsn = tf[3] ? keys_of(tnode, 0, 3) : 0;
    if (!in_range(&L->b, tp, 8 * (size_t)tpn) || (ts && !in_range(&L->b, ts, 8 * (size_t)tsn)) ||
        !in_range(&L->b, wp, 8) || (ws && !in_range(&L->b, ws, 8)))
        return 0;
    double tpx = rdf(&L->b, tp), tpy = rdf(&L->b, tp + 4);
    double tsx = ts ? rdf(&L->b, ts) : 1, tsy = ts ? rdf(&L->b, ts + 4) : 1;
    double wpx = rdf(&L->b, wp), wpy = rdf(&L->b, wp + 4);
    double wsx = ws ? rdf(&L->b, ws) : 1, wsy = ws ? rdf(&L->b, ws + 4) : 1;
    double lt = wpy + r[wi].y * wsy, lb = wpy + (r[wi].y + r[wi].h) * wsy, ll = wpx + r[wi].x * wsx;
    double top0 = tpy + tsy * lt, h = tsy * (lb - lt), band = BAND_BOTTOM - BAND_TOP;
    if (h <= 0 || tsx <= 0 || tsy <= 0) return 0;
    double s = h > band ? band / h : 1;
    double top1 = top0 < BAND_TOP ? BAND_TOP : top0 > BAND_BOTTOM - s * h ? BAND_BOTTOM - s * h : top0;
    if (s == 1 && top1 == top0) return 1;
    if (!apply) return s;
    if (!ts) return 0;   // nothing to scale with
    double dy = top1 - s * tsy * lt - tpy, dx = (tpx + tsx * ll) - s * tsx * ll - tpx;
    for (uint32_t j = 0; j < tpn; j++) {
        wrf(&L->b, tp + 8 * j, rdf(&L->b, tp + 8 * j) + dx);
        wrf(&L->b, tp + 8 * j + 4, rdf(&L->b, tp + 8 * j + 4) + dy);
    }
    for (uint32_t j = 0; j < tsn; j++) {
        wrf(&L->b, ts + 8 * j, rdf(&L->b, ts + 8 * j) * s);
        wrf(&L->b, ts + 8 * j + 4, rdf(&L->b, ts + 8 * j + 4) * s);
    }
    return s;
}

// The rows' cursor in a panel scaled by s: Map Shortcuts' cursor box is drawn a fixed amount inside its rect
// (rect_Cursor and its highlight rect_CursorHi in each row; the game sets their scale when the cursor moves), the same
// amount at any scale, so scaled down it would come out narrower than its row (measured at s 0.51 on a 2560 px wide
// screen: 216 px for a 263 px row, with 616 s - 95 px across and 85 s - 7 px down). Each such rect grows about its center
// by 1 + k (1/s - 1), k the share of the rect the picture leaves empty at full size (kind->cursor_w across, cursor_h
// down), its pixel box with it, so the box lands on its row. Rows may share a rect table: each rect is grown once.
static void cursor_grow(uint8_t *data, size_t n, double s, double kw, double kh) {
    Lagp L = {{data, n, n, 0}, 0, 0};
    if (!lagp_ok(&L.b) || s <= 0 || s >= 1) return;
    L.u4 = rd32(&L.b, 0x18);
    L.u13 = rd32(&L.b, 0x3c);
    Comp *c = malloc(sizeof(Comp) * MAXC);
    size_t *seen = malloc(sizeof(size_t) * MAXC);
    int nc = c ? comps_of(&L, c) : -1, ns = 0;
    double fw = 1 + kw * (1 / s - 1), fh = 1 + kh * (1 / s - 1);
    for (int i = 0; seen && i < nc; i++) {
        if (strncmp(comp_name(&L, &c[i]), "mc_List", 7) || !c[i].e[4]) continue;
        size_t b = L.u13 + c[i].e[4];
        uint32_t cnt = rd32(&L.b, b), off = rd32(&L.b, b + 4);
        if (cnt > MAXC || !in_range(&L.b, b + off, (size_t)cnt * 32)) continue;
        for (uint32_t j = 0; j < cnt; j++) {
            size_t o = b + off + 32 * j;
            const char *nm = cstr(&L.b, L.u13 + rd32(&L.b, o));
            if (strcmp(nm, "rect_Cursor") && strcmp(nm, "rect_CursorHi")) continue;
            int dup = 0;
            for (int k = 0; k < ns; k++) dup |= seen[k] == o;
            if (dup || ns >= MAXC) continue;
            seen[ns++] = o;
            double x = rdf(&L.b, o + 8), y = rdf(&L.b, o + 12), w = rdf(&L.b, o + 16), h = rdf(&L.b, o + 20);
            wrf(&L.b, o + 8, x + w / 2 - w * fw / 2);
            wrf(&L.b, o + 12, y + h / 2 - h * fh / 2);
            wrf(&L.b, o + 16, w * fw);
            wrf(&L.b, o + 20, h * fh);
            int16_t px[4];                      // the rect's pixel box: x0, y0, x1, y1
            memcpy(px, L.b.d + o + 24, 8);
            double mx = (px[0] + px[2]) / 2.0, my = (px[1] + px[3]) / 2.0, hw = (px[2] - px[0]) * fw / 2,
                   hh = (px[3] - px[1]) * fh / 2;
            px[0] = (int16_t)lround(mx - hw);
            px[1] = (int16_t)lround(my - hh);
            px[2] = (int16_t)lround(mx + hw);
            px[3] = (int16_t)lround(my + hh);
            memcpy(L.b.d + o + 24, px, 8);
        }
    }
    free(seen);
    free(c);
}

int region_layout_grow(uint8_t **data, size_t *n, int rows, const RegionLayoutKind *kind) {
    Lagp L = {{*data, *n, *n, 0}, 0, 0};
    if (!lagp_ok(&L.b)) return 0;
    L.u4 = rd32(&L.b, 0x18);
    L.u13 = rd32(&L.b, 0x3c);
    Work *W = calloc(1, sizeof *W);
    if (!W) return 0;
    int done = 0;
    uint8_t *keep = NULL;
    for (; done < rows; done++) {
        double widen = done == 0 ? kind->widen : 0;
        size_t n0 = L.b.n;
        uint8_t *k = realloc(keep, n0 ? n0 : 1);   // the file before this row, to put back if the row fails
        if (!k) break;
        keep = k;
        memcpy(keep, L.b.d, n0);
        uint16_t keys[MAXC + 4];
        memcpy(keys, W->keys, sizeof keys);
        if (add_row(&L, W, kind->move_up, widen, kind->widen_also, kind->band) && !L.b.bad) continue;
        memcpy(L.b.d, keep, n0);                   // L.b.d holds at least n0 bytes: it only grows
        L.b.n = n0;
        L.b.bad = 0;
        memcpy(W->keys, keys, sizeof keys);
        break;
    }
    free(keep);
    free(W);
    *data = L.b.d;
    *n = L.b.n;
    return done;
}

// As many of rows as fit: the rows added to *data, the panel then fitted on the screen (region_layout_fit) when it is
// taller than the band, and its scale in *scale. Rows stop where the panel would go below REGION_LAYOUT_MIN_SCALE; the
// count is found by halving (the scale only falls as rows are added), each try made from a copy of the original.
int region_layout_grow_fit(uint8_t **data, size_t *n, int rows, const RegionLayoutKind *kind, double *scale) {
    uint8_t *src = malloc(*n ? *n : 1);
    size_t n0 = *n;
    if (!src) return 0;
    memcpy(src, *data, n0);
    int added = region_layout_grow(data, n, rows, kind);
    double s = region_layout_fit(*data, *n, 0);
    if (s > 0 && s < REGION_LAYOUT_MIN_SCALE) {
        int lo = 0, hi = added;                       // lo rows fit (none do: the game's panel), hi rows don't
        while (hi - lo > 1) {
            int mid = lo + (hi - lo) / 2;
            uint8_t *d = malloc(n0);
            size_t n1 = n0;
            if (!d) break;
            memcpy(d, src, n0);
            int got = region_layout_grow(&d, &n1, mid, kind);
            double sm = region_layout_fit(d, n1, 0);
            free(d);
            if (got == mid && sm >= REGION_LAYOUT_MIN_SCALE) lo = mid;
            else hi = mid;
        }
        uint8_t *d = realloc(*data, n0 ? n0 : 1);
        if (d) {
            *data = d;
            memcpy(d, src, n0);
            *n = n0;
            added = region_layout_grow(data, n, lo, kind);
            s = region_layout_fit(*data, *n, 0);
        }
    }
    if (s > 0 && s < 1) s = region_layout_fit(*data, *n, 1);   // 0 when it can't be scaled (no scale keys)
    if (s > 0 && s < 1 && (kind->cursor_w > 0 || kind->cursor_h > 0))
        cursor_grow(*data, *n, s, kind->cursor_w, kind->cursor_h);
    free(src);
    if (scale) *scale = s;
    return added;
}

// ---- serving the grown layouts (left out of the standalone build tests/region_layout_test.py makes)
#ifndef REGION_LAYOUT_CORE_ONLY
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "rom_archive.h"
#include "romfs_mods.h"
#include <ctype.h>

// The panels' layouts: the rows the game's copy has, and the per-layout placement rules (see the top of the file).
static const char *const fn_widen[] = {"rect_TextIndex02", NULL};
static const struct { const char *key; RegionLayoutKind kind; } layouts[] = {
    // Region Intel, on the Segments and Map tabs
    {"ui/image/04010_parts05.wilay", {REGION_PANEL_INTEL, REGION_INTEL_GAME_ROWS, 0, 0.0, NULL, 0.0, 0.0, 0.0}},
    // Map Shortcuts
    // its header band grows 0.0075 screen widths per unit of window scale (measured: 53 px at the game's 8 rows, about
    // 104 px at 34, on a 2560 px wide screen); its cursor box leaves 95 of its rect's 616 px across and 7 of 85 down
    // empty at any scale (measured at full size and at 51%, cursor_grow)
    {"ui/image/04010_parts04.wilay",
     {REGION_PANEL_SHORTCUTS, REGION_SHORTCUTS_GAME_ROWS, 1, 0.0, NULL, 0.0075, 95.0 / 616, 7.0 / 85}},
    // Region Intel on the FrontierNav tab
    {"ui/image/04030_frontiernet_parts08.wilay", {REGION_PANEL_FN, REGION_FN_GAME_ROWS, 0, 0.016, fn_widen, 0.0, 0.0, 0.0}},
};
#define NLAYOUTS ((int)(sizeof layouts / sizeof *layouts))

static PlatMutex lock = PLAT_MUTEX_INIT;
// Per layout, for the rows wanted and the source last asked about: 1 = a file to serve at path, -1 = nothing to serve
// (no copy could be read or rows added to it, or the file couldn't be written: the game reads what it would have read),
// 0 = not made yet.
static struct { int state, rows; char src[1100]; char path[1100]; } made[NLAYOUTS];   // under lock

// The game's own copy: in its archive (where the game keeps these layouts), else a loose RomFS file.
static uint8_t *game_copy(const char *key, size_t *n) {
    uint8_t *d = rom_archive_file(key, n);
    if (d || !romfs_mount_name()) return d;
    char path[1200];
    uint64_t h = 0;
    int64_t size = 0;
    snprintf(path, sizeof path, "%s:/%s", romfs_mount_name(), key);
    if (romfs_open(&h, path, FS_OPEN_READ)) return NULL;
    if (!romfs_size(&size, h) && size > 0 && size < (16 << 20) && (d = malloc((size_t)size))) {
        uint64_t got = 0;
        if (romfs_read(&got, h, 0, d, (uint64_t)size) || got != (uint64_t)size) {
            free(d);
            d = NULL;
        } else {
            *n = (size_t)size;
        }
    }
    romfs_close(h);
    return d;
}

// Makes the layout for the rows wanted from the source last asked about. made[li] keeps what was decided for that
// (want, src) so later opens don't make it again. A file already served stays served when a later make fails (a
// lookup claimed for it must find a file to open); the usual path (state -1) is taken only when nothing was served.
static void make(int li, const char *key, int want, int from_mod, const char *src, uint32_t mods) {
    const RegionLayoutKind *kind = &layouts[li].kind;
    uint64_t t0 = plat_now_ns();
    made[li].rows = want;
    snprintf(made[li].src, sizeof made[li].src, "%s", src);   // src fits: both are 1100 bytes
    int served_before = made[li].state > 0;
    if (!served_before) made[li].state = -1;
    const RtConfig *cfg = rt_config();
    if (!cfg || !cfg->profile) return;
    size_t n = 0;
    uint8_t *d = NULL;
    if (from_mod) {
        uint64_t n64 = 0;
        d = plat_file_read(src, &n64);
        n = (size_t)n64;
    } else {
        d = game_copy(key, &n);
    }
    int have = d ? region_layout_rows(d, n) : -1, added = 0;
    if (from_mod && have > kind->game_rows) {
        // a mod's copy with rows of its own for regions (made by hand or by the old layout tool, before rows were made
        // here): two such mods would clash on the file, and the rows are made here for every mod region anyway, so it
        // is set aside and the game's copy used. A mod's copy with the game's row count (a mod that changes the panel
        // itself) keeps its changes and gets the rows.
        rt_log("regions: %s: a mod's copy brings %d rows of its own; it is not used (rows for mod regions are made "
               "from the game's copy)", key, have);
        free(d);
        d = game_copy(key, &n);
        from_mod = 0;
        have = d ? region_layout_rows(d, n) : -1;
    }
    if (!d || have < 0) {
        // the usual path: the copy the game would read anyway
        if (!d)
            rt_log("regions: %s: the %s copy can't be read; no rows for mod regions in this panel", key,
                   from_mod ? "mod's" : "game's");
        else
            rt_log("regions: %s: the %s copy is not a layout rows can be added to; used as it is", key,
                   from_mod ? "mod's" : "game's");
        free(d);
        return;
    }
    double sc = 1;
    if (have < want) {
        added = region_layout_grow_fit(&d, &n, want - have, kind, &sc);
        if (added < want - have)
            rt_log("regions: %s: rows for %d of %u mod region(s): more would make the panel smaller than half size on "
                   "the screen, or the layout's shape stopped them", key, added, mods);
        if (sc > 0 && sc < 1)
            rt_log("regions: %s: the panel is drawn at %.0f%% to fit the screen with %d rows", key, sc * 100,
                   have + added);
        // Region Intel draws a divider line above each row; a copy whose lines don't follow the rows' names (a mod's
        // that renamed or dropped them) gets its rows without lines
        int lines0 = region_layout_lines(d, n);
        if (lines0 > 0 && lines0 < have + added)
            rt_log("regions: %s: %d divider line(s) for %d rows: the copy's lines don't follow its rows' names, so the "
                   "added rows have none", key, lines0, have + added);
    }
    char dir[1100], file[1100];
    const char *base = strrchr(key, '/');
    int ok = snprintf(dir, sizeof dir, "%s/region-layouts", cfg->profile) < (int)sizeof dir &&
             snprintf(file, sizeof file, "%s/%s", dir, base ? base + 1 : key) < (int)sizeof file;
    if (ok) {
        plat_file_mkdir_p(dir);
        ok = plat_file_write_atomic(file, d, n);
    }
    if (ok) {
        memcpy(made[li].path, file, sizeof file);
        made[li].state = 1;
        rt_log("regions: %s: %d row(s) added to %s's %d for %u mod region(s) (%.2f ms)", key, added,
               from_mod ? "a mod's copy" : "the game's copy", have, mods, (double)(plat_now_ns() - t0) / 1e6);
    } else {
        rt_log("regions: %s: the layout with rows for mod regions can't be written under %s%s", key, cfg->profile,
               served_before ? "; the one made before stays" : from_mod ? "; the mod's copy is used as it is"
                                                                         : "; the game's own copy is used");
    }
    free(d);
}

int region_layout_file(const char *path, char *out, size_t cap) {
    uint32_t mods = region_mod_count();
    if (!mods || !path) return 0;
    const char *p = strchr(path, ':');
    p = p ? p + 1 : path;
    while (*p == '/' || *p == '\\') p++;
    char key[256];
    size_t kn = strlen(p);
    if (kn >= sizeof key) return 0;
    for (size_t i = 0; i <= kn; i++) key[i] = p[i] == '\\' ? '/' : (char)tolower((unsigned char)p[i]);
    int li = -1;
    for (int i = 0; i < NLAYOUTS; i++) if (!strcmp(key, layouts[i].key)) li = i;
    if (li < 0) return 0;
    int want = layouts[li].kind.game_rows + (int)region_panel_rows(layouts[li].kind.panel);
    char src[1100] = "";
    int from_mod = romfs_mod_file(key, src, sizeof src);
    if (!from_mod) src[0] = 0;
    plat_mutex_lock(&lock);
    if (!made[li].state || made[li].rows != want || strcmp(made[li].src, src)) make(li, key, want, from_mod, src, mods);
    int ok = made[li].state > 0 && (!out || snprintf(out, cap, "%s", made[li].path) < (int)cap);
    plat_mutex_unlock(&lock);
    return ok;
}
#endif
