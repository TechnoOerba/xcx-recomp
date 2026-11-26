"""Data-driven regions, for the verified XCX DE 2.0.0 main: a game-file mod's data makes a map region with hexagons, a
survey rate, rewards and rows in the survey panels (docs/regions.md). Every site below is checked against its
instruction word (WORDS) when the code is generated; runtime/regions.c answers the hooks. The sites, in order: the hex
table switch, the minimap, the Segments tab's markers and layers, the Survey Reward List, Region Intel on the Segments,
FrontierNav and Map tabs, Map Shortcuts, far regions' segment slots and the manager's slot loops, the field-region
test, the probe reveal and the discovery flag.

The first piece, the hex table. The segment manager's init (main+0x3fd110) fills one slot per map region id 0..23 and asks main+0x3fe1f4 for each
slot's hex table: a fixed switch that knows ids 2-7, 10 and 13-16 only (SEG_Terminal1501, ...) and returns the table
handle in x0 and found in w1 (low byte). Its only caller is the BL at main+0x3fd3a8. After that call the generated C
passes the result through runtime/regions.c, which looks for a table named SEG_TerminalZone<id> when the game found
none, so a mod can give a new region (a spare id, e.g. 20) its hexes by shipping that table. Without such a table the
result is the game's own.
"""

BUILD = 'ec4ed46bbc4a9ebbe7ce0af7a4a8ebbc09e5724a'
SHA256 = '0a9dbf1f3816dd0fab0a08883d538c74d9b9fd7d939cb29aaf0947d71a201026'
SITE = 0x3fd3a8            # bl main+0x3fe1f4 in the segment manager's init
SWITCH = 0x3fe1f4          # the region -> hex table switch
# The minimap's mode switch (main+0x87e200): regions 10-23 pick a case from a byte table (region - 10); 18, 20, 22
# and 23 get the "no field" case (no hexagon outline), regions below 10 the field case at main+0x87e3bc. Right after
# the byte load the generated C lets runtime/regions.c send a region with a mod hex table to the field case.
MINIMAP_LOAD = 0x87e2a8    # ldrb w13, [x11, x10]; w9 = the region (ldr w9, [x19, #0x1c8] at main+0x87e28c)
# The Segments tab's marker table per region (main+0x5619ac): MNU_SegMapInfo<zone> for regions 2-7, 10 and 13-16
# (bitmask 0x793f over region - 2, hashes at main+0x17f78f8), else none, and with none the map screen draws nothing
# of that region. Its two callers pass the result through runtime/regions.c.
SEGMAP = 0x5619ac
SEGMAP_SITES = (0x56ff04, 0x7b0570)
# The Segments tab's field layers (main+0x7fe24c, a virtual): one call of the layer builder main+0x7b0530 (map, region)
# for each continent, 2, 4, 5, 6 and 7, a fixed list. At its entry the generated C lets runtime/regions.c build a
# layer for each region that got a mod hex table too.
# Its sibling main+0x7fe1fc does the same with main+0x7af83c (the hexagon cells) for the same five regions.
FIELD_LAYERS = {0x7fe24c: 0x7b0530, 0x7fe1fc: 0x7af83c}   # function -> the per-region builder it calls
# Which regions the Segments tab updates and draws each frame: main+0x7fd020 empties the map's layer list (map+0xa50,
# 0x30-byte entries, count at map+0xed0, room for 24) and adds 2, 4, 5, 6 and 7, unrolled. After its last count store
# runtime/regions.c appends an entry for each region that got a mod hex table.
LAYER_LIST_END = 0x7fd1c8  # str x8, [x19, #0xed0]; x19 = the map, x0 = the value the entries keep at +0x20
# The Survey Reward List (Y on the Segments tab, one class at main+0x8547d0 .. 0x856a50). Its filler (main+0x856040)
# sorts every SEG_ExploreReward row into a vector per region id 0-23 (panel+0x7c0 + region * 0x20), but it pages through
# a fixed list of 11 regions (u32s at panel+0xac0, page index at panel+0x5c, last page 10 hard-coded). When a mod region
# has reward rows, runtime/regions.c takes over the page turns, the jump to a region and the header at their entry
# (each returns 0 to let the game's own code run), and gives the two other page -> region lookups the region for a
# page past 10 right after their "page > 10: region 0" mov.
REWARD_ENTRIES = {0x855af8: 'region_reward_next', 0x855c40: 'region_reward_prev', 0x85694c: 'region_reward_jump',
                  0x8566b0: 'region_reward_header'}
