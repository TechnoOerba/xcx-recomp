// Panel rows for mod regions made from the game's own menu layouts (runtime/region_layout.c, docs/regions.md).
#pragma once
#include <stddef.h>
#include <stdint.h>

// The panels with a row per mod region, and the rows the game's own layout of each has (its lists in
// runtime/regions.c fill as many).
typedef enum { REGION_PANEL_INTEL, REGION_PANEL_SHORTCUTS, REGION_PANEL_FN } RegionPanel;
#define REGION_INTEL_GAME_ROWS 12       // Region Intel (Segments and Map tabs): 11 regions and Mira
#define REGION_SHORTCUTS_GAME_ROWS 8    // Map Shortcuts
#define REGION_FN_GAME_ROWS 5           // Region Intel on the FrontierNav tab: the five continents
// runtime/regions.c: regions that got a mod hex table this boot, and how many of them a panel has room for.
uint32_t region_mod_count(void);
uint32_t region_panel_rows(RegionPanel panel);

// A layout's rows (mc_ListNN components): whether the panel moves up a row per row added (it reaches the screen's
// bottom), how much wider the first added row makes it and the rects that move right with its columns.
typedef struct {
    RegionPanel panel;
    int game_rows, move_up;
    double widen;
    const char *const *widen_also;
} RegionLayoutKind;
// The functions below work on a layout file in memory and keep no state between calls.
// The mc_ListNN rows of a menu layout (LAGP), -1 when it isn't one.
int region_layout_rows(const uint8_t *data, size_t n);
// Region Intel's divider lines (rect_LineNN rects in mc_Top), -1 when it isn't a layout.
int region_layout_lines(const uint8_t *data, size_t n);
// Adds rows to the layout in *data (reallocated): the rows added (fewer when the layout's shape stops it; a row that
// fails leaves the file as it was before that row).
int region_layout_grow(uint8_t **data, size_t *n, int rows, const RegionLayoutKind *kind);
// The scale at which the layout's panel fits in the screen's band (1: as it is; 0: not a shape this reads); with
// apply, the panel is scaled and moved so (0 when it has no scale keys to do it with).
double region_layout_fit(uint8_t *data, size_t n, int apply);
#define REGION_LAYOUT_MIN_SCALE 0.5
// As many of rows as fit at REGION_LAYOUT_MIN_SCALE or more: added and the panel fitted; its scale in *scale.
int region_layout_grow_fit(uint8_t **data, size_t *n, int rows, const RegionLayoutKind *kind, double *scale);

// At the game's RomFS opens and archive lookups (runtime/fs_host.c, runtime/romfs_mods.c): for a panel layout while mod
// regions exist, the host path of the layout with a row for each (made from the file the game would read: a mod's
// copy, else its own) in out (out NULL: only whether there is one) and 1; else 0.
int region_layout_file(const char *path, char *out, size_t cap);
