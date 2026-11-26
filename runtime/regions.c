// Data-driven regions: a game-file mod's data makes a map region with hexagons, a survey rate, rewards and rows in the
// survey panels (docs/regions.md; the hooked sites are in recomp/regions.py). The sections below, in order: which
// regions are the mods' (decided once a boot) and far regions' own segment slots, the slot loops and lookups, the
// field-region test, the probe reveal, the discovery flag, the hex table, the Segments tab, the Survey Reward List, and
// the panel lists (Region Intel on three tabs, Map Shortcuts).
//
// The first piece, the hex table:
// Which map region the player is in comes from the world raster zone/zone0001.atr (one byte per 10 m cell). The
// segment manager keeps a slot for each region id 0..23 and fills it, when the world loads, with the region's hex table
// (the map's segments: the hexagons, the minimap's current-hexagon outline, the survey counts). It gets that table from
// a fixed switch in the game (main+0x3fe1f4) that knows only the regions with hexes in the base game: 2-7, 10 and 13-16.
// A mod that paints a new region into the raster (a spare id such as 20) therefore gets no hexes for it.
//
// recomp/regions.py passes the switch's answer through region_hex_table. When the game found a table, nothing changes.
// When it found none, the table named "SEG_TerminalZone<id>" (for example SEG_TerminalZone20) is looked up in the
// loaded data, by the murmur3 hash of its name as the game looks up every table (main+0x3dd6e0); if a mod shipped one,
// it is used, else the answer stays "none". The base game has no table by that name, so without such a mod nothing
// changes. XCX_REGION_TABLES=0 turns the lookup off.
#include "regions.h"
#include "cfg.h"
#include "host.h"
#include "romfs_mods.h"
#include <stdio.h>
#include <string.h>

#define MAIN_BASE 0x7000000000ull
#define TABLE_BY_HASH (MAIN_BASE + 0x3dd6e0)   // the game's table lookup: x0 = name hash -> table, 0 if none
#define REGION_SLOTS 24                       // the segment manager's own slots (main+0x3fd110 stops at 24)
// Far regions (ids 24..255) get slots of their own past the manager's (see "Region ids past the slots"): slot numbers
// 25 and up, so a slot number is below 24 or from 25 to VSLOTS - 1.
#define EXT_FIRST 25
#define FAR_MAX (256 - REGION_SLOTS)
#define VSLOTS (EXT_FIRST + FAR_MAX)
// The minimap's mode switch (main+0x87e200) jumps to main+0x87e2bc + 4 * byte; 0x40 is its field case main+0x87e3bc,
// where regions below 10 go (the continents: hexagon outline).
#define MINIMAP_FIELD_CASE 0x40
// The Segments tab's list of regions it updates and draws (main+0x7fd020): entries of 0x30 bytes at map+0xa50, the
// count at map+0xed0 right after room for 24.
#define LAYER_LIST 0xa50
#define LAYER_SIZE 0x30
#define LAYER_COUNT 0xed0
#define LAYER_ROOM ((LAYER_COUNT - LAYER_LIST) / LAYER_SIZE)

// A bit per slot that got a mod hex table at a world load, how many such slots there are, and those slots in region id
// order (the panels list mod regions by id), kept in order as they are found.
static volatile uint8_t mod_bits[(VSLOTS + 7) / 8];
static volatile uint32_t mod_regions;
static uint32_t mod_order[VSLOTS];
static uint32_t region_id(uint32_t slot);

static int is_mod(uint32_t slot) { return slot < VSLOTS && mod_bits[slot >> 3] >> (slot & 7) & 1; }

static int layout_claims(const char *key) { return region_layout_file(key, NULL, 0); }

static void set_mod(uint32_t slot) {
    if (slot >= VSLOTS || is_mod(slot)) return;
    if (!mod_regions) romfs_mods_claim_also(layout_claims);   // the panel layouts get rows from now on
    mod_bits[slot >> 3] |= (uint8_t)(1u << (slot & 7));
    uint32_t k = mod_regions;
    while (k > 0 && region_id(mod_order[k - 1]) > region_id(slot)) { mod_order[k] = mod_order[k - 1]; k--; }
    mod_order[k] = slot;
    mod_regions++;
}