REWARD_LOOKUPS = {0x85686c: 0, 0x855fd0: 19}   # mov x8, xzr -> the register holding the panel
# Region Intel (hold ZL on the Segments tab): 12 rows, 11 regions + Mira, filled by main+0x84edd0 into the panel
# (allocated 0x19a8 bytes at main+0x840a50, rows of 0x38 bytes from panel+0x1708), shown by a list (panel+0xb38) with
# one widget per "mc_List%02d" node of the menu layout, at most 12 (the list keeps them inline at list+0x1c0, 0x48
# each, count at list+0x520). When a mod region exists, runtime/regions.c gives the panel room for 12 + 48 rows and
# widgets past its end: the alloc size, the list's widget block and count (all their uses, below), the widget limit,
# the fill (mod regions before Mira, as many as the layout has rows for), the row callback and the destructor.
INTEL_ALLOC_SIZE = 0x840a50        # mov w0, #0x19a8
INTEL_ALLOC_CALL = 0x840a68        # bl main+0x13e17f0 (the allocator; x0 = the panel after it)
INTEL_FILL_CALL = 0x84ecc0         # bl main+0x84edd0 (the rows), x0 = the panel
INTEL_ROW = 0x850604               # the row callback: x0 = context (+8 = the panel), x1 = widget, w2 = row
INTEL_DTOR = 0x84c69c              # the panel's destructor, x0 = the panel
INTEL_MAX = 0x670ce4               # mov w4, #12: the widget limit the list's constructor passes its builder; x19 = list
INTEL_COUNT_LOADS = {0x670d30: (21, 0), 0x670df0: (27, 19), 0x670dfc: (8, 19), 0x670e3c: (8, 19), 0x671964: (20, 19),
                     0x6719cc: (8, 0), 0x671aa8: (9, 0), 0x671abc: (8, 0), 0x671f38: (20, 19), 0x671fe8: (20, 19)}
INTEL_COUNT_STORES = {0x670bd4: (31, 19), 0x670d70: (31, 19), 0x670e44: (8, 19), 0x67198c: (31, 19),
                      0x671f68: (31, 19), 0x67201c: (31, 19)}   # address: (value register, 31 = zero; list register)
INTEL_BLOCK_ADDS = {0x670d38: (20, 0), 0x67196c: (0, 19), 0x6719e0: (8, 0), 0x671aac: (8, 0), 0x671acc: (9, 0),
                    0x671f40: (0, 19), 0x671ff0: (0, 19)}       # add xd, xn, #0x1c0 (#0x178: the last widget)
INTEL_WORDS = {INTEL_ALLOC_SIZE: 0x52833500, INTEL_ALLOC_CALL: 0x942e8362, INTEL_FILL_CALL: 0x94000044,
               INTEL_ROW: 0x71002c5f, INTEL_DTOR: 0xa9be7bfd, INTEL_MAX: 0x52800184,
               0x670d30: 0xf9429015, 0x670df0: 0xf942927b, 0x670dfc: 0xf9429268, 0x670e3c: 0xf9429268,
               0x671964: 0xf9429274, 0x6719cc: 0xf9429008, 0x671aa8: 0xf9429009, 0x671abc: 0xf9429008,
               0x671f38: 0xf9429274, 0x671fe8: 0xf9429274,
               0x670bd4: 0xf902927f, 0x670d70: 0xf902927f, 0x670e44: 0xf9029268, 0x67198c: 0xf902927f,
               0x671f68: 0xf902927f, 0x67201c: 0xf902927f,
               0x670d38: 0x91070014, 0x67196c: 0x91070260, 0x6719e0: 0x91070108, 0x671aac: 0x91070008,
               0x671acc: 0x9105e129, 0x671f40: 0x91070260, 0x671ff0: 0x91070260}
# Map Shortcuts (X on the map, "Select Target Area"): 8 fixed entries (BLADE Barracks, NLA Armory Alley, the five
# continents, Skell). The panel (0x7f8 bytes, allocated at main+0x841850) has a list (panel+0x1b8) of up to 8
# "mc_List%02d" widgets inline (0x58 bytes each at list+0x1c0, count at list+0x480); the entry name (main+0x868eb8),
# the row callback (main+0x86b5cc), the current entry (main+0x868834), the selection (main+0x86a188) and the map's
# "can jump" / "jump" (main+0x7a5dec, main+0x7fd200, a switch whose continent cases post a "show region" command) all
# stop at index 7. When a mod region exists, runtime/regions.c moves the list's widgets past the panel's end (as for
# Region Intel above) and takes over those six functions for an entry past the 8th: a mod region, in id order, shown
# like a continent.
SHORTCUT_ALLOC_SIZE = 0x841850     # mov w0, #0x7f8
SHORTCUT_ALLOC_CALL = 0x841864     # bl main+0x13e17f0
SHORTCUT_MAX = 0x67d73c            # mov w4, #8; x19 = the list
SHORTCUT_ENTRIES = {0x868eb8: 'region_shortcut_name', 0x86b5cc: 'region_shortcut_row', 0x86a188: 'region_shortcut_select',
                    0x7a5dec: 'region_shortcut_can_jump', 0x7fd200: 'region_shortcut_jump',
                    0x868834: 'region_shortcut_current'}
