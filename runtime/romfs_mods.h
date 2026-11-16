// Loose RomFS mod files in place of sts.ard entries, switchable while the game runs (runtime/romfs_mods.c; the game
// side is recomp/romfs_mods.py).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Called by the generated C where the archive device loads its list of declined paths (prefixes, NUL-terminated,
// ended by an empty string) for the path operation on path (a guest string): a list holding just that path when an
// enabled mod has a file for it, so the game opens rom:/<path> instead; else list unchanged.
uint64_t romfs_mod_declines(uint64_t list, uint64_t path);

// The host path of an enabled mod's file for a RomFS path ("rom:/chr/..." or "chr/..."), copied to out (may be NULL);
// 0 when none.
int romfs_mod_file(const char *path, char *out, size_t cap);

// The mods folder (XCX_MOD_DIR, else "mods" beside xcx.exe); NULL when there is none.
const char *romfs_mods_dir(void);
// Rebuilds the file map at the next lookup, for a mod whose files changed (an update) while its id stayed listed.
void romfs_mods_reload(void);
// Turns a mod on (first in romfs-enabled.txt, so its files win) or off; 1 when the list was written. The game picks
// the change up within CHECK_MS of romfs_mods.c.
int romfs_mods_set(const char *id, int on);
// 1 when the mods that data tables the game read at start-up would be made from (which mods, in which order) are no
// longer the ones they were made from: the game reads those tables only at start-up, so it needs a restart.
int romfs_mods_tables_changed(void);
// Merges the data tables the game read at start-up from the mods as they are now, on a thread of its own, so the next
// start finds them made (bdat_merge_prepare; the F1 Mods tab's Merge now). 1 when it started; 0 when one is running
// or the game has read no data table yet.
int romfs_mods_premerge(void);
// 0: no merge made ahead for the mods as they are now; 1: one running; 2: made for them (what it said:
// bdat_merge_next_status, bdat_merge_next_mod_tag).
int romfs_mods_premerge_state(void);
// 1 when the enabled mod id has files other than data tables (bdat/...): turning it off changes more than tables.
int romfs_mod_has_other_files(const char *id);
// The ids of romfs-enabled.txt in order, the top (winning) mod first, into ids; the count.
int romfs_mods_priority(char (*ids)[65], int cap);
// Swaps the mod with the one above it (up) or below it in romfs-enabled.txt; 1 when the list was written. Files follow
// the new order the next time the game loads them, as with romfs_mods_set; the game reads its data tables at
// start-up, so a new order for those applies at the next start.
int romfs_mods_move(const char *id, int up);