// The game's table name hash: murmur3, seed 0, over the name's bytes.
static uint32_t name_hash(const char *s) {
    const uint8_t *d = (const uint8_t *)s;
    size_t n = strlen(s), i = 0;
    uint32_t h = 0, k;
    for (; i + 4 <= n; i += 4) {
        k = (uint32_t)d[i] | (uint32_t)d[i + 1] << 8 | (uint32_t)d[i + 2] << 16 | (uint32_t)d[i + 3] << 24;
        k *= 0xcc9e2d51u;
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

// ---- Region ids past the slots ("far" regions) ----
// The raster is a byte, so a region id can be up to 255, but the segment manager (main+0x3fce9c, one object of 0x37bd0
// bytes made by main+0x3fcb0c) has a slot only for ids 0..23: 24 slots of 0x2510 bytes from +0x20, the manager's own
// fields after them (+0x379a0..), and it indexes the slots by region id with no bounds check (the accessors at
// main+0x563460..0x5643a0, main+0x3fe980, main+0x3ffb10). A mod region with an id of 24 or more (its own
// FLD_FieldNameList row: name, discovery flag, weather, sound by data) and a SEG_TerminalZone<id> table gets a slot of
// its own past the manager's end, so every far region (up to 232) has one and the game's own slots stay as they are:
// - the manager is allocated bigger (region_mgr_size): room up to slot 24 + n, where n is the number of far regions;
//   slot 24 itself would overlap the manager's fields, so far region k (in id order) has slot 25 + k;
// - each place that indexes a slot by region id (region_slot, recomp/regions.py) sends a far id to its slot;
// - the manager's five loops over its slots (the init main+0x3fd110 and main+0x3feef0, 0x3ff060, 0x3ff330, 0x3ff870)
//   run from 0 to 23 and then on from 25 over the far regions' slots: at 24 their counter and the running pointers
//   they keep skip a slot (region_loop_skip) and their end test compares with the last slot (region_slot_end);
// - everything in the segment system speaks region ids, never slot numbers: the init writes the far id, not the
//   slot number, into the slot's header and records (region_loop_value), and the loops that pass their counter on as
//   a region (a debug log, an event post, their messages) do the same (region_loop_value);
// - the survey rate average over all slots (main+0x3fe658, unrolled over the 24) counts the far regions too;
// - the survey reward list sorts its rows into one list per region id 0-23 in its panel (main+0x856040): the panel
//   gets a list for each far region past its end (see "Survey Reward List pages").
// Names, rates and map places are asked for by the region id (region_id). The new slots and lists are freed only with
// the manager (never, while the game runs) and with the reward panel (region_rw_free).
#define SEG_MGR_SIZE 0x37bd0u
#define SEG_SLOT 0x2510u
#define SEG_SLOTS_AT 0x20u
#define SEG_RATE_UPDATE (MAIN_BASE + 0x3ffe50) // x0 = slot, x1 = manager + 0x379dc: the slot's survey rate
#define GAME_HEX_REGIONS 0x1e4fcu         // the regions with a hex table in the game: 2-7, 10, 13-16
static uint16_t far_vslot[256];          // far id -> its slot (0: none)
static uint8_t vslot_far[VSLOTS];        // slot -> far id (0: the slot's own region)
static uint32_t own_regions;             // slots a mod gives a hex table of their own number (SEG_TerminalZone<n>, n < 24)
static int far_ready;
static uint32_t far_count;               // far regions with a table, each with a slot number from EXT_FIRST
static uint32_t ext_room, ext_pending;   // far slots the manager has room for; asked for at its allocation
static uint64_t seg_mgr;
static uint64_t names_table;             // FLD_FieldNameList, looked up at the segment init's entry

// A far region's slot exists only if the manager was allocated with room for it.
static int live(uint32_t slot) { return slot >= EXT_FIRST && slot < EXT_FIRST + ext_room; }

static uint32_t region_id(uint32_t slot) { return slot >= EXT_FIRST && slot < VSLOTS && vslot_far[slot] ? vslot_far[slot] : slot; }

static uint32_t region_slot_of(uint32_t id) {
    if (id < REGION_SLOTS) return id;
    return id < 256 && far_vslot[id] ? far_vslot[id] : id;
}

static int table_exists(Cpu *c, const char *base, uint32_t id) {
    char name[40];
    snprintf(name, sizeof name, "%s%u", base, id);
    return rt_call(c, TABLE_BY_HASH, 1, (uint64_t)name_hash(name)) != 0;
}

// Which far regions exist: decided once a boot, where the guest state is whole, so the game's table lookup can run
// (the loaded data doesn't change): at the entry of the manager's maker (main+0x3fcb0c, region_seg_create) and of its
// init (region_far_setup). Only once the game's data is loaded (FLD_FieldNameList is found). The hooks inside functions
// only read what this decided; they never call into the game.
static void far_setup(Cpu *c) {
    if (!names_table) names_table = rt_call(c, TABLE_BY_HASH, 1, (uint64_t)0x97991FA4u);
    if (far_ready || !names_table) return;
    far_ready = 1;
    static int off = -1;
    if (off < 0) {
        const char *v = cfg_get("XCX_REGION_TABLES");
        off = v && !strcmp(v, "0");
    }
    if (off) return;
    for (uint32_t id = 0; id < REGION_SLOTS; id++)
        if (!(GAME_HEX_REGIONS & 1u << id) && table_exists(c, "SEG_TerminalZone", id)) own_regions |= 1u << id;
    for (uint32_t id = REGION_SLOTS; id < 256; id++) {
        if (!table_exists(c, "SEG_TerminalZone", id)) continue;
        uint32_t slot = EXT_FIRST + far_count++;
        far_vslot[id] = (uint16_t)slot;
        vslot_far[slot] = (uint8_t)id;
    }
    if (far_count) rt_log("regions: %u far region(s), slots %u..%u", far_count, EXT_FIRST, EXT_FIRST + far_count - 1);
}

// The game's table lookups clobber guest registers, and these run at a function's entry with its arguments live.
static void far_setup_saved(Cpu *c) {
    uint64_t x[31], sp = c->sp;
    memcpy(x, c->x, sizeof x);
    far_setup(c);
    memcpy(c->x, x, sizeof x);
    c->sp = sp;
}

// FLD_FieldNameList again, with the guest registers kept (this runs at a function's entry).
static void names_saved(Cpu *c) {
    uint64_t x[31], sp = c->sp;
    memcpy(x, c->x, sizeof x);
    names_table = rt_call(c, TABLE_BY_HASH, 1, (uint64_t)0x97991FA4u);
    memcpy(c->x, x, sizeof x);
    c->sp = sp;
}

void region_seg_create(Cpu *c) {
    if (!far_ready) far_setup_saved(c);
}

uint64_t region_mgr_size(uint64_t size) {
    if ((uint32_t)size != SEG_MGR_SIZE || !far_count) return size;
    ext_pending = far_count;
    rt_log("regions: segment manager allocated with room for %u far region slot(s)", far_count);
    return SEG_SLOTS_AT + (uint64_t)(EXT_FIRST + far_count) * SEG_SLOT;
}

// At the init's entry (x0 = the manager): the far slots of a new manager start zeroed (the allocator doesn't clear and
// the maker main+0x3fce9c clears only its own 24 slots' pointers); a later init of the same manager keeps them.
void region_far_setup(Cpu *c) {
    if (!far_ready) far_setup_saved(c);
    else names_saved(c);   // the table may have been loaded again (at the title): never keep a stale pointer
    uint64_t mgr = c->x[0];
    if (!mgr || (mgr == seg_mgr && !ext_pending)) return;   // a later init of the same manager
    seg_mgr = mgr;
    ext_room = ext_pending;
    ext_pending = 0;
    if (far_count > ext_room)
        rt_log("regions: the segment manager has room for %u of %u far region(s); the others get no hexagons", ext_room,
               far_count);
    uint64_t at = mgr + SEG_SLOTS_AT + (uint64_t)EXT_FIRST * SEG_SLOT, end = at + (uint64_t)ext_room * SEG_SLOT;
    for (; at < end; at += 8) ST64(at, 0);
}

// Once per panel kind: mod regions past the rows a panel's layout has (layout rows minus the game's) get no row there.
static void log_rows(volatile int *logged, const char *panel, int64_t layout_rows, int game_rows) {
    if (*logged) return;
    *logged = 1;
    int64_t room = layout_rows - game_rows;
    if (room < (int64_t)mod_regions)
        rt_log("regions: %s: rows for %lld of %u mod region(s); the others are left out (the panel has rows for as "
               "many as fit on the screen, runtime/region_layout.c)", panel, (long long)(room > 0 ? room : 0),
               (uint32_t)mod_regions);
}

uint64_t region_loop_value(uint64_t slot) { return slot < VSLOTS ? region_id((uint32_t)slot) : slot; }

int region_loop_skip(uint64_t counter) { return counter == REGION_SLOTS && ext_room; }

uint64_t region_slot_end(void) { return ext_room ? EXT_FIRST + ext_room : REGION_SLOTS; }

uint64_t region_slot(uint64_t id, uint64_t site) {
    if (!far_count || id < REGION_SLOTS || id > 255 || !far_vslot[id]) return id;
    uint32_t slot = far_vslot[id];
    if (!live(slot)) slot = 0;                        // no room: the empty slot 0 (Mira's), never past the manager
    static volatile uint8_t seen[0x100];
    uint32_t k = ((uint32_t)(site >> 2) ^ (uint32_t)id * 0x9e) & 0xff;
    if (!seen[k]) {
        seen[k] = 1;
        rt_log("regions: region %llu -> slot %u at main+%#llx", (unsigned long long)id, slot, (unsigned long long)site);
    }
    return slot;
}

// main+0x3fe658 (x0 = the manager, x1 = a slot): updates the slot's survey rate (main+0x3ffe50), then stores the
// average rate of all slots that have hexagons into slot 0's (Mira's) rate, unrolled over the 24 slots. With far
// slots this does the same over every slot and returns 1 (the game's own code doesn't run).
int region_seg_rates(Cpu *c) {
    uint64_t mgr = c->x[0];
    if (!ext_room || mgr != seg_mgr) return 0;
    uint64_t x[31], sp = c->sp;
    memcpy(x, c->x, sizeof x);
    rt_call(c, SEG_RATE_UPDATE, 2, c->x[1], mgr + 0x379dc);
    memcpy(c->x, x, sizeof x);
    c->sp = sp;
    uint32_t sum = 0, n = 0;
    for (uint32_t s = 0; s < EXT_FIRST + ext_room; s++) {
        if (s == REGION_SLOTS) continue;
        uint64_t slot = mgr + SEG_SLOTS_AT + (uint64_t)s * SEG_SLOT;
        if (!LD32(slot + 0x1210)) continue;           // no hexagons
        sum += LD32(slot + 8);
        n++;
    }
    if (n) ST32(mgr + SEG_SLOTS_AT + 8, sum / n);
    return 1;
}

// main+0x1823e8 (w0 = a region): "past 24", i.e. not a field region, for the field object manager's gimmick set and
// main+0xe2dc0. A far region is a field region: it answers 0 and returns 1 (the game's own test doesn't run).
int region_field_test(Cpu *c) {
    uint32_t id = (uint32_t)c->x[0];
    if (id <= REGION_SLOTS || id > 255 || !live(far_vslot[id])) return 0;   // 24: the game counts it a field already
    static volatile uint8_t logged[256];
    if (!logged[id]) {
        logged[id] = 1;
        rt_log("regions: far region %u counts as a field region (gimmicks spawn in it)", id);
    }
    c->x[0] = 0;
    return 1;
}

// A bdat cell: row r (0-based), column col, as an unsigned number (column types 1-13: 1, 2 or 4 bytes).
static uint32_t bdat_cell(uint64_t t, uint32_t r, uint32_t col) {
    static const uint8_t size[14] = {0, 1, 2, 4, 1, 2, 4, 4, 4, 4, 1, 4, 1, 2};
    uint32_t off = 0;
    for (uint32_t i = 0; i < col; i++) off += size[LD8(t + LD32(t + 24) + 3 * i) % 14];
    uint32_t s = size[LD8(t + LD32(t + 24) + 3 * col) % 14];
    uint64_t p = t + LD32(t + 32) + (uint64_t)r * LD32(t + 36) + off;
    return s == 1 ? LD8(p) : s == 2 ? LD16(p) : LD32(p);
}

// ---- Probe reveals across regions ----
// Installing a probe uncovers segments: main+0x3257e4 calls main+0x3fe980 (x0 = the segment manager, w1 = the FN
// site) when a site's state first rises above 0, and that reveals the hexes its SEG_ProbeList row names, all in the
// site's own region (the game's rows name one zone each). A far region can name more in a table of its own,
// SEG_ProbeListZone<id>, laid out as SEG_ProbeList (ID, type, zone, site, then up to 6 hex ids of the far region's
// SEG_TerminalZone<id>, 0 = none): after the game's own reveal, every hex such a row lists for the site is opened
// (its open flag set, as the game does it). So a site in one region can uncover a segment in a neighboring one.
#define PROBE_REVEAL (MAIN_BASE + 0x3fe980)
#define FLAG_GET (MAIN_BASE + 0xc96d0)
#define FLAG_SET (MAIN_BASE + 0xc94a0)
#define SEG_HEX 0x24u                    // a slot's hex records from +0x10, numbered from the id at +4; open flag at +8
#define SEG_HEXES 0x1210u                // a slot's hex record count
#define SEG_HEX_MAX 128u                 // the hex records a slot has room for (0x10 + 128 * 0x24 = 0x1210)

int region_probe_reveal(Cpu *c) {
    static _Thread_local int inside;
    if (inside || !far_count || c->x[0] != seg_mgr) return 0;
    uint64_t x[31], sp = c->sp, mgr = c->x[0];
    uint32_t site = (uint32_t)c->x[1];
    memcpy(x, c->x, sizeof x);
    inside = 1;
    uint64_t ret = rt_call(c, PROBE_REVEAL, 2, mgr, (uint64_t)site);
    inside = 0;
    for (uint32_t id = REGION_SLOTS; id < 256; id++) {
        uint32_t slot = far_vslot[id];
        if (!slot || !live(slot)) continue;
        char name[40];
        snprintf(name, sizeof name, "SEG_ProbeListZone%u", id);
        uint64_t t = rt_call(c, TABLE_BY_HASH, 1, (uint64_t)name_hash(name));
        if (!t || LD32(t + 8) < 5) continue;
        int ints = 1;   // the site and hex columns must be integers (column types 1-6)
        for (uint32_t col = 3; col < LD32(t + 8) && col < 10; col++) {
            uint32_t type = LD8(t + LD32(t + 24) + 3 * col);
            ints &= type >= 1 && type <= 6;
        }
        if (!ints) continue;
        snprintf(name, sizeof name, "SEG_TerminalZone%u", id);
        uint64_t hexes = rt_call(c, TABLE_BY_HASH, 1, (uint64_t)name_hash(name));
        uint64_t s = mgr + SEG_SLOTS_AT + (uint64_t)slot * SEG_SLOT;
        uint32_t nhex = hexes ? LD32(hexes + 12) : 0, held = LD32(s + SEG_HEXES);   // the slot's own hex records
        if (nhex > held) nhex = held;
        if (nhex > SEG_HEX_MAX) nhex = SEG_HEX_MAX;
        uint32_t first = LD32(s + 4), opened = 0, cols = LD32(t + 8) < 10 ? LD32(t + 8) : 10;   // site + 6 hexes
        for (uint32_t r = 0; r < LD32(t + 12); r++) {
            if (bdat_cell(t, r, 3) != site) continue;
            for (uint32_t col = 4; col < cols; col++) {
                uint32_t h = bdat_cell(t, r, col);
                if (!h || h < first || h - first >= nhex) continue;
                uint32_t flag = LD16(s + 0x10 + (uint64_t)(h - first) * SEG_HEX + 8);
                if (!flag || (rt_call(c, FLAG_GET, 2, 1ull, (uint64_t)flag) & 0xff)) continue;
                rt_call(c, FLAG_SET, 4, 1ull, (uint64_t)flag, 1ull, 1ull);
                opened++;
            }
        }
        if (opened) rt_log("regions: site %u's probe uncovers %u segment(s) of far region %u (SEG_ProbeListZone%u)", site,
                           opened, id, id);
    }
    memcpy(c->x, x, sizeof x);
    c->sp = sp;
    c->x[0] = ret;
    return 1;
}

// The discovery flag the game sets on entering a region is 6148 + id (main+0xcbdbc, main+0xcbe50), which runs into
// other flags past id 27. A far region's flag is the one its FLD_FieldNameList row names (flg, column 7), so the
// region's data decides; ids below 24 keep the game's flag.
uint64_t region_flag(Cpu *c, uint64_t region, uint64_t vanilla) {
    (void)c;
    region = (uint32_t)region;
    if (region < REGION_SLOTS || region > 255 || !far_vslot[region]) return vanilla;
    // FLD_FieldNameList: row = region id - 1, flg in column 7 (an integer column). Without a usable flg the region gets
    // the mod flag 0xFE00 + id (kept in the mod state, runtime/mod_state.c), never the game's sum, which past id 27 is
    // some other game flag.
    uint64_t t = names_table;
    uint32_t flg = 0;
    if (t && region <= LD32(t + 12) && LD32(t + 8) > 7) {
        uint32_t type = LD8(t + LD32(t + 24) + 3 * 7);
        if (type >= 1 && type <= 6) flg = bdat_cell(t, (uint32_t)region - 1, 7);
    }
    if (!flg) flg = 0xFE00u + (uint32_t)region;
    static volatile uint8_t logged[256];
    if (!logged[region]) {
        logged[region] = 1;
        rt_log("regions: far region %llu's discovery flag: %u (the game's sum: %llu)", (unsigned long long)region, flg,
               (unsigned long long)vanilla);
    }
    return flg;
}

void region_hex_table(Cpu *c, uint64_t region) {
    if (c->x[1] & 0xff) return;                       // the game has this region's table
    uint32_t id = (uint32_t)region;
    if (id >= REGION_SLOTS ? id > 255 || !live(far_vslot[id]) : !(own_regions & 1u << id)) return;
    char name[32];
    snprintf(name, sizeof name, "SEG_TerminalZone%u", id);
    uint64_t table = rt_call(c, TABLE_BY_HASH, 1, (uint64_t)name_hash(name));
    if (!table) {
        c->x[0] = 0;                                  // as the game left it: no table
        c->x[1] &= ~0xffull;
        return;
    }
    if (LD32(table + 12) > SEG_HEX_MAX) {
        static volatile uint8_t warned[256];
        if (!warned[id]) {
            warned[id] = 1;
            rt_log("regions: %s has %u rows; a region holds %u hexagons at most: not used", name, LD32(table + 12),
                   SEG_HEX_MAX);
        }
        c->x[0] = 0;
        c->x[1] &= ~0xffull;
        return;
    }
    c->x[0] = table;
    c->x[1] = (c->x[1] & ~0xffull) | 1;
    uint32_t slot = region_slot_of(id);
    if (!is_mod(slot))
        rt_log("regions: region %u has no hex table in the game; using the mod's %s (slot %u)", id, name, slot);
    set_mod(slot);
}

void region_segmap_table(Cpu *c, uint64_t region) {
    region = (uint32_t)region;   // the game passes it in a w register
    if (region < 256) region = region_slot_of((uint32_t)region);
    if (c->x[0] || !is_mod((uint32_t)region)) return;
    char name[40];
    snprintf(name, sizeof name, "MNU_SegMapInfoZone%u", region_id((uint32_t)region));
    uint64_t table = rt_call(c, TABLE_BY_HASH, 1, (uint64_t)name_hash(name));
    c->x[0] = table;
    static volatile uint8_t logged[VSLOTS];
    if (!logged[region]) {
        logged[region] = 1;
        rt_log("regions: region %u's Segments tab markers: %s", region_id((uint32_t)region),
               table ? name : "none (the mod has no such table)");
    }
}

void region_field_layers(Cpu *c, uint64_t builder) {
    if (!mod_regions) return;
    static volatile int logged;
    if (logged < 2) {
        logged++;
        rt_log("regions: Segments tab layers (builder %#llx) for %u mod region(s)", (unsigned long long)builder,
               (uint32_t)mod_regions);
    }
    uint64_t saved[8];
    memcpy(saved, c->x, sizeof saved);                // the arguments the function itself still reads
    for (uint32_t s = 0; s < VSLOTS; s++) {
        if (!is_mod(s)) continue;
        rt_call(c, builder, 2, saved[0], (uint64_t)region_id(s));
        memcpy(c->x, saved, sizeof saved);
    }
}

void region_layer_entries(uint64_t map, uint64_t keep) {
    if (!mod_regions || !map) return;
    uint64_t count = LD64(map + LAYER_COUNT);
    uint32_t skipped = 0;
    for (uint32_t j = 0; j < mod_regions; j++) {
        if (count >= LAYER_ROOM) { skipped++; continue; }
        uint64_t e = map + LAYER_LIST + count * LAYER_SIZE;   // as main+0x7fd020 writes its own entries
        ST32(e, region_id(mod_order[j]));
        ST16(e + 0x28, 0);
        ST64(e + 0x08, 0);
        ST64(e + 0x10, 0);
        ST64(e + 0x18, 0);
        ST64(e + 0x20, keep);
        count++;
    }
    ST64(map + LAYER_COUNT, count);
    static volatile int logged;
    if (skipped && !logged) {
        logged = 1;
        rt_log("regions: the Segments tab's region list has room for %d; %u mod region(s) not drawn there", LAYER_ROOM,
               skipped);
    }
}

uint64_t region_minimap_case(uint64_t region, uint64_t byte) {
    region = (uint32_t)region;   // the game passes it in a w register
    if (region < 256) region = region_slot_of((uint32_t)region);
    if (is_mod((uint32_t)region)) return MINIMAP_FIELD_CASE;
    return byte;
}

// ---- Survey Reward List pages ----
// The panel (main+0x8547d0, 0xaf0 bytes, allocated at main+0x841508): page index u32 at +0x5c, the game's 11 page
// regions (u32) at +0xac0, a row vector per region id 0-23 at +0x7c0 + id * 0x20 ({begin, end, capacity, allocator};
// 0x40-byte rows with a string at +8), filled from every SEG_ExploreReward row by main+0x856040, which clears each
// vector first and skips a row whose zone is past 23. Its list widget: *(panel + 0x7b8), the rows list at +0x630, the
// header widget at +0xb0. With far regions the panel is allocated with a vector for each after its end (vector index =
// slot + 1, from 26: the first index past the panel's 0xaf0 bytes), and a tag; the fill sorts a far region's rows into
// its vector, which this clears at the fill's entry and frees at the panel's destructor (region_rw_fill, region_rw_free).
#define RW_PAGE 0x5c
#define RW_LIST 0xac0
#define RW_PAGES 11
#define RW_VEC 0x7c0
#define RW_UI 0x7b8
#define RW_RATE (MAIN_BASE + 0x5619dc)       // w0 = region -> survey rate in hundredths
#define RW_SET_RATE (MAIN_BASE + 0x672500)   // x0 = header widget, s0 = rate in percent
#define RW_NAME (MAIN_BASE + 0x562460)       // w0 = region, x8 = string out
#define RW_SET_NAME (MAIN_BASE + 0x6724e4)   // x0 = header widget, x1 = string
#define RW_FREE_STR (MAIN_BASE + 0x926214)   // x0 = string
#define RW_SLIDER (MAIN_BASE + 0x6726d8)     // x0 = header widget, w1 = page, w2 = last page
#define RW_ROWS_COUNT (MAIN_BASE + 0x5f26e4) // x0 = rows list, x1 = row count
#define RW_ROWS_TOP (MAIN_BASE + 0x5f17f0)   // x0 = rows list, 0, 0
#define RW_ROWS_REDRAW (MAIN_BASE + 0x5f0f50)

#define RW_SIZE 0xaf0
#define RW_EXT_VEC 26                        // the first vector index past the panel's own bytes
#define RW_MAGIC 0x5257455854564543ull

static uint64_t rw_filling;   // the panel whose fill runs (region_rw_fill), when it has far vectors
static uint32_t rw_room, rw_pending;

static uint64_t rw_tag_at(uint32_t room) { return RW_VEC + (uint64_t)(RW_EXT_VEC + room) * 0x20; }

static int rw_on(uint64_t panel) {
    return panel && rw_room && LD64(panel + rw_tag_at(rw_room)) == (panel ^ RW_MAGIC);
}

// The panel's vector index for a slot: the region id below 24, a far slot's own vector past the panel's end.
static uint32_t rw_vec(uint32_t slot) { return slot < REGION_SLOTS ? slot : slot + 1; }

static uint64_t rw_rows(uint64_t panel, uint32_t slot) {
    uint32_t i = rw_vec(slot);
    if (i >= RW_EXT_VEC && (!rw_on(panel) || i >= RW_EXT_VEC + rw_room)) return 0;
    uint64_t v = panel + RW_VEC + (uint64_t)i * 0x20;
    return (LD64(v + 8) - LD64(v)) >> 6;
}

uint64_t region_rw_size(uint64_t size) {
    uint32_t far = ext_room;
    rw_pending = (uint32_t)size == RW_SIZE && far ? far : 0;
    return rw_pending ? rw_tag_at(rw_pending) + 8 : size;
}

void region_rw_alloced(Cpu *c) {
    uint64_t panel = c->x[0];
    if (rw_pending && panel) {
        for (uint64_t o = RW_SIZE; o < rw_tag_at(rw_pending); o += 8) ST64(panel + o, 0);
        ST64(panel + rw_tag_at(rw_pending), panel ^ RW_MAGIC);
        rw_room = rw_pending;   // the same for every panel of a boot: the manager's far slots
    }
    rw_pending = 0;
}

// Empties the far regions' vectors as the fill does the game's (each row's string freed, end = begin), and gives each
// the allocator the game's vectors use; with free, also gives the buffers back (as the panel's destructor does).
static void rw_clear(Cpu *c, uint64_t panel, int free) {
    uint64_t x[31], sp = c->sp;
    memcpy(x, c->x, sizeof x);
    uint64_t alloc = LD64(panel + RW_VEC + 0x18);
    for (uint32_t k = 0; k < rw_room; k++) {
        uint64_t v = panel + RW_VEC + (uint64_t)(RW_EXT_VEC + k) * 0x20, b = LD64(v), e = LD64(v + 8);
        if (!LD64(v + 0x18)) ST64(v + 0x18, alloc);
        for (uint64_t r = e; r > b; r -= 0x40) rt_call(c, RW_FREE_STR, 1, r - 0x38);
        ST64(v + 8, b);
        if (free && b && LD64(v + 0x18)) {
            uint64_t a = LD64(v + 0x18);
            rt_call(c, LD64(LD64(a) + 0x18), 2, a, b);
            ST64(v, 0);
            ST64(v + 8, 0);
            ST64(v + 0x10, 0);
        }
    }
    memcpy(c->x, x, sizeof x);
    c->sp = sp;
}

void region_rw_fill(Cpu *c) {
    rw_filling = rw_on(c->x[0]) ? c->x[0] : 0;
    if (rw_filling) rw_clear(c, c->x[0], 0);
}

void region_rw_free(Cpu *c) {
    uint64_t panel = c->x[0];
    if (!rw_on(panel)) return;
    rw_clear(c, panel, 1);
    ST64(panel + rw_tag_at(rw_room), 0);
    if (rw_filling == panel) rw_filling = 0;
}

// The fill's row sort (main+0x856040), after and x26, x0, #0xff (the row's zone): the vector index, a far region's
// past the panel's end, or 0xff (skipped by the fill's own test) when the panel has no vector for it.
uint64_t region_rw_index(uint64_t zone) {
    if (zone < REGION_SLOTS || zone > 255 || !live(far_vslot[zone])) return zone;
    uint32_t i = rw_vec(far_vslot[zone]);
    return rw_filling && i < RW_EXT_VEC + rw_room ? i : 0xff;
}

// "Show this reward in the list" (main+0x856ad0, the select button on the survey reward pop-up), after mov w8, w22
// (the reward's region): the region's vector index, which the game takes to be the region id. A far region's is past
// the panel's end; vector 0 (Mira's) when the panel has none for it, which region_reward_jump already refuses.
uint64_t region_rw_show_index(uint64_t panel, uint64_t region) {
    uint32_t id = (uint32_t)region;
    if (id < REGION_SLOTS || id > 255 || !far_vslot[id]) return region;
    return rw_rows(panel, far_vslot[id]) ? rw_vec(far_vslot[id]) : 0;
}

// Its test that the index is at most 23: true also for a far region's vector.
uint64_t region_rw_test(uint64_t index) {
    return rw_filling && index >= RW_EXT_VEC && index < RW_EXT_VEC + rw_room ? 0 : index;
}

// The extra pages: mod regions (not in the game's list) with reward rows, in id order. Returns how many.
static int rw_extras(uint64_t panel, uint32_t out[VSLOTS]) {
    int n = 0;
    if (!mod_regions) return 0;
    for (uint32_t j = 0; j < mod_regions; j++) {
        uint32_t id = mod_order[j];
        if (!rw_rows(panel, id)) continue;
        int listed = 0;
        for (int i = 0; i < RW_PAGES; i++) listed |= LD32(panel + RW_LIST + 4 * i) == id;
        if (!listed) out[n++] = id;
    }
    static volatile int logged;
    if (n && !logged) {
        logged = 1;
        rt_log("regions: Survey Reward List: %d extra page(s) for mod regions, first region %u", n, region_id(out[0]));
    }
    return n;
}

static uint32_t rw_region(uint64_t panel, uint32_t page, const uint32_t *extra, int n) {
    if (page < RW_PAGES) return LD32(panel + RW_LIST + 4 * page);
    return page - RW_PAGES < (uint32_t)n ? extra[page - RW_PAGES] : 0;
}

uint64_t region_reward_region(uint64_t panel, uint64_t vanilla) {
    uint32_t page = LD32(panel + RW_PAGE), extra[VSLOTS];
    if (page < RW_PAGES) return vanilla;
    int n = rw_extras(panel, extra);
    if (!n) return vanilla;
    return rw_vec(rw_region(panel, page, extra, n));
}

// As main+0x8566b0: the page's region name, its survey rate and the page slider.
static void rw_header(Cpu *c, uint64_t panel, const uint32_t *extra, int n) {
    uint64_t ui = LD64(panel + RW_UI), w;
    if (!ui || !(w = LD64(ui + 0xb0))) return;
    uint32_t page = LD32(panel + RW_PAGE), region = rw_region(panel, page, extra, n);
    uint32_t rate = (uint32_t)rt_call(c, RW_RATE, 1, (uint64_t)region_id(region));
    float pct = (float)rate * 0.01f;
    uint32_t bits;
    memcpy(&bits, &pct, 4);
    c->v[0] = bits;
    rt_call(c, RW_SET_RATE, 1, w);
    uint64_t sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;                    // room for the name string, as the game's own frame has
    uint64_t str = c->sp;
    c->x[8] = str;
    rt_call(c, RW_NAME, 1, (uint64_t)region_id(region));
    rt_call(c, RW_SET_NAME, 2, w, str);
    rt_call(c, RW_FREE_STR, 1, str);
    c->sp = sp;
    rt_call(c, RW_SLIDER, 3, w, (uint64_t)page, (uint64_t)(RW_PAGES - 1 + n));
}

// As the page turns do after storing the page: the header, then the rows list sized to the page's region.
static void rw_show(Cpu *c, uint64_t panel, const uint32_t *extra, int n) {
    rw_header(c, panel, extra, n);
    uint64_t ui = LD64(panel + RW_UI), rows;
    if (!ui || !(rows = LD64(ui + 0x630))) return;
    uint32_t region = rw_region(panel, LD32(panel + RW_PAGE), extra, n);
    rt_call(c, RW_ROWS_COUNT, 2, rows, rw_rows(panel, region));
    rt_call(c, RW_ROWS_TOP, 3, rows, 0ull, 0ull);
    rt_call(c, RW_ROWS_REDRAW, 1, rows);
}

int region_reward_header(Cpu *c) {
    uint32_t extra[VSLOTS];
    uint64_t panel = c->x[0];
    int n = rw_extras(panel, extra);
    if (!n) return 0;
    rw_header(c, panel, extra, n);
    return 1;
}

static int rw_turn(Cpu *c, int dir) {
    uint32_t extra[VSLOTS];
    uint64_t panel = c->x[0];
    int n = rw_extras(panel, extra);
    if (!n) return 0;
    uint32_t last = RW_PAGES - 1 + n, page = LD32(panel + RW_PAGE), to;
    if (dir > 0) to = page >= last ? 0 : page + 1;
    else to = page == 0 || page > last ? last : page - 1;
    if (dir > 0 && to == page) { c->x[0] = 0; return 1; }
    ST32(panel + RW_PAGE, to);
    rw_show(c, panel, extra, n);
    c->x[0] = 1;
    return 1;
}

int region_reward_next(Cpu *c) { return rw_turn(c, 1); }
int region_reward_prev(Cpu *c) { return rw_turn(c, -1); }

int region_reward_jump(Cpu *c) {
    uint32_t extra[VSLOTS];
    uint64_t panel = c->x[0];
    uint32_t region = (uint32_t)c->x[1];
    int n = rw_extras(panel, extra);
    if (!n) return 0;
    uint32_t last = RW_PAGES - 1 + n, to = ~0u;
    for (uint32_t p = 0; p <= last && to == ~0u; p++)
        if (region_id(rw_region(panel, p, extra, n)) == region) to = p;
    if (to == ~0u || to == LD32(panel + RW_PAGE)) { c->x[0] = 0; return 1; }
    ST32(panel + RW_PAGE, to);
    rw_show(c, panel, extra, n);
    c->x[0] = 1;
    return 1;
}

// ---- Region Intel (hold ZL on the Segments tab)
// The panel (allocated with 0x19a8 bytes, main+0x840a50) keeps 12 rows of 0x38 bytes from +0x1708 {u32 region, f32
// rate in percent, name string at +8}, the last one Mira (region 0). Its list (+0xb38) holds one widget per layout node
// mc_List01.. inline at list+0x1c0 (0x48 bytes each) with the count right after at list+0x520: room for 12. With a mod
// region the panel is allocated bigger: rows for up to MOD_ROWS_MAX mod regions follow the game's 12, then a block of
// widgets for them all, its count, a tag and how many mod rows are shown. Every use of the list's block and count goes
// to the new place (recomp/regions.py), only for the list whose tag matches; zeroed memory is an empty string for the
// game's string type.
#define RI_SIZE 0x19a8
#define RI_ROWS 0x1708
#define RI_ROW 0x38
#define RI_GAME_ROWS REGION_INTEL_GAME_ROWS
// Mod regions a panel's lists have room for (runtime/region_layout.c gives its layout as many rows as fit on screen).
#define MOD_ROWS_MAX 48
#define RI_MAX_ROWS (RI_GAME_ROWS + MOD_ROWS_MAX)
#define RI_LIST 0xb38
#define RI_OLD_BLOCK 0x1c0
#define RI_WIDGET 0x48
#define RI_BLOCK (RI_ROWS + RI_MAX_ROWS * RI_ROW - RI_LIST)       // list-relative
#define RI_COUNT (RI_BLOCK + RI_MAX_ROWS * RI_WIDGET)
#define RI_TAG (RI_COUNT + 8)
#define RI_SHOWN (RI_TAG + 8)
#define RI_EXT_END (RI_LIST + RI_SHOWN + 8)                     // Region Intel's part; the FN tab's follows
#define RI_MAGIC 0x5245474e494e5431ull
#define RI_SET_TEXT (MAIN_BASE + 0x6709e8)   // x0 = row widget, x1 = string
#define RI_SET_NUM (MAIN_BASE + 0x670a00)    // x0 = row widget, s0 = value
#define RI_ASSIGN (MAIN_BASE + 0x92622c)     // x0 = string, x1 = string to copy

// The panels of one kind with mod rows (the Map tab and the Segments tab each make their own), by list address: up to
// LS_MAX. A full set first drops lists whose tag is gone (their memory was reused) or whose panel's destructor ran
// (region_intel_free; its lists are dropped only at the next add, after the destructor's own teardown); a live list is
// never dropped. Still full: the new panel is left as the game made it (no mod rows), and the log says so once.
#define LS_MAX 32
typedef struct { uint64_t v[LS_MAX]; uint64_t tag_at, magic, list_at; const char *what; } ListSet;
static uint64_t dying[8];   // Region Intel panels whose destructor ran (their FN and Map tab lists live in them too)
static int is_dying(uint64_t panel) {
    for (int i = 0; i < 8; i++) if (panel && dying[i] == panel) return 1;
    return 0;
}
static int ls_has(const ListSet *s, uint64_t list) {
    if (!list) return 0;
    for (int i = 0; i < LS_MAX; i++) if (s->v[i] == list) return 1;
    return 0;
}
// 1 when the list is in the set (added now or before), 0 when the set has no room.
static int ls_add(ListSet *s, uint64_t list) {
    if (ls_has(s, list)) return 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < LS_MAX; i++)
            if (!s->v[i]) { s->v[i] = list; return 1; }
        for (int i = 0; i < LS_MAX; i++)   // full: drop the dead ones
            if (LD64(s->v[i] + s->tag_at) != (s->v[i] ^ s->magic) || is_dying(s->v[i] - s->list_at)) s->v[i] = 0;
    }
    static volatile int logged;
    if (!logged) {
        logged = 1;
        rt_log("regions: more than %d %s panels alive at once: the newest has no mod rows", LS_MAX, s->what);
    }
    return 0;
}
static int ls_any(const ListSet *s) {
    for (int i = 0; i < LS_MAX; i++) if (s->v[i]) return 1;
    return 0;
}
// Panels allocated bigger and not yet initialized, per kind: an init takes its panel's entry.
typedef struct { uint64_t v[4]; } Pending;
static void pend_add(Pending *p, uint64_t panel) {
    for (int i = 0; i < 4; i++) if (!p->v[i] || p->v[i] == panel) { p->v[i] = panel; return; }
    memmove(p->v, p->v + 1, 3 * sizeof *p->v);   // the oldest has waited through three newer ones: let it go
    p->v[3] = panel;
}
static int pend_take(Pending *p, uint64_t panel) {
    for (int i = 0; i < 4; i++) if (panel && p->v[i] == panel) { p->v[i] = 0; return 1; }
    return 0;
}
static Pending ri_pend, bp_pend, mi_pend, sc_pend;

