// Map mods shipped as pieces (docs/map-mods.md): the world map's header and data files served as the game's own copy
// plus every enabled mod's pieces, built while the game runs. No map file is written anywhere.
//
// The world map is two header + data pairs in the game's archive: map/ma9990.wismhd (packed) indexes
// map/ma9990.wismda (stored raw, ~960 MB), and the enemy navmesh map/ma9990.winvhe (packed) indexes
// map/ma9990.winvda (raw, ~140 MB). A header is a set of tables (a count and an offset in its first words); the
// entries of some point into the data file with an (offset, size) pair. A map mod changes entries of those tables,
// adds entries, and brings the data the changed entries point at.
//
// A mod's pieces: <mods>/<id>/romfs/map/ma9990.pieces/pieces.txt and the files it names (tools/map_pieces.py writes
// them from an edited map). Lines, '#' starts a comment:
//   xcx-map-pieces 1                                       the first line
//   made-from <hd> <size> <xxh64>                          the game header the pieces were made against: one for each
//                                                          header the mod changes (a mod made against another game
//                                                          version, or without it, is left out)
//   add <hd> <table> <name>                                a new entry, all zero until set (see "Adding")
//   set <hd> <table> <entry> <at> <u8|u16|u32> <value> [<bias>]
//                                                          a field of an entry: <at> bytes into it. value: a number,
//                                                          or @<table>:<entry> for the index of an entry (+ bias)
//   chunk <hd> <table> <entry> <at> <file> <size>          the (offset, size) pair at <at> of the entry points at
//                                                          <file>, served after the game's data; size is written as
//                                                          given (the unpacked size of an xbc1 chunk, a nav tile's
//                                                          length)
//   fix <file> <at> <u16|u32> <table> <entry> <recorded>   inside <file> (its unpacked bytes for an xbc1 zlib chunk),
//                                                          at byte <at>, the index of an entry; recorded: the number
//                                                          the file holds now
// <hd> is wismhd or winvhe; <entry> is a number (the game's entries) or +<name> (the mod's added ones). Numbers are
// decimal; files are plain paths inside the pieces folder, found case-insensitively like every mod file.
//
// A mod is used whole or not at all: a bad line, a file that is missing or doesn't match its line, an entry this
// game's header doesn't have, a number too big for its field, or pieces past a size limit leave that mod's pieces out
// (with a line in the log and the Mods tab) and the overlay is made from the others. Its other files still load.
//
// Combining: the mods are taken in priority order (romfs-enabled.txt, the top line first). An entry of the game's
// that two mods change belongs to the higher mod: the lower mod's lines for that entry are left out, and the Mods tab
// and the log say so. Added entries never clash: each table's added entries are numbered after the game's, the top
// mod's first. A mod's file holding such a number (a prop group's texture, stream and model lists name textures and
// vertex data, its placement links name placement entries, a terrain set's groups name terrain vertex data) has a
// fix line for it, and when the number it gets differs from the recorded one, the file is served patched (an xbc1 zlib
// chunk unpacked, patched and stored again as a zlib stream of stored blocks).
//
// Serving: the header from memory; the data file as the game's bytes (read from the archive in place) up to the
// game's end, then each mod file the kept chunk lines name, 4 KB aligned, read from the mod's file (kept open, in a
// way that still lets the mod be updated or removed: plat_file_open_shared) or from memory when patched. The overlay is built at the first ask for any map file and kept for the run: the game holds
// header offsets, so pieces that change while it runs apply at the next start (map_overlay_changed).
//
// Adding: a table that grows is copied whole to the end of the header with its new entries and its count and offset
// words repointed (the game's copy stays in place, unused). The three texture tables grow together: an add to
// "texture" adds the same entry to texture2 and texture3. The navmesh grids and their cell tables don't grow.
//
// Collision: a region's collision is map/mapcoli/<name>.wihkar, a '1RAS' archive whose first entry is a Havok
// tagfile (sections SDKV, DATA, TYPE, INDX: the data as a memory image, its items (type, offset, count) and patch
// lists (the data offsets of pointer fields, which hold item numbers)). A mod that adds land adds static bodies: its
// pieces name them with
//   made-from mapcoli/<name> <size> <xxh64>                the game's collision file they were made against
//   bodies mapcoli/<name> <file> <size>                    the bodies the mod adds to it
// <file> (written by tools/map_pieces.py from the mod's whole collision file): 'XCXB', 1, then u32s: the body array's
// item, its element size, the bodies' count, the new items' count, the pointer fields' count, the tail's size; then
// the body records, the new items (type | flags << 24, region, offset, count), the pointer fields (patch type,
// region, offset, item: bit 31 set for one of the file's new items), and the tail (the bodies' shapes, which link
// inside themselves only). Region 0 is the records, 1 the tail. The file is built in memory at the first ask for it:
// the body array grows by every used mod's bodies, top mod first, and each mod's tail goes to the end of the data.
// Bodies never clash, so a mod keeps all of them; a collision file no used mod adds bodies to isn't claimed (a mod's
// whole copy of it loads as before).
//
// Known and not known: every chunk pointer of the game's map header (16,101 of them) lies in one of the tables below,
// found by checking each (offset, size) pair against the data file's chunks. What the tables named by their header
// words (t10 ... t49) hold is not known; their entries are changed like any other. The third texture table differs
// from the first two in a few entries; which copy the game reads when is not traced, so a mod changes each it means
// to. The game reaches the four map files only by path, through the archive's declined list (romfs_mods.c,
// romfs_mod_declines), as for any mod file.
#include "map_overlay.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "fs.h"
#include "host.h"
#include "mod_zip.h"
#include "platform/platform.h"
#include "romfs_mods.h"

#define MAP "ma9990"
#define PIECES_DIR "romfs/map/" MAP ".pieces"
#define NH 65536                    // open served files (romfs.c allows as many)
#define MAP_MODS_MAX 32             // enabled mods looked at (romfs_mods.c reads at most 32)
#define MAX_HEADER (64u << 20)
#define DATA_ALIGN 4096u
#define MAX_LINES 256               // Mods tab lines; more are counted in a last line
#define MAX_COLI 8                  // collision files served (and named by one mod)
#define MAX_COLI_FILE (256u << 20)  // a collision file, the game's or built
#define COLI_NAME 33                // a collision file's name: 1 to 32 of [a-z0-9_]
#define COLI_HEAD 32                // a bodies file's header

enum { F_MAP, F_NAV, NFILE };
static const char *const hd_name[NFILE] = {"wismhd", "winvhe"};
static const char *const hd_key[NFILE] = {"map/" MAP ".wismhd", "map/" MAP ".winvhe"};
static const char *const da_key[NFILE] = {"map/" MAP ".wismda", "map/" MAP ".winvda"};

// ---- the header tables
// K_WORDS: count at header word a, offset at word b. K_DESC: descriptor a of the list whose offset is header word b
// (16 bytes each: index, tag, count, offset). K_CELLS: the cell table of navmesh grid a (grid entry word 9 offset,
// word 10 count), u16 per cell (1-based tile entry, 0 = no nav).
enum { K_WORDS, K_DESC, K_CELLS };
typedef struct { const char *name; uint8_t file, kind, grows; int8_t link; uint16_t esize, a, b; } Table;
static const Table tables[] = {
    {"terrain", F_MAP, K_WORDS, 1, -1, 76, 6, 7},      // terrain sets; chunk at +52
    {"group", F_MAP, K_WORDS, 1, -1, 64, 8, 9},        // prop model groups; chunk at +52
    {"t10", F_MAP, K_WORDS, 1, -1, 88, 10, 11},
    {"t12", F_MAP, K_WORDS, 1, -1, 60, 12, 13},        // chunk at +52
    {"propvd", F_MAP, K_WORDS, 1, -1, 8, 20, 21},      // prop vertex data (offset, size); shared map-wide
    {"texture", F_MAP, K_WORDS, 1, -1, 20, 22, 23},    // global textures: mid chunk, mid size, full chunk, full size, flags
    {"t25", F_MAP, K_WORDS, 1, -1, 60, 25, 26},        // chunk at +52
    {"placement", F_MAP, K_WORDS, 1, -1, 8, 27, 28},   // prop placement chunks
    {"t29", F_MAP, K_WORDS, 1, -1, 8, 29, 30},
    {"light", F_MAP, K_WORDS, 1, -1, 56, 34, 35},      // lighting areas; chunk at +24
    {"t36", F_MAP, K_WORDS, 1, -1, 8, 36, 37},
    {"t44", F_MAP, K_WORDS, 1, -1, 8, 44, 45},
    {"t46", F_MAP, K_WORDS, 1, -1, 72, 46, 47},        // chunk at +40
    {"t49", F_MAP, K_WORDS, 1, -1, 8, 49, 50},
    {"terrainvd", F_MAP, K_WORDS, 1, -1, 8, 51, 52},   // terrain vertex data
    {"texture2", F_MAP, K_DESC, 0, 5, 20, 0, 56},      // texture table copies 2 and 3 (copy 3 differs in places)
    {"texture3", F_MAP, K_DESC, 0, 5, 20, 1, 56},
    {"tile", F_NAV, K_WORDS, 1, -1, 16, 3, 2},         // nav tiles: 3, data offset, length, second part offset
    {"grid", F_NAV, K_WORDS, 0, -1, 64, 5, 4},
    {"cells0", F_NAV, K_CELLS, 0, -1, 2, 0, 0},
    {"cells1", F_NAV, K_CELLS, 0, -1, 2, 1, 0},
    {"cells2", F_NAV, K_CELLS, 0, -1, 2, 2, 0},
};
#define NTABLE ((int)(sizeof tables / sizeof tables[0]))
#define GRID_TABLE 18

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static int table_named(const char *name, int file) {
    for (int t = 0; t < NTABLE; t++)
        if (tables[t].file == file && !strcmp(tables[t].name, name)) return t;
    return -1;
}

// Where the table's count and offset words are in header h (n bytes); 0 when they are outside it.
static int table_words(const uint8_t *h, uint64_t n, int t, uint64_t *cw, uint64_t *ow) {
    const Table *T = &tables[t];
    if (T->kind == K_WORDS) {
        *cw = 4ull * T->a;
        *ow = 4ull * T->b;
    } else if (T->kind == K_DESC) {
        if (4ull * T->b + 4 > n) return 0;
        uint64_t d = rd32(h + 4ull * T->b) + 16ull * T->a;
        *cw = d + 8;
        *ow = d + 12;
    } else {
        uint64_t gc, go;
        if (!table_words(h, n, GRID_TABLE, &gc, &go) || go + 4 > n || gc + 4 > n) return 0;
        if (T->a >= rd32(h + gc)) return 0;
        uint64_t g = rd32(h + go) + 64ull * T->a;
        *cw = g + 40;
        *ow = g + 36;
    }
    return *cw + 4 <= n && *ow + 4 <= n;
}

// The table's entry count and offset; 0 when it doesn't fit in the header.
static int table_at(const uint8_t *h, uint64_t n, int t, uint32_t *count, uint32_t *off) {
    uint64_t cw, ow;
    if (!table_words(h, n, t, &cw, &ow)) return 0;
    *count = rd32(h + cw);
    *off = rd32(h + ow);
    return (uint64_t)*off + (uint64_t)*count * tables[t].esize <= n;
}

// ---- XXH64, seed 0 (the archive's path hash, and the made-from check), after the xxHash specification
#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull
static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
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

