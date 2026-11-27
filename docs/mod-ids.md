# Numbers handed out to content mods

A content mod adds things the game finds by number: a map region (its id is a row of `FLD_FieldNameList` and a byte
of the world raster), a mod flag ([mod-state.md](mod-state.md)), a survey reward's claimed index, a FrontierNav site's
number, a new row of a table other tables point at by row id (a place name's text, a name condition, a landing spot,
a location, a probe list row). An author picks such numbers
by hand from the free ones, and two authors who never heard of each other pick the same ones. The data table merge
([bdat-merge.md](bdat-merge.md)) would then take their rows for the same rows and keep one: the second mod's region
loses its name, its rewards or its flags, or both mods share them.

So each mod names those things in its `mod.json`, with the number its own files use for each, and the recomp gives
every name a number no other enabled mod has. Before the data tables are merged, each mod's numbers are moved to the
ones it was given, wherever its tables hold them; the game only ever sees the given numbers.

## For mod authors

`mod.json` ([mod-state.md](mod-state.md) has the rest of the file):

```json
{
  "id": "ashen-shoal",
  "regions": [ { "name": "ashen-shoal", "id": 24 }, { "name": "ashen-reef", "id": 25 } ],
  "flags":   [ { "name": "discovered/ashen-reef", "type": 1 } ],
  "rewards": [ { "name": "ashen-reef/5" } ],
  "rows":    [ { "table": "FLD_NameCondition", "name": "cond/ashen-reef", "id": 23 },
               { "table": "<F05B0D47>", "name": "name/ashen-reef", "id": 617,
                 "refs": ["FLD_FieldNameList.name", "MNU_ZoneNameConvert.name"] },
               { "table": "<D12FB959>", "name": "loc/ashen-site", "id": 467 },
               { "table": "SEG_ProbeList", "name": "probe/ashen-site", "id": 107 } ],
  "sites":   [ { "name": "fn/ashen-site", "id": 110 } ]
}
```

- **Regions** (ids 24 to 255: past the game's 24 segment slots, ids 0 to 23, and within a byte of the world raster):
  `id`, which a region must have, is the region id the mod's own files use: its `FLD_FieldNameList` row, the bytes it
  paints in `zone/zone0001.atr`, its region-named tables (`SEG_TerminalZone<id>` and the others below). Every author
  may write 24. A bare number (`"regions": [24, 25]`, also accepted) is a region named after its number (`#24`).
- **Flags** (0xC000 to 0xFFFE) and **rewards** (0x8000 to 0xFFFF): `id` / `index` is optional. Without one, the name
  is used only through `@flag:` / `@reward:` cells (below).
- **Sites** (110 to 143): a FrontierNav site's number is its row of `FnetVeinList`, which the game's FrontierNav keeps
  144 of (its own sites are rows 0 to 109). `id`, which a site must have, is the row the mod's own copy adds the site
  at (110 to 255: a site added after the game's rows, not one of the game's unused rows taken over; it is moved to
  one of 110 to 143). Its location row,
  probe list row and hexagon are `rows` (below).
- A number outside its kind's range is left out with a line (moving it would rewrite the game's own values), and so is
  a second name with the same number in one mod (moving one of them would move every use of both).