// The FN tab's Region Intel ("Battle Probe Effects", hold ZL on the FrontierNav tab) lives in the same panel: 5 blocks of
// 0xd0 bytes from +0x12a0 {u32 region, the region's battle probe entries (12 bytes each) from +8, count at +0xc8},
// filled by main+0x84dee0 for regions 2, 4, 5, 7, 6 from the game data's per-region record (main+0x32594c), shown by a
// list (+0x88) of up to 5 "mc_List%02d" widgets inline at list+0x1c0 (0x48 bytes each, count at list+0x328) through
// the row function main+0x84e084 (rows 0-4). With a mod region the same bigger panel also holds a block per mod region
// and the list's widgets for them all, its count, a tag and how many mod rows are shown; rows 5.. are the mod regions
// (id order).
#define BP_BLOCKS 0x12a0
#define BP_BLOCK 0xd0
#define BP_GAME_ROWS REGION_FN_GAME_ROWS
#define BP_EXTRA (RI_EXT_END)                                    // panel offset of the mod regions' blocks
#define BP_LIST 0x88
#define BP_OLD_BLOCK 0x1c0
#define BP_WIDGET 0x48
#define BP_MAX_ROWS (BP_GAME_ROWS + MOD_ROWS_MAX)
#define BP_WIDGETS (BP_EXTRA + (BP_MAX_ROWS - BP_GAME_ROWS) * BP_BLOCK - BP_LIST)   // list-relative
#define BP_COUNT (BP_WIDGETS + BP_MAX_ROWS * BP_WIDGET)
#define BP_TAG (BP_COUNT + 8)
#define BP_SHOWN (BP_TAG + 8)
#define BP_PANEL_END (BP_LIST + BP_SHOWN + 8)
// The Map tab's Region Intel ("Mission Info", hold ZL on the Map tab) is a third list in the same panel: up to 11 rows
// {u32 region, u32 mission count} at +0x16b0, filled by main+0x84e390, shown by a list (+0x508) of up to 11
// "mc_List%02d" widgets inline (0x48 bytes at list+0x1c0, count at list+0x4d8) through the row function main+0x85057c
// (rows 0-10). Its rows come from Region Intel's layout, ui/image/04010_parts05.wilay (12 rows: the 11 and Mira's,
// which this view hides; runtime/region_layout.c adds the mod regions' rows before Mira's). With a mod region the
// bigger panel also holds that list's widgets (as many as Region Intel's), its count and a tag; rows 11.. are the mod
// regions (id order), with their mission counts (region_mi_missions).
#define MI_LIST 0x508
#define MI_OLD_BLOCK 0x1c0
#define MI_WIDGET 0x48
#define MI_GAME_ROWS 11
#define MI_MAX_ROWS RI_MAX_ROWS                // the same layout as Region Intel's (Mira's row hidden here)
#define MI_BLOCK (BP_PANEL_END - MI_LIST)                         // list-relative
#define MI_COUNT (MI_BLOCK + MI_MAX_ROWS * MI_WIDGET)
#define MI_TAG (MI_COUNT + 8)
#define MI_PANEL_END (MI_LIST + MI_TAG + 8)
#define MI_MAGIC 0x4d4953494e464f31ull
#define MI_SET_TEXT (MAIN_BASE + 0x6704e8)   // x0 = row widget, x1 = string
#define MI_SET_COUNT (MAIN_BASE + 0x6704fc)  // x0 = row widget, w1 = mission count
#define BP_MAGIC 0x42504c4953543031ull
#define BP_ROW_FN (MAIN_BASE + 0x84e084)     // x0 = panel, x1 = row widget, w2 = row (0-4)
#define BP_RECORD (MAIN_BASE + 0x32594c)     // x0 = game data, w1 = region -> its record (entries, count at +0xc0)
#define BP_GAME_DATA (MAIN_BASE + 0x1d560d8) // GOT slot of the pointer to the game data pointer