// ---- the game's copy, from its archive: sts.arh holds 'arh2', the entry count, the alignment and a 0, then per entry
// the XXH64 of its path ("/map/ma9990.wismhd"), its stored size and its unpacked size (0: stored as is); the entries'
// data follows each other in sts.ard, each at a multiple of the alignment. A packed entry is an xbc1 chunk of type 3
// (zstd).
static uint64_t ard_h;            // a RomFS handle of sts.ard, kept open for the data reads

static uint32_t rom_handle(const char *name, uint64_t *h) {
    char path[256];
    snprintf(path, sizeof path, "%s:/%s", romfs_mount_name(), name);
    return romfs_open(h, path, FS_OPEN_READ);
}

static uint8_t *rom_read_all(const char *name, uint64_t *n) {
    uint64_t h;
    if (rom_handle(name, &h)) return NULL;
    int64_t size = 0;
    uint8_t *p = NULL;
    uint64_t got = 0;
    if (!romfs_size(&size, h) && size > 0 && size < (1ll << 31) && (p = malloc((size_t)size)) &&
        (romfs_read(&got, h, 0, p, (uint64_t)size) || got != (uint64_t)size)) {
        free(p);
        p = NULL;
    }
    romfs_close(h);
    if (p) *n = (uint64_t)size;
    return p;
}

// Where key's stored bytes are in sts.ard and how many; 0 when the archive has no such entry.
static int arh_find(const uint8_t *arh, uint64_t n, const char *key, uint64_t *at, uint32_t *stored, uint32_t *unpacked) {
    if (n < 16 || memcmp(arh, "arh2", 4)) return 0;
    uint32_t count = rd32(arh + 4), align = rd32(arh + 8);
    if (!align || (align & (align - 1)) || 16 + 16ull * count > n) return 0;
    char path[128];
    int len = snprintf(path, sizeof path, "/%s", key);
    uint64_t want = xxh64((const uint8_t *)path, (size_t)len), off = 0;
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *e = arh + 16 + 16ull * k;
        uint32_t s = rd32(e + 8);
        if (rd64(e) == want) {
            *at = off;
            *stored = s;
            *unpacked = rd32(e + 12);
            return 1;
        }
        off += (s + (uint64_t)align - 1) & ~(uint64_t)(align - 1);
    }
    return 0;
}

static uint8_t *ard_read(uint64_t at, uint64_t n) {
    uint8_t *p = malloc(n ? n : 1);
    uint64_t got = 0;
    if (p && (romfs_read(&got, ard_h, (int64_t)at, p, n) || got != n)) {
        free(p);
        p = NULL;
    }
    return p;
}

// The archive's entry for key, unpacked when it is an xbc1 chunk; NULL on failure or past max bytes.
static uint8_t *archive_file(const uint8_t *arh, uint64_t an, const char *key, uint64_t max, uint64_t *n) {
    uint64_t at;
    uint32_t stored, un;
    if (!arh_find(arh, an, key, &at, &stored, &un) || stored > max) return NULL;
    uint8_t *p = ard_read(at, stored);
    if (!p || stored < 48 || memcmp(p, "xbc1", 4)) {   // stored as is
        *n = stored;
        return p;
    }
    uint32_t type = rd32(p + 4), size = rd32(p + 8), packed = rd32(p + 12);
    uint8_t *out = type == 3 && packed <= stored - 48 && size <= max ? malloc(size ? size : 1) : NULL;
    size_t r = out ? ZSTD_decompress(out, size, p + 48, packed) : 0;
    free(p);
    if (!out || ZSTD_isError(r) || r != size) {
        free(out);
        return NULL;
    }
    *n = size;
    return out;
}

// The game's header for file f, unpacked; NULL on failure.
static uint8_t *game_header(const uint8_t *arh, uint64_t an, int f, uint64_t *n) {
    return archive_file(arh, an, hd_key[f], MAX_HEADER, n);
}

// ---- pieces
enum { OP_SET, OP_CHUNK, OP_FIX };
typedef struct {
    uint8_t op, file, width, ref;     // ref: the value is the index of an entry (vtable, ventry) plus bias
    int16_t table, vtable;
    int64_t entry, ventry;            // >= 0 a game number; < 0: added name -1 - k of the mod
    uint32_t at, line;
    uint64_t value, size;             // set's value (or bias); chunk's size word; fix's recorded number
    int path;                         // chunk, fix: the mod's file
    int kept;
} Op;
typedef struct { int16_t table; char name[48]; uint32_t index; } Added;
// A file a mod's lines name: where it is, what check_files found, and how it is served.
typedef struct {
    char name[200];                   // as pieces.txt names it
    char host[1400];                  // found (case-insensitively, as romfs_mods.c finds mod files)
    PlatFile *f;                      // kept open (plat_file_open_shared) while the mod's pieces are used
    uint64_t len, un;                 // its length; an xbc1 chunk's unpacked size (0: not an xbc1 chunk)
    int64_t time;                     // its last write, for map_overlay_changed
    uint8_t xbc1, zlib, need_fix;     // an xbc1 chunk; of type 1 (zlib); its fix lines change it
    uint8_t *mem;                     // the file with its fix lines applied (need_fix), served from memory
    uint64_t mem_n;
    uint64_t placed[NFILE];           // offset + 1 in each data file (0: not placed)
} Piece;
// A collision file a mod adds bodies to (see "Collision"): its lines and its bodies file, read whole by check_files.
typedef struct {
    char name[COLI_NAME];             // map/mapcoli/<name>.wihkar
    uint8_t made, has;                // a made-from line; a bodies line
    uint64_t made_size, made_hash, want;
    int path;                         // the bodies file
    uint32_t line;
    uint8_t *raw;                     // the bodies file
    uint32_t array, esize, count, nitems, nptrs, tail;
    const uint8_t *recs, *items, *ptrs, *tailp;
} ColiUse;
typedef struct {
    char id[65];
    char dir[1100];                   // its pieces folder
    ColiUse coli[MAX_COLI];
    int ncoli;
    Op *ops;
    int nops, ops_cap;
    Added *adds;
    int nadds, adds_cap;
    Piece *paths;
    int npaths, paths_cap;
    int64_t pieces_size, pieces_time; // of pieces.txt, for map_overlay_changed
    int kept, lost;                   // entries
    int used;                         // its pieces are in the overlay
    char why[240];                    // left out: why
} Mod;

// A segment of a served data file after the game's bytes.
typedef struct { uint64_t off, len; PlatFile *f; const uint8_t *mem; } Seg;
typedef struct {
    uint8_t *hd;
    uint64_t hd_n, game_n, game_at;   // game data size and its place in sts.ard
    Seg *seg;
    int nseg, seg_cap;
    uint64_t da_n;
    uint64_t hd_hash;
} Served;

// A collision file the used mods add bodies to: the game's copy (where its parts are) and the one served.
typedef struct {
    char name[COLI_NAME];
    uint8_t *game;
    uint64_t game_n, game_hash;
    int bad;                          // the game's copy is missing or not a layout this file reads
    uint32_t nent, ent_off[64], ent_size[64];   // the 1RAS archive's entries; entry 0 is the tagfile
    uint32_t top_flags, data_flags, indx_flags, item_flags, ptch_flags;
    uint64_t sdkv_at, sdkv_n, data_at, data_n, type_at, type_n, item_at, item_n, ptch_at, ptch_n;   // in game
    uint32_t array, esize;            // the body array (an item) and its element size, as the first mod says
    char first[65];                   // that mod
    uint64_t grow;                    // what the kept mods add, for the size limit
    uint8_t *out;
    uint64_t out_n, out_hash;
    int bodies, mods;
} Coli;

static PlatMutex lock = PLAT_MUTEX_INIT;
static volatile int state;            // 0 not built, 1 on, -1 off
static int map_on;                    // the map files are served (some used mod changes the map)
static Served served[NFILE];
static Coli colis[MAX_COLI];
static int ncolis;
static Mod *mods;
static int nmods;
static struct { char *text; uint8_t left_out; } lines[MAX_LINES];
static int nlines, lines_dropped;
static struct { uint8_t used, which; } handles[NH];   // which: 0/1 header, 2/3 data
static int handles_open, handles_peak;

// A line for the log and the Mods tab; left_out: a change, a mod or a file not used (the tab shows it orange).
static void say(int left_out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int left_out, const char *fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("map pieces: %s", buf);
    if (nlines < MAX_LINES - 1 && (lines[nlines].text = strdup(buf))) lines[nlines++].left_out = (uint8_t)left_out;
    else lines_dropped++;
}
static void lines_back_to(int n) {   // forgets the lines of a build attempt that was redone
    while (nlines > n) free(lines[--nlines].text);
}

static void *grow(void *p, int *cap, int n, size_t each) {
    if (n < *cap) return p;
    int c = *cap ? *cap * 2 : 16;
    void *q = realloc(p, (size_t)c * each);
    if (q) *cap = c;
    return q;
}

// A decimal number (digits only: no sign, no hex, no octal), as tools/map_pieces.py reads them.
static int number(const char *s, uint64_t *v) {
    if (!*s || strlen(s) > 19) return 0;
    uint64_t x = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        x = x * 10 + (uint64_t)(*s - '0');
    }
    *v = x;
    return 1;
}
static int hex64(const char *s, uint64_t *v) {   // exactly 16 hex digits
    if (strlen(s) != 16) return 0;
    uint64_t x = 0;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
              : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0) return 0;
        x = x << 4 | (uint64_t)d;
    }
    *v = x;
    return 1;
}

// A file name a mod may give: plain ASCII names joined by '/', none empty, ".", "..", ending in a dot or a space, or
// a Windows device name (con, prn, aux, nul, com1-9, lpt1-9, with or without an extension), so a path means the same
// file on every system.
static int safe_path(const char *p) {
    if (!*p || *p == '/' || strlen(p) >= 200) return 0;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n <= 2 && s[0] == '.' && s[n - 1] == '.') || s[n - 1] == '.' || s[n - 1] == ' ') return 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126 || strchr(":\\*?\"<>|", s[i])) return 0;
        size_t stem = 0;
        while (stem < n && s[stem] != '.') stem++;
        char low[8] = "";
        if (stem <= 4) {
            for (size_t i = 0; i < stem; i++) low[i] = (char)tolower((unsigned char)s[i]);
            low[stem] = 0;
            static const char *const dev[] = {"con", "prn", "aux", "nul"};
            for (int i = 0; i < 4; i++)
                if (!strcmp(low, dev[i])) return 0;
            if (stem == 4 && (!strncmp(low, "com", 3) || !strncmp(low, "lpt", 3)) && low[3] >= '1' && low[3] <= '9')
                return 0;
        }
        if (!e) break;
        s = e + 1;
    }
    return 1;
}
static int safe_name(const char *s) {   // an added entry's name: [A-Za-z0-9_.-], 1 to 47 characters
    size_t n = strlen(s);
    if (!n || n >= 48) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("_.-", *s)) return 0;
    return 1;
}

// base/rel found with each name of rel matched case-insensitively when the exact name isn't there (romfs_mods.c keys
// mod files by lowercase path, so a mod folder named with other case works there; this matches it on Linux too).
typedef struct { const char *want; size_t n; char found[260]; } CiFind;
static int ci_entry(const char *name, int kind, uint64_t size, void *ctx) {
    (void)kind;
    (void)size;
    CiFind *c = ctx;
    if (strlen(name) != c->n) return 1;
    for (size_t i = 0; i < c->n; i++)
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)c->want[i])) return 1;
    snprintf(c->found, sizeof c->found, "%s", name);
    return 0;
}
static int find_ci(const char *base, const char *rel, char *out, size_t cap) {
    if ((size_t)snprintf(out, cap, "%s", base) >= cap) return 0;
    for (const char *s = rel; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s), len = strlen(out);
        if (len + n + 2 > cap || n >= 260) return 0;
        out[len] = '/';
        memcpy(out + len + 1, s, n);
        out[len + 1 + n] = 0;
        if (plat_fs_kind(out) == PLAT_FS_MISSING) {
            CiFind c = {s, n, ""};
            out[len] = 0;
            plat_fs_list(out, ci_entry, &c);
            if (!c.found[0]) return 0;
            snprintf(out + len, cap - len, "/%s", c.found);
        }
        if (!e) break;
        s = e + 1;
    }
    return 1;
}

