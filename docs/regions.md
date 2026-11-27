# Map regions from mod data

The game knows its map regions by number: the five continents of Mira, New Los Angeles' districts and a few without
segments (Mira's region list, `FLD_FieldNameList`, has rows 0-23). Several places in the code list those regions by
hand: which ones have a segment (hexagon) table, which the Segments tab draws, which the survey panels show. So a game-
file mod could name a new region and paint it onto the world, but it would have no hexagons, no survey rate, no
rewards and no row in any panel.

With this feature a mod's data is enough. A region counts as the mod's own when the mod ships a hexagon table for it,
`SEG_TerminalZone<id>` (for example `SEG_TerminalZone24`). Without such a table nothing changes: the game runs its own
code paths with its own data.

## Region ids

- **20 and 23**: rows of the game's region list without a hexagon table of their own. A mod can give one of them
  hexagons. Region 20 is also the Enemy Index's "Other" group (the story and add-on bosses), so naming row 20 renames
  that group in every enemy's info; prefer 23 or a far region.
- **24 to 255 ("far regions")**: ids past the game's list. Each gets a segment slot of its own past the game's 24, so
  every far region has its own hexagons, survey rate and survey rewards. The game's code treats a region past 24 as
  "not a field"; for a far region it counts as one, so its wrecks, enemies and collection points spawn.

## What a region mod ships

All of it is data in the mod's `romfs/` ([installer.md](installer.md), game-file mods):

- The world's region map, `zone/zone0001.atr`, with the region's id painted on its cells.
- A row for the region in `FLD_FieldNameList` (its name, a name condition, the field it lies in, and its discovery flag
  `flg`), and rows with its id in `MNU_ZoneNameConvert`, `FLD_WeatherRefer` and `FLD_ZoneEnvSe`. The region list is
  read by position (row = id - 1), so a far region needs the rows up to its id.
- `MNU_MapZoneInfo` row: where its name and label sit on the map, and the base of its hexagon grid.
- New tables, found by name: `SEG_TerminalZone<id>` (its hexagons, in the layout of the game's `SEG_Terminal` tables,
  128 rows at most: a region's slot holds 128 hexagons) and `MNU_SegMapInfoZone<id>` (its Segments tab markers).
- Survey rewards: rows in `SEG_ExploreReward` with the region's id. Their claimed marks are mod state: give them a
  `flag_index` of 0x8000 or more and name them in `mod.json` ([mod-state.md](mod-state.md)).
- A far region's discovery flag is its `flg` (an integer column); use a mod flag (0xC000 or more,
  [mod-state.md](mod-state.md)) so it is kept beside the save. A far region without one gets the mod flag 0xFE00 + id,
  never the game's own discovery flags (whose numbers, past the game's regions, belong to other flags). Ids 20 and 23
  use the game's discovery flags 6168 and 6171.
- `SEG_ProbeListZone<id>` (optional, far regions only): rows in the layout of `SEG_ProbeList` (integer columns: the
  site in column 3, up to 6 hexagon ids after it) that name a data probe site and hexagons of this region to uncover
  when a probe is installed there, after the game's own list for that site. A site that lies in one region can
  uncover hexagons of a neighboring far region this way.
- Nothing for the panels: their rows are made when the game opens their layouts (see "Panel rows" below), so a
  region mod ships no menu layout (a mod's own copy with region rows of its own is set aside, see Limits).
- Missions count for the region their point lies in on the region map (the Map tab's Region Intel counts them by
  where they are), so a mod's missions on its region's cells count for it.

## What the game then does

- **Segments tab**: the region's hexagons and markers, with its name and survey rate; the minimap outlines the
  current hexagon of the region as for the game's own.
- **Survey rate**: the region has its own rate, and Mira's total counts it like every other region with hexagons (the
  game's total is the plain mean of the regions' rates, so a small region weighs as much as a continent).