static volatile int ri_pending;
static ListSet ri_lists = {{0}, RI_TAG, RI_MAGIC, RI_LIST, "Region Intel"};
static ListSet bp_lists = {{0}, BP_TAG, BP_MAGIC, BP_LIST, "Region Intel (FrontierNav tab)"};
static ListSet mi_lists = {{0}, MI_TAG, MI_MAGIC, MI_LIST, "Region Intel (Map tab)"};

static int ri_on(uint64_t list) {
    return ls_has(&ri_lists, list) && LD64(list + RI_TAG) == (list ^ RI_MAGIC);
}

uint64_t region_intel_size(uint64_t size) {
    ri_pending = (uint32_t)size == RI_SIZE && mod_regions;
    return ri_pending ? MI_PANEL_END : size;
}

void region_intel_alloced(Cpu *c) {
    uint64_t panel = c->x[0];
    if (ri_pending && panel) {
        pend_add(&ri_pend, panel);
        pend_add(&bp_pend, panel);
        pend_add(&mi_pend, panel);
        for (int i = 0; i < 8; i++) if (dying[i] == panel) dying[i] = 0;   // the address lives again
    }
    ri_pending = 0;
}

void region_intel_init(uint64_t list) {
    uint64_t panel = list - RI_LIST;
    if (!pend_take(&ri_pend, panel) || !ls_add(&ri_lists, list)) return;
    for (uint64_t o = RI_SIZE; o < RI_EXT_END; o += 8) ST64(panel + o, 0);
    ST64(list + RI_TAG, list ^ RI_MAGIC);
}