static int path_of(Mod *m, const char *p) {
    for (int i = 0; i < m->npaths; i++)
        if (!strcmp(m->paths[i].name, p)) return i;
    void *q = grow(m->paths, &m->paths_cap, m->npaths, sizeof *m->paths);
    if (!q) return -1;
    m->paths = q;
    memset(&m->paths[m->npaths], 0, sizeof m->paths[0]);
    snprintf(m->paths[m->npaths].name, sizeof m->paths[0].name, "%s", p);
    return m->npaths++;
}

static int add_of(const Mod *m, int table, const char *name) {
    if (tables[table].link >= 0) table = tables[table].link;   // texture2/3 share texture's added names
    for (int i = 0; i < m->nadds; i++)
        if (m->adds[i].table == table && !strcmp(m->adds[i].name, name)) return i;
    return -1;
}

// "<n>" or "+<name>" (an added entry of the mod's); 0 when neither.
static int entry_ref(const Mod *m, int table, const char *s, int64_t *e) {
    uint64_t v;
    if (*s == '+') {
        int k = add_of(m, table, s + 1);
        if (k < 0) return 0;
        *e = -1 - (int64_t)k;
        return 1;
    }
    if (!number(s, &v) || v > 0x7fffffff) return 0;
    *e = (int64_t)v;
    return 1;
}

static int width_of(const char *s) {
    return !strcmp(s, "u8") ? 1 : !strcmp(s, "u16") ? 2 : !strcmp(s, "u32") ? 4 : 0;
}

static int file_of(const char *s) {
    for (int f = 0; f < NFILE; f++)
        if (!strcmp(s, hd_name[f])) return f;
    return -1;
}