- **Rows**: a row the mod adds to a table that other tables point at by row id. `table` is the table's name or
  `<8 hex digits>` (its hash), `id` the row id it has in the mod's own copy (after the game's last row). `refs`
  lists further `Table.column` pairs that hold this row's id. Some tables' pointers are known and need no `refs`:
  `FLD_NameCondition` rows (from `FLD_FieldNameList.nameCondition`); location rows (`<D12FB959>`: a location's id is
  its row id; from its own id column, a site's location in `FnetVeinList`, map marks, hexagons' field location and
  Segments-tab markers in the hexagon and marker tables of every zone and mod region), whose row of the map's location menu (`<06FBB0CF>`) goes with it; and
  `SEG_ProbeList` rows (from a probe hexagon's segment id, in hexagons of type 1 only). A hexagon a probe reveals is
  a row of its region's hexagon table: name it with `refs` to the probe list's six reveal columns.
- Names: as in [mod-state.md](mod-state.md) (at most 95 bytes, no quote, backslash or control character), and no
  space at either end. A name is the mod's own: two mods may both have a region called `isle`.

In a JSON table ([bdat-merge.md](bdat-merge.md), "Tables as JSON"), an integer cell may hold **`"@kind:name"`**
instead of a number (`"@region:ashen-reef"`, `"@flag:discovered/ashen-reef"`, `"@reward:ashen-reef/5"`,
`"@row:name/ashen-reef"`, `"@site:fn/ashen-site"`): the number that name was given. A JSON table may also say which table it is with a
**`"table"`** key, the name in which may end in a name of the same kind: `"table": "SEG_TerminalZone@region:ashen-reef"`
is the table `SEG_TerminalZone26` when Ashen Reef was given 26. A table named that way that the game doesn't have is a
new table of the mod. A name the mod's `mod.json` doesn't declare leaves that table out, with a line.

## How numbers are given

- A name keeps the number it was given at an earlier start for as long as no other mod holds it, also while its mod
  is off. So the numbers stay the same from one start to the next, a save made with a mod finds its regions where they
  were, and turning one mod off or adding a new one never moves another mod's numbers.
- Then every name whose own number (the one its mod's files use) is free keeps it, and the rest get the first free
  number of their kind. Both go in Mod priority order: of two mods that picked the same numbers, the lower one moves.
  (Once both have numbers, priority no longer moves them.)
- A row's numbers start at the lowest row id the enabled mods' files use for that table; a number among the game's
  own rows is refused by the merge, which moves nothing for it.
- The numbers are kept in `<profile>/mod-ids.txt`, one line per name: kind, table (rows), number, mod, name. When a
  kind runs out, the oldest names of mods that are off give theirs back. A line that doesn't read as a record (a hand
  edit, a damaged file), or holds a number outside its kind's range, is ignored, and the file as it was is kept as
  `mod-ids.txt.bad` before it is written again; a file that is there but can't be read is never written over.
- The log has a line for every number moved from the one its mod's files use, and for a name that got none. The Mods
  tab lists, under the Mod priority list, each content mod's regions and how many numbers it was given and moved,
  then the moved ones in yellow and the names that got none in orange (for the mods the game started with).

## Moving a number

Before anything is compared, in each mod's copy of each data table:

- `@kind:name` cells and `"table"` names get their numbers;
- the mod's own (local) numbers get the given ones, in the cells known to hold that kind, and only where the mod's
  copy differs from the game's (the game's own values are never touched):
  - regions: columns named `zone` or `ZoneID` in any table, and the region columns of `FLD_WeatherRefer`,
    `MNU_ZoneNameConvert`, `MNU_MapZoneInfo` and the location menu rows; the region-named new tables
    (`SEG_TerminalZone<id>`, `MNU_SegMapInfoZone<id>`, `SEG_ProbeListZone<id>`) are renamed with the region;
  - flags: every 16- or 32-bit unsigned cell;
  - rewards: `SEG_ExploreReward`'s claimed index;
  - sites: `FnetVeinList`'s site number and its four network links, a location's site number (F_spot), and a probe
    list row's site in `SEG_ProbeList` and the `SEG_ProbeListZone<id>` tables;
  - rows: the columns the `refs` and the known pairs name;
- a region's `FLD_FieldNameList` row, a site's `FnetVeinList` row and every row a `mod.json` names are put at the row
  id they were given. Such a table grows, in the game's copy and in every mod's, by rows up to the highest id given; a
  row no enabled mod has is a copy of a placeholder (for `FLD_FieldNameList`, the game's unused region 20; for
  `FnetVeinList`, the game's unused site 47 with no location and the gap's own number; for locations and their menu
  rows, the game's unused camp slot `CAMP_1501_10`; else the table's last row) with
  a key of its own. Different mods' rows are then edits of different rows, and combine as such;
- a FrontierNav site of the game's that mods link their sites to (the game's links go both ways, so a mod puts its
  site in a free link column of the game site's row too): every mod's added links go into the free link columns, the
  game's links staying in theirs, higher priority first; a link that no longer fits (four per site) is left out with a
  line. Two mods linking to the same game site then both keep their links.

A number of a kind not listed, or in a column the list doesn't name, keeps its old value: a JSON name cell is always
exact, a local number only where the census knows the column.

### What can't be moved

- **Spare game rows** (an unused site, location, vein or probe list row of the game's, a camp slot) that an older mod
  takes over instead of adding its own: they are not handed out. When two content mods edit the same such row, the
  lower one's edits to that row are left out whole, so no site is half one mod's and half the other's (a "left out"
  line). A mod that adds its sites (`sites` and `rows`, above) needs none.
- **Other values the game keeps one of per row** (an event flag of the game's own range, a site number): when rows two
  mods add share such a value in a column where every one of the game's rows has a value of its own, the lower mod
  gets a caveat line naming it.
- New rows a `mod.json` doesn't name are added at the end; when that gives them other row ids than in the
  mod's copy, the merge's caveat says so.

## The world raster

`zone/zone0001.atr` says which region each 10 m cell of Mira is in. A region mod ships it whole, painted with its own
region ids. When two or more enabled mods have it (with or without a `mod.json`), or one any of whose numbers moved,
the recomp combines them: the
game's raster, then each mod's cells that differ from the game's, lowest priority first (a higher mod's cell wins),
painted with the numbers its mod was given. The result is kept with the merged tables (`<profile>/bdat-merge/`,
with a report); a cell two mods paint differently is counted in a "left out" line for the lower one.