- **Survey Reward List**: a page per mod region with rewards, after the game's pages.
- **Region Intel** (hold ZL on the map; the Segments, Map and FrontierNav tabs) and **Map Shortcuts** (X on the map):
  a row or entry for each mod region, in id order (on the Segments tab before Mira's row); on the Map tab with the
  region's mission count.
- **Region Discovered** banner, wrecks, enemies, data probes and the survey reward pop-up work in the region as in the
  game's own.

## Setting

`region_tables` in `xcx.ini`, or `XCX_REGION_TABLES`: `1` (default) on, `0` off. Off, a mod's `SEG_TerminalZone<id>`
is not looked for and the game runs as without this feature. Read once a boot.

## How it works

The code generator (`recomp/regions.py`) adds hooks to the 2.0.0 build's main module, each site checked against the
instruction it expects when the code is generated; `runtime/regions.c` answers them. Which regions are the mods' own
is decided once a boot, when the game's data is loaded and the segment manager is made: a table lookup by name for
ids 0-23 without a game table and for every id 24-255. From then on the hooks only read that decision; with no mod
region every hook returns what the game had.

- The segment manager's init asks a fixed switch for each region's hexagon table; for a mod region the hook supplies
  `SEG_TerminalZone<id>`. The manager is allocated bigger when there are far regions, with one slot each after the
  game's 24, and its slot loops step over the manager's own fields to those slots. Where the code turns a region id
  into a slot, a far id becomes its slot; where it writes a slot number as a region, a far slot writes its id.
- The minimap's outline switch, the Segments tab's marker table and its layer lists (the continents, unrolled in the
  code) get the mod regions too.
- Survey panels: the Survey Reward List gets a row vector per far region past the panel's end and pages for them.
  Region Intel's lists and the Map Shortcuts list keep their row widgets in fixed arrays inside their panel; when mod
  regions exist the panel is allocated bigger (room for 48 mod regions in each list) and the lists' widget arrays move
  past its end, so the layout's extra rows get widgets. The Map tab's mission counts come from the tree the game
  builds of the current missions per region, read for the mod regions after the game read its own.
- Panel rows (`runtime/region_layout.c`): the panels build one row widget per row node `mc_List01`, `mc_List02`, ...
  of their menu layout (`ui/image/04010_parts05.wilay` for Region Intel on the Segments and Map tabs,
  `04010_parts04.wilay` for Map Shortcuts, `04030_frontiernet_parts08.wilay` for the FrontierNav tab). When the game
  opens one of them and mod regions exist, the copy it would read (a mod's, else its own from its archive; see Limits
  for a mod's copy that brings rows of its own) gets a row
  per mod region and is served from `<profile>/region-layouts/`: the last row and its child components copied one row
  step on, a special last row (Mira's, hidden in the Map tab's mission view by a track of its placement node) kept
  last, Region Intel's divider line above each row (`rect_LineNN`) added the same way, the window one row taller per
  row; Map Shortcuts moves up a row per row (it reaches the screen's bottom) and its rows move down by what its
  window's header band (part of the window's picture) grows as the window stretches, the FrontierNav panel widens once
  for a longer name. A panel taller than the screen's band is scaled down as a whole,
  to half size at the least. With no mod region nothing is served.
- The field-region test (`region > 24` means "no field") answers 0 for a far region; the discovery flag of a far
  region comes from its `flg`; a probe install also uncovers the hexagons its site names in `SEG_ProbeListZone<id>`.

## Cost

- No mod region: one table lookup per id at the segment manager's creation (once a boot); every hook is a comparison
  that returns the game's value, and each RomFS open and archive lookup asks one comparison more.
- With mod regions, every RomFS open and archive lookup compares its path with the three layouts' (a short copy and
  three string compares); each layout is made once a boot when the game first opens it (1-10 ms; the time is in its
  log line) and written to `<profile>/region-layouts/`, which is left there when the mods go (the next boot with mod
  regions writes it again); the Region Intel panel is allocated with 31 KB instead of 6.5 KB, Map Shortcuts
  with 5 KB more.
- With mod regions: the panels and the Segments tab do the game's own work for each extra region. The segment manager
  is 0x2510 bytes bigger per far region, plus one slot it skips (the 25th would overlap its own fields).

## Limits

- Only the 2.0.0 update's build gets the hooks.
- Which regions are mod regions is decided once a boot: a mod with regions switched on or off while the game runs takes
  effect at the next start (as its data tables do).
- The minimap outlines a hexagon only while the player is in that hexagon's region (the region map decides, cell by
  cell), as for the game's regions; paint a region's cells to match its hexagons.
- The Segments tab's region list holds 24 entries (the five continents and 19 mod regions): it lives inside the map
  screen's object, whose fields after it are used throughout the map code. Mod regions past the 19th have no hexagons
  drawn there (their survey rate, rewards and panel rows are unaffected); the log says so once.
- Region Intel (Segments, Map and FrontierNav tabs) and Map Shortcuts have rows for as many mod regions as fit on the
  screen with the panel at half size or more (25 to 27 in each). Mod regions go in id order; one past that is left out
  of the panel, and the log says so once.
- A mod's own copy of one of the three panel layouts that brings region rows of its own (more rows than the game's) is
  not used: the rows are made from the game's copy instead, so two region mods never clash on that file. A copy with
  the game's row count (a mod that changes the panel itself) keeps its changes and gets the rows added.
- The Enemy Index's "Region Found" filter has fixed checkboxes for the five continents and "Other"; enemies of a far
  region are listed under "Other".
- A region whose hexagons the game can't find (no `SEG_TerminalZone<id>`) is not a mod region, even if other tables
  name it.