// The mod's use of collision file "mapcoli/<name>" (s), made on first sight; NULL for a name that isn't 1 to 32 of
// [a-z0-9_] (as the archive keys its files) or past MAX_COLI files.
static ColiUse *coli_use(Mod *m, const char *s) {
    if (strncmp(s, "mapcoli/", 8)) return NULL;
    s += 8;
    size_t n = strlen(s);
    if (!n || n >= COLI_NAME || strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_") != n) return NULL;
    for (int c = 0; c < m->ncoli; c++)
        if (!strcmp(m->coli[c].name, s)) return &m->coli[c];
    if (m->ncoli == MAX_COLI) return NULL;
    ColiUse *u = &m->coli[m->ncoli++];
    memset(u, 0, sizeof *u);
    memcpy(u->name, s, n + 1);
    return u;
}

// Splits line at spaces, tabs and carriage returns into at most cap tokens; the count, or cap + 1 when there are more.
static int tokens(char *line, char **tok, int cap) {
    int n = 0;
    for (char *s = line;;) {
        s += strspn(s, " \t\r");
        if (!*s) return n;
        if (n == cap) return cap + 1;
        tok[n++] = s;
        s += strcspn(s, " \t\r");
        if (*s) *s++ = 0;
    }
}

// Parses pieces.txt of m; 0 with m->why set on the first bad line. Every header the mod changes needs its made-from
// line, so pieces made for another game version never land on the wrong entries.
static int parse(Mod *m, char *text, const uint64_t *gn, const uint64_t *gh_hash) {
    int lineno = 0, first = 1, made[NFILE] = {0}, uses[NFILE] = {0};
    for (char *p = text, *next; p && *p; p = next) {
        next = strchr(p, '\n');
        if (next) *next++ = 0;
        lineno++;
        char *hash = strchr(p, '#');
        if (hash) *hash = 0;
        char *tok[8];
        int nt = tokens(p, tok, 8);
        if (!nt) continue;
#define BAD(...)                                                                                          \
    do {                                                                                                  \
        char w_[160];                                                                                     \
        snprintf(w_, sizeof w_, __VA_ARGS__);                                                             \
        snprintf(m->why, sizeof m->why, "pieces.txt line %d: %s", lineno, w_);                            \
        return 0;                                                                                         \
    } while (0)
        if (first) {
            if (nt != 2 || strcmp(tok[0], "xcx-map-pieces")) BAD("not \"xcx-map-pieces 1\"");
            if (strcmp(tok[1], "1")) BAD("map pieces of a newer format (version %.20s); this build reads version 1", tok[1]);
            first = 0;
            continue;
        }
        if (!strcmp(tok[0], "made-from") && nt == 4 && !strncmp(tok[1], "mapcoli/", 8)) {
            uint64_t size, h;
            ColiUse *u = coli_use(m, tok[1]);
            if (!u) BAD("collision file %.48s: a name of [a-z0-9_], and at most %d files", tok[1], MAX_COLI);
            if (!number(tok[2], &size) || !hex64(tok[3], &h)) BAD("made-from <hd> <size> <xxh64>");
            u->made = 1;
            u->made_size = size;
            u->made_hash = h;
            continue;
        }
        if (!strcmp(tok[0], "bodies")) {
            ColiUse *u = nt == 4 ? coli_use(m, tok[1]) : NULL;
            uint64_t size;
            if (!u || !safe_path(tok[2]) || !number(tok[3], &size)) BAD("bodies mapcoli/<name> <file> <size>");
            if (u->has) BAD("a second bodies line for %.48s", tok[1]);
            if ((u->path = path_of(m, tok[2])) < 0) BAD("out of memory");
            u->has = 1;
            u->want = size;
            u->line = (uint32_t)lineno;
            continue;
        }
        if (!strcmp(tok[0], "made-from")) {
            int f = nt == 4 ? file_of(tok[1]) : -1;
            uint64_t size, h;
            if (f < 0 || !number(tok[2], &size) || !hex64(tok[3], &h)) BAD("made-from <hd> <size> <xxh64>");
            if (size != gn[f] || h != gh_hash[f])
                BAD("made for another game version (its %s differs from this game's)", hd_name[f]);
            made[f] = 1;
            continue;
        }
        if (!strcmp(tok[0], "add")) {
            int f = nt == 4 ? file_of(tok[1]) : -1, t = f >= 0 ? table_named(tok[2], f) : -1;
            if (t < 0 || !tables[t].grows) BAD("add <hd> <table> <name>: no such table, or one that can't grow");
            if (!safe_name(tok[3]) || add_of(m, t, tok[3]) >= 0) BAD("added name: [A-Za-z0-9_.-], under 48, once");
            void *q = grow(m->adds, &m->adds_cap, m->nadds, sizeof *m->adds);
            if (!q) BAD("out of memory");
            m->adds = q;
            m->adds[m->nadds] = (Added){(int16_t)t, "", 0};
            snprintf(m->adds[m->nadds].name, sizeof m->adds[0].name, "%s", tok[3]);
            m->nadds++;
            uses[f] = 1;
            continue;
        }
        Op o = {0};
        o.line = (uint32_t)lineno;
        uint64_t v;
        if (!strcmp(tok[0], "set") || !strcmp(tok[0], "chunk")) {
            int chunk = tok[0][0] == 'c';
            if (!(nt == 7 || (nt == 8 && !chunk))) BAD("%s: wrong number of fields", tok[0]);
            int f = file_of(tok[1]), t = f >= 0 ? table_named(tok[2], f) : -1;
            if (t < 0) BAD("no table %.40s in %.10s", tok[2], tok[1]);
            o.op = chunk ? OP_CHUNK : OP_SET;
            o.file = (uint8_t)f;
            o.table = (int16_t)t;
            uses[f] = 1;
            if (!entry_ref(m, t, tok[3], &o.entry)) BAD("entry %.48s: not a number or an added name", tok[3]);
            if (!number(tok[4], &v) || v >= tables[t].esize) BAD("field %.20s outside the entry", tok[4]);
            o.at = (uint32_t)v;
            if (chunk) {
                if (o.at + 8 > tables[t].esize) BAD("the pair at %u doesn't fit in the entry", o.at);
                if (!safe_path(tok[5])) BAD("file %.60s: not a plain path in the pieces folder", tok[5]);
                if ((o.path = path_of(m, tok[5])) < 0) BAD("out of memory");
                if (!number(tok[6], &o.size) || o.size > 0xffffffffull) BAD("size %.20s", tok[6]);
            } else {
                o.width = (uint8_t)width_of(tok[5]);
                if (!o.width || o.at + o.width > tables[t].esize) BAD("width %.8s at %u", tok[5], o.at);
                if (tok[6][0] == '@') {
                    char *colon = strchr(tok[6] + 1, ':');
                    if (!colon) BAD("value %.60s: @<table>:<entry>", tok[6]);
                    *colon = 0;
                    int vt = table_named(tok[6] + 1, f);
                    if (vt < 0 || !entry_ref(m, vt, colon + 1, &o.ventry)) BAD("value: no entry %.48s", colon + 1);
                    o.vtable = (int16_t)vt;
                    o.ref = 1;
                    if (nt == 8 && (!number(tok[7], &o.value) || o.value > 0xffff)) BAD("bias %.20s (0 to 65535)", tok[7]);
                } else if (nt != 7 || !number(tok[6], &o.value)) {
                    BAD("value %.20s", tok[6]);
                }
                if (!o.ref && o.value >> (8 * o.width)) BAD("value %.20s too big for %s", tok[6], tok[5]);
            }
        } else if (!strcmp(tok[0], "fix")) {
            int t = -1;
            if (nt == 7)
                for (int f = 0; f < NFILE && t < 0; f++) t = table_named(tok[4], f);
            if (t < 0) BAD("fix <file> <at> <u16|u32> <table> <entry> <recorded>");
            o.op = OP_FIX;
            o.file = tables[t].file;
            o.vtable = (int16_t)t;
            o.ref = 1;
            if (!safe_path(tok[1]) || (o.path = path_of(m, tok[1])) < 0) BAD("file %.60s", tok[1]);
            o.width = (uint8_t)width_of(tok[3]);
            if (!number(tok[2], &v) || v > 0xffffffffull || o.width < 2) BAD("at %.20s / width %.8s", tok[2], tok[3]);
            o.at = (uint32_t)v;
            if (!entry_ref(m, t, tok[5], &o.ventry) || !number(tok[6], &o.size) || o.size >> (8 * o.width))
                BAD("entry %.48s / recorded %.20s", tok[5], tok[6]);
        } else {
            BAD("unknown line \"%.20s\"", tok[0]);
        }
        void *q = grow(m->ops, &m->ops_cap, m->nops, sizeof *m->ops);
        if (!q) BAD("out of memory");
        m->ops = q;
        m->ops[m->nops++] = o;
    }
    if (first) {
        snprintf(m->why, sizeof m->why, "pieces.txt is empty");
        return 0;
    }
    for (int f = 0; f < NFILE; f++)
        if (uses[f] && !made[f]) {
            snprintf(m->why, sizeof m->why, "pieces.txt has no made-from line for %s", hd_name[f]);
            return 0;
        }
    for (int c = 0; c < m->ncoli; c++)
        if (m->coli[c].has && !m->coli[c].made) {
            snprintf(m->why, sizeof m->why, "pieces.txt has no made-from line for mapcoli/%s", m->coli[c].name);
            return 0;
        }
#undef BAD
    return 1;
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// The index entry e of mod m names in table t.
static uint32_t index_of(const Mod *m, int64_t e) { return e >= 0 ? (uint32_t)e : m->adds[-1 - e].index; }

// Reads bodies file x of use u whole and checks that its parts are where its header says (see "Collision"); 0 with
// m->why set.
static int coli_bodies(Mod *m, ColiUse *u, const Piece *x) {
    uint64_t got = 0;
    u->raw = x->len <= MAX_COLI_FILE ? malloc(x->len) : NULL;
    while (u->raw && got < x->len) {
        int64_t r = plat_file_pread(x->f, u->raw + got, (uint32_t)(x->len - got), got);
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    if (!u->raw || got != x->len) {
        snprintf(m->why, sizeof m->why, "%s is too big or can't be read", x->name);
        return 0;
    }
    const uint8_t *r = u->raw;
    if (x->len < COLI_HEAD || memcmp(r, "XCXB", 4) || rd32(r + 4) != 1) {
        snprintf(m->why, sizeof m->why, "%s: not a collision bodies file (XCXB version 1)", x->name);
        return 0;
    }
    u->array = rd32(r + 8);
    u->esize = rd32(r + 12);
    u->count = rd32(r + 16);
    u->nitems = rd32(r + 20);
    u->nptrs = rd32(r + 24);
    u->tail = rd32(r + 28);
    uint64_t recs = (uint64_t)u->esize * u->count;
    if (COLI_HEAD + recs + 16ull * u->nitems + 16ull * u->nptrs + u->tail != x->len) {
        snprintf(m->why, sizeof m->why, "%s: its counts don't add up to its length", x->name);
        return 0;
    }
    if (!u->esize || !u->count || u->esize % 8 || u->esize > 4096) {
        snprintf(m->why, sizeof m->why, "%s: its element size or count is out of range", x->name);
        return 0;
    }
    u->recs = r + COLI_HEAD;
    u->items = u->recs + recs;
    u->ptrs = u->items + 16ull * u->nitems;
    u->tailp = u->ptrs + 16ull * u->nptrs;
    int bad = 0;
    for (uint32_t k = 0; k < u->nitems && !bad; k++) {   // type | flags << 24, region (0 bodies, 1 tail), offset, count
        const uint8_t *e = u->items + 16ull * k;
        bad = rd32(e + 4) > 1 || rd32(e + 8) > (rd32(e + 4) ? u->tail : recs);
    }
    for (uint32_t k = 0; k < u->nptrs && !bad; k++) {    // patch type, region, offset, item (bit 31: one of the file's)
        const uint8_t *e = u->ptrs + 16ull * k;
        uint32_t target = rd32(e + 12);
        bad = rd32(e + 4) > 1 || rd32(e + 8) + 8ull > (rd32(e + 4) ? u->tail : recs) ||
              (target >> 31 && (target & 0x7fffffffu) >= u->nitems);
    }
    if (bad) {
        snprintf(m->why, sizeof m->why, "%s: an item or pointer outside its bodies", x->name);
        return 0;
    }
    return 1;
}

// Finds, opens and checks every file the mod's lines name, before anything of the mod is used; 0 with m->why set.
// A chunk line of the map header names an xbc1 chunk whose unpacked size is the line's size; a navmesh tile file is
// at least the line's size long; a fix line names a file a chunk line names, at a place inside its (unpacked) bytes;
// a chunk that gets fixed is a zlib xbc1 chunk (the only kind this file unpacks) or a plain file.
static int check_files(Mod *m) {
    for (int p = 0; p < m->npaths; p++) {
        Piece *x = &m->paths[p];
        if (!find_ci(m->dir, x->name, x->host, sizeof x->host) || plat_fs_kind(x->host) != PLAT_FS_FILE ||
            !(x->f = plat_file_open_shared(x->host))) {
            x->host[0] = 0;   // not found: nothing for map_overlay_changed to look at
            snprintf(m->why, sizeof m->why, "%s is missing or not a file", x->name);
            return 0;
        }
        PlatFileId id;
        uint8_t head[48];
        int64_t size, time;
        if (!plat_file_id(x->f, &id) || !id.size || !plat_file_stat(x->host, &size, &time)) {
            snprintf(m->why, sizeof m->why, "%s is empty or can't be read", x->name);
            return 0;
        }
        x->len = id.size;
        x->time = time;
        if (x->len >= 48 && plat_file_pread(x->f, head, 48, 0) == 48 && !memcmp(head, "xbc1", 4)) {
            if (48ull + rd32(head + 12) > x->len) {
                snprintf(m->why, sizeof m->why, "%s: its xbc1 header says more data than the file has", x->name);
                return 0;
            }
            x->xbc1 = 1;
            x->zlib = rd32(head + 4) == 1;
            x->un = rd32(head + 8);
        }
    }
    for (int k = 0; k < m->nops; k++) {
        const Op *o = &m->ops[k];
        Piece *x = o->op == OP_SET ? NULL : &m->paths[o->path];
        const char *bad = NULL;
        if (o->op == OP_CHUNK && o->file == F_MAP && (!x->xbc1 || x->un != o->size))
            bad = "is not an xbc1 chunk of the line's unpacked size";
        else if (o->op == OP_CHUNK && o->file == F_NAV && o->size > x->len)
            bad = "is shorter than the line's size";
        else if (o->op == OP_FIX) {
            int named = 0;
            for (int q = 0; q < m->nops && !named; q++) named = m->ops[q].op == OP_CHUNK && m->ops[q].path == o->path;
            if (!named) bad = "has a fix line but no chunk line";
            else if ((uint64_t)o->at + o->width > (x->xbc1 ? x->un : x->len)) bad = "has a fix line past its end";
            else if (x->xbc1 && !x->zlib) bad = "has a fix line but is not a zlib xbc1 chunk";
        }
        if (bad) {
            snprintf(m->why, sizeof m->why, "pieces.txt line %u: %s %s", o->line, x->name, bad);
            return 0;
        }
    }
    for (int c = 0; c < m->ncoli; c++) {
        ColiUse *u = &m->coli[c];
        if (!u->has) continue;
        const Piece *x = &m->paths[u->path];
        if (x->len != u->want) {
            snprintf(m->why, sizeof m->why, "pieces.txt line %u: %s is not the line's size", u->line, x->name);
            return 0;
        }
        if (!coli_bodies(m, u, x)) return 0;
    }
    return 1;
}

static void mod_release(Mod *m) {   // closes the mod's files and drops its patched copies and bodies
    for (int p = 0; p < m->npaths; p++) {
        if (m->paths[p].f) plat_file_close(m->paths[p].f);
        free(m->paths[p].mem);
        m->paths[p].f = NULL;
        m->paths[p].mem = NULL;
    }
    for (int c = 0; c < m->ncoli; c++) {
        free(m->coli[c].raw);
        m->coli[c].raw = NULL;
    }
}

// zlib stream of stored blocks (RFC 1950/1951) of src: what the game's zlib reads like any other.
static uint8_t *zlib_stored(const uint8_t *src, size_t n, size_t *out_n) {
    size_t blocks = n / 65535 + 1, cap = 2 + n + 5 * blocks + 4;
    uint8_t *z = malloc(cap), *w = z;
    if (!z) return NULL;
    *w++ = 0x78;
    *w++ = 0x01;
    size_t at = 0;
    do {
        size_t k = n - at > 65535 ? 65535 : n - at;
        *w++ = at + k == n;
        *w++ = (uint8_t)k;
        *w++ = (uint8_t)(k >> 8);
        *w++ = (uint8_t)~k;
        *w++ = (uint8_t)(~k >> 8);
        memcpy(w, src + at, k);
        w += k;
        at += k;
    } while (at < n);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + src[i]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t ad = (b << 16) | a;
    *w++ = (uint8_t)(ad >> 24);
    *w++ = (uint8_t)(ad >> 16);
    *w++ = (uint8_t)(ad >> 8);
    *w++ = (uint8_t)ad;
    *out_n = (size_t)(w - z);
    return z;
}

// File p of mod m with its fix lines applied, into x->mem; 0 with m->why set. An xbc1 chunk keeps its 48-byte header
// but for the type and packed size (the 4-byte hash at +16 is kept as it was: the game doesn't check it, which the
// in-game runs with patched chunks show).
static int patch(Mod *m, int p) {
    Piece *x = &m->paths[p];
    uint8_t *raw = x->len < (1u << 30) ? malloc(x->len) : NULL;
    uint64_t got = 0;
    while (raw && got < x->len) {
        uint32_t c = x->len - got > (1u << 30) ? 1u << 30 : (uint32_t)(x->len - got);
        int64_t r = plat_file_pread(x->f, raw + got, c, got);
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    uint8_t *body = raw;
    if (raw && got == x->len && x->xbc1) {
        uint32_t packed = rd32(raw + 12);
        body = x->un <= (1u << 30) && packed >= 6 ? malloc(x->un ? x->un : 1) : NULL;
        if (body && !mod_inflate(raw + 50, packed - 6, body, x->un)) {   // the zlib stream: 2-byte head, Adler-32 tail
            free(body);
            body = NULL;
        }
    }
    if (!raw || got != x->len || !body) {
        if (body != raw) free(body);
        free(raw);
        snprintf(m->why, sizeof m->why, "%s can't be read or unpacked", x->name);
        return 0;
    }
    for (int i = 0; i < m->nops; i++) {
        const Op *o = &m->ops[i];
        if (o->op != OP_FIX || o->path != p) continue;
        uint32_t v = index_of(m, o->ventry);
        if (o->width == 2) memcpy(body + o->at, &(uint16_t){(uint16_t)v}, 2);
        else memcpy(body + o->at, &v, 4);
    }
    if (!x->xbc1) {
        x->mem = raw;
        x->mem_n = x->len;
        return 1;
    }
    size_t zn = 0;
    uint8_t *z = zlib_stored(body, (size_t)x->un, &zn);
    free(body);
    uint64_t n = align_up(48 + zn, 16);
    uint8_t *out = z ? calloc(1, n) : NULL;
    if (out) {
        memcpy(out, raw, 48);
        wr32(out + 4, 1);
        wr32(out + 12, (uint32_t)zn);
        memcpy(out + 48, z, zn);
        x->mem = out;
        x->mem_n = n;
    }
    free(z);
    free(raw);
    if (!out) snprintf(m->why, sizeof m->why, "out of memory patching %s", x->name);
    return out != NULL;
}

// An op on an entry: the ownership pass sorts these (plan).
typedef struct { int16_t table; int64_t entry; int mod, op; } Key;
static int key_order(const void *pa, const void *pb) {
    const Key *a = pa, *b = pb;
    if (a->table != b->table) return a->table - b->table;
    if (a->entry != b->entry) return a->entry < b->entry ? -1 : 1;
    if (a->mod != b->mod) return a->mod - b->mod;
    return a->op - b->op;
}

// Tables as the game's headers have them, and with the used mods' added entries.
typedef struct { uint32_t count[NTABLE], off[NTABLE], extra[NTABLE]; uint64_t hd_n[NFILE], da_n[NFILE]; } Plan;

enum { PLAN_OK = -1, PLAN_NOMEM = -2 };
// One attempt at the overlay from the used mods: numbers the added entries, decides each entry's owner, checks that
// every number fits where it goes, and places and patches the pieces. PLAN_OK, PLAN_NOMEM, or the index of a mod to
// leave out (its why set) before trying again: one mod that can't be used never takes the others with it.
static int plan(uint8_t *const *gh, const uint64_t *gn, const uint64_t *game_da, Plan *pl) {
    memset(pl, 0, sizeof *pl);
    for (int t = 0; t < NTABLE; t++)
        if (!table_at(gh[tables[t].file], gn[tables[t].file], t, &pl->count[t], &pl->off[t]))
            pl->count[t] = pl->off[t] = UINT32_MAX;   // not in this header: nothing may use it
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        m->kept = m->lost = 0;
        for (int k = 0; k < m->nops; k++) m->ops[k].kept = 0;
        for (int p = 0; p < m->npaths; p++) {
            free(m->paths[p].mem);
            m->paths[p].mem = NULL;
            memset(m->paths[p].placed, 0, sizeof m->paths[p].placed);
            m->paths[p].need_fix = 0;
        }
        if (!m->used) continue;
        for (int k = 0; k < m->nadds; k++) {
            int t = m->adds[k].table;
            int bad = pl->count[t] == UINT32_MAX;
            for (int u = 0; u < NTABLE; u++)   // texture2/3 grow with texture: their counts must be the same
                bad |= tables[u].link == t && pl->count[u] != pl->count[t];
            if (bad) {
                snprintf(m->why, sizeof m->why, "adds to %s, which this game's header can't grow", tables[t].name);
                return i;
            }
            m->adds[k].index = pl->count[t] + pl->extra[t]++;
        }
        for (int f = 0; f < NFILE; f++) {   // the headers stay under MAX_HEADER, mod by mod
            uint64_t n = gn[f];
            for (int t = 0; t < NTABLE; t++) {
                uint32_t extra = tables[t].link >= 0 ? pl->extra[tables[t].link] : pl->extra[t];
                if (tables[t].file == f && extra) n = align_up(n, 16) + (uint64_t)(pl->count[t] + extra) * tables[t].esize;
            }
            if (n > MAX_HEADER) {
                snprintf(m->why, sizeof m->why, "its added entries would make the %s header too big", hd_name[f]);
                return i;
            }
            pl->hd_n[f] = n;
        }
    }
    for (int t = 0; t < NTABLE; t++)
        if (tables[t].link >= 0) pl->extra[t] = pl->extra[tables[t].link];
    // entries: each of the game's belongs to the top mod that changes it. One sorted pass over (table, entry, mod, op):
    // the first mod of each game entry's run owns it; a mod's added entries are its own (their run is per mod)
    Key *keys = NULL;
    int nkeys = 0, keys_cap = 0;
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        if (!m->used) continue;
        for (int k = 0; k < m->nops; k++) {
            Op *o = &m->ops[k];
            if (o->op == OP_FIX) continue;
            if ((o->entry >= 0 && (uint64_t)o->entry >= pl->count[o->table]) || pl->count[o->table] == UINT32_MAX ||
                (o->ref && o->ventry >= 0 && (uint64_t)o->ventry >= pl->count[o->vtable])) {
                snprintf(m->why, sizeof m->why, "pieces.txt line %u: no such entry in this game's %s", o->line,
                         tables[o->ref && o->ventry >= 0 ? o->vtable : o->table].name);
                free(keys);
                return i;
            }
            void *q = grow(keys, &keys_cap, nkeys, sizeof *keys);
            if (!q) {
                free(keys);
                return PLAN_NOMEM;
            }
            keys = q;
            keys[nkeys++] = (Key){o->table, o->entry, i, k};
        }
    }
    if (nkeys) qsort(keys, (size_t)nkeys, sizeof *keys, key_order);
    for (int a = 0, b; a < nkeys; a = b) {
        int owner = keys[a].mod;
        for (b = a; b < nkeys && keys[b].table == keys[a].table && keys[b].entry == keys[a].entry &&
                    (keys[a].entry >= 0 || keys[b].mod == owner);
             b++) {
            Mod *m = &mods[keys[b].mod];
            if (keys[b].mod == owner) {
                m->ops[keys[b].op].kept = 1;
                if (b == a) m->kept++;
            } else if (keys[b - 1].mod != keys[b].mod) {   // one line per mod and entry
                m->lost++;
                say(1, "%s: %s %lld is %s's (a higher mod); %s's change to it is left out", m->id,
                    tables[keys[b].table].name, (long long)keys[b].entry, mods[owner].id, m->id);
            }
        }
    }
    free(keys);
    // every number that names an entry fits its field (0xffff, 0xff: "none" in the game's u16 and u8 lists), and the
    // pieces are placed after the game's data, each data file under 4 GB
    for (int f = 0; f < NFILE; f++) pl->da_n[f] = game_da[f];
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        if (!m->used) continue;
        for (int k = 0; k < m->nops; k++) {
            const Op *o = &m->ops[k];
            if (!o->ref || (o->op == OP_SET && !o->kept)) continue;
            if (o->ventry >= 0 && (uint64_t)o->ventry >= pl->count[o->vtable]) {
                snprintf(m->why, sizeof m->why, "pieces.txt line %u: no such entry in this game's %s", o->line,
                         tables[o->vtable].name);
                return i;
            }
            uint64_t v = (uint64_t)index_of(m, o->ventry) + (o->op == OP_SET ? o->value : 0);
            if (v >= (o->width == 4 ? 0xffffffffull : (1ull << (8 * o->width)) - 1)) {
                snprintf(m->why, sizeof m->why, "pieces.txt line %u: entry number %llu doesn't fit in %u bytes", o->line,
                         (unsigned long long)v, o->width);
                return i;
            }
        }
        for (int k = 0; k < m->nops; k++) {
            const Op *o = &m->ops[k];
            if (o->op != OP_FIX || index_of(m, o->ventry) == (uint32_t)o->size) continue;
            m->paths[o->path].need_fix = 1;
        }
        for (int k = 0; k < m->nops; k++) {
            const Op *o = &m->ops[k];
            Piece *x = &m->paths[o->path];
            if (o->op != OP_CHUNK || !o->kept || x->placed[o->file]) continue;
            if (x->need_fix && !x->mem && !patch(m, o->path)) return i;
            uint64_t at = align_up(pl->da_n[o->file], DATA_ALIGN), len = x->mem ? x->mem_n : x->len;
            if (at + len > 0xffffffffull) {
                snprintf(m->why, sizeof m->why, "its pieces would take the %s data past 4 GB", hd_name[o->file]);
                return i;
            }
            x->placed[o->file] = at + 1;
            pl->da_n[o->file] = at + len;
        }
    }
    return PLAN_OK;
}

static void served_free(void) {
    for (int f = 0; f < NFILE; f++) {
        free(served[f].hd);
        free(served[f].seg);
        memset(&served[f], 0, sizeof served[f]);
    }
}

// Builds the served headers and data layouts from the game's headers gh and the parsed mods (top first). 0 on a
// failure that leaves the overlay off (out of memory), with a line saying so.
static int combine(uint8_t *const *gh, const uint64_t *gn, const uint64_t *game_da) {
    Plan pl;
    int base = nlines, r;
    while ((r = plan(gh, gn, game_da, &pl)) >= 0) {
        lines_back_to(base);   // the attempt's lines go; the mod left out is said, and the next attempt speaks
        mods[r].used = 0;
        mod_release(&mods[r]);
        say(1, "%s: map pieces left out: %s", mods[r].id, mods[r].why);
        base = nlines;
    }
    if (r == PLAN_NOMEM) {
        say(1, "out of memory putting the map together; map pieces off");
        return 0;
    }
    int used = 0;
    for (int i = 0; i < nmods; i++) used += mods[i].used;
    if (!used) return 0;   // every mod was left out, each with its line
    for (int f = 0; f < NFILE; f++) {
        uint8_t *h = calloc(1, pl.hd_n[f] ? pl.hd_n[f] : gn[f]);
        if (!h) {
            served_free();
            say(1, "out of memory putting the map together; map pieces off");
            return 0;
        }
        memcpy(h, gh[f], gn[f]);
        uint64_t end = gn[f];
        for (int t = 0; t < NTABLE; t++) {   // each table that grows copied to the end with its new entries
            if (tables[t].file != f || !pl.extra[t]) continue;
            uint64_t cw, ow, at = align_up(end, 16);
            table_words(h, gn[f], t, &cw, &ow);   // plan checked that the table is there
            memcpy(h + at, gh[f] + pl.off[t], (size_t)pl.count[t] * tables[t].esize);
            wr32(h + cw, pl.count[t] + pl.extra[t]);
            wr32(h + ow, (uint32_t)at);
            pl.off[t] = (uint32_t)at;
            end = at + (uint64_t)(pl.count[t] + pl.extra[t]) * tables[t].esize;
        }
        served[f] = (Served){.hd = h, .hd_n = end, .game_n = game_da[f], .da_n = game_da[f]};
    }
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        if (!m->used) continue;
        for (int k = 0; k < m->nops; k++) {
            Op *o = &m->ops[k];
            if (!o->kept) continue;
            uint8_t *e = served[o->file].hd + pl.off[o->table] + (uint64_t)index_of(m, o->entry) * tables[o->table].esize;
            if (o->op == OP_SET) {
                uint64_t v = o->ref ? index_of(m, o->ventry) + o->value : o->value;
                if (o->width == 1) e[o->at] = (uint8_t)v;
                else if (o->width == 2) memcpy(e + o->at, &(uint16_t){(uint16_t)v}, 2);
                else wr32(e + o->at, (uint32_t)v);
                continue;
            }
            const Piece *x = &m->paths[o->path];
            wr32(e + o->at, (uint32_t)(x->placed[o->file] - 1));
            wr32(e + o->at + 4, (uint32_t)o->size);
        }
        for (int p = 0; p < m->npaths; p++)
            for (int f = 0; f < NFILE; f++) {
                Piece *x = &m->paths[p];
                if (!x->placed[f]) continue;
                Served *s = &served[f];
                Seg *q = grow(s->seg, &s->seg_cap, s->nseg, sizeof *q);
                if (!q) {
                    served_free();
                    say(1, "out of memory putting the map together; map pieces off");
                    return 0;
                }
                s->seg = q;
                uint64_t len = x->mem ? x->mem_n : x->len;
                s->seg[s->nseg++] = (Seg){x->placed[f] - 1, len, x->mem ? NULL : x->f, x->mem};
                if (x->placed[f] - 1 + len > s->da_n) s->da_n = x->placed[f] - 1 + len;
                if (x->mem)
                    rt_log("map pieces: %s: %s served with its numbers fixed (%llu bytes, xxh64 %016llx)", m->id,
                           x->name, (unsigned long long)len, (unsigned long long)xxh64(x->mem, (size_t)len));
            }
    }
    for (int f = 0; f < NFILE; f++) {   // pieces in place order, for the reads' binary search
        Served *s = &served[f];
        for (int a = 1; a < s->nseg; a++)
            for (int b = a; b > 0 && s->seg[b - 1].off > s->seg[b].off; b--) {
                Seg t = s->seg[b];
                s->seg[b] = s->seg[b - 1];
                s->seg[b - 1] = t;
            }
    }
    return 1;
}