## Placement files

`gimmick/<zone>/arrangement_*.gimk` place a zone's location boxes, FrontierNav sounds (where a site's probe is
installed), treasure, collection points, enemy spawns and so on (each a file with a label table; a file this can't
read as one is used whole, the top mod's, with a line): records of 56 bytes, each with a label (a location's
label is what its location row's key is the hash of). A content mod ships such a file whole, the game's records with
its own appended (a location box and a sound record per site). When two or more enabled mods ship the same one, the
recomp combines them label by label, as the data tables are combined row by row:

- a label of the game's (by its place in the file, which a mod keeps) that one mod changes (its entry, or the game's
  records under it moved, edited or removed) is that mod's; records a mod only adds under it are additions, every
  mod's kept; two mods changing the same one: the higher keeps it whole, the lower's changes to it are
  left out (a line);
- the labels a mod appends are its own; two mods appending the very same label with the same records: one copy, with
  other records: the higher one's;
- the game's records keep their places (but for ones a mod removes); records mods add come after them, and one whose
  instance id (numbered in the FrontierNav sound file) another record has gets a free one.

The result is kept with the merged tables (`<profile>/bdat-merge/`, with a report). A single mod's file is used as it
is.

## Cost

- At start, and again only when the mods change (the mod loader's rebuild): each enabled mod's `mod.json` is read by
  the hand-out and by the mod state (one reader, two reads), and the records file is read, and written when a number
  was given. A few milliseconds.
- The rewrite runs inside the merge of a data table some content mod changes; a merge found in the cache (the numbers
  given are part of its key) costs nothing more. A single mod's table is merged only when one of its numbers moved.
- The raster: found in the cache by the same quick key as the merged tables (the files' sizes, times and identities,
  and the numbers given), it costs the check of the cached file. Otherwise the game's raster and each mod's (4 MB each)
  are read, hashed for the cache name and combined in one pass per mod.
- Placement files: only when two or more enabled mods ship the same file (one mod's copy is used as it is), at its
  first open after each change of the mod list, on the game's loader thread during a map load: the files read and
  hashed, combined, kept in the cache; under a millisecond for location and sound files (about 110 records), an
  estimated few milliseconds for an enemy spawn file (about 2000). A later start finds the cached file by its quick
  key. It doesn't wait for a data table merge made ahead (after a change in the Mods tab): placement files have a
  lock of their own.

## Limits

- Region ids are a byte: 24 to 255 (232 regions over all mods).
- FrontierNav sites: 34 over all mods (110 to 143). The game's FrontierNav keeps its sites in a fixed list of 144; a
  site row past it is not loaded, and a network link to one stops the game at start-up.
- Moving a local number is only as good as the census of where such numbers live; a mod that keeps a region id or a
  flag id somewhere else (a column not listed, a script, a map file other than the raster) keeps its old number
  there. Names in JSON tables avoid that.
- A flag's own number is moved in every 16- or 32-bit unsigned cell the mod changed: a cell of another kind that happens
  to hold the same number (0xC000 and up) moves too. `@flag:` names in JSON tables are exact.
- A gap row (a number of a mod that is off) is a live copy of a game row with a key of its own: a table the game walks
  as a list (rather than by row id) sees it as one more entry, a copy of the one it was copied from. A hexagon gap is a
  copy of its region's last hexagon (the same cell and flags): it draws and opens with it, and the survey rate doesn't
  change (checked in game: Primordia 5.93% with and without three gap hexagons).
- The raster is combined at start-up only: the Mods tab's restart line and a merge made ahead follow the data tables,
  not the raster, which the next start combines as it finds it.
- When a moved mod's table can't be served moved (the merge fails), the game reads that mod's own numbers there while
  the mod state uses the numbers given; the log says so.
- Layouts and whole map files are not split here; two mods shipping the same one still clash by priority.
- Placement files: the game's labels are matched by their place in the file, so a mod that inserts or removes one
  instead of appending changes every later one, and the other mods' edits to those are left out. Labels a mod adds
  are matched by name, which authors pick: two mods adding the same next free name (a treasure box's
  `t_box_15_NNN`) clash, the higher one's kept, as their rows with that key do in the tables. Numbers inside records
  or entries (an enemy row in a spawn group) are not moved.
- The records file is per profile (per game folder). A save copied to another computer with another records file may
  meet other numbers for mods that were set up there in another order.
