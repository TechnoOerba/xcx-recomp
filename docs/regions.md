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
- For the panels below: menu layouts with a row for each mod region (the game's layouts have exactly as many rows as
  it fills).

## What the game then does

- **Segments tab**: the region's hexagons and markers, with its name and survey rate. For ids 20 and 23 the minimap
  also outlines the current hexagon (not for a far region: see Limits).
- **Survey rate**: the region has its own rate, and Mira's total counts it like every other region with hexagons (the
  game's total is the plain mean of the regions' rates, so a small region weighs as much as a continent).
- **Survey Reward List**: a page per mod region with rewards, after the game's pages.
- **Region Intel** (hold ZL on the map; the Segments, Map and FrontierNav tabs) and **Map Shortcuts** (X on the map):
  a row or entry for each mod region, in id order (on the Segments tab before Mira's row), as many as the mod's layout
  has rows for.
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
  regions exist the panel is allocated bigger and the lists' widget arrays move past its end, so the layout's extra
  rows get widgets.
- The field-region test (`region > 24` means "no field") answers 0 for a far region; the discovery flag of a far
  region comes from its `flg`; a probe install also uncovers the hexagons its site names in `SEG_ProbeListZone<id>`.

## Cost

- No mod region: one table lookup per id at the segment manager's creation (once a boot); every hook is a comparison
  that returns the game's value.
- With mod regions: the panels and the Segments tab do the game's own work for each extra region. The segment manager
  is 0x2510 bytes bigger per far region, plus one slot it skips (the 25th would overlap its own fields).

## Limits

- Only the 2.0.0 update's build gets the hooks.
- Which regions are mod regions is decided once a boot: a mod with regions switched on or off while the game runs takes
  effect at the next start (as its data tables do).
- In a far region the minimap shows the land but no outline of the current hexagon; the Segments tab, the survey
  rate and the panels show the region's hexagons as for any other.
- What the panels show is capped by the mod's layouts and by the code, which grows each list to 16 rows: the Segments
  tab's region list holds 24 entries (the five continents and 19 mod regions); Region Intel on the Segments tab and on
  the Map tab has 16 rows (the game's 11 regions and Mira, so 4 mod regions), on the FrontierNav tab 16 (the game's 5,
  so 11 mod regions); Map Shortcuts 16 entries (the game's 8, so 8 mod regions). Mod regions go in id order; one past
  a cap, or past the rows its layout has, is left out of that panel, and the log says so once.
- The Enemy Index's "Region Found" filter has fixed checkboxes for the five continents and "Other"; enemies of a far
  region are listed under "Other".
- A region whose hexagons the game can't find (no `SEG_TerminalZone<id>`) is not a mod region, even if other tables
  name it.