uint64_t region_intel_count(uint64_t list, uint64_t vanilla) { return ri_on(list) ? LD64(list + RI_COUNT) : vanilla; }
void region_intel_set_count(uint64_t list, uint64_t count) { if (ri_on(list)) ST64(list + RI_COUNT, count); }
uint64_t region_intel_block(uint64_t list, uint64_t vanilla) {
    return ri_on(list) ? vanilla - RI_OLD_BLOCK + RI_BLOCK : vanilla;
}
uint64_t region_intel_max(uint64_t list, uint64_t vanilla) { return ri_on(list) ? RI_MAX_ROWS : vanilla; }

void region_intel_fill(Cpu *c, uint64_t panel) {
    uint64_t list = panel + RI_LIST;
    if (!ri_on(list)) return;
    uint32_t ids[RI_MAX_ROWS];
    int n = 0, room = (int)LD64(list + RI_COUNT) - RI_GAME_ROWS;
    for (uint32_t j = 0; j < mod_regions && n < room; j++) ids[n++] = mod_order[j];
    ST64(list + RI_SHOWN, n > 0 ? (uint64_t)n : 0);
    static volatile int logged;
    if (!logged) {
        logged = 1;
        rt_log("regions: Region Intel: %d layout row(s), %d row(s) for mod regions before Mira",
               (int)LD64(list + RI_COUNT), n > 0 ? n : 0);
    }
    static volatile int rows_logged;
    log_rows(&rows_logged, "Region Intel", (int64_t)LD64(list + RI_COUNT), RI_GAME_ROWS);
    if (n <= 0) return;
    uint64_t x[31];
    memcpy(x, c->x, sizeof x);
    uint64_t mira = panel + RI_ROWS + (RI_GAME_ROWS - 1) * RI_ROW, last = mira + (uint64_t)n * RI_ROW;
    ST32(last, LD32(mira));
    ST32(last + 4, LD32(mira + 4));
    rt_call(c, RI_ASSIGN, 2, last + 8, mira + 8);
    uint64_t sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;                    // room for a name string
    uint64_t str = c->sp;
    for (int k = 0; k < n; k++) {
        uint64_t r = mira + (uint64_t)k * RI_ROW;
        float pct = (float)(uint32_t)rt_call(c, RW_RATE, 1, (uint64_t)region_id(ids[k])) * 0.01f;
        uint32_t bits;
        memcpy(&bits, &pct, 4);
        ST32(r, region_id(ids[k]));
        ST32(r + 4, bits);
        c->x[8] = str;
        rt_call(c, RW_NAME, 1, (uint64_t)region_id(ids[k]));
        rt_call(c, RI_ASSIGN, 2, r + 8, str);
        rt_call(c, RW_FREE_STR, 1, str);
    }
    c->sp = sp;
    memcpy(c->x, x, sizeof x);
}

