// Map mods shipped as pieces, combined with the game's own map files while the game runs (runtime/map_overlay.c,
// docs/map-mods.md).
#pragma once
#include <stddef.h>
#include <stdint.h>

// File handles of the served map files: 0x4_0000_0000 + j (RomFS uses 0x1_..., host storage 0x2_... and 0x3_...).
#define MAP_OVERLAY_HANDLE_BASE 0x400000000ull

// 1 when the overlay serves this game path (lowercase, no leading '/', no mount: "map/ma9990.wismda"). The first ask
// for any of the map files builds the overlay from the enabled mods (once per run). 0 when no enabled mod has map
// pieces: the game then reads its own files, or a mod's whole copies, as before.
int map_overlay_claims(const char *key);
// The same for a RomFS path ("rom:/map/ma9990.wismda").
int map_overlay_serves(const char *path);
// Opens a served map file for a RomFS path ("rom:/map/ma9990.wismhd"); 1 with *out set, 0 when the overlay does not
// serve it.
int map_overlay_open(const char *path, uint64_t *out);
int map_overlay_handle(uint64_t h);
uint32_t map_overlay_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
uint32_t map_overlay_size(int64_t *out, uint64_t h);
void map_overlay_close(uint64_t h);

// For the F1 Mods tab: whether the overlay was built this run (0 not yet, 1 on, -1 off: no map pieces could be used),
// and line i of what it said (a mod's entries, an entry it lost to a higher mod, a mod or file left out; *left_out
// set for the lines about something not used); 0 past the last line.
int map_overlay_state(void);
int map_overlay_line(int i, char *out, size_t cap, int *left_out);
// 1 when the enabled mods' map pieces are no longer what the overlay was built from (another list, a pieces.txt or a
// piece it serves changed): the map is put together once per run, so they apply at the next start. Looks at the
// files (a stat each): call it from a refresh, not every frame.
int map_overlay_changed(void);
