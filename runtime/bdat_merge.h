// Merging the BDAT data tables of several game-file mods (runtime/bdat_merge.c; runtime/romfs_mods.c calls it).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *name;     // for notes: a mod id, "game"
} BdatInput;

typedef struct {
    int tables_changed;   // tables at least one mod changes
    int tables_merged;    // of those, merged cell by cell from two or more mods
    int tables_whole;     // changed by two or more mods but taken whole from the top one (the others' changes lost)
    int cells_overridden; // cells two mods set to different values (the earlier mod's value is kept)
} BdatSummary;

// One line per clash or lost table, for the log and the report file.
typedef void (*BdatNote)(void *ctx, const char *line);

// base: the game's own file. mods: their copies of it, highest priority first (romfs-enabled.txt order), n >= 1.
// Per table: unchanged -> the base's bytes; changed by one mod -> that mod's bytes; changed by several -> their cell
// edits merged (the earlier mod wins a cell both set) plus rows appended at the end by one of them (or the same rows
// by several); a table that several change where one change isn't cell edits (other columns, another base id, rows
// removed, a row given another row's key) or two append different rows -> the top mod's table, the rest noted as lost.
// Returns the merged file (malloc'd, size in *out_n), or NULL with the reason in why when the files can't be merged
// (not a BDAT file of this layout, a damaged table, another list of tables).
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

// Line i (from 0) of what the merges did this run, for the F1 Mods tab; 0 past the last.
int bdat_merge_status(int i, char *out, size_t cap);
