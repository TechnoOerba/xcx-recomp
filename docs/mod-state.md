# Mod state

A game-file mod (`mods/<id>/romfs/`, [installer.md](installer.md)) can add content that has to remember things: an
event that happened, a place found, a survey reward taken. The game keeps such things as flags and marks in its save,
but every flag and mark in the save already means something to the game. A mod that borrowed some would clash with
another mod borrowing the same ones, could run out, and would leave values in the save that the game without the mod
reads as its own.

So the recomp gives mods their own flags and survey reward marks, and keeps them in a file of their own next to each
save slot, never in the game's save data. A save made with mods loads the same without them, and the mods' progress is
still there when they are turned on again.

## For mod authors

- **Flags:** every flag id from `0xC000` (49152) to `0xFFFE`, of any flag type, belongs to mods. Use them in your
  tables wherever the game takes a flag id (a condition, a discovery flag, an event flag). The game's own data uses ids
  up to about 41200, and `0xFFFF` is the game's "no flag". A mod flag keeps the value it was set to as a whole 32-bit
  number, whatever its type.
- **Survey rewards:** a `SEG_ExploreReward` row whose `flag_index` is `0x8000` (32768) or more keeps its claimed mark
  in the mod state. The game's rows use 1 to 260.
- **mod.json**, at the top of the mod's folder (`mods/<id>/mod.json`, next to `romfs/`), names what the mod uses:

  ```json
  {
    "id": "ashen-shoal",
    "version": "1",
    "regions": [ { "name": "ashen-shoal", "id": 24 }, { "name": "ashen-reef", "id": 25 } ],
    "flags":   [ { "name": "discovered/ashen-reef", "type": 1, "id": 49153 } ],
    "rewards": [ { "name": "ashen-reef/5", "index": 32788 } ]
  }
  ```

  The recomp gives every region, flag and reward a number of its own ([mod-ids.md](mod-ids.md)): `id` and `index` are
  the numbers the mod's files use (optional for flags and rewards that the mod's JSON tables name as `@flag:` and
  `@reward:`), and another mod that picked the same ones doesn't take them. The state is stored by mod id and name,
  not by number, so a later version of the mod may renumber its flags and rewards and keep the player's progress, as
  long as the names stay. `mod.json` may also name new rows and FrontierNav sites (`rows`, `sites`, [mod-ids.md](mod-ids.md)); `regions` may still
  be a list of bare numbers, as older files have it. `id` defaults to the folder's name; like folder
  ids it is lower case letters, digits, `.`, `_` and `-`, at most 63 bytes. Names are at most 95 bytes, with no
  quote, backslash or control character (UTF-8 is fine). `regions` lists the map regions that are the mod's own land
  (see "Leaving a mod's land"), up to 16, by the numbers they were given. `type` is the game's flag type (the first argument of its flag test). A
  mod.json that breaks a rule, or isn't JSON (nested at most 32 deep), is left out with a log line; its saved state is
  kept.
- A number another mod uses is handed out anew ([mod-ids.md](mod-ids.md)) and the mod's tables are rewritten to match;
  the log says which moved. A number outside the ranges above, a name declared twice in one mod, or a second name with
  the same number: left out, with a log line. A `mod.json` in this form (regions as names, rows, sites) needs a build with
  the hand-out: an older one reads it as not JSON and uses none of it. Flags and rewards in the mods' ranges that no enabled mod declares still work and are kept, under the owner
  `_undeclared`.
- The mod state never changes the game's own flags or marks.

## The file

`<save folder>/mods/state-<slot>.json`, one per save slot (`gamedata01` to `gamedata03`, and `gamedataa` for the
autosave). Right before the save's Commit the recomp writes the file of each slot the game wrote, into the save's
working copy, so it is committed, backed up and copied with the save itself. It records the save's time; when the
game loads a slot, the file is used only once the loaded game holds that same time. A file left from an older save in
that slot (one made without the recomp, or with mod state off) is never applied.

The file is JSON with keys in a fixed order: `format` (1), `mods` (one section per mod: `flags`, `rewards`,
`version`), `player`, `save_time`, `slot`, any top-level parts this version doesn't know, `written_by`, and a
`checksum`: FNV-1a 64-bit, as 16 hex digits, over every byte before the line that holds it. A file whose checksum
doesn't match (a hand edit voids it), or whose `format` is newer than this version writes, is not used and is renamed
to `state-<slot>.json.bad`, so it is never overwritten; the log says so.

## Rules

- **Mods that are off.** A mod that is off keeps its section of the file as it was: it is copied into every later
  file, so turning the mod on again brings its progress back. This also holds when mods are switched while the game
  runs (the recomp follows the enabled mods as the file loader reads `mods/romfs-enabled.txt`, and a mod updated in
  place has its `mod.json` read again). Top-level parts of the file this version doesn't know are kept as they are;
  inside a mod's section only `flags`, `rewards` and `version` are kept.