SHORTCUT_COUNT_LOADS = {0x67d7e0: (20, 0), 0x67d8b0: (28, 19), 0x67d8bc: (8, 19), 0x67d910: (8, 19), 0x67dca4: (20, 19),
                        0x67dd0c: (8, 0), 0x67dde8: (9, 0), 0x67ddfc: (8, 0), 0x67df98: (20, 19), 0x67e048: (20, 19)}
SHORTCUT_COUNT_STORES = {0x67d680: (31, 19), 0x67d820: (31, 19), 0x67d918: (8, 19), 0x67dccc: (31, 19),
                         0x67dfc8: (31, 19), 0x67e07c: (31, 19)}
SHORTCUT_BLOCK_ADDS = {0x67d7e8: (21, 0), 0x67dcac: (0, 19), 0x67dd20: (8, 0), 0x67ddec: (8, 0), 0x67de0c: (9, 0),
                       0x67dfa0: (0, 19), 0x67e050: (0, 19)}   # add xd, xn, #0x1c0 (#0x168: the last widget)
SHORTCUT_WORDS = {SHORTCUT_ALLOC_SIZE: 0x5280ff00, SHORTCUT_ALLOC_CALL: 0x942e7fe3, SHORTCUT_MAX: 0x52800104,
                  0x868eb8: 0xd10103ff, 0x86b5cc: 0xd10183ff, 0x86a188: 0xa9bd7bfd, 0x7a5dec: 0xd10143ff,
                  0x7fd200: 0xd10183ff, 0x868834: 0xa9be7bfd,
                  0x67d7e0: 0xf9424014, 0x67d8b0: 0xf942427c, 0x67d8bc: 0xf9424268, 0x67d910: 0xf9424268,
                  0x67dca4: 0xf9424274, 0x67dd0c: 0xf9424008, 0x67dde8: 0xf9424009, 0x67ddfc: 0xf9424008,
                  0x67df98: 0xf9424274, 0x67e048: 0xf9424274,
                  0x67d680: 0xf902427f, 0x67d820: 0xf902427f, 0x67d918: 0xf9024268, 0x67dccc: 0xf902427f,
                  0x67dfc8: 0xf902427f, 0x67e07c: 0xf902427f,
                  0x67d7e8: 0x91070015, 0x67dcac: 0x91070260, 0x67dd20: 0x91070108, 0x67ddec: 0x91070008,
                  0x67de0c: 0x9105a129, 0x67dfa0: 0x91070260, 0x67e050: 0x91070260}
# Region Intel on the FrontierNav tab ("Battle Probe Effects"), same panel: 5 continents from 5 blocks (panel+0x12a0,
# 0xd0 each) filled by main+0x84dee0, shown by a list (panel+0x88) of up to 5 "mc_List%02d" widgets inline (0x48
# bytes at list+0x1c0, count at list+0x328; only this panel makes one) through the row function main+0x84e084.
# Region Intel on the Map tab ("Mission Info"), same panel: 11 rows {region, missions} at panel+0x16b0 (fill
# main+0x84e390), a list (panel+0x508) of up to 11 widgets inline (0x48 at list+0x1c0, count at list+0x4d8) through the
# row function main+0x85057c (cmp w2, #10). Its own copy of the list template: the sites below mirror INTEL's.
MI_ROW = 0x85057c
# The fill main+0x84e390 has the mission manager count the current missions per region into a sorted tree
# (main+0xd51bc: nodes {left +0, right +8, u32 region +0x1c, u32 count +0x20}), then reads the 11 regions' counts from
# it. After `ldr x1, [sp, #8]` (the tree's root, where the paths with and without a mission manager join; x19 = the
# panel) the generated C lets runtime/regions.c read the mod regions' counts from the same tree.
MI_TREE = 0x84e3f0
MI_MAX = 0x670674                       # mov w4, #11; x19 = the list
MI_COUNT_LOADS = {0x6706c0: (21, 0), 0x670780: (27, 19), 0x67078c: (8, 19), 0x6707cc: (8, 19), 0x671624: (20, 19),
                  0x67168c: (8, 0), 0x671768: (9, 0), 0x67177c: (8, 0), 0x671dc8: (20, 19), 0x671e78: (20, 19)}
