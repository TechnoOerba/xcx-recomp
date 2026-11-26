// Data-driven regions: hex tables a mod ships for map regions the game has none for (runtime/regions.c; the game side
// is recomp/regions.py).
#pragma once
#include "cpu.h"
#include <stdint.h>

// Called by the generated C right after the segment manager asked the game's region -> hex table switch for region
// (x0 = table handle, low byte of w1 = found). When the game found none, a table named SEG_TerminalZone<region> in
// the loaded data takes its place; else the result is left as it is.
void region_hex_table(Cpu *c, uint64_t region);

// Called by the generated C in the minimap's mode switch with the region and the case byte the game's table gave it:
// a region that got a mod hex table above takes the field case (hexagon outline, as on the continents); any other
// keeps the game's byte.
uint64_t region_minimap_case(uint64_t region, uint64_t byte);

// Called by the generated C after the game looked up the Segments tab's marker table for region (x0 = table, 0 for
// none): for a region that got a mod hex table, a table named MNU_SegMapInfoZone<region> takes the place of none.
void region_segmap_table(Cpu *c, uint64_t region);

// Called by the generated C at the entry of the Segments tab's two field layer setups (x0 = the map; markers and
// hexagon cells): calls that setup's per-region builder (a guest function) for each region that got a mod hex table;
// the game then builds its continents' layers.
void region_field_layers(Cpu *c, uint64_t builder);

// Called by the generated C after the Segments tab filled its list of regions to update and draw (map = the map
// object, keep = the value each entry keeps at +0x20): appends an entry for each region that got a mod hex table.
void region_layer_entries(uint64_t map, uint64_t keep);

// The Survey Reward List (Y on the Segments tab) pages through the game's 11 regions, then through each mod region
// (one that got a mod hex table above) that has SEG_ExploreReward rows. Each entry function takes over the game's
// function of the same job when there is at least one such page, and returns 1; it returns 0 (the game's own code
// runs, unchanged) when there is none. next/prev: x0 = the panel; jump: x0 = the panel, w1 = a region; header: x0 =
// the panel (the page's region name, survey rate and the page slider).
int region_reward_next(Cpu *c);
int region_reward_prev(Cpu *c);
int region_reward_jump(Cpu *c);
int region_reward_header(Cpu *c);
// The panel's page -> region lookups the hooks above don't cover give 0 for a page past 10; this gives that page's mod
// region instead (vanilla = the game's answer).
uint64_t region_reward_region(uint64_t panel, uint64_t vanilla);

// Region Intel (hold ZL on the Segments tab) lists 11 regions and Mira in 12 rows, one per "mc_List%02d" node of its
// menu layout, and has room for exactly 12. When a mod region exists as the panel is made, these give the panel room
// for 12 + 48 rows and widgets past its end, and the fill puts the mod regions (in id order) between the game's 11 and
// Mira, as many as the layout has rows for (a layout with 12 rows: none; the panel is still made bigger, unused). With no mod
// region every function leaves the game's values. Called by the generated C:
// size: after the panel's allocation size is set (the game's size in, the size to allocate out);
// alloced: after the allocation (c->x[0] = the new panel); init: at the start of the list's constructor (x19 = the list);
// count / set_count / block / max: at each use of the list's widget count, widget block and widget limit (list =
// the list; the game's value in, the value to use out); fill: after the game filled the 12 rows; row: at the entry
// of the row callback (returns 1 when it drew a row past the game's 12); free: at the panel's destructor's entry.
uint64_t region_intel_size(uint64_t size);
void region_intel_alloced(Cpu *c);
void region_intel_init(uint64_t list);
uint64_t region_intel_count(uint64_t list, uint64_t vanilla);
void region_intel_set_count(uint64_t list, uint64_t count);
uint64_t region_intel_block(uint64_t list, uint64_t vanilla);
uint64_t region_intel_max(uint64_t list, uint64_t vanilla);
void region_intel_fill(Cpu *c, uint64_t panel);
int region_intel_row(Cpu *c);
void region_intel_free(Cpu *c);