int region_intel_row(Cpu *c) {
    uint32_t row = (uint32_t)c->x[2];
    if (row < RI_GAME_ROWS) return 0;
    uint64_t panel = LD64(c->x[0] + 8), list = panel + RI_LIST, widget = c->x[1];
    if (!panel || !ri_on(list)) return 0;
    if (row < RI_GAME_ROWS + LD64(list + RI_SHOWN)) {
        uint64_t r = panel + RI_ROWS + (uint64_t)row * RI_ROW;
        rt_call(c, RI_SET_TEXT, 2, widget, r + 8);
        c->v[0] = LD32(r + 4);
        rt_call(c, RI_SET_NUM, 1, widget);
    }
    c->x[0] = 1;
    return 1;
}

void region_intel_free(Cpu *c) {
    uint64_t panel = c->x[0], list = panel + RI_LIST;
    if (!ri_on(list)) return;
    uint64_t x[31], sp = c->sp;
    memcpy(x, c->x, sizeof x);                       // the destructor goes on with the registers it was called with
    for (int i = RI_GAME_ROWS; i < RI_MAX_ROWS; i++)
        rt_call(c, RW_FREE_STR, 1, panel + RI_ROWS + (uint64_t)i * RI_ROW + 8);
    memcpy(c->x, x, sizeof x);
    c->sp = sp;
    static volatile int logged;
    if (!logged) { logged = 1; rt_log("regions: a Region Intel panel with mod rows freed"); }
    // The tags stay: this runs at the destructor's entry, and the destructor then tears down its lists through the list
    // code, which must still find their counts and widgets where the hooks put them. The panel is marked dying, and its
    // lists leave the sets when a set needs room (by then the teardown is over); a panel made later at the same address
    // is tagged again by its init.
    for (int i = 0; i < 8; i++) if (!dying[i] || dying[i] == panel) { dying[i] = panel; return; }
    memmove(dying, dying + 1, 7 * sizeof *dying);
    dying[7] = panel;
}