MI_COUNT_STORES = {0x670570: (31, 19), 0x670700: (31, 19), 0x6707d4: (8, 19), 0x67164c: (31, 19),
                   0x671df8: (31, 19), 0x671eac: (31, 19)}
MI_BLOCK_ADDS = {0x6706c8: (20, 0), 0x67162c: (0, 19), 0x6716a0: (8, 0), 0x67176c: (8, 0), 0x67178c: (9, 0),
                 0x671dd0: (0, 19), 0x671e80: (0, 19)}     # add xd, xn, #0x1c0 (#0x178: the last widget)
MI_WORDS = {MI_ROW: 0x7100285f, MI_MAX: 0x52800164, MI_TREE: 0xf94007e1,
            0x6706c0: 0xf9426c15, 0x670780: 0xf9426e7b, 0x67078c: 0xf9426e68, 0x6707cc: 0xf9426e68,
            0x671624: 0xf9426e74, 0x67168c: 0xf9426c08, 0x671768: 0xf9426c09, 0x67177c: 0xf9426c08,
            0x671dc8: 0xf9426e74, 0x671e78: 0xf9426e74,
            0x670570: 0xf9026e7f, 0x670700: 0xf9026e7f, 0x6707d4: 0xf9026e68, 0x67164c: 0xf9026e7f,
            0x671df8: 0xf9026e7f, 0x671eac: 0xf9026e7f,
            0x6706c8: 0x91070014, 0x67162c: 0x91070260, 0x6716a0: 0x91070108, 0x67176c: 0x91070008,
            0x67178c: 0x9105e129, 0x671dd0: 0x91070260, 0x671e80: 0x91070260}
BP_FILL_CALLS = (0x84ddd0, 0x84fc4c)    # bl main+0x84dee0, x0 = the panel
BP_ROW = 0x84e084                       # cmp w2, #4
BP_MAX = 0x670168                       # mov w4, #5; x19 = the list
BP_COUNT_LOADS = {0x6701c0: (21, 0), 0x670280: (27, 19), 0x67028c: (8, 19), 0x6702cc: (8, 19), 0x6712e4: (20, 19),
                  0x67134c: (8, 0), 0x671428: (9, 0), 0x67143c: (8, 0), 0x671c58: (20, 19), 0x671d08: (20, 19)}
BP_COUNT_STORES = {0x670074: (31, 19), 0x670200: (31, 19), 0x6702d4: (8, 19), 0x67130c: (31, 19),
                   0x671c88: (31, 19), 0x671d3c: (31, 19)}
BP_BLOCK_ADDS = {0x6701c8: (20, 0), 0x6712ec: (0, 19), 0x671360: (8, 0), 0x67142c: (8, 0), 0x67144c: (9, 0),
                 0x671c60: (0, 19), 0x671d10: (0, 19)}     # add xd, xn, #0x1c0 (#0x178: the last widget)
BP_WORDS = {0x84ddd0: 0x94000044, 0x84fc4c: 0x97fff8a5, BP_ROW: 0x7100105f, BP_MAX: 0x528000a4,
            0x6701c0: 0xf9419415, 0x670280: 0xf941967b, 0x67028c: 0xf9419668, 0x6702cc: 0xf9419668,
            0x6712e4: 0xf9419674, 0x67134c: 0xf9419408, 0x671428: 0xf9419409, 0x67143c: 0xf9419408,
            0x671c58: 0xf9419674, 0x671d08: 0xf9419674,
            0x670074: 0xf901967f, 0x670200: 0xf901967f, 0x6702d4: 0xf9019668, 0x67130c: 0xf901967f,
            0x671c88: 0xf901967f, 0x671d3c: 0xf901967f,
            0x6701c8: 0x91070014, 0x6712ec: 0x91070260, 0x671360: 0x91070108, 0x67142c: 0x91070008,
            0x67144c: 0x9105e129, 0x671c60: 0x91070260, 0x671d10: 0x91070260}