// Map Shortcuts (X on the map) lists 8 fixed entries. When a mod region exists as the panel is made, the panel's
// list moves past its end with room for 8 + 48 (size / alloced / init / count / set_count / block / max, as for Region
// Intel above), and each mod region (id order) becomes an entry after the 8th, as many as the layout has rows for.
// The entry functions take over the game's function of the same job for such an entry and return 1, else return 0:
// name (w1 = entry, x8 = string out), row (the row callback), select (the pick), can_jump / jump (the map's: a mod
// region is shown as a continent is, at its MNU_MapZoneInfo place).
uint64_t region_shortcut_size(uint64_t size);
void region_shortcut_alloced(Cpu *c);
void region_shortcut_init(uint64_t list);
uint64_t region_shortcut_count(uint64_t list, uint64_t vanilla);
void region_shortcut_set_count(uint64_t list, uint64_t count);
uint64_t region_shortcut_block(uint64_t list, uint64_t vanilla);
uint64_t region_shortcut_max(uint64_t list, uint64_t vanilla);
int region_shortcut_name(Cpu *c);
int region_shortcut_row(Cpu *c);
int region_shortcut_current(Cpu *c);
int region_shortcut_select(Cpu *c);
int region_shortcut_can_jump(Cpu *c);
int region_shortcut_jump(Cpu *c);

// The FN tab's Region Intel ("Battle Probe Effects") in the same panel: 5 continents, each with its battle probe
// effects. With a mod region the panel (enlarged above) also has rows for the mod regions after the five, as many as
// the layout has rows for; the list's widgets move as above (init / count / set_count / block / max), fill runs after
// the game's fill (main+0x84dee0), row at the entry of the row function (returns 1 for a row past the game's 5).
void region_bp_init(uint64_t list);
uint64_t region_bp_count(uint64_t list, uint64_t vanilla);
void region_bp_set_count(uint64_t list, uint64_t count);
uint64_t region_bp_block(uint64_t list, uint64_t vanilla);
uint64_t region_bp_max(uint64_t list, uint64_t vanilla);
void region_bp_fill(Cpu *c, uint64_t panel);
int region_bp_row(Cpu *c);

// Far regions (ids 24..255; runtime/regions.c "Region ids past the slots"): each has a segment slot of its own past
// the manager's 24 (slot 25 + k for the k-th far region). region_slot is called by the generated C with each slot index
// the game is about to use (site = the instruction's main offset): a far id becomes its slot, any other value is
// returned as it is.
uint64_t region_slot(uint64_t id, uint64_t site);
// The region id for a slot number (a far slot's far id), else the value as it is: init_value at the segment init's uses
// of its counter, loop_value in the slot loops that pass their counter on.
uint64_t region_loop_value(uint64_t slot);
// At the entry of the manager's maker (main+0x3fcb0c) and of its init (main+0x3fd110, x0 = the manager): find the far
// regions once the game's data is loaded; the init's also zeroes a new manager's far slots.
void region_seg_create(Cpu *c);
void region_far_setup(Cpu *c);
// The manager's allocation size (main+0x3fcb48): room for the far regions' slots.
uint64_t region_mgr_size(uint64_t size);
// The slot loops: at the counter's increment, 1 when it reached 24 and far slots follow (the caller then steps the
// counter and its running pointers over slot 24); the end test's limit.
int region_loop_skip(uint64_t counter);
uint64_t region_slot_end(void);
// At the entry of main+0x3fe658 (a slot's survey rate, then the average of all slots): with far slots, does it over
// all slots and returns 1.
int region_seg_rates(Cpu *c);
// The survey reward list's panel: its allocation size (main+0x841508) and the allocation's result, the fill's entry
// (main+0x856040), the destructor's entry (main+0x854f20), and the fill's row zone -> vector index and its range test.
uint64_t region_rw_size(uint64_t size);
void region_rw_alloced(Cpu *c);
void region_rw_fill(Cpu *c);
void region_rw_free(Cpu *c);
uint64_t region_rw_index(uint64_t zone);
uint64_t region_rw_test(uint64_t index);
uint64_t region_rw_show_index(uint64_t panel, uint64_t region);
int region_probe_reveal(Cpu *c);
void region_mi_init(uint64_t list);
uint64_t region_mi_count(uint64_t list, uint64_t vanilla);
void region_mi_set_count(uint64_t list, uint64_t count);
uint64_t region_mi_block(uint64_t list, uint64_t vanilla);
uint64_t region_mi_max(uint64_t list, uint64_t vanilla);
int region_mi_row(Cpu *c);
// After the Map tab's fill built its mission count tree (root = the tree's root): the mod regions' counts.
void region_mi_missions(uint64_t root);
// At the entry of main+0x1823e8 ("region past 24": not a field region): a far region answers 0 (1 returned).
int region_field_test(Cpu *c);
// The discovery flag to set on entering region (the game's 6148 + id in vanilla): a far region's FLD_FieldNameList flg.
uint64_t region_flag(Cpu *c, uint64_t region, uint64_t vanilla);

// ---- Panel rows made from the loaded layout: runtime/region_layout.h
#include "region_layout.h"

