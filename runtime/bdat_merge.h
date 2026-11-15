// Merging the BDAT data tables of several game-file mods (runtime/bdat_merge.c; runtime/romfs_mods.c calls it). The
// rules: docs/bdat-merge.md.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *name;     // for events: a mod id, "game"
} BdatInput;

enum { BDAT_FILE_MERGED, BDAT_FILE_COPIED, BDAT_FILE_WHOLE };

typedef struct {
    int tables_changed;   // tables at least one mod changes
    int tables_merged;    // of those, merged from two or more mods
    int tables_whole;     // changed by several but taken whole from the top one (an opaque change)
    int priority;         // events: values or rows two mods changed, settled by priority
    int left_out;         // events: a mod's change not used
    int caveats;          // events: used, but may not behave as its mod expects
    int file_mode;        // BDAT_FILE_*: merged; one mod's file as it is (the others can't be used); the top mod's
                          // whole file (it has another list of tables)
    int file_mod;         // the mod of BDAT_FILE_COPIED / BDAT_FILE_WHOLE, else -1
} BdatSummary;

// One line per event: "priority: ...", "left out: <mod>: ...", "caveat: <mod>: ...".
typedef void (*BdatNote)(void *ctx, const char *line);

// base: the game's own file. mods: their copies of it, highest priority first (romfs-enabled.txt order), n >= 1.
// Returns the merged file (malloc'd, size in *out_n), or NULL with the reason in why when nothing can be merged (the
// game's copy can't be read, or no mod's can). A mod whose copy can't be used is left out and reported.
uint8_t *bdat_merge(const BdatInput *base, const BdatInput *mods, int n, size_t *out_n, BdatSummary *sum,
                    BdatNote note, void *ctx, char *why, size_t why_cap);

// Every table of data rebuilt by the merger's writer, for the round-trip check (tests/bdat_merge_check.c): the
// count of tables whose rebuilt bytes differ from the file's own, or -1 when data isn't a file it reads.
int bdat_rewrite_check(const uint8_t *data, size_t size);

// For runtime/romfs_mods.c: the host path of the merged copy of RomFS file key ("bdat/common.bdat") made from the
// enabled mods' copies hosts[0..n) (ids[i] names hosts[i]; highest priority first), the base read from the game's
// archive on mount. Made once per set of inputs and kept in <profile>/bdat-merge/. 0 when it can't be merged: the
// caller serves hosts[0], as without merging.
int bdat_merge_serve(const char *mount, const char *key, char *const *hosts, char *const *ids, int n, char *out,
                     size_t cap);

// Line i (from 0) of what the merges did this run, for the F1 Mods tab; 0 past the last. *kind: 0 a summary line,
// 1 a change left out, 2 a caveat.
int bdat_merge_status(int i, char *out, size_t cap, int *kind);