# Far regions (runtime/regions.c "Region ids past the slots"): each mov w<n>, #0x2510 right before a slot index multiply
# (umaddl) -> the registers holding the region id used as the index there.
# (main+0x3fe9e4 indexes with w9 the site before it already mapped, and main+0x5639dc's w1 is a hexagon's index.)
SLOT_SITES = {0x3fe9d0: (9,), 0x3ffb14: (0,), 0x563490: (9,), 0x563898: (20,), 0x563940: (0,),
              0x5639c8: (10,), 0x5639dc: (0,), 0x563a40: (0,), 0x563b00: (0,), 0x563b70: (0,), 0x563c20: (0,),
              0x563d10: (9,), 0x56410c: (9,), 0x5643f8: (9,)}
# The manager's maker (main+0x3fcb0c: stp xzr, xzr, [x0, #304]) and its allocation size (movk w0, #3, lsl #16 after
# mov w0, #0x7bd0: 0x37bd0 bytes): room for the far regions' slots.
SEG_CREATE = 0x3fcb0c
SEG_SIZE = 0x3fcb48
# main+0x3fe658 (sub sp, sp, #0x70): a slot's survey rate, then the average over the 24 slots, unrolled.
SEG_RATES = 0x3fe658
# The manager's loops over its slots: at the counter's add (address: counter register, running pointer registers that
# step with it) and at its end test (cmp x<counter>, #24: address -> counter register).
LOOP_SKIPS = {0x3fd228: (27, (20, 12)), 0x3fef40: (22, (20,)), 0x3ff0d0: (20, (22,)), 0x3ff3b0: (23, ()),
              0x3ff8c0: (22, (25, 23))}
# The end tests' hooks set the compare's flags again (fsub64); the branch after each reads them with fc_sub64 (as
# generated today: a planner change that packs these flags must handle the hooks too).
LOOP_ENDS = {0x3fd230: 27, 0x3fef48: 22, 0x3ff0d8: 20, 0x3ff3b4: 23, 0x3ff8cc: 22}
# Where those loops put their counter into a message they post (str w<counter>, [sp, #off]): the region id instead.
LOOP_POSTS = {0x3fef7c: (22, 0x18), 0x3ff120: (20, 0x68), 0x3ff3dc: (23, 0x78), 0x3ff8ec: (22, 0x18)}
LOOP_WORDS = {SEG_CREATE: 0xa9137c1f, SEG_SIZE: 0x72a00060, SEG_RATES: 0xd101c3ff,
              0x3fd228: 0x9100077b, 0x3fef40: 0x910006d6, 0x3ff0d0: 0x91000694, 0x3ff3b0: 0x910006f7, 0x3ff8c0: 0x910006d6,
              0x3fd230: 0xf100637f, 0x3fef48: 0xf10062df, 0x3ff0d8: 0xf100629f, 0x3ff3b4: 0xf10062ff, 0x3ff8cc: 0xf10062df,
              0x3fef7c: 0xb9001bf6, 0x3ff120: 0xb9006bf4, 0x3ff3dc: 0xb9007bf7, 0x3ff8ec: 0xb9001bf6}
# The survey reward list's panel (runtime/regions.c "Survey Reward List pages"): its allocation (mov w0, #0xaf0, then
# bl main+0x13e17f0), the fill's entry (sub sp, sp, #0xe0), the destructor's entry (stp x29, x30, [sp, #-0x40]!), and
# in the fill the row's zone (and x26, x0, #0xff) and its test (cmp x26, #0x17).
RW_ALLOC_SIZE = 0x841508
RW_ALLOC_CALL = 0x841520
RW_FILL = 0x856040
RW_DTOR = 0x854f20
RW_ZONE = 0x85654c
RW_ZONE_TEST = 0x856550
RW_SHOW = 0x856b58   # in main+0x856ad0 (select on the survey reward pop-up: show that reward in the list): mov w8, w22
RW_WORDS = {RW_ALLOC_SIZE: 0x52815e00, RW_ALLOC_CALL: 0x942e80b4, RW_FILL: 0xd10383ff, RW_DTOR: 0xa9bc7bfd,
            RW_ZONE: 0x92401c1a, RW_ZONE_TEST: 0xf1005f5f,
            RW_SHOW: 0x2a1603e8}