// ---- collision (see "Collision" at the top)
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// A tagfile section at o of b (n bytes): its tag, whole size (header included) and flags; 0 when it doesn't fit.
static int section(const uint8_t *b, uint64_t n, uint64_t o, const char *tag, uint64_t *size, uint32_t *flags) {
    if (o + 8 > n || memcmp(b + o + 4, tag, 4)) return 0;
    uint32_t w = be32(b + o);
    *size = w & 0x3fffffffu;
    *flags = w >> 30;
    return *size >= 8 && o + *size <= n;
}

// Where the parts of the game's collision file c->game are: a '1RAS' archive (its size at +4, entry count at +12,
// 64-byte entries at 160: offset, size, ...) whose first entry is a Havok tagfile of exactly SDKV, DATA, TYPE and
// INDX (ITEM, PTCH). 0 for any other layout.
static int coli_view(Coli *c) {
    const uint8_t *b = c->game;
    uint64_t n = c->game_n, s;
    uint32_t fl;
    if (n < 160 || memcmp(b, "1RAS", 4)) return 0;
    c->nent = rd32(b + 12);
    if (!c->nent || c->nent > 64 || 160 + 64ull * c->nent > n) return 0;
    for (uint32_t i = 0; i < c->nent; i++) {
        c->ent_off[i] = rd32(b + 160 + 64ull * i);
        c->ent_size[i] = rd32(b + 164 + 64ull * i);
        if ((uint64_t)c->ent_off[i] + c->ent_size[i] > n || c->ent_off[i] < 160 + 64ull * c->nent) return 0;
    }
    uint64_t t = c->ent_off[0], tn, o;
    if (!section(b, t + c->ent_size[0], t, "TAG0", &tn, &c->top_flags)) return 0;
    uint64_t end = t + tn;
    o = t + 8;
    if (!section(b, end, o, "SDKV", &c->sdkv_n, &fl)) return 0;
    c->sdkv_at = o;
    o += c->sdkv_n;
    if (!section(b, end, o, "DATA", &s, &c->data_flags)) return 0;
    c->data_at = o + 8;
    c->data_n = s - 8;
    o += s;
    if (!section(b, end, o, "TYPE", &c->type_n, &fl)) return 0;
    c->type_at = o;
    o += c->type_n;
    uint64_t in;
    if (!section(b, end, o, "INDX", &in, &c->indx_flags) || o + in != end) return 0;
    uint64_t io = o + 8;
    if (!section(b, o + in, io, "ITEM", &s, &c->item_flags) || (s - 8) % 12) return 0;
    c->item_at = io + 8;
    c->item_n = s - 8;
    io += s;
    if (!section(b, o + in, io, "PTCH", &s, &c->ptch_flags) || io + s != o + in) return 0;
    c->ptch_at = io + 8;
    c->ptch_n = s - 8;
    for (uint64_t p = 0; p + 8 <= c->ptch_n;) {   // every patch list inside PTCH
        uint64_t cnt = rd32(b + c->ptch_at + p + 4);
        if (p + 8 + 4 * cnt > c->ptch_n) return 0;
        p += 8 + 4 * cnt;
    }
    return c->item_n >= 12;
}

