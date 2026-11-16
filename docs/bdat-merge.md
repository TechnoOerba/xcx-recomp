# Merging data tables from several mods

Game-file mods change the game's data by shipping a whole `bdat/common.bdat` (and `bdat/<language>/common_ms.bdat`
for text). When two or more enabled mods have the same one, the game merges them (`runtime/bdat_merge.c`) instead of
using only the top mod's copy. The merged file and a report of what happened go to `<profile>/bdat-merge/`.

The merge follows four aims, in this order of weight when they pull apart:

1. **Priority settles like changes.** When two mods change the same thing, the one higher in `romfs-enabled.txt`
   (the F1 Mods tab's **Mod priority** list) wins.
2. **The result holds together.** The aim is not that every mod's every change survives, but that the merged data is
   consistent enough that the game isn't broken.
3. **When a change can't be combined safely, it is left out**, even when priority would allow it. Leaving the game's
   own data (or the other mod's) in place is the default.
4. **Everything that didn't go to plan is reported**: in the game's log, the report file next to the merged file, and
   the Mods tab. The point is understanding what to expect, not necessarily acting on it.

## What a mod changed

Each mod's copy is compared with the game's own file, table by table. Tables are matched by their name; rows are
matched as follows.

- **By key.** Most tables (all text tables, and 677 of the 704 tables of `common.bdat` in 2.0.0) start with a hash
  column whose values are unique per row: the row's key. Rows with the same key are the same row wherever they sit.
  Rows left over on both sides that sit between the same matched rows are paired in order when at least half of their
  other cells are equal: a row whose key a mod renamed. What remains is a row the mod removed, or one it added.
- **By row number**, in tables without unique keys. Then a mod may only edit rows in place and add or remove rows at
  the end; a row given another row's key means rows moved, and the table can't be compared row by row.

So a mod's change to a table is a set of cell edits, removed rows, added rows and, when its rows come in another
order or rows were removed or added before the end, a new **row layout**. A mod whose table has other columns or
another first row number, or whose rows can't be matched, has an **opaque** change: the table only as a whole.

## How changes combine

Per table:

| Mods changing it | Result |
|---|---|
| none | the game's own table, as it is |
| one | that mod's table, as it is |
| several | the rules below |

- **Opaque changes.** If the top mod changing a table has an opaque change, its table is used whole and the other
  mods' changes to it are left out. A lower mod's opaque change is left out.
- **Row layout.** Of the mods with a new row layout, the top one's is used: its rows, in its order. A lower mod's
  removals and reordering are left out; the rows it added are kept, at the end. A lower mod whose layout is the same
  as the top one's (the same game rows, in the same order, with its added rows in the same places) loses nothing,
  like two mods setting a cell alike, and isn't reported.
- **Removed rows.** A row the layout's mod removed stays removed, unless a higher mod edits it or another mod's
  changes refer to its key. Then it is kept, but only where it doesn't move any other row: a row put back in the
  middle would renumber every row after it, and other tables may point at rows by number. A row that can't be put
  back stays removed, and the edits or references to it are reported.
- **Cells.** On the rows that remain, every mod's cell edits are applied. Where two mods set one cell to different
  values, the higher mod's value is kept.
- **Added rows.** Rows mods add are kept, after the layout's rows. Two mods adding a row with the same key: the
  higher mod's row is kept. Keys stay unique: a renamed or added key that another row already has gives way to the
  higher mod's row, and the lower change is left out.
- **Row numbers.** Rows that end up at another number than in a mod's own copy are reported for that mod: its other
  tables may point at them by number (text ids, for one).

A mod whose file has another list of tables, or that can't be read, is not merged: if it is the top mod its whole
file is used (with the other mods' changes to that file left out), otherwise its changes to that file are left out.

## Tables as JSON

Some mods ship only the tables they change, as [bdat-toolset](https://github.com/RoccoDev/bdat-rs) writes them with
`-f json`: one file per table, `bdat/<file>/<table>.json` (for example `bdat/common/ITM_BeaconList.json` for
`bdat/common.bdat`, `bdat/us/common_ms/ITM_BeaconList_ms.json` for the text). Each holds the table's columns
(`schema`: name and type) and its rows (`rows`: `$id` and a value per column). Names are either `<8 hex digits>`, the
hash itself, or the plain name, which hashes (murmur3) to the game's; hash cells are written the same way. The file
name is the table's: its plain name, or for a table known only by its hash the 8 hex digits without brackets
(`1A2B3C4D.json`, as bdat-toolset writes it on every system); `<1A2B3C4D>.json` is read too. A file name of 8 hex
digits is taken as the hash when the game's file has a table with that hash, else hashed like any plain name.

A mod's JSON tables are its copy of just those tables: the mod's full file, or the game's when it has none, gives
the rest, and the result is merged like any other copy. A cell a JSON table leaves as the game has it is not a
change, so options of one mod that change different cells of a table stack, and an option that repeats what another
part already changes adds nothing. The game merges a file whenever an enabled mod has JSON tables for it, also when
that mod is the only one.

Only whole tables are read: a JSON file whose rows aren't numbered one after another (a table with only some of its
rows), that isn't valid JSON, has a value that doesn't fit its column, or names a table the game's file doesn't have
is left out and reported. If a mod has only JSON tables and none of it can be used, the game gets its own copy.

## The report

Each merge writes `<hash>-<file>.txt` next to the merged file. It lists the mods in priority order, what was merged,
and one line per event:

- **priority**: a value or row two mods both changed, and whose was kept;
- **left out**: a mod's change that wasn't used, and why;
- **caveat**: something used that may not behave as its mod expects (rows renumbered, a removed row kept).

The log has the same lines (the first few of each merge), and the Mods tab shows the counts and the caveats and
left-out changes of this run.