# The places that use a slot number as a region value, which give a far region's id for its own slot: the segment
# init's counter (main+0x3fd110: mov w0, w27; str w27, [x13]; mov w1, w27; stp w27, w19, [x29, #-124];
# str w27, [x10, #4]) and two slot loops (main+0x3ff060: mov w0, w20; main+0x3ff330: mov w1, w23 / mov w0, w23).
SEG_INIT = 0x3fd110          # the segment manager's init: stp x29, x30, [sp, #-0x60]!
# main+0x1823e8: w0 = a region -> 1 when it is past 24 (cmp w0, #0x18; cset w0, gt), "not a field region": the field
# object manager (main+0x36cf40.., the gimmick set it loads for the current region) and main+0xe2dc0 skip such a region,
# so no enemies, wrecks or other gimmicks spawn in it. A far region answers 0 (runtime/regions.c region_field_test).
FIELD_TEST = 0x1823e8
PROBE_REVEAL = 0x3fe980      # a site's probe install reveals segments (x0 = manager, w1 = site): stp x29, x30, [sp, #-0x60]!
INIT_VALUES = {0x3fd378: 'c->x[0] = region_loop_value(c->x[0]);',
               0x3fd37c: 'ST32(c->x[13], (uint32_t)region_loop_value(c->x[27]));',
               0x3fd39c: 'c->x[1] = region_loop_value(c->x[1]);',
               0x3fd658: 'ST32(c->x[29] - 124, (uint32_t)region_loop_value(c->x[27]));',
               0x3fdc7c: 'ST32(c->x[10] + 4, (uint32_t)region_loop_value(c->x[27]));',
               0x3ff0bc: 'c->x[0] = region_loop_value(c->x[0]);',
               0x3ff7e0: 'c->x[0] = region_loop_value(c->x[0]);',
               0x3ff630: 'c->x[1] = region_loop_value(c->x[1]);',
               # the discovery flag on entering a region: add w21, w19, w8 / add w20, w21, w8 (6148 + region)
               0xcb07c: 'c->x[21] = region_flag(c, c->x[19], c->x[21]);',
               0xcbdfc: 'c->x[20] = region_flag(c, c->x[21], c->x[20]);',
               0xcbe90: 'c->x[20] = region_flag(c, c->x[21], c->x[20]);'}
INIT_WORDS = {SEG_INIT: 0xa9ba7bfd, FIELD_TEST: 0x7100601f, PROBE_REVEAL: 0xa9ba7bfd, 0x3fd378: 0x2a1b03e0, 0x3fd37c: 0xb90001bb, 0x3fd39c: 0x2a1b03e1, 0x3fd658: 0x2930cfbb,
              0x3fdc7c: 0xb900055b, 0x3ff0bc: 0x2a1403e0, 0x3ff7e0: 0x2a1703e0, 0x3ff630: 0x2a1703e1,
              0xcb07c: 0x0b080275, 0xcbdfc: 0x0b0802b4, 0xcbe90: 0x0b0802b4}
SLOT_WORDS = {0x3fe9d0: 0x5284a20a, 0x3ffb14: 0x5284a209, 0x563490: 0x5284a20a,
              0x563898: 0x5284a208, 0x563940: 0x5284a209, 0x5639c8: 0x5284a20b, 0x5639dc: 0x5284a20a,
              0x563a40: 0x5284a20a, 0x563b00: 0x5284a209, 0x563b70: 0x5284a20a, 0x563c20: 0x5284a209,
              0x563d10: 0x5284a20a, 0x56410c: 0x5284a20a, 0x5643f8: 0x5284a20a}
WORDS = {SITE: 0x94000393,  # bl +0xe4c
         LAYER_LIST_END: 0xf9076a68,
         0x7fd1cc: 0xa9434ff4,     # ldp x20, x19, [sp, #0x30]
         0x7fe24c: 0xa9be7bfd, 0x7fe250: 0xf9000bf3,      # stp x29, x30, [sp, #-32]!; str x19, [sp, #16]
         0x7fe1fc: 0xa9be7bfd, 0x7fe200: 0xf9000bf3,
         SEGMAP: 0x51000808,       # sub w8, w0, #2
         0x56ff04: 0x97ffc6aa,     # bl main+0x5619ac
         0x7b0570: 0x97f6c50f,     # bl main+0x5619ac
         SWITCH + 8: 0x51000828,   # sub w8, w1, #2 (the switch keys on the region id in w1)
         MINIMAP_LOAD: 0x386a696d,
         0x87e28c: 0xb9412e68,
         0x87e290: 0x5100292a,     # sub w10, w9, #10
         0x855af8: 0xa9bd7bfd, 0x855c40: 0xa9be7bfd, 0x85694c: 0xa9be7bfd,   # stp x29, x30, [sp, #-n]!
         0x8566b0: 0xf943dc08,     # ldr x8, [x0, #0x7b8]
         0x855f3c: 0xb94ac108,     # ldr w8, [x8, #0xac0]
         0x85686c: 0xaa1f03e8, 0x855fd0: 0xaa1f03e8,   # mov x8, xzr
         **INTEL_WORDS, **SHORTCUT_WORDS, **BP_WORDS, **SLOT_WORDS, **INIT_WORDS,
         **LOOP_WORDS, **RW_WORDS, **MI_WORDS}


