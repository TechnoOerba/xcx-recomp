// Loose RomFS mod files in place of sts.ard entries, switchable while the game runs (runtime/romfs_mods.c; the game
// side is recomp/romfs_mods.py).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Called by the generated C where the archive device loads its list of declined paths (prefixes, NUL-terminated,
// ended by an empty string) for the path operation on path (a guest string): a list holding just that path when an
// enabled mod has a file for it, so the game opens rom:/<path> instead; else list unchanged.
uint64_t romfs_mod_declines(uint64_t list, uint64_t path);
// Another part of the runtime's claims on those lookups: claims(key) (the path as the lookup has it: lowercase, no
// mount) returns 1 for a path the game should open through rom:/ although no mod has it, because that part serves it
// (runtime/region_layout.c, set by runtime/regions.c when the first mod region appears). Set once; NULL = none.
void romfs_mods_claim_also(int (*claims)(const char *key));

// The host path of an enabled mod's file for a RomFS path ("rom:/chr/..." or "chr/..."), copied to out (may be NULL);
// 0 when none.
int romfs_mod_file(const char *path, char *out, size_t cap);

// The scene, from the files the game loads while a mod is enabled (runtime/mod_state.c reads it): 1 when the title
// screen's files (ev/title/) came after the world's last (gimmick/world_info.bin, map/mapcoli/); and how many times the
// world loaded after the title.
int romfs_mods_at_title(void);
uint32_t romfs_mods_world_loads(void);
// The enabled mods' ids as this file reads romfs-enabled.txt (valid ids, in order, without repeats; each 65 bytes); how
// many. A generation bumped whenever the list changed or a mod was updated in place. And, once any mod declares state
// (runtime/mod_state.c), the scene above is followed even while no mod has files on.
#define ROMFS_MODS_MAX 32
int romfs_mods_enabled(char (*ids)[65], int cap);
uint32_t romfs_mods_generation(void);
void romfs_mods_watch_scene(int on);
// The mods folder (XCX_MOD_DIR, else "mods" beside xcx.exe); NULL when there is none.
const char *romfs_mods_dir(void);
// Rebuilds the file map at the next lookup, for a mod whose files changed (an update) while its id stayed listed.
void romfs_mods_reload(void);
// Turns a mod on (first in romfs-enabled.txt, so its files win) or off; 1 when the list was written. The game picks
// the change up within CHECK_MS of romfs_mods.c.
int romfs_mods_set(const char *id, int on);
