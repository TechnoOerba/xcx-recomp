// Mod state: what game-file mods add that must last (their flags, their survey rewards' claimed marks), kept in a
// JSON file per save slot inside the save folder, never in the game's save data (runtime/mod_state.c,
// docs/mod-state.md). The game side is recomp/mod_state.py.
#pragma once
#include "cpu.h"
#include <stdint.h>

// The flag ids that belong to mods: every id from here up, of any flag type, but 0xFFFF (the game's "no flag"). The
// game's tables keep flag ids in 16-bit cells, so mods' ids must fit there too; the game's own data uses ids up to about
// 41200 (the highest: SYS_ClearRewriteFlags, FLD_ConditionList_Flag), so 0xC000..0xFFFE (16383 ids) is left to mods.
#define MOD_FLAG_FIRST 0xC000u
#define MOD_FLAG_NONE 0xFFFFu
#define MOD_IS_FLAG(id) ((id) >= MOD_FLAG_FIRST && (id) != MOD_FLAG_NONE)
// The survey reward rows (SEG_ExploreReward.flag_index, a 16-bit cell) whose claimed mark belongs to mods: from here
// up. The game's claimed marks are a 320-bit block in its save (flag_index 1-260 in the game's data).
#define MOD_REWARD_FIRST 0x8000u

// Called by the generated C at the entry of the game's flag test (main+0xc96d0: w0 = flag type, w1 = id) and flag
// set (main+0xc94a0: w0 type, w1 id, w2 value, w3 notify): for a mod flag they answer from the mod state and return 1
// (the game's function doesn't run; the game's notifications only cover ids far below mod flags), else 0.
int mod_flag_get(Cpu *c);
int mod_flag_set(Cpu *c);
// The survey reward grant (main+0x561b90), at its claimed-mark test (word = the game's word for the row's mark, mask =
// the row's bit in it, index = the row's flag_index) and at its claim (word = the word with the bit set, old = the
// word in the game's block before): for a mod row the answer comes from and goes to the mod state, and the game's
// block is left as it was.
void mod_reward_begin(void);   // the grant's entry: no mod row tested yet in this call
uint64_t mod_reward_test(uint64_t index, uint64_t word, uint64_t mask);
uint64_t mod_reward_claim(uint64_t word, uint64_t old);

// 1 when name is a save slot's data file: "gamedata" and more, no '.' (not its .tmb picture) and no separator,
// at most MOD_STATE_SLOT_MAX - 1 bytes (the game's are gamedata01 .. gamedata03 and gamedataa).
#define MOD_STATE_SLOT_MAX 24
int mod_state_slot_file(const char *name);
// fs_host.c: a save slot's data file was opened for writing.
void mod_state_slot_written(const char *name);
// fs_host.c, the save Commit, before it snapshots the working copy (root = the save mount's working copy): writes the
// mod state into root/mods/state-<slot>.json for each slot written since the last Commit.
void mod_state_before_commit(const char *work_root);
// fs_host.c: a save slot's data file was read to its end (the game is loading that slot, or listing it in its save and
// load screens), and the folder the game reads its save from (the working copy, which the Commit thread never writes).
void mod_state_save_read(const char *name);
void mod_state_set_save_dir(const char *save_dir);
// Every present: loads the mod state of a slot the game loaded, follows mods switched on or off, and samples the spot
// to return to when a save made on a mod's land loads with that mod off.
void mod_state_update(void);

// ---- Runtime values and features
// For the runtime's own features (not game-file mods): numbers kept per save in the same file, and features a mod
// turns on by naming them in its mod.json ("features": ["example.extra-rules"], ids by the mod id rule). This is
// how a feature built into the runtime is shipped as an opt-in mod: its code is here, a mod switches it on (and may
// tune it through the game's tables, bdat-merge.md), and its numbers live in that mod's section of the file, so they
// stay dormant while the mod is off and come back with it.
int mod_state_on(void);   // 0 with XCX_MOD_STATE=0: then nothing below keeps anything
// A runtime owner: its section ("values") is read and written whatever mods are on. Call before the first load.
void mod_state_owner(const char *id);
// 1 when an enabled mod names this feature (the top one by priority), its id into owner.
int mod_state_feature(const char *feature, char *owner, size_t cap);
// A runtime value of an owner (an enabled mod or a runtime owner) in the state of the save the game holds; names are
// at most 95 bytes, no quote, backslash or control character. Setting one for an owner that is off does nothing.
int mod_state_value(const char *owner, const char *name, uint32_t *v);
void mod_state_set_value(const char *owner, const char *name, uint32_t v);
// Bumped each time the state is replaced (a slot loaded, another save or a New Game, mods switched): read the values
// again. mod_state_current: 1 while the state is the one of the save the game holds (0 during a load and before it).
uint32_t mod_state_loads(void);
int mod_state_current(void);