// ---- Map Shortcuts (X on the map, "Select Target Area")
// The panel (0x7f8 bytes, main+0x841850) shows 8 fixed entries through a list (+0x1b8) of up to 8 "mc_List%02d"
// widgets inline at list+0x1c0 (0x58 bytes each), count at list+0x480. With a mod region the panel is allocated
// bigger and the list's widgets moved past its end (room for MOD_ROWS_MAX more), as for Region Intel; entry 8 and up
// are the mod regions (id order), as many as the layout has rows for. Such an entry is named after its region, checked
// like Primordia's entry and, when picked, posts the map the same "show region" command the continents' entries post,
// with the mod region.
#define SC_SIZE 0x7f8
#define SC_LIST 0x1b8
#define SC_OLD_BLOCK 0x1c0
#define SC_WIDGET 0x58
#define SC_GAME_ROWS REGION_SHORTCUTS_GAME_ROWS
#define SC_MAX_ROWS (SC_GAME_ROWS + MOD_ROWS_MAX)
#define SC_BLOCK (SC_SIZE - SC_LIST)                              // list-relative
#define SC_COUNT (SC_BLOCK + SC_MAX_ROWS * SC_WIDGET)
#define SC_TAG (SC_COUNT + 8)
#define SC_BIG_SIZE (SC_LIST + SC_TAG + 8)
#define SC_MAGIC 0x53484f5254435431ull
#define SC_PANEL_LIST 0x788                  // the panel's pointer to its list
#define SC_OWNER 0x188                       // the panel's owner callback (vtable +0x10: pick entry w1)
#define SC_CLOSE (MAIN_BASE + 0x867da0)      // x0 = panel, w1 = 0, w2 = 1
#define SC_ROWS (MAIN_BASE + 0x5f17d0)       // x0 = list -> row count
#define SC_SET_TEXT (MAIN_BASE + 0x67d650)   // x0 = row widget, x1 = string
#define SC_CAN_JUMP (MAIN_BASE + 0x7a5dec)   // x0 = map, w1 = entry
#define SC_QUEUE (MAIN_BASE + 0x793d2c)      // x0 = map -> its command queue
#define SC_POST (MAIN_BASE + 0x7fe880)       // x0 = queue, x1 = command
#define SC_SHOW_REGION (MAIN_BASE + 0x1c78c90)   // "show region" command: {vtable, u32 region}
#define SC_PRIMORDIA_ENTRY 2

static volatile int sc_pending;
static ListSet sc_lists = {{0}, SC_TAG, SC_MAGIC, SC_LIST, "Map Shortcuts"};

static int sc_on(uint64_t list) {
    return ls_has(&sc_lists, list) && LD64(list + SC_TAG) == (list ^ SC_MAGIC);
}

// The mod region of entry 8 + k, or ~0u.
static uint32_t sc_region(uint32_t entry) {
    if (entry < SC_GAME_ROWS) return ~0u;
    uint32_t k = entry - SC_GAME_ROWS;
    return k < mod_regions ? mod_order[k] : ~0u;
}

uint64_t region_shortcut_size(uint64_t size) {
    sc_pending = (uint32_t)size == SC_SIZE && mod_regions;
    return sc_pending ? SC_BIG_SIZE : size;
}

void region_shortcut_alloced(Cpu *c) {
    if (sc_pending && c->x[0]) pend_add(&sc_pend, c->x[0]);
    sc_pending = 0;
}

void region_shortcut_init(uint64_t list) {
    uint64_t panel = list - SC_LIST;
    if (!pend_take(&sc_pend, panel) || !ls_add(&sc_lists, list)) return;
    for (uint64_t o = SC_SIZE; o < SC_BIG_SIZE; o += 8) ST64(panel + o, 0);
    ST64(list + SC_TAG, list ^ SC_MAGIC);
}

uint64_t region_shortcut_count(uint64_t list, uint64_t vanilla) { return sc_on(list) ? LD64(list + SC_COUNT) : vanilla; }
void region_shortcut_set_count(uint64_t list, uint64_t count) { if (sc_on(list)) ST64(list + SC_COUNT, count); }
uint64_t region_shortcut_block(uint64_t list, uint64_t vanilla) {
    return sc_on(list) ? vanilla - SC_OLD_BLOCK + SC_BLOCK : vanilla;
}
uint64_t region_shortcut_max(uint64_t list, uint64_t vanilla) { return sc_on(list) ? SC_MAX_ROWS : vanilla; }

// main+0x868eb8: w1 = entry, x8 = the name string out.
int region_shortcut_name(Cpu *c) {
    uint32_t region = sc_region((uint32_t)c->x[1]);
    if (region == ~0u || !ls_any(&sc_lists)) return 0;
    rt_call(c, RW_NAME, 1, (uint64_t)region_id(region));         // x8 is still the caller's string
    return 1;
}

// main+0x86b5cc: x0 = context (+8 = the panel), x1 = row widget, w2 = entry.
int region_shortcut_row(Cpu *c) {
    uint32_t entry = (uint32_t)c->x[2], region = sc_region(entry);
    uint64_t panel = LD64(c->x[0] + 8), widget = c->x[1];
    if (panel && sc_on(panel + SC_LIST)) {
        static volatile int rows_logged;
        log_rows(&rows_logged, "Map Shortcuts", (int64_t)LD64(panel + SC_LIST + SC_COUNT), SC_GAME_ROWS);
    }
    if (region == ~0u || !panel || !sc_on(panel + SC_LIST)) return 0;
    if (widget) {
        uint64_t sp = c->sp;
        c->sp = (sp - 0x40) & ~15ull;
        uint64_t str = c->sp;
        c->x[8] = str;
        rt_call(c, RW_NAME, 1, (uint64_t)region_id(region));
        rt_call(c, SC_SET_TEXT, 2, widget, str);
        rt_call(c, RW_FREE_STR, 1, str);
        c->sp = sp;
        ST8(widget + 0x37, 1);                         // enabled
        ST8(widget + 0x36, 1);
    }
    if (entry < 16) ST16(panel + 0x790, LD16(panel + 0x790) | 1u << entry);   // the panel's enabled bits (Skell's is off until the license)
    c->x[0] = widget != 0;
    return 1;
}

// main+0x868834: x0 = the panel -> the current entry as {u8 entry, valid in bit 32}, valid only below 8 in the game.
int region_shortcut_current(Cpu *c) {
    uint64_t list = LD64(c->x[0] + SC_PANEL_LIST);
    if (!list || !sc_on(list)) return 0;
    uint32_t entry = LD32(list + 0xc);
    if (entry < SC_GAME_ROWS || sc_region(entry) == ~0u) return 0;
    if (entry >= (uint32_t)rt_call(c, SC_ROWS, 1, list)) return 0;
    c->x[0] = entry | 1ull << 32;
    return 1;
}

// main+0x86a188: x0 = context (+8 = the panel); the list's cursor is the picked entry.
int region_shortcut_select(Cpu *c) {
    uint64_t panel = LD64(c->x[0] + 8), list = panel ? LD64(panel + SC_PANEL_LIST) : 0;
    if (!list || !sc_on(list)) return 0;
    uint32_t entry = LD32(list + 0xc);
    if (entry < SC_GAME_ROWS || sc_region(entry) == ~0u) return 0;
    if (entry >= (uint32_t)rt_call(c, SC_ROWS, 1, list)) return 0;
    ST8(panel + 0x5a, 0);
    uint64_t owner = LD64(panel + SC_OWNER);
    if (owner && (rt_call(c, LD64(LD64(owner) + 0x10), 2, owner, (uint64_t)entry) & 1))
        rt_call(c, SC_CLOSE, 3, panel, 0ull, 1ull);
    return 1;
}

// main+0x7a5dec: x0 = map, w1 = entry -> may jump. A mod region's entry answers as Primordia's does.
int region_shortcut_can_jump(Cpu *c) {
    if (sc_region((uint32_t)c->x[1]) == ~0u || !ls_any(&sc_lists)) return 0;
    c->x[0] = rt_call(c, SC_CAN_JUMP, 2, c->x[0], (uint64_t)SC_PRIMORDIA_ENTRY) & 1;
    return 1;
}

// main+0x7fd200: x0 = map, w1 = entry. A mod region's entry posts "show region <id>" as the continents' entries do.
int region_shortcut_jump(Cpu *c) {
    uint32_t entry = (uint32_t)c->x[1], region = sc_region(entry);
    if (region == ~0u || !ls_any(&sc_lists)) return 0;
    uint64_t map = c->x[0], sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;
    uint64_t cmd = c->sp;
    for (int i = 0; i < 0x40; i += 8) ST64(cmd + i, 0);
    ST64(cmd, SC_SHOW_REGION);
    ST32(cmd + 8, region_id(region));
    uint64_t queue = rt_call(c, SC_QUEUE, 1, map);
    rt_call(c, SC_POST, 2, queue, cmd);
    c->sp = sp;
    static volatile int logged;
    if (!logged) { logged = 1; rt_log("regions: Map Shortcuts: entry %u shows mod region %u", entry, region_id(region)); }
    c->x[0] = 1;
    return 1;
}