static Coli *coli_named(const char *name) {
    for (int c = 0; c < ncolis; c++)
        if (!strcmp(colis[c].name, name)) return &colis[c];
    return NULL;
}

// The game's copy of every collision file the used mods add bodies to, then each mod's bodies checked against it in
// priority order; a mod whose bodies don't fit is left out (all its pieces) with a line saying why.
static void coli_prepare(const uint8_t *arh, uint64_t an) {
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        char why[240] = "";
        for (int u = 0; m->used && u < m->ncoli && !why[0]; u++) {
            if (!m->coli[u].has || coli_named(m->coli[u].name)) continue;
            if (ncolis == MAX_COLI) {
                snprintf(why, sizeof why, "more than %d collision files in all", MAX_COLI);
                break;
            }
            Coli *c = &colis[ncolis++];
            memset(c, 0, sizeof *c);
            snprintf(c->name, sizeof c->name, "%s", m->coli[u].name);
            char key[80];
            snprintf(key, sizeof key, "map/mapcoli/%s.wihkar", c->name);
            c->game = archive_file(arh, an, key, MAX_COLI_FILE, &c->game_n);
            c->bad = !c->game || !coli_view(c);
            if (!c->bad) c->game_hash = xxh64(c->game, (size_t)c->game_n);
        }
        if (why[0]) {
            m->used = 0;
            mod_release(m);
            say(1, "%s: map pieces left out: %s", m->id, why);
        }
    }
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        if (!m->used) continue;
        char why[240] = "";
        uint64_t add[MAX_COLI] = {0};
        for (int k = 0; k < m->ncoli && !why[0]; k++) {
            const ColiUse *u = &m->coli[k];
            if (!u->has) continue;
            Coli *c = coli_named(u->name);
            const char *file = m->paths[u->path].name;
            if (c->bad) {
                snprintf(why, sizeof why, "this game has no map/mapcoli/%s.wihkar it can read", u->name);
                break;
            }
            if (u->made_size != c->game_n || u->made_hash != c->game_hash) {
                snprintf(why, sizeof why, "made for another game version (its mapcoli/%s differs from this game's)",
                         u->name);
                break;
            }
            uint32_t ng = (uint32_t)(c->item_n / 12);
            const uint8_t *it = c->game + c->item_at + 12ull * u->array;
            int fits = u->array && u->array < ng &&
                       (uint64_t)rd32(it + 4) + (uint64_t)u->esize * rd32(it + 8) <= c->data_n;
            for (uint32_t p = 0; p < u->nptrs && fits; p++) {
                uint32_t target = rd32(u->ptrs + 16ull * p + 12);
                fits = target >> 31 || target < ng;
            }
            if (!fits) {
                snprintf(why, sizeof why, "%s: its bodies don't fit this game's mapcoli/%s", file, u->name);
                break;
            }
            if (c->first[0] && (u->array != c->array || u->esize != c->esize)) {
                snprintf(why, sizeof why, "%s: its bodies are for another body array than %s's", file, c->first);
                break;
            }
            add[c - colis] = (uint64_t)u->esize * u->count + u->tail + 16 + 12ull * u->nitems + 4ull * u->nptrs + 8;
            if (c->game_n + c->grow + add[c - colis] + 4096 > MAX_COLI_FILE) {
                snprintf(why, sizeof why, "its collision bodies would make mapcoli/%s too big", u->name);
                break;
            }
        }
        if (why[0]) {
            m->used = 0;
            mod_release(m);
            say(1, "%s: map pieces left out: %s", m->id, why);
            continue;
        }
        for (int k = 0; k < m->ncoli; k++) {
            if (!m->coli[k].has) continue;
            Coli *c = coli_named(m->coli[k].name);
            if (!c->first[0]) {
                snprintf(c->first, sizeof c->first, "%s", m->id);
                c->array = m->coli[k].array;
                c->esize = m->coli[k].esize;
            }
            c->grow += add[c - colis];
        }
    }
}