def verified(mod):
    if mod.name != 'main' or mod.nso.build_id != BUILD:
        return False
    if mod.nso.source_sha256 != SHA256 or mod.base != 0x7000000000:
        raise ValueError('regions: build identity mismatch')
    for offset, word in WORDS.items():
        if mod.word(mod.base + offset) != word:
            raise ValueError(f'regions: instruction mismatch at main+{offset:#x}')
    return True


def entry(mod, address):
    """C to run at the entry of the function at address, or None."""
    if (reward := REWARD_ENTRIES.get(address - mod.base)):
        return f'if ({reward}(c)) return;'
    if (fn := SHORTCUT_ENTRIES.get(address - mod.base)):
        return f'if ({fn}(c)) return;'
    if address - mod.base == SEG_INIT:
        return 'region_far_setup(c);'
    if address - mod.base == PROBE_REVEAL:
        return 'if (region_probe_reveal(c)) return;'
    if address - mod.base == FIELD_TEST:
        return 'if (region_field_test(c)) return;'
    if address - mod.base == SEG_CREATE:
        return 'region_seg_create(c);'
    if address - mod.base == SEG_RATES:
        return 'if (region_seg_rates(c)) return;'
    if address - mod.base == RW_FILL:
        return 'region_rw_fill(c);'
    if address - mod.base == RW_DTOR:
        return 'region_rw_free(c);'
    if address - mod.base == BP_ROW:
        return 'if (region_bp_row(c)) return;'
    if address - mod.base == MI_ROW:
        return 'if (region_mi_row(c)) return;'
    if address - mod.base == INTEL_ROW:
        return 'if (region_intel_row(c)) return;'
    if address - mod.base == INTEL_DTOR:
        return 'region_intel_free(c);'
    builder = FIELD_LAYERS.get(address - mod.base)
    return f'region_field_layers(c, {mod.base + builder:#x}ull);' if builder else None


