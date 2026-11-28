# Map mods as pieces

All of Mira is one map: `map/ma9990.wismhd` (a header of tables) indexes `map/ma9990.wismda` (about 960 MB of
compressed chunks: terrain, prop groups, textures, lighting), and the enemy navmesh is the same kind of pair,
`map/ma9990.winvhe` + `map/ma9990.winvda` (about 140 MB). A mod that changes the world (new land, moved props, a texture
of its own, navmesh for new ground) changes some entries of those tables and brings the chunks they point at. New
ground also needs collision: each region's `map/mapcoli/<name>.wihkar`, to which such a mod adds static bodies (and
in which a mod that reshapes land changes the game's).

As whole files, such a mod is a 1 GB data file, and two map mods can't be on together: each brings its own whole
header, and the higher mod's whole collision file leaves the lower mod's ground without collision.

With map pieces a mod ships only what it changed, keyed by table and entry, and the game puts the map together from
its own files and every enabled mod's pieces **while it runs**. Nothing is built on disk, and map mods combine like
any other game-file mods, by mod priority.

## What a map mod ships

A folder in the mod's `romfs/`, so it installs like any game-file mod ([installer.md](installer.md)):

```
<mod>/romfs/map/ma9990.pieces/pieces.txt
<mod>/romfs/map/ma9990.pieces/<the files pieces.txt names>
```

`pieces.txt` is lines of text (`#` starts a comment), written by `tools/map_pieces.py make`:

| Line | Means |
|---|---|
| `xcx-map-pieces 1` | the first line: the format's version |
| `made-from <hd> <size> <xxh64>` | the game header the pieces were made against; one for each header the mod changes |
| `add <hd> <table> <name>` | a new entry, numbered after the game's (and after higher mods' new entries) |
| `set <hd> <table> <entry> <at> <u8\|u16\|u32> <value> [<bias>]` | a field, `<at>` bytes into the entry; the value may be `@<table>:<entry>`, the number of an entry (plus a bias, 0 to 65535) |
| `chunk <hd> <table> <entry> <at> <file> <size>` | the (offset, size) pair at `<at>` points at `<file>`, served after the game's data |
| `fix <file> <at> <u16\|u32> <table> <entry> <recorded>` | inside `<file>` (an xbc1 chunk's unpacked bytes), the number of an entry, which the file holds as `<recorded>` |
| `made-from mapcoli/<name> <size> <xxh64>` | the game's collision file `map/mapcoli/<name>.wihkar` the bodies were made against |
| `bodies mapcoli/<name> <file> <size>` | the collision bodies the mod adds to that file, and the game bodies it changes (see [Collision](#collision)) |

`<hd>` is `wismhd` (the map) or `winvhe` (the navmesh). `<entry>` is a decimal number, or `+<name>` for the mod's own
added entries (names of letters, digits, `_`, `.` and `-`). A file is a plain path inside the pieces folder (no `..`,
drive, device name such as `aux`, or name ending in a dot or space), found case-insensitively like every mod file. A
map chunk's `<size>` is its unpacked size, as its xbc1 header says; a navmesh tile's is its length.

The tables, by the name the lines use:

| Header | Table | Entry |
|---|---|---|
| wismhd | `terrain` | terrain sets (76 bytes; chunk at 52) |
| wismhd | `group` | prop model groups (64 bytes: box, streaming distance at 48, chunk at 52, flags at 60) |
| wismhd | `propvd`, `terrainvd` | prop and terrain vertex data (offset, size) |
| wismhd | `texture`, `texture2`, `texture3` | the three global texture tables (mid chunk, mid size, full chunk, full size, flags); an `add` to `texture` adds the entry to all three; the third differs from the first two in a few entries |
| wismhd | `placement` | prop placement chunks (offset, size) |
| wismhd | `light` | lighting areas (56 bytes; chunk at 24) |
| wismhd | `t10`, `t12`, `t25`, `t29`, `t36`, `t44`, `t46`, `t49` | tables whose contents are not known, named by the header words that hold their count |
| winvhe | `tile` | navmesh tiles (16 bytes: 3, data offset, length, offset of the second part) |
| winvhe | `grid`, `cells0`-`cells2` | the navmesh grids and their cells (u16, 1-based tile, 0 = no navmesh); these don't grow |

Every chunk pointer in the game's map header (16,101 of them) lies in one of these tables: each (offset, size) pair of
the header was checked against the chunks of the data file.

## Making one

Edit a copy of the map with any tool that keeps the game's chunks where they are: append each new or changed chunk
after the game's data and point the header entries at it (that is how the game's own map file grows, so the game
reads it like any other). Then:

```
python tools/map_pieces.py make <mod>/romfs/map/ma9990.pieces --game <folder with the game's four map files>
    [--map <edited .wismhd> <edited .wismda>] [--nav <edited .winvhe> <edited .winvda>] [--coli <edited .wihkar>]...
python tools/map_pieces.py verify <the same arguments>
```

The edited data file may also be given as a folder of `chunk-<offset>.xbc1` / `.bin` files holding only what lies past
the game's end. `make` compares every table entry with the game's and writes a line per change, a file per new chunk,
and a `fix` line wherever a new chunk names an added entry by number: a prop group's texture list, its stream table's
lists and its model -> vertex data list name textures and vertex data, its links to placement chunks name placement
entries, and a terrain set's groups name terrain vertex data. It refuses an edit outside the known tables. `verify` puts
the map together the way the game will and checks each entry and each chunk against the edited files; it prints the
built headers' xxh64, which the game also logs. Each of `--map`, `--nav` and `--coli` is optional: a mod that only adds
collision gives `--coli` alone. Close the game (or turn the mod off and restart) before running `make` into an installed
mod: the game reads the map's pieces once per start, so a running game goes on with the old ones.

`python tools/map_pieces.py combine --game <folder> <pieces folder>...` shows what several mods give together (top
mod first): which entries each keeps and loses, which mods are left out and why, and the built files' xxh64.

## Collision

A region's collision, `map/mapcoli/<name>.wihkar`, is a `1RAS` archive whose first entry is a Havok tagfile: the
physics scene as a memory image (`DATA`), its objects (`ITEM`: type, offset, count) and the places that hold object
numbers (`PTCH`). New ground is static bodies added to the scene's body array, each with its own shapes.

`make --coli <edited .wihkar>` (the game's copy of the same file in the `--game` folder, as `<name>.wihkar`,
`map__mapcoli__<name>.wihkar` or `mapcoli/<name>.wihkar`, in any case) finds the body array by its Havok type
(`hknpPhysicsSystemData::ExtendedBodyCinfo`), then the bodies the edit adds and the game bodies it changes, and writes
them as `bodies-<name>.bin` with its two lines. A changed game body is replaced whole: moved or turned (its position and
rotation), pointed at a new shape the mod brings, or moved out of the world to take that ground away. `make` accepts an
edit only when building the file back from the game's copy and the bodies gives exactly the edited file; an edit of the
game's own shapes (their triangles) can't be pieces, and `make` says so (ship that file whole).

The game builds the collision file with the map, at the first read of any map or collision file: the body array grows by
every used mod's bodies, top mod first, the game's data after it moves along (its objects and pointer fields follow),
each mod's shapes go to the end, and each changed game body takes the record of the highest mod that changes it. Added
bodies never clash, so two island mods on one collision file both keep their ground; a game body two mods change is the
higher mod's, and the lower mod's change to it is left out (the log and the Mods tab say so).

A bodies file: `XCXB`, then little-endian u32s: 2 (the version), the body array's item, its element size, the number of
added bodies, of new items, of pointer fields, the shapes' size, and the number of changed game bodies; then the added
body records, the changed game bodies' numbers and their records, the new items (type | flags << 24, region 0 or 1,
offset, count), the pointer fields (patch type, region, offset, item; bit 31 set for one of the file's own new items)
and the shapes. Region 0 is the added records, 1 the shapes, 2 the changed game bodies' records. The body array holds at
least one game body, the element size is a multiple of 16 (so the game's data after the array keeps its alignment) and
every pointer field is 8-aligned inside its region. A changed game body's pointer fields are the game's: a region 2
field is one the game's patch lists hold, of the same patch type, and every other pointer field of the record names one
of the game's objects. Type numbers (of the new items and pointer fields) are taken as written: `make` takes them from
the edited file, whose type table must be the game's.

## Combining

- The mods are taken in priority order (the F1 Mods tab's Mod priority, `romfs-enabled.txt`, top line first).
- An entry of the game's that two mods change belongs to the higher mod; the lower mod's lines for that entry are left
  out. Both the log and the Mods tab say so.
- Added entries never clash: each table's added entries are numbered after the game's, the top mod's first. When a
  mod's numbers move because a higher mod added entries too, its chunks that hold them are served with the numbers
  fixed (the chunk is unpacked, patched and stored again).
- Added collision bodies never clash: every used mod's bodies are in, the top mod's first. A game body two mods change
  is the higher mod's.
- Only used mods decide: a mod left out for any reason is out before the mods below it are judged (its collision
  file's body array, the file count and the size limits are judged by the used mods above each mod).
- A mod's pieces, map and collision, are used whole or not at all. A bad line, a newer format, a missing `made-from`
  or one for another game version, a file that is missing or doesn't match its line, an entry this game's header
  doesn't have, an entry number too big for its field, bodies that don't fit the game's collision file, or pieces that
  would pass a size limit (a 64 MB header, a 4 GB data file, a 256 MB collision file as built, 8 collision files in
  all) leave that
  mod's pieces out, with a line in the log and the Mods tab, and the map is put together from the others. The mod's
  other files (its data tables, a whole collision file) still load. Out of memory while a collision file is put
  together turns map pieces off for the run, so no mod is half in.
- While a file is served from pieces, a mod's whole copy of it (`romfs/map/ma9990.wismhd`,
  `romfs/map/mapcoli/<name>.wihkar` and so on) is not used; the Mods tab names it. A collision file no used mod adds
  bodies to or changes is not served from pieces, so a whole copy of it loads as before; likewise the map files while
  no used mod changes the map. Make whole files pieces with `tools/map_pieces.py make`. A mod can ship both its pieces
  and whole files: a build without map pieces ignores the `ma9990.pieces` folder and uses the whole files, and this
  build uses the pieces (the Mods tab says so, as a note, not a warning).
- The region map (`zone/zone0001.atr`) is a whole file, combined by the data table merge, not here.

## How it works

At the first read of any of the four map files or a collision file, `runtime/map_overlay.c` reads the enabled mods'
`pieces.txt`, takes the game's two headers (and each collision file a mod adds bodies to or changes) from its archive,
builds each collision file in memory (see [Collision](#collision)), and builds both headers in memory: tables that grow
are copied whole to the end of the header with their count and offset repointed. The data files are served as the game's
own bytes (read in place from its archive) up to the game's end, then each mod chunk the kept lines name, 4 KB aligned,
read from the mod's file or from memory when it was patched. The mod's files stay open for the run, in a way that still
lets the mod be updated or removed meanwhile (the run goes on reading the files it opened). Without any enabled mod that
has map pieces the game reads its own files (log line `map pieces: off`).

The map is put together once per start: the game keeps header offsets, so pieces that change while it runs apply at
the next start, and the Mods tab says so.

## Cost

- Start-up, once, at the first map or collision read (at the title): one look for a `ma9990.pieces` folder in each
  enabled mod. With pieces: reading the archive's index and the two packed headers (about 2 MB in all), the mods'
  `pieces.txt` and the first bytes of each piece, and unpacking the chunks whose numbers are fixed; 4 to 9 ms for map
  pieces alone and about 30 ms with two collision files on a quiet PC, up to 315 ms while four games start at once.
- Opens and reads: the game opens the map data file again for each streamed read. With map pieces on, each open
  and close takes the overlay's handle lock, and each read takes it shared, then reads the game's bytes from the
  archive as before, or a piece (a binary search over the pieces, then a read of the mod's file): well under a
  microsecond beside the copy itself. Without map pieces, every other file's open pays one prefix compare.
- The Mods tab, while it is open: a look at each map mod's files once a second.
- Collision: the game's copy of each collision file a mod adds bodies to or changes, read once and built with the
  mods' bodies (the sea-wide file with two island mods: 5.6 MB); its reads are copies from memory.
- Memory: the two headers (about 2.2 MB), the chunks whose numbers are fixed, and the built collision files.

## Limits

- Only the world map `ma9990` (Mira). Other maps are read as before.
- `make` writes `fix` lines only for the numbers it knows inside chunks (prop groups and terrain sets, listed under
  Making one). A chunk of another table that names an added entry by number keeps the number it was made with, which
  is right only while no higher mod adds entries to that table.
- Two mods that change the same entry: the lower one's change is left out whole (no record-by-record merge of one
  placement chunk).
- Collision as pieces covers added bodies and changed game bodies (whole records), not edits of the game's own shape
  data: such an edit ships as a whole file, which then loads only while no used mod adds or changes bodies in that
  file.
- The navmesh grids don't grow: a mod can add navmesh tiles only where a grid already has cells, so land far outside
  Mira's grids has no enemy pathing.
- A piece rewritten in place while the game runs (not replaced by a new file) can't be read consistently; the game
  stops with a line saying so.