typedef struct { uint32_t ti, n, cap; uint32_t *off; } PList;
static int u32_order(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static uint64_t align16(uint64_t v) { return (v + 15) & ~15ull; }
static uint64_t align128(uint64_t v) { return (v + 127) & ~127ull; }
static uint8_t *put_section(uint8_t *w, const char *tag, uint32_t flags, uint64_t size) {
    uint32_t v = (uint32_t)size | flags << 30;
    w[0] = (uint8_t)(v >> 24);
    w[1] = (uint8_t)(v >> 16);
    w[2] = (uint8_t)(v >> 8);
    w[3] = (uint8_t)v;
    memcpy(w + 4, tag, 4);
    return w + 8;
}

// Collision file c built from the game's copy and the used mods' bodies, top mod first: the body array grows in place
// by every mod's bodies, the game's data after it moves along (its items and pointer fields follow), each mod's tail
// goes to the end of the data (16-aligned), its items after the game's, and its pointer fields are set to the item
// numbers they now have and added to the tagfile's patch lists. The other archive entries are kept as they are. 0 when
// out of memory.
static int coli_build(Coli *c) {
    const uint8_t *g = c->game;
    uint32_t ng = (uint32_t)(c->item_n / 12);
    const uint8_t *gi = g + c->item_at;
    uint64_t boff = rd32(gi + 12ull * c->array + 4), ins = boff + (uint64_t)c->esize * rd32(gi + 12ull * c->array + 8);
    uint64_t delta = 0, nitems = ng, added = 0;
    for (int i = 0; i < nmods; i++)
        for (int k = 0; mods[i].used && k < mods[i].ncoli; k++) {
            const ColiUse *u = &mods[i].coli[k];
            if (!u->has || strcmp(u->name, c->name)) continue;
            delta += (uint64_t)u->esize * u->count;
            added += u->count;
            nitems += u->nitems;
        }
    uint64_t dn = c->data_n + delta;   // the data: the game's with the bodies inside, then each mod's tail
    for (int i = 0; i < nmods; i++)
        for (int k = 0; mods[i].used && k < mods[i].ncoli; k++) {
            const ColiUse *u = &mods[i].coli[k];
            if (u->has && !strcmp(u->name, c->name)) dn = align16(dn) + u->tail;
        }
    dn = align16(dn);
    // the patch lists: the game's in their order, then new types as the mods' pointers bring them
    int nl = 0, lcap = 0;
    PList *pl = NULL;
    int ok = 1;
    for (uint64_t p = 0; ok && p + 8 <= c->ptch_n;) {
        uint32_t ti = rd32(g + c->ptch_at + p), cnt = rd32(g + c->ptch_at + p + 4);
        PList *q = grow(pl, &lcap, nl, sizeof *pl);
        if (!(ok = q != NULL)) break;
        pl = q;
        pl[nl] = (PList){ti, 0, cnt, NULL};
        nl++;
        p += 8 + 4ull * cnt;
    }
    for (int i = 0; ok && i < nmods; i++)   // room for each list's new offsets
        for (int k = 0; ok && mods[i].used && k < mods[i].ncoli; k++) {
            const ColiUse *u = &mods[i].coli[k];
            if (!u->has || strcmp(u->name, c->name)) continue;
            for (uint32_t p = 0; ok && p < u->nptrs; p++) {
                uint32_t ti = rd32(u->ptrs + 16ull * p);
                int l = 0;
                while (l < nl && pl[l].ti != ti) l++;
                if (l == nl) {
                    PList *q = grow(pl, &lcap, nl, sizeof *pl);
                    if (!(ok = q != NULL)) break;
                    pl = q;
                    pl[nl++] = (PList){ti, 0, 0, NULL};
                }
                pl[l].cap++;
            }
        }
    for (int l = 0; ok && l < nl; l++) ok = (pl[l].off = malloc(4ull * (pl[l].cap ? pl[l].cap : 1))) != NULL;
    uint64_t ptch = 0;
    for (int l = 0; l < nl; l++) ptch += 8 + 4ull * pl[l].cap;
    uint64_t tag = 8 + c->sdkv_n + 8 + dn + c->type_n + 8 + 8 + 12 * nitems + 8 + ptch;
    uint64_t total = c->ent_off[0] + align128(tag);
    for (uint32_t e = 1; e < c->nent; e++) total += align128(c->ent_size[e]);
    total = (total + 2047) & ~2047ull;
    uint8_t *out = ok && total <= MAX_COLI_FILE ? calloc(1, total) : NULL;
    uint8_t *data = out ? malloc(dn) : NULL, *items = out ? malloc(12 * nitems) : NULL;
    if (!out || !data || !items) {
        for (int l = 0; l < nl; l++) free(pl[l].off);
        free(pl);
        free(out);
        free(data);
        free(items);
        return 0;
    }
    // the data
    memset(data, 0, dn);
    memcpy(data, g + c->data_at, ins);
    uint64_t w = ins;
    uint64_t rec_at[MAP_MODS_MAX], tail_at[MAP_MODS_MAX], first_new[MAP_MODS_MAX];
    for (int i = 0; i < nmods; i++)
        for (int k = 0; mods[i].used && k < mods[i].ncoli; k++) {
            const ColiUse *u = &mods[i].coli[k];
            if (!u->has || strcmp(u->name, c->name)) continue;
            rec_at[i] = w;
            memcpy(data + w, u->recs, (size_t)u->esize * u->count);
            w += (uint64_t)u->esize * u->count;
        }
    memcpy(data + w, g + c->data_at + ins, c->data_n - ins);
    w += c->data_n - ins;
    for (int i = 0; i < nmods; i++)
        for (int k = 0; mods[i].used && k < mods[i].ncoli; k++) {
            const ColiUse *u = &mods[i].coli[k];
            if (!u->has || strcmp(u->name, c->name)) continue;
            w = align16(w);
            tail_at[i] = w;
            memcpy(data + w, u->tailp, u->tail);
            w += u->tail;
        }
    // the items: the game's, moved past the insertion; the body array longer; then each mod's
    memcpy(items, gi, 12ull * ng);
    for (uint32_t k = 1; k < ng; k++)
        if (rd32(items + 12ull * k + 4) >= ins) wr32(items + 12ull * k + 4, (uint32_t)(rd32(items + 12ull * k + 4) + delta));
    wr32(items + 12ull * c->array + 8, (uint32_t)(rd32(items + 12ull * c->array + 8) + added));
    uint64_t ni = ng;
    for (int i = 0; i < nmods; i++)
        for (int k = 0; mods[i].used && k < mods[i].ncoli; k++) {
            const ColiUse *u = &mods[i].coli[k];
            if (!u->has || strcmp(u->name, c->name)) continue;
            first_new[i] = ni;
            for (uint32_t j = 0; j < u->nitems; j++, ni++) {
                const uint8_t *e = u->items + 16ull * j;
                wr32(items + 12 * ni, rd32(e));
                wr32(items + 12 * ni + 4, (uint32_t)((rd32(e + 4) ? tail_at[i] : rec_at[i]) + rd32(e + 8)));
                wr32(items + 12 * ni + 8, rd32(e + 12));
            }
        }
    // the pointer fields: the game's moved, the mods' set and listed
    uint64_t p = 0;
    for (int l = 0; p + 8 <= c->ptch_n; l++) {   // the game's lists are pl's first, in their order
        uint32_t cnt = rd32(g + c->ptch_at + p + 4);
        for (uint32_t j = 0; j < cnt; j++) {
            uint32_t o = rd32(g + c->ptch_at + p + 8 + 4ull * j);
            pl[l].off[pl[l].n++] = (uint32_t)(o >= ins ? o + delta : o);
        }
        p += 8 + 4ull * cnt;
    }
    for (int i = 0; i < nmods; i++)
        for (int k = 0; mods[i].used && k < mods[i].ncoli; k++) {
            const ColiUse *u = &mods[i].coli[k];
            if (!u->has || strcmp(u->name, c->name)) continue;
            for (uint32_t p = 0; p < u->nptrs; p++) {
                const uint8_t *e = u->ptrs + 16ull * p;
                uint64_t at = (rd32(e + 4) ? tail_at[i] : rec_at[i]) + rd32(e + 8);
                uint32_t target = rd32(e + 12);
                uint64_t v = target >> 31 ? first_new[i] + (target & 0x7fffffffu) : target;
                memcpy(data + at, &v, 8);
                int l = 0;
                while (pl[l].ti != rd32(e)) l++;
                pl[l].off[pl[l].n++] = (uint32_t)at;
            }
        }
    // the file: the archive's header, the tagfile, the other entries; each entry 128-aligned, the file 2048
    memcpy(out, g, c->ent_off[0]);
    uint8_t *q = out + c->ent_off[0];
    q = put_section(q, "TAG0", c->top_flags, tag);
    memcpy(q, g + c->sdkv_at, c->sdkv_n);
    q += c->sdkv_n;
    q = put_section(q, "DATA", c->data_flags, 8 + dn);
    memcpy(q, data, dn);
    q += dn;
    memcpy(q, g + c->type_at, c->type_n);
    q += c->type_n;
    q = put_section(q, "INDX", c->indx_flags, 8 + 8 + 12 * nitems + 8 + ptch);
    q = put_section(q, "ITEM", c->item_flags, 8 + 12 * nitems);
    memcpy(q, items, 12 * nitems);
    q += 12 * nitems;
    q = put_section(q, "PTCH", c->ptch_flags, 8 + ptch);
    for (int l = 0; l < nl; l++) {
        qsort(pl[l].off, pl[l].n, 4, u32_order);
        wr32(q, pl[l].ti);
        wr32(q + 4, pl[l].n);
        memcpy(q + 8, pl[l].off, 4ull * pl[l].n);
        q += 8 + 4ull * pl[l].n;
        free(pl[l].off);
    }
    free(pl);
    free(data);
    free(items);
    uint64_t pos = c->ent_off[0];
    wr32(out + 160, (uint32_t)pos);
    wr32(out + 164, (uint32_t)align128(tag));
    pos += align128(tag);
    for (uint32_t e = 1; e < c->nent; e++) {
        memcpy(out + pos, g + c->ent_off[e], c->ent_size[e]);
        wr32(out + 160 + 64ull * e, (uint32_t)pos);
        wr32(out + 164 + 64ull * e, (uint32_t)align128(c->ent_size[e]));
        pos += align128(c->ent_size[e]);
    }
    wr32(out + 4, (uint32_t)total);
    c->out = out;
    c->out_n = total;
    c->out_hash = xxh64(out, (size_t)total);
    c->bodies = (int)added;
    for (int i = 0; i < nmods; i++)
        for (int k = 0; mods[i].used && k < mods[i].ncoli; k++) c->mods += mods[i].coli[k].has && !strcmp(mods[i].coli[k].name, c->name);
    return 1;
}

static void coli_free(void) {
    for (int c = 0; c < ncolis; c++) {
        free(colis[c].game);
        free(colis[c].out);
    }
    memset(colis, 0, sizeof colis);
    ncolis = 0;
}

static void build(void) {
    const char *root = romfs_mods_dir();
    char ids[MAP_MODS_MAX][65];
    int n = root ? romfs_mods_priority(ids, MAP_MODS_MAX) : 0;
    if (n <= 0) return;   // no mods on: nothing allocated, nothing more read
    mods = calloc((size_t)n, sizeof *mods);
    if (!mods) return;
    char base[1100];
    for (int i = 0; i < n; i++) {
        Mod *m = &mods[nmods];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%s", ids[i]);
        char path[1300];
        if ((size_t)snprintf(base, sizeof base, "%s/%s", root, ids[i]) >= sizeof base ||
            !find_ci(base, PIECES_DIR, m->dir, sizeof m->dir))
            continue;
        snprintf(path, sizeof path, "%s/pieces.txt", m->dir);
        if (!plat_file_stat(path, &m->pieces_size, &m->pieces_time)) continue;
        nmods++;
    }
    if (!nmods) return;
    uint64_t an = 0;
    uint8_t *arh = rom_read_all("sts.arh", &an);
    uint8_t *gh[NFILE] = {0};
    uint64_t gn[NFILE] = {0}, da[NFILE] = {0}, da_at[NFILE] = {0};
    int ok = arh && !rom_handle("sts.ard", &ard_h);
    for (int f = 0; ok && f < NFILE; f++) {
        uint32_t stored = 0, un = 0;
        ok = (gh[f] = game_header(arh, an, f, &gn[f])) && arh_find(arh, an, da_key[f], &da_at[f], &stored, &un) && !un;
        da[f] = stored;
    }
    if (!ok) {
        free(arh);
        say(1, "the game's map files can't be read from its archive; %d mod%s' map pieces left out", nmods,
            nmods == 1 ? "" : "s");
        for (int f = 0; f < NFILE; f++) free(gh[f]);
        if (ard_h) romfs_close(ard_h);
        ard_h = 0;
        return;
    }
    uint64_t gh_hash[NFILE];   // for the made-from lines
    for (int f = 0; f < NFILE; f++) gh_hash[f] = xxh64(gh[f], (size_t)gn[f]);
    int used = 0;
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        char path[1200];
        snprintf(path, sizeof path, "%s/pieces.txt", m->dir);
        uint64_t size = 0;
        uint8_t *text = plat_file_read(path, &size);
        char *t = text ? malloc(size + 1) : NULL;
        if (t) {
            memcpy(t, text, size);
            t[size] = 0;
        }
        free(text);
        if (!t) snprintf(m->why, sizeof m->why, "pieces.txt can't be read");
        m->used = t && parse(m, t, gn, gh_hash) && check_files(m);
        free(t);
        if (!m->used) {
            mod_release(m);
            say(1, "%s: map pieces left out: %s", m->id, m->why);
        }
        used += m->used;
    }
    if (used) coli_prepare(arh, an);
    free(arh);
    int on = used && combine(gh, gn, da);
    if (on) {
        map_on = 0;   // the map files are served only when a used mod changes the map
        for (int i = 0; i < nmods; i++) map_on |= mods[i].used && (mods[i].nops || mods[i].nadds);
        if (!map_on) {   // collision only: the map stays the game's, read as usual
            served_free();
            if (ard_h) romfs_close(ard_h);
            ard_h = 0;
        }
        for (int c = 0; c < ncolis; c++) {   // and each collision file a used mod adds bodies to
            int any = 0;
            for (int i = 0; i < nmods; i++)
                for (int k = 0; mods[i].used && k < mods[i].ncoli; k++)
                    any |= mods[i].coli[k].has && !strcmp(mods[i].coli[k].name, colis[c].name);
            if (any && !colis[c].bad && !coli_build(&colis[c]))
                say(1, "out of memory putting mapcoli/%s together; its collision bodies are left out", colis[c].name);
        }
        int served_any = map_on;
        for (int c = 0; c < ncolis; c++) served_any |= colis[c].out != NULL;
        on = served_any;
    }
    if (on) {
        for (int f = 0; map_on && f < NFILE; f++) {
            served[f].game_at = da_at[f];
            served[f].hd_hash = xxh64(served[f].hd, (size_t)served[f].hd_n);
        }
        for (int i = 0; i < nmods; i++) {
            const Mod *m = &mods[i];
            if (!m->used) continue;
            int bodies = 0;
            for (int k = 0; k < m->ncoli; k++) bodies += m->coli[k].has ? (int)m->coli[k].count : 0;
            char more[64] = "";
            if (bodies) snprintf(more, sizeof more, "; %d collision bod%s", bodies, bodies == 1 ? "y" : "ies");
            say(0, "%s: %d map entr%s (%d added), %d lost to higher mods%s", m->id, m->kept,
                m->kept == 1 ? "y" : "ies", m->nadds, m->lost, more);
        }
        if (map_on)
            for (int f = 0; f < NFILE; f++)
                rt_log("map pieces: %s %llu bytes (xxh64 %016llx); data %llu + %llu bytes in %d piece%s", hd_name[f],
                       (unsigned long long)served[f].hd_n, (unsigned long long)served[f].hd_hash,
                       (unsigned long long)served[f].game_n, (unsigned long long)(served[f].da_n - served[f].game_n),
                       served[f].nseg, served[f].nseg == 1 ? "" : "s");
        for (int c = 0; c < ncolis; c++)
            if (colis[c].out)
                rt_log("map pieces: mapcoli/%s %llu bytes (xxh64 %016llx); %d bod%s from %d mod%s", colis[c].name,
                       (unsigned long long)colis[c].out_n, (unsigned long long)colis[c].out_hash, colis[c].bodies,
                       colis[c].bodies == 1 ? "y" : "ies", colis[c].mods, colis[c].mods == 1 ? "" : "s");
        // a mod's whole copy of a file served from pieces is not used beside them
        for (int i = 0; i < n; i++)
            for (int f = 0; f < 2 * NFILE + ncolis; f++) {
                char path[1300], key[80];
                if (f < 2 * NFILE && !map_on) continue;
                if (f >= 2 * NFILE && !colis[f - 2 * NFILE].out) continue;
                if (f < 2 * NFILE) snprintf(key, sizeof key, "%s", f < NFILE ? hd_key[f] : da_key[f - NFILE]);
                else snprintf(key, sizeof key, "map/mapcoli/%s.wihkar", colis[f - 2 * NFILE].name);
                char rel[96];
                snprintf(rel, sizeof rel, "romfs/%s", key);
                if ((size_t)snprintf(base, sizeof base, "%s/%s", root, ids[i]) < sizeof base &&
                    find_ci(base, rel, path, sizeof path) && plat_fs_kind(path) == PLAT_FS_FILE)
                    say(1, "%s: its whole %s is left out (map pieces are on); make it pieces with tools/map_pieces.py",
                        ids[i], key);
            }
        for (int c = 0; c < ncolis; c++) {   // the game's copies are not needed any more
            free(colis[c].game);
            colis[c].game = NULL;
        }
        for (int i = 0; i < nmods; i++)   // the bodies are in the built files
            for (int k = 0; k < mods[i].ncoli; k++) {
                free(mods[i].coli[k].raw);
                mods[i].coli[k].raw = NULL;
            }
        __atomic_store_n(&state, 1, __ATOMIC_RELEASE);
    } else {
        for (int i = 0; i < nmods; i++) mod_release(&mods[i]);
        served_free();
        coli_free();
        if (ard_h) romfs_close(ard_h);
        ard_h = 0;
    }
    for (int f = 0; f < NFILE; f++) free(gh[f]);
}

