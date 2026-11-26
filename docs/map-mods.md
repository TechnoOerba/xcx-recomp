# Map mods as pieces

All of Mira is one map: `map/ma9990.wismhd` (a header of tables) indexes `map/ma9990.wismda` (about 960 MB of
compressed chunks: terrain, prop groups, textures, lighting), and the enemy navmesh is the same kind of pair,
`map/ma9990.winvhe` + `map/ma9990.winvda` (about 140 MB). A mod that changes the world (new land, moved props, a texture
of its own, navmesh for new ground) changes some entries of those tables and brings the chunks they point at.

As whole files, such a mod is a 1 GB data file, and two map mods can't be on together: each brings its own whole
header.

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
    --map <edited .wismhd> <edited .wismda> [--nav <edited .winvhe> <edited .winvda>]
python tools/map_pieces.py verify <the same arguments>
```

The edited data file may also be given as a folder of `chunk-<offset>.xbc1` / `.bin` files holding only what lies past
the game's end. `make` compares every table entry with the game's and writes a line per change, a file per new chunk,
and a `fix` line wherever a new prop group chunk names an added texture or vertex data entry (the group's texture list
and its stream table's lists). It refuses an edit outside the known tables. `verify` puts the map together the way the
game will and checks each entry and each chunk against the edited files; it prints the built headers' xxh64, which
the game also logs. Close the game (or turn the mod off and restart) before running `make` into an installed mod: the
game reads the map's pieces once per start, so a running game goes on with the old ones.

`python tools/map_pieces.py combine --game <folder> <pieces folder>...` shows what several mods give together (top
mod first): which entries each keeps and loses, which mods are left out and why, and the headers' xxh64.

## Combining

- The mods are taken in priority order (the F1 Mods tab's Mod priority, `romfs-enabled.txt`, top line first).
- An entry of the game's that two mods change belongs to the higher mod; the lower mod's lines for that entry are left
  out. Both the log and the Mods tab say so.
- Added entries never clash: each table's added entries are numbered after the game's, the top mod's first. When a
  mod's numbers move because a higher mod added entries too, its chunks that hold them are served with the numbers
  fixed (the chunk is unpacked, patched and stored again).
- A mod's pieces are used whole or not at all. A bad line, a newer format, a missing `made-from` or one for another
  game version, a file that is missing or doesn't match its line, an entry this game's header doesn't have, an entry
  number too big for its field, or pieces that would pass a size limit (a 64 MB header, a 4 GB data file) leave that
  mod's pieces out, with a line in the log and the Mods tab, and the map is put together from the others. The mod's
  other files (its collision, data tables) still load.
- While the map is put together from pieces, a mod's whole copy of a map file (`romfs/map/ma9990.wismhd` and so on) is
  not used; the Mods tab names it. When no mod's pieces can be used, whole copies are used as before. Make them pieces
  with `tools/map_pieces.py make`. A mod can ship both its pieces and whole files: a build without map pieces ignores
  the `ma9990.pieces` folder and uses the whole files.
- Collision (`map/mapcoli/*.wihkar`) and the region map (`zone/zone0001.atr`) stay whole files, one per tile or for
  the world: the highest mod's copy is used. Two mods that both add collision on the same collision tile clash: the
  lower mod's ground there has no collision.

## How it works

At the first read of any of the four map files, `runtime/map_overlay.c` reads the enabled mods' `pieces.txt`, takes
the game's two headers from its archive, and builds both headers in memory: tables that grow are copied whole to the
end of the header with their count and offset repointed. The data files are served as the game's own bytes (read in
place from its archive) up to the game's end, then each mod chunk the kept lines name, 4 KB aligned, read from the
mod's file or from memory when it was patched. The mod's files stay open for the run, in a way that still lets the mod
be updated or removed meanwhile (the run goes on reading the files it opened). Without any enabled mod that has map
pieces the game reads its own files (log line `map pieces: off`).

The map is put together once per start: the game keeps header offsets, so pieces that change while it runs apply at
the next start, and the Mods tab says so.

## Cost

- Start-up, at the first map read: one look for a `ma9990.pieces` folder in each enabled mod. With pieces: reading the
  archive's index and the two packed headers (about 2 MB in all), the mods' `pieces.txt` and the first bytes of each
  piece, and unpacking the chunks whose numbers are fixed; 4 to 75 ms with our test mods.
- Opens and reads: the game opens the map data file again for each streamed read. With map pieces on, each open
  and close takes the overlay's handle lock, and each read takes it shared, then reads the game's bytes from the
  archive as before, or a piece (a binary search over the pieces, then a read of the mod's file): well under a
  microsecond beside the copy itself. Without map pieces, every other file's open pays one prefix compare.
- The Mods tab, while it is open: a look at each map mod's files once a second.
- Memory: the two headers (about 2.2 MB) and the chunks whose numbers are fixed.

## Limits

- Only the world map `ma9990` (Mira). Other maps are read as before.
- `make` writes `fix` lines only for the numbers it knows inside chunks: a prop group's texture list and stream lists.
  A chunk of another table that names an added entry by number keeps the number it was made with, which is right
  only while no higher mod adds entries to that table.
- Two mods that change the same entry: the lower one's change is left out whole (no record-by-record merge of one
  placement chunk).
- A piece rewritten in place while the game runs (not replaced by a new file) can't be read consistently; the game
  stops with a line saying so.