- **Loads.** A slot's state replaces the one in memory when the slot is loaded. The save and load screens read every
  slot whole to list them; such a read changes nothing: the file of another save never matches the game's save time,
  and the save in play is read again from its file only after the title screen (loading it again from there drops
  what wasn't saved, as the game does). A save loaded without a state file starts empty: when the game holds a save
  time the state isn't for, with no save being written and no load waiting for its file, for two seconds, the state
  is cleared. Values the game set since that load began are kept. A New Game is meant to be caught the same way.
- **Leaving a mod's land.** The file records the region the player saved in and the last spot outside every mod's
  `regions`, sampled twice a second while playing (a spot counts once the next sample is outside too: right after a
  warp the position changes before the region does). A save made on a mod's land and loaded with that mod off would put
  the party in the sea or a void; the party is moved to that spot instead (all four positions of the game data, before
  the world loads them), and the log says so.

## Features of the runtime that mods switch on

The same file can hold state for the runtime's own features, so a feature built into the recomp can ship as a mod
that a player turns off like any other:

- **Features.** A mod's `mod.json` may name features, for example `"features": ["example.extra-rules"]` (ids follow
  the mod id rule, up to 8). A feature is on while an enabled mod names it. If several do, the one highest in the
  priority list owns it (`mod_state_feature` in `runtime/mod_state.h`). The mod needs no game files. To tune the
  feature, it can change the game tables the feature reads, which the bdat merge combines like any other change
  ([bdat-merge.md](bdat-merge.md)).
- **Values.** Runtime code keeps per-save numbers of its own in a `values` object inside a section
  (`mod_state_value`, `mod_state_set_value`), in one of two places:
  - Under the id of the mod that turns the feature on. While that mod is off, the values stay dormant like its flags,
    and they come back when it is on again.
  - Under a runtime owner the code registers (`mod_state_owner`), whose section is always read.

  A section without values is written exactly as before. A build without the feature keeps the section as it was.
- **Loads.** `mod_state_loads()` changes each time the state is replaced: a slot loaded, another save or a New Game,
  or mods switched. `mod_state_current()` is 1 only while the state belongs to the save the game holds. So a feature
  reads its values again after each load, and it writes nothing while a load is under way.

## Setting

`mod_state` in `xcx.ini` (`[files]`), or `XCX_MOD_STATE`: `1` (default) on, `0` off (any other value is on). Off,
mods' flag ids reach the game's own flag storage as they would without this feature, and no state file is written.
Read at the first frame.

## How it works

The code generator (`recomp/mod_state.py`) adds five hooks to the 2.0.0 build's main module, each site checked
against the instruction it expects when the code is generated:

- the entry of the game's flag test (main+0xc96d0) and flag set (main+0xc94a0): for a mod flag id the value comes
  from or goes to the mod state, and the game's function doesn't run;
- the survey reward grant's entry (main+0x561b90), its claimed-mark test (main+0x561cd0) and its claim
  (main+0x561f78): for a mod row the mark comes from the mod state and the claim leaves the game's 320-bit block as
  it was.

The runtime side is `runtime/mod_state.c`. `runtime/fs_host.c` tells it which slot files the game wrote and which one
it read to the end (a load), and calls it right before each Commit. `runtime/romfs_mods.c` lists the enabled mods and
tells from the files the game loads whether it is at the title screen or in the world (the game data stays in place
at the title, so this is how a load is told from the save screen listing the slots). `runtime/boot.c` calls it once a
frame.

## Cost

- A flag test or set of the game's own flags: one comparison. A mod flag: a lock and a search of the enabled mods'
  declarations and of the stored values (a few microseconds with a few hundred of each).
- Once a frame: a lock and a few reads of game memory. Twice a second, while a mod with a `mod.json` is on: the party
  leader's position is sampled. The enabled mods' `mod.json` files are read when the enabled mods change, under the
  lock the mod flag calls wait on (a few milliseconds at most for small files).
- Each save with mods: one small JSON file per slot written, without a flush of its own (the Commit flushes the save).
- With no game-file mod that has a `mod.json`, and no mod flag ever set, nothing is written or logged.

## Limits

- Only the 2.0.0 update's build gets the hooks (as with the game-file mod loader); on other builds mod flags reach the
  game's storage.
- Changing a mod's tables needs a restart, as for any data the game reads at start-up; `mod.json` is read again when
  the enabled mods change.
- A load whose game data takes more than a minute to hold the save's time after its slot was read is not given its
  file.
- The spot outside mod land is the last one sampled; a save made on a mod's land in a session that never left it uses
  the spot from the file it loaded.