static void build_once(void) {
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&lock);
    if (!state) {
        uint64_t t0 = plat_now_ns();
        build();
        if (lines_dropped) {
            char more[80];
            snprintf(more, sizeof more, "%d more line%s in the log", lines_dropped, lines_dropped == 1 ? "" : "s");
            lines[nlines].text = strdup(more);
            lines[nlines].left_out = 0;
            if (lines[nlines].text) nlines++;
        }
        if (state != 1) {
            __atomic_store_n(&state, -1, __ATOMIC_RELEASE);
            if (nmods) rt_log("map pieces: off (no map pieces could be used)");
            else rt_log("map pieces: off (no enabled mod has map pieces)");
        } else {
            rt_log("map pieces: on, built in %.1f ms", (double)(plat_now_ns() - t0) / 1e6);
        }
    }
    plat_mutex_unlock(&lock);
}

// ---- serving
// The served file a key names: 0/1 a header, 2/3 a data file, 2 * NFILE + c collision file c; -1 for none. Before
// the build, a key that may be served gives -2.
static int which_of(const char *key) {
    if (!strncmp(key, "map/" MAP ".", sizeof "map/" MAP "." - 1)) {
        for (int f = 0; f < NFILE; f++) {
            if (!strcmp(key, hd_key[f])) return f;
            if (!strcmp(key, da_key[f])) return NFILE + f;
        }
        return -1;
    }
    size_t n = strlen(key);
    if (strncmp(key, "map/mapcoli/", 12) || n < 12 + 7 + 1 || strcmp(key + n - 7, ".wihkar")) return -1;
    if (!__atomic_load_n(&state, __ATOMIC_ACQUIRE)) return -2;
    for (int c = 0; c < ncolis; c++)
        if (colis[c].out && strlen(colis[c].name) == n - 19 && !strncmp(key + 12, colis[c].name, n - 19))
            return 2 * NFILE + c;
    return -1;
}

int map_overlay_claims(const char *key) {
    if (!key || strncmp(key, "map/", 4) || which_of(key) == -1) return 0;
    build_once();
    int w = which_of(key);
    return state == 1 && w >= 0 && (w >= 2 * NFILE || map_on);
}

// Which served file a RomFS path ("rom:/map/ma9990.wismhd") is, when the overlay serves it; else -1.
static int served_path(const char *path) {
    if (!path) return -1;
    const char *colon = strchr(path, ':');
    const char *p = colon ? colon + 1 : path;
    while (*p == '/' || *p == '\\') p++;
    // every RomFS open and entry check passes here: only a map path gets the copy below
    static const char prefix[] = "map/";
    for (size_t i = 0; i < sizeof prefix - 1; i++)
        if (tolower((unsigned char)p[i]) != prefix[i] && !(prefix[i] == '/' && p[i] == '\\')) return -1;
    char key[64];
    size_t n = strlen(p);
    if (n >= sizeof key) return -1;
    for (size_t i = 0; i <= n; i++) key[i] = p[i] == '\\' ? '/' : (char)tolower((unsigned char)p[i]);
    return map_overlay_claims(key) ? which_of(key) : -1;
}

int map_overlay_serves(const char *path) { return served_path(path) >= 0; }

int map_overlay_open(const char *path, uint64_t *out) {
    int w = out ? served_path(path) : -1;
    if (w < 0) return 0;
    plat_mutex_lock(&lock);
    for (int j = 0; j < NH; j++)
        if (!handles[j].used) {
            handles[j].used = 1;
            handles[j].which = (uint8_t)w;
            if (++handles_open > handles_peak) handles_peak = handles_open;
            plat_mutex_unlock(&lock);
            *out = MAP_OVERLAY_HANDLE_BASE + (uint64_t)j;
            return 1;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("map pieces: file handle table full (%d open)", NH);
}

int map_overlay_handle(uint64_t h) { return h >= MAP_OVERLAY_HANDLE_BASE && h < MAP_OVERLAY_HANDLE_BASE + NH; }

static int slot(uint64_t h) {
    if (!map_overlay_handle(h)) return -1;
    int j = (int)(h - MAP_OVERLAY_HANDLE_BASE);
    plat_mutex_lock_shared(&lock);
    int w = handles[j].used ? handles[j].which : -1;
    plat_mutex_unlock_shared(&lock);
    return w;
}

uint32_t map_overlay_size(int64_t *out, uint64_t h) {
    int w = slot(h);
    if (!out || w < 0) return FS_R_INVALID;
    *out = (int64_t)(w < NFILE ? served[w].hd_n : w < 2 * NFILE ? served[w - NFILE].da_n : colis[w - 2 * NFILE].out_n);
    return 0;
}

uint32_t map_overlay_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    int w = slot(h);
    if (!out || w < 0 || offset < 0 || (!buffer && size)) return FS_R_INVALID;
    if (w >= 2 * NFILE) {   // a collision file, from memory
        const Coli *c = &colis[w - 2 * NFILE];
        if ((uint64_t)offset > c->out_n) return FS_R_INVALID;
        uint64_t n = size < c->out_n - (uint64_t)offset ? size : c->out_n - (uint64_t)offset;
        memcpy(buffer, c->out + offset, (size_t)n);
        *out = n;
        return 0;
    }
    const Served *s = &served[w % NFILE];
    uint64_t total = w < NFILE ? s->hd_n : s->da_n, pos = (uint64_t)offset;
    if (pos > total) return FS_R_INVALID;
    uint64_t n = size < total - pos ? size : total - pos;
    uint8_t *b = buffer;   // memcpy into a frozen page faults and thaws it; only the kernel's write (pread) needs mem_thaw
    if (w < NFILE) {
        memcpy(b, s->hd + pos, (size_t)n);
        *out = n;
        return 0;
    }
    uint64_t done = 0;
    if (pos < s->game_n) {   // the game's bytes, read in place from the archive
        uint64_t k = n < s->game_n - pos ? n : s->game_n - pos, got = 0;
        if (romfs_read(&got, ard_h, (int64_t)(s->game_at + pos), b, k) || got != k)
            rt_fatal("map pieces: read of the game's %s failed at %llu", da_key[w - NFILE], (unsigned long long)pos);
        done = k;
    }
    while (done < n) {
        uint64_t at = pos + done;
        int lo = 0, hi = s->nseg;   // the first segment ending after at
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (s->seg[mid].off + s->seg[mid].len <= at) lo = mid + 1;
            else hi = mid;
        }
        uint64_t left = n - done;
        if (lo == s->nseg || s->seg[lo].off > at) {   // padding before the next piece
            uint64_t k = lo == s->nseg ? left : s->seg[lo].off - at;
            if (k > left) k = left;
            memset(b + done, 0, (size_t)k);
            done += k;
            continue;
        }
        const Seg *g = &s->seg[lo];
        uint64_t in = at - g->off, k = g->len - in < left ? g->len - in : left;
        if (g->mem) {
            memcpy(b + done, g->mem + in, (size_t)k);
        } else {
            mem_thaw((uint64_t)(uintptr_t)(b + done), k);
            for (uint64_t r = 0; r < k;) {
                uint32_t c = k - r > 0x40000000 ? 0x40000000 : (uint32_t)(k - r);
                int64_t got = plat_file_pread(g->f, b + done + r, c, in + r);
                if (got <= 0)
                    rt_fatal("map pieces: a mod's map piece came back short (was it rewritten while the game runs?)");
                r += (uint64_t)got;
            }
        }
        done += k;
    }
    *out = n;
    return 0;
}

void map_overlay_close(uint64_t h) {
    int ok = map_overlay_handle(h);
    plat_mutex_lock(&lock);
    if (ok) {
        ok = handles[h - MAP_OVERLAY_HANDLE_BASE].used;
        handles[h - MAP_OVERLAY_HANDLE_BASE].used = 0;
        handles_open -= ok;
    }
    plat_mutex_unlock(&lock);
    if (!ok) rt_fatal("map pieces: close of invalid file handle %#llx", (unsigned long long)h);
}

// ---- the Mods tab
int map_overlay_state(void) { return __atomic_load_n(&state, __ATOMIC_ACQUIRE); }

int map_overlay_line(int i, char *out, size_t cap, int *left_out) {
    if (!map_overlay_state() || i < 0 || i >= nlines) return 0;
    snprintf(out, cap, "%s", lines[i].text);
    if (left_out) *left_out = lines[i].left_out;
    return 1;
}

// Whether a file the overlay read still has the size and last write it had then.
static int same_file(const char *path, int64_t size, int64_t time) {
    int64_t s, t;
    return plat_file_stat(path, &s, &t) && s == size && t == time;
}

int map_overlay_changed(void) {
    int st = map_overlay_state();
    if (!st) return 0;   // not built yet: it will be built from the mods as they are
    const char *root = romfs_mods_dir();
    char ids[MAP_MODS_MAX][65];
    int n = root ? romfs_mods_priority(ids, MAP_MODS_MAX) : 0, k = 0;
    for (int i = 0; i < n; i++) {
        char base[1100], dir[1100], path[1300];
        int64_t size, time;
        if ((size_t)snprintf(base, sizeof base, "%s/%s", root, ids[i]) >= sizeof base ||
            !find_ci(base, PIECES_DIR, dir, sizeof dir))
            continue;
        snprintf(path, sizeof path, "%s/pieces.txt", dir);
        if (!plat_file_stat(path, &size, &time)) continue;
        if (k >= nmods || strcmp(mods[k].id, ids[i]) || mods[k].pieces_size != size || mods[k].pieces_time != time) return 1;
        for (int p = 0; p < mods[k].npaths; p++)   // the pieces it serves, replaced with pieces.txt left as it was
            if (mods[k].paths[p].host[0] && mods[k].paths[p].len &&
                !same_file(mods[k].paths[p].host, (int64_t)mods[k].paths[p].len, mods[k].paths[p].time))
                return 1;
        k++;
    }
    return k != nmods;
}
