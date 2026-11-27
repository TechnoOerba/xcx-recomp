// Numbers handed out to content mods (runtime/mod_ids.c, docs/mod-ids.md): a mod's mod.json names the region ids, mod
// flags, survey reward indices and new rows it adds; the recomp gives each name a number no other enabled mod has, keeps
// it across starts in <profile>/mod-ids.txt, and the data table merge (runtime/bdat_merge.c) and the mod state
// (runtime/mod_state.c) use the numbers given.
#pragma once
#include "bdat_merge.h"
#include <stddef.h>
#include <stdint.h>

#define MOD_ID_NAME 96     // a name's bytes, with its NUL
#define MOD_ID_REFS 8      // (table, column) pairs a row may name

// FrontierNav site numbers handed out: a site's number is its FnetVeinList row, and the game's FrontierNav manager
// holds 144 sites (rows 0-143; the game's own are 0-109).
#define MOD_SITE_FIRST 110u
#define MOD_SITE_LAST 143u
// The highest site row a mod's own copy may use (its local number, moved to a handed-out one): the site table's
// link columns are a byte.
#define MOD_SITE_LOCAL_LAST 255u

// One thing a mod.json names (kind: BDAT_ID_*).
typedef struct {
    uint8_t kind, has_local;
    char name[MOD_ID_NAME];
    uint32_t local;            // the number the mod's own files use for it
    uint32_t type;             // a flag's type (the game's flag test's first argument)
    uint32_t table;            // a row: its table's name hash
    uint32_t refs[2 * MOD_ID_REFS];
    int nrefs;
} ModIdDecl;

#define MOD_MANIFEST_FEATURES 8   // features a mod.json may name (mod_state_feature)

typedef struct {
    char id[64], version[32];
    char features[MOD_MANIFEST_FEATURES][64];
    int nfeatures;
    ModIdDecl *decls;          // malloc'd
    int n;
} ModManifest;

// runtime/mod_state.c: mods/<folder>/mod.json read (the one reader of that file). 1 read, 0 the mod has none, -1 it
// breaks a rule (logged; nothing of it is used). Free with mod_manifest_free.
int mod_manifest_read(const char *folder, ModManifest *m);
void mod_manifest_free(ModManifest *m);

// The hand-out for the enabled mods as romfs_mods.c lists them now (made again when its generation changes).
// The number the entry (kind, table for a row, name) of the mod whose mod.json "id" is mod was given: 1, or 0 when it
// has none (not declared, or no number was free).
int mod_ids_number(const char *mod, int kind, uint32_t table, const char *name, uint32_t *out);
// Makes the hand-out now if the mods changed since the last one: callers that hold a lock others wait on (the mod
// state's) call it first, so the reads and the records file's write happen outside that lock.
void mod_ids_refresh(void);

// Everything the mod in folder mod (its id in romfs-enabled.txt) was given, as the data table merge takes it: one malloc'd block (free it), NULL when none (*n = 0).
BdatId *mod_ids_of(const char *mod, int *n);
// 1 when one of the numbers of the mod in folder mod is not the one its own files use: its data tables must be rewritten even when it is the
// only mod that has them.
int mod_ids_moved(const char *mod);
// Line i (from 0) of what the hand-out did, for the Mods tab; 0 past the
// last: first a line per mod ("ashen-shoal: regions 24, 25; 46 flag(s), ...; 0 moved from its own number(s)"), then
// one per number moved or name that got none. *kind: 0 nothing moved, 1 moved, 2 a name that got none.
int mod_ids_status(int i, char *out, size_t cap, int *kind);

// ---- the rules alone (tests/mod_state_test.py drives them through tests/mod_state_check.c)
typedef struct {
    char mod[64];
    uint8_t kind;
    uint32_t table;
    char name[MOD_ID_NAME];
    uint32_t number;
} ModIdRecord;

typedef void (*ModIdLog)(void *ctx, int kind, const char *line);
// The hand-out: mods[0..n) highest priority first with their declarations; recs (the file's records, *nrecs of them,
// room for cap) updated in place: a name with a record keeps its number while no one else holds it, the rest get their
// own number when free, else the first free one of their kind; when a kind runs out, the oldest records of mods that
// are off give theirs back. numbers[i][d] gets the number of mods[i].decls[d] (UINT32_MAX: none). 0 when out of room.
int mod_ids_assign(const ModManifest *mods, int n, ModIdRecord *recs, int *nrecs, int cap, uint32_t **numbers,
                   ModIdLog log, void *ctx);
// The records file: one line per record ("<kind> <table hex or -> <number> <mod> <name>"), # comments.
// *bad: the lines that are not comments and don't read as a record.
int mod_ids_parse(const char *text, size_t len, ModIdRecord *recs, int cap, int *bad);
char *mod_ids_format(const ModIdRecord *recs, int n);   // malloc'd text