// ---- Region Intel, FN tab ("Battle Probe Effects")
static int bp_on(uint64_t list) {
    return ls_has(&bp_lists, list) && LD64(list + BP_TAG) == (list ^ BP_MAGIC);
}

void region_bp_init(uint64_t list) {
    uint64_t panel = list - BP_LIST;
    if (!pend_take(&bp_pend, panel) || !ls_add(&bp_lists, list)) return;
    for (uint64_t o = RI_EXT_END; o < BP_PANEL_END; o += 8) ST64(panel + o, 0);
    ST64(list + BP_TAG, list ^ BP_MAGIC);
}

uint64_t region_bp_count(uint64_t list, uint64_t vanilla) { return bp_on(list) ? LD64(list + BP_COUNT) : vanilla; }
void region_bp_set_count(uint64_t list, uint64_t count) { if (bp_on(list)) ST64(list + BP_COUNT, count); }
uint64_t region_bp_block(uint64_t list, uint64_t vanilla) {
    return bp_on(list) ? vanilla - BP_OLD_BLOCK + BP_WIDGETS : vanilla;
}
uint64_t region_bp_max(uint64_t list, uint64_t vanilla) { return bp_on(list) ? BP_MAX_ROWS : vanilla; }

void region_bp_fill(Cpu *c, uint64_t panel) {
    uint64_t list = panel + BP_LIST;
    if (!bp_on(list)) return;
    int n = 0, room = (int)LD64(list + BP_COUNT) - BP_GAME_ROWS;
    if (room > BP_MAX_ROWS - BP_GAME_ROWS) room = BP_MAX_ROWS - BP_GAME_ROWS;
    uint64_t x[31], sp = c->sp;
    memcpy(x, c->x, sizeof x);
    uint64_t gd = LD64(LD64(BP_GAME_DATA));
    for (uint32_t j = 0; j < mod_regions && n < room; j++) {
        uint32_t id = mod_order[j];
        // The game's records cover regions 0-23 (main+0x321cec gives region 0's for any id past 23): a far region has
        // no battle probe entries of its own.
        uint32_t rid = region_id(id);
        uint64_t b = panel + BP_EXTRA + (uint64_t)n * BP_BLOCK,
                 rec = rid < REGION_SLOTS ? rt_call(c, BP_RECORD, 2, gd, (uint64_t)rid) : 0;
        uint64_t cnt = rec ? LD64(rec + 0xc0) : 0;
        if (cnt > 16) cnt = 16;
        ST32(b, region_id(id));                        // the region the row names (a far region by its own id)
        for (uint64_t i = 0; i < cnt * 12; i += 4) ST32(b + 8 + i, LD32(rec + i));
        ST64(b + 0xc8, cnt);
        n++;
    }
    memcpy(c->x, x, sizeof x);
    c->sp = sp;
    ST64(list + BP_SHOWN, n > 0 ? (uint64_t)n : 0);
    static volatile int logged;
    if (!logged) {
        logged = 1;
        rt_log("regions: Region Intel (FN tab): %d layout row(s), %d row(s) for mod regions", (int)LD64(list + BP_COUNT),
               n > 0 ? n : 0);
    }
    static volatile int rows_logged;
    log_rows(&rows_logged, "Region Intel (FN tab)", (int64_t)LD64(list + BP_COUNT), BP_GAME_ROWS);
}

// main+0x84e084: x0 = panel, x1 = row widget, w2 = row. A mod region's row is the game's row 4 drawn from its block:
// the function reaches the blocks only through panel + row * 0xd0, so a shifted panel address does it.
int region_bp_row(Cpu *c) {
    uint32_t row = (uint32_t)c->x[2];
    uint64_t panel = c->x[0];
    if (row < BP_GAME_ROWS || !bp_on(panel + BP_LIST)) return 0;
    uint32_t k = row - BP_GAME_ROWS;
    if (k < LD64(panel + BP_LIST + BP_SHOWN)) {
        uint64_t b = panel + BP_EXTRA + (uint64_t)k * BP_BLOCK;
        rt_call(c, BP_ROW_FN, 3, b - BP_BLOCKS - (BP_GAME_ROWS - 1) * BP_BLOCK, c->x[1], (uint64_t)(BP_GAME_ROWS - 1));
    } else {
        c->x[0] = 1;   // a row with no region: the game's answer for a row past its own
    }
    return 1;
}

// ---- Region Intel, Map tab ("Mission Info")
static int mi_on(uint64_t list) {
    return ls_has(&mi_lists, list) && LD64(list + MI_TAG) == (list ^ MI_MAGIC);
}

void region_mi_init(uint64_t list) {
    uint64_t panel = list - MI_LIST;
    if (!pend_take(&mi_pend, panel) || !ls_add(&mi_lists, list)) return;
    for (uint64_t o = BP_PANEL_END; o < MI_PANEL_END; o += 8) ST64(panel + o, 0);
    ST64(list + MI_TAG, list ^ MI_MAGIC);
}

uint64_t region_mi_count(uint64_t list, uint64_t vanilla) { return mi_on(list) ? LD64(list + MI_COUNT) : vanilla; }
void region_mi_set_count(uint64_t list, uint64_t count) { if (mi_on(list)) ST64(list + MI_COUNT, count); }
uint64_t region_mi_block(uint64_t list, uint64_t vanilla) {
    return mi_on(list) ? vanilla - MI_OLD_BLOCK + MI_BLOCK : vanilla;
}
uint64_t region_mi_max(uint64_t list, uint64_t vanilla) { return mi_on(list) ? MI_MAX_ROWS : vanilla; }

// The mission counts of the mod regions (by slot), from the tree the Map tab's fill has the mission manager build
// (main+0x84e390 -> main+0xd51bc: a sorted tree of {region, count}, missions of any region id). Read after the tree is
// built (x1 = its root), each fill; a region with no missions has no node (0).
static uint32_t mi_missions[VSLOTS];

void region_mi_missions(uint64_t root) {
    for (uint32_t j = 0; j < mod_regions; j++) {
        uint32_t slot = mod_order[j], id = region_id(slot), n = 0;
        uint64_t node = root;
        for (int steps = 0; node && steps < 64; steps++) {   // a balanced tree of the game's regions: a few steps
            uint32_t key = LD32(node + 0x1c);
            if (key == id) { n = LD32(node + 0x20); break; }
            node = LD64(node + (key > id ? 0 : 8));
        }
        static volatile uint8_t logged[VSLOTS];   // once per region a boot
        if (n && !logged[slot]) {
            logged[slot] = 1;
            rt_log("regions: Region Intel (Map tab): mod region %u has %u mission(s)", id, n);
        }
        mi_missions[slot] = n;
    }
}

// main+0x85057c: x0 = context (+8 = the panel), x1 = row widget, w2 = row. Row 11 + k is mod region k (id order), as
// many as the layout has rows for: its name and its missions.
int region_mi_row(Cpu *c) {
    uint32_t row = (uint32_t)c->x[2];
    if (row < MI_GAME_ROWS) return 0;
    uint64_t panel = LD64(c->x[0] + 8), widget = c->x[1];
    if (!panel || !mi_on(panel + MI_LIST)) return 0;
    if ((uint64_t)row + 1 >= LD64(panel + MI_LIST + MI_COUNT)) return 0;   // the last row: Mira's, hidden here
    static volatile int rows_logged;   // the layout's last row is Mira's, which this view hides
    log_rows(&rows_logged, "Region Intel (Map tab)", (int64_t)LD64(panel + MI_LIST + MI_COUNT), MI_GAME_ROWS + 1);
    uint32_t k = row - MI_GAME_ROWS, region = k < mod_regions ? mod_order[k] : ~0u;
    if (region != ~0u && widget) {
        uint64_t sp = c->sp;
        c->sp = (sp - 0x40) & ~15ull;
        uint64_t str = c->sp;
        c->x[8] = str;
        rt_call(c, RW_NAME, 1, (uint64_t)region_id(region));
        rt_call(c, MI_SET_TEXT, 2, widget, str);
        rt_call(c, RW_FREE_STR, 1, str);
        c->sp = sp;
        rt_call(c, MI_SET_COUNT, 2, widget, (uint64_t)mi_missions[region]);
        static volatile int logged;
        if (!logged) { logged = 1; rt_log("regions: Region Intel (Map tab): mod region %u in row %u", region_id(region), row); }
    }
    c->x[0] = 1;
    return 1;
}

// ---- Rows for the panels' layouts (runtime/region_layout.c)
uint32_t region_mod_count(void) { return mod_regions; }

uint32_t region_panel_rows(RegionPanel panel) {
    uint32_t room = panel == REGION_PANEL_INTEL       ? RI_MAX_ROWS - RI_GAME_ROWS
                    : panel == REGION_PANEL_SHORTCUTS ? SC_MAX_ROWS - SC_GAME_ROWS
                                                      : BP_MAX_ROWS - BP_GAME_ROWS;
    return mod_regions < room ? mod_regions : room;
}