def after(mod, address):
    """C to run after the instruction at address, or None."""
    off = address - mod.base
    if (code := INIT_VALUES.get(off)):
        return code
    if (loop := LOOP_SKIPS.get(off)):
        counter, running = loop
        steps = ''.join(f' c->x[{r}] += 0x2510;' for r in running)
        return f'if (region_loop_skip(c->x[{counter}])) {{ c->x[{counter}] += 1;{steps} }}'
    if (counter := LOOP_ENDS.get(off)) is not None:
        return f'(void)(fsub64(&F, c->x[{counter}], region_slot_end()));'
    if (post := LOOP_POSTS.get(off)):
        return f'ST32(c->sp + {post[1]:#x}, (uint32_t)region_loop_value(c->x[{post[0]}]));'
    if off == SEG_SIZE:
        return 'c->x[0] = region_mgr_size(c->x[0]);'
    if off == MI_TREE:
        return 'region_mi_missions(c->x[1]);'
    if off == RW_ALLOC_SIZE:
        return 'c->x[0] = region_rw_size(c->x[0]);'
    if off == RW_ZONE:
        return 'c->x[26] = region_rw_index(c->x[26]);'
    if off == RW_SHOW:
        return 'c->x[8] = region_rw_show_index(c->x[21], c->x[8]);'
    if off == RW_ZONE_TEST:
        return '(void)(fsub64(&F, region_rw_test(c->x[26]), 0x17ull));'
    if (regs := SLOT_SITES.get(off)):
        return ' '.join(f'c->x[{r}] = region_slot(c->x[{r}], {off:#x}ull);' for r in regs)
    if (site := INTEL_COUNT_LOADS.get(off)):
        return f'c->x[{site[0]}] = region_intel_count(c->x[{site[1]}], c->x[{site[0]}]);'
    if (site := INTEL_COUNT_STORES.get(off)):
        value = '0' if site[0] == 31 else f'c->x[{site[0]}]'
        init = 'region_intel_init(c->x[19]); ' if off == 0x670bd4 else ''
        return f'{init}region_intel_set_count(c->x[{site[1]}], {value});'
    if (site := INTEL_BLOCK_ADDS.get(off)):
        return f'c->x[{site[0]}] = region_intel_block(c->x[{site[1]}], c->x[{site[0]}]);'
    if (site := SHORTCUT_COUNT_LOADS.get(off)):
        return f'c->x[{site[0]}] = region_shortcut_count(c->x[{site[1]}], c->x[{site[0]}]);'
    if (site := SHORTCUT_COUNT_STORES.get(off)):
        value = '0' if site[0] == 31 else f'c->x[{site[0]}]'
        init = 'region_shortcut_init(c->x[19]); ' if off == 0x67d680 else ''
        return f'{init}region_shortcut_set_count(c->x[{site[1]}], {value});'
    if (site := SHORTCUT_BLOCK_ADDS.get(off)):
        return f'c->x[{site[0]}] = region_shortcut_block(c->x[{site[1]}], c->x[{site[0]}]);'
    if (site := MI_COUNT_LOADS.get(off)):
        return f'c->x[{site[0]}] = region_mi_count(c->x[{site[1]}], c->x[{site[0]}]);'
    if (site := MI_COUNT_STORES.get(off)):
        value = '0' if site[0] == 31 else f'c->x[{site[0]}]'
        init = 'region_mi_init(c->x[19]); ' if off == 0x670570 else ''
        return f'{init}region_mi_set_count(c->x[{site[1]}], {value});'
    if (site := MI_BLOCK_ADDS.get(off)):
        return f'c->x[{site[0]}] = region_mi_block(c->x[{site[1]}], c->x[{site[0]}]);'
    if off == MI_MAX:
        return 'c->x[4] = region_mi_max(c->x[19], c->x[4]);'
    if (site := BP_COUNT_LOADS.get(off)):
        return f'c->x[{site[0]}] = region_bp_count(c->x[{site[1]}], c->x[{site[0]}]);'
    if (site := BP_COUNT_STORES.get(off)):
        value = '0' if site[0] == 31 else f'c->x[{site[0]}]'
        init = 'region_bp_init(c->x[19]); ' if off == 0x670074 else ''
        return f'{init}region_bp_set_count(c->x[{site[1]}], {value});'
    if (site := BP_BLOCK_ADDS.get(off)):
        return f'c->x[{site[0]}] = region_bp_block(c->x[{site[1]}], c->x[{site[0]}]);'
    if off == BP_MAX:
        return 'c->x[4] = region_bp_max(c->x[19], c->x[4]);'
    if off == SHORTCUT_ALLOC_SIZE:
        return 'c->x[0] = region_shortcut_size(c->x[0]);'
    if off == SHORTCUT_MAX:
        return 'c->x[4] = region_shortcut_max(c->x[19], c->x[4]);'
    if off == INTEL_ALLOC_SIZE:
        return 'c->x[0] = region_intel_size(c->x[0]);'
    if off == INTEL_MAX:
        return 'c->x[4] = region_intel_max(c->x[19], c->x[4]);'
    if (reg := REWARD_LOOKUPS.get(address - mod.base)) is not None:
        return f'c->x[8] = region_reward_region(c->x[{reg}], c->x[8]);'
    if address - mod.base == LAYER_LIST_END:
        return 'region_layer_entries(c->x[19], c->x[0]);'
    if address - mod.base != MINIMAP_LOAD:
        return None
    return 'c->x[13] = region_minimap_case(c->x[9], c->x[13]);'


def call(mod, address, target):
    """C for the BL at address (to target), or None to emit the plain call."""
    if address - mod.base in BP_FILL_CALLS:
        return f'{{ uint64_t panel_ = c->x[0]; f_{target:x}(c); region_bp_fill(c, panel_); }}'
    if address - mod.base == RW_ALLOC_CALL:
        return f'{{ f_{target:x}(c); region_rw_alloced(c); }}'
    if address - mod.base == SHORTCUT_ALLOC_CALL:
        return f'{{ f_{target:x}(c); region_shortcut_alloced(c); }}'
    if address - mod.base == INTEL_ALLOC_CALL:
        return f'{{ f_{target:x}(c); region_intel_alloced(c); }}'
    if address - mod.base == INTEL_FILL_CALL:
        return f'{{ uint64_t panel_ = c->x[0]; f_{target:x}(c); region_intel_fill(c, panel_); }}'
    if address - mod.base in SEGMAP_SITES:
        assert target == mod.base + SEGMAP
        return f'{{ uint64_t region_ = c->x[0]; f_{target:x}(c); region_segmap_table(c, region_); }}'
    if address - mod.base != SITE:
        return None
    assert target == mod.base + SWITCH
    return f'{{ uint64_t region_ = c->x[1]; f_{target:x}(c); region_hex_table(c, region_); }}'
