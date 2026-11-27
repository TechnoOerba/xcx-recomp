"""The data table merge's side of the numbers handed out to content mods (runtime/bdat_merge.c, docs/mod-ids.md):
'@kind:name' in JSON tables, local numbers moved, new tables, rows put at the ids they were given, spare game rows two
mods take, values two mods' added rows share. Made-up tables under the names of the game's that the rules know.

The C check (tests/bdat_merge_check.c, clang as tests/bdat_merge_test.py finds it) runs the cases; with
XCX_BDAT_MERGE_PY=<script> a script with the same command line runs them instead. With both, test_c_and_script_agree
also compares the two on random cases byte for byte.
"""
import json
import os
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bdat_merge_test import ROOT, Table, write_file, read_file, mm3, clang   # noqa: E402

NAMES = 0x97991FA4          # FLD_FieldNameList: region id = row id
COND = 0x7A058ADA           # FLD_NameCondition, pointed at by FieldNameList.nameCondition
WEATHER = 0xFC830E48        # FLD_WeatherRefer: column 0x8C44DF98 holds a region
REWARD = 0xFA12D19B         # SEG_ExploreReward: zone, flag_index
SLOT = 0xF63C8049           # FnetVeinList: spare rows content mods take over
TEXT = 0xF05B0D47           # place names (a table other tables point at by row id)
H_COND = mm3(b'nameCondition')
H_ZONE = mm3(b'zone')
H_FLAG = 0x91C93FAD
H_NAME = mm3(b'name')
H_FLG = mm3(b'flg')
VEIN = 0xF63C8049           # FnetVeinList: a FrontierNav site's number is its row id (base 0)
V_LOC, V_SITE, V_LINK0, V_LINK1, V_LINK2, V_LINK3 = 0xDBAF43F0, 0xBF16FE7D, 0x3791D641, 0xF14C0AAC, 0x38E329FD, 0xD270368A
LOCATIONS = 0xD12FB959      # a location's id is its row id; LOC_MENU's rows go with it
L_ID, L_FLAG, L_SPOT = 0x9759C6FA, 0xBC943D14, 0x655B3679
LOC_MENU = 0x06FBB0CF
PROBES = 0x8B4096AE         # SEG_ProbeList: a probe hexagon's segment id is its row id
P_SITE = 0x073607AA
X_TYPE, X_SEG, X_LOC = 0x5BF33ED8, 0x70BAC821, 0x018D2A75   # a hexagon's type, segment id, field location
MAPINFO, M_LOC = 0x097D1D32, 0x70F37937


def game():
    names = Table(NAMES, [(0x11, 9), (H_NAME, 2), (H_COND, 2), (H_FLG, 2)],
                  [(0x9000 + r, 500 + r, r, 6148 + r) for r in range(23)])
    cond = Table(COND, [(0x12, 9), (0x13, 2)], [(0x9100 + r, r) for r in range(21)])
    weather = Table(WEATHER, [(0x14, 9), (0x8C44DF98, 1), (0x15, 1)], [(0x9200 + r, r + 1, 2) for r in range(19)])
    reward = Table(REWARD, [(0x16, 9), (H_ZONE, 1), (H_FLAG, 13)], [(0x9300 + r, 2 + r // 4, r + 1) for r in range(12)])
    slot = Table(SLOT, [(0x17, 9), (0x18, 2), (H_ZONE, 1)], [(0x9400 + r, 0, 0) for r in range(10)])
    text = Table(TEXT, [(0x19, 9), (0x1A, 7)], [(0x9500 + r, b'place %d' % r) for r in range(30)])
    return [names, cond, weather, reward, slot, text]


def region_mod(tag, regions=(24, 25), flag=0xC000, reward=0x8000, text0=31, cond0=22, slot=None):
    """A region mod as its author writes it, with hand-picked numbers: per region a FieldNameList row (at its id),
    a name condition and a place-name row (appended), a weather row and two reward rows; the region's flag in flg."""
    t = game()
    names, cond, weather, rew, sl, text = t
    for i, z in enumerate(regions):
        names.rows.append((mm3(b'%s names %d' % (tag, i)), text0 + i, cond0 + i, flag + i))
        cond.rows.append((mm3(b'%s cond %d' % (tag, i)), text0 + i))
        text.rows.append((mm3(b'%s text %d' % (tag, i)), b'%s %d' % (tag, i)))
        weather.rows.append((mm3(b'%s weather %d' % (tag, i)), z, 2))
        for j in range(2):
            rew.rows.append((mm3(b'%s reward %d %d' % (tag, i, j)), z, reward + 2 * i + j))
    if slot is not None:
        sl.rows[slot] = (sl.rows[slot][0], 700, regions[0])
    return t


def ids_of(regions=(24, 25), flag=0xC000, reward=0x8000, text0=31, cond0=22, given=None, names=None):
    """The ids entries of region_mod's numbers; given: (regions, flag, reward, text0, cond0) handed out."""
    g = given or (regions, flag, reward, text0, cond0)
    nm = names or ['r%d' % i for i in range(len(regions))]
    out = []
    for i in range(len(regions)):
        out.append({'kind': 'region', 'name': nm[i], 'local': regions[i], 'number': g[0][i]})
        out.append({'kind': 'flag', 'name': 'seen/' + nm[i], 'local': flag + i, 'number': g[1] + i})
        for j in range(2):
            out.append({'kind': 'reward', 'name': '%s/%d' % (nm[i], j), 'local': reward + 2 * i + j,
                        'number': g[2] + 2 * i + j})
        out.append({'kind': 'row', 'name': 'text/' + nm[i], 'table': '<%08X>' % TEXT, 'local': text0 + i,
                    'number': g[3] + i, 'refs': ['<%08X>.<%08X>' % (NAMES, H_NAME), '<%08X>.<%08X>' % (COND, 0x13)]})
        out.append({'kind': 'row', 'name': 'cond/' + nm[i], 'table': '<%08X>' % COND, 'local': cond0 + i,
                    'number': g[4] + i})
    return out


def engines():
    """Every engine there is: ('c', clang) and ('py', script)."""
    out = []
    if clang():
        out.append(('c', clang()))
    if os.environ.get('XCX_BDAT_MERGE_PY'):
        out.append(('py', os.environ['XCX_BDAT_MERGE_PY']))
    return out


def json_of(t, cells=None, table=None):
    """t as bdat-toolset JSON; cells {(row, col): value} put in (names); table: a "table" name of its own."""
    rows = []
    for i, r in enumerate(t.rows):
        row = {'$id': t.base + i}
        for c, ((h, ty), v) in enumerate(zip(t.cols, r)):
            v = '<%08X>' % v if ty == 9 else v.decode() if ty in (7, 11) else v
            row['<%08X>' % h] = cells.get((i, c), v) if cells else v
        rows.append(row)
    d = {'schema': [{'name': '<%08X>' % h, 'type': ty} for h, ty in t.cols], 'rows': rows}
    if table:
        d['table'] = table
    return json.dumps(d)


@unittest.skipUnless(engines(), 'needs clang (MSYS2 UCRT64, or XCX_CLANG) or XCX_BDAT_MERGE_PY')
class ModIds(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.cmds = {}
        for kind, path in engines():
            if kind == 'py':
                cls.cmds['py'] = [sys.executable, path]
                continue
            exe = cls.dir / ('bdat_merge_check' + ('.exe' if os.name == 'nt' else ''))
            rt = ROOT / 'runtime'
            subprocess.run([path, '-O1', '-Wall', '-Werror', f'-I{rt}', str(ROOT / 'tests/bdat_merge_check.c'),
                            str(rt / 'bdat_merge.c'), str(rt / 'gimk_merge.c'), '-o', str(exe)], check=True)
            cls.cmds['c'] = [str(exe)]
        cls.cmd = cls.cmds.get('c') or cls.cmds['py']

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_merge(self, cmd, base, mods, ids, tag='m'):
        """mods: (name, tables or None, {stem: json text}) highest priority first; ids: {name: entries}."""
        d = self.dir / tag
        d.mkdir(exist_ok=True)
        (d / 'ids.json').write_text(json.dumps(ids))
        (d / 'game.bdat').write_bytes(write_file(base))
        args = cmd + ['merge', str(d / 'game.bdat'), str(d / 'out.bdat'), '--ids=' + str(d / 'ids.json')]
        for name, tables, jsons in mods:
            if tables is not None:
                (d / (name + '.bdat')).write_bytes(write_file(tables))
                args.append('%s=%s' % (name, d / (name + '.bdat')))
            for stem, text in sorted((jsons or {}).items()):
                (d / name).mkdir(exist_ok=True)
                (d / name / (stem + '.json')).write_text(text)
                args.append('%s=%s' % (name, d / name / (stem + '.json')))
        r = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        notes = [x[5:] for x in r.stdout.splitlines() if x.startswith('note ')]
        return (d / 'out.bdat').read_bytes(), notes

    def merge(self, base, mods, ids):
        out, notes = self.run_merge(self.cmd, base, mods, ids)
        return {t.name: t for t in read_file(out)}, notes

    def has(self, notes, *parts):
        self.assertTrue(any(all(p in n for p in parts) for n in notes), '%s not in %s' % (parts, notes))

    # ---- two region mods with the same hand-picked numbers

    def test_two_mods_with_the_same_numbers_combine_once_the_lower_ones_move(self):
        a, b = region_mod(b'a'), region_mod(b'b')
        ids = {'a': ids_of(), 'b': ids_of(given=((26, 27), 0xC002, 0x8004, 33, 24))}
        t, notes = self.merge(game(), [('a', a, None), ('b', b, None)], ids)
        names = t[NAMES]
        self.assertEqual(len(names.rows), 27)
        self.assertEqual([r[1:] for r in names.rows[23:]],
                         [(31, 22, 0xC000), (32, 23, 0xC001), (33, 24, 0xC002), (34, 25, 0xC003)])
        self.assertEqual([r[1] for r in t[COND].rows[21:]], [31, 32, 33, 34])
        self.assertEqual([r[1] for r in t[TEXT].rows[30:]], [b'a 0', b'a 1', b'b 0', b'b 1'])
        self.assertEqual(sorted(r[1] for r in t[WEATHER].rows[19:]), [24, 25, 26, 27])
        self.assertEqual(sorted(r[2] for r in t[REWARD].rows[12:]), list(range(0x8000, 0x8008)))
        self.assertFalse([n for n in notes if 'caveat' in n or 'left out' in n], notes)

    def test_either_priority_gives_each_mod_its_own_rows(self):
        a, b = region_mod(b'a'), region_mod(b'b')
        ids = {'b': ids_of(), 'a': ids_of(given=((26, 27), 0xC002, 0x8004, 33, 24))}
        t, _ = self.merge(game(), [('b', b, None), ('a', a, None)], ids)
        self.assertEqual([r[1] for r in t[TEXT].rows[30:]], [b'b 0', b'b 1', b'a 0', b'a 1'])
        self.assertEqual(t[NAMES].rows[25][1:3], (33, 24))

    def test_a_gap_left_by_a_mod_that_is_off_is_filled(self):
        b = region_mod(b'b')
        ids = {'b': ids_of(given=((26, 27), 0xC002, 0x8004, 33, 24))}
        t, notes = self.merge(game(), [('b', b, None)], ids)
        names = t[NAMES]
        self.assertEqual(len(names.rows), 27)
        self.assertEqual(names.rows[23][1:], game()[0].rows[19][1:])   # a copy of the placeholder region 20
        self.assertEqual(names.rows[25][1:], (33, 24, 0xC002))
        self.assertEqual(len({r[0] for r in names.rows}), 27)           # keys stay unique
        self.assertEqual([r[1] for r in t[TEXT].rows[32:34]], [b'b 0', b'b 1'])

    # ---- names in JSON tables

    def test_names_in_json_cells_and_a_new_table_named_after_a_region(self):
        g = game()
        w = g[2]
        w.rows.append((0x99990001, 24, 2))
        new = Table(mm3(b'SEG_TerminalZone26'), [(0x1B, 9), (H_ZONE, 1), (0x1C, 2)], [(0x99990002, 24, 0xC000)])
        cells = {(19, 1): '@region:isle'}
        jsons = {'%08X' % WEATHER: json_of(w, cells),
                 'hexes': json_of(new, {(0, 1): '@region:isle', (0, 2): '@flag:hex'},
                                  table='SEG_TerminalZone@region:isle')}
        ids = {'n': [{'kind': 'region', 'name': 'isle', 'number': 30}, {'kind': 'flag', 'name': 'hex', 'number': 0xC123}]}
        t, notes = self.merge(game(), [('n', None, jsons)], ids)
        self.assertEqual(t[WEATHER].rows[19][1], 30)
        self.assertIn(mm3(b'SEG_TerminalZone30'), t)
        self.assertEqual(t[mm3(b'SEG_TerminalZone30')].rows[0][1:], (30, 0xC123))

    def test_a_name_no_mod_json_declares_leaves_its_table_out(self):
        w = game()[2]
        w.rows.append((0x99990001, 24, 2))
        jsons = {'%08X' % WEATHER: json_of(w, {(19, 1): '@region:nowhere'})}
        t, notes = self.merge(game(), [('n', None, jsons)], {'n': []})
        self.has(notes, 'left out', 'names @region:nowhere, which its mod.json does not declare')
        self.assertEqual(len(t[WEATHER].rows), 19)

    def test_a_new_table_two_mods_add_is_the_higher_ones(self):
        new = lambda v: Table(mm3(b'SEG_TerminalZone24'), [(0x1B, 9), (H_ZONE, 1)], [(0x99990002, v)])
        ids = {'x': [{'kind': 'region', 'name': 'r', 'number': 24}], 'y': [{'kind': 'region', 'name': 'r', 'number': 24}]}
        t, notes = self.merge(game(), [('x', None, {'a': json_of(new(1), table='SEG_TerminalZone@region:r')}),
                                       ('y', None, {'a': json_of(new(2), table='SEG_TerminalZone@region:r')})], ids)
        self.assertEqual(t[mm3(b'SEG_TerminalZone24')].rows[0][1], 1)
        self.has(notes, 'left out: y: its new table', "x's is")

    def test_a_binary_copy_with_new_tables_keeps_them_and_merges_the_rest(self):
        a = region_mod(b'a') + [Table(mm3(b'SEG_TerminalZone24'), [(0x1B, 9), (H_ZONE, 1)], [(0x99990002, 24)])]
        b = region_mod(b'b') + [Table(mm3(b'SEG_TerminalZone24'), [(0x1B, 9), (H_ZONE, 1)], [(0x99990003, 24)])]
        ids = {'a': ids_of(), 'b': ids_of(given=((26, 27), 0xC002, 0x8004, 33, 24))}
        t, notes = self.merge(game(), [('a', a, None), ('b', b, None)], ids)
        self.assertEqual(t[mm3(b'SEG_TerminalZone24')].rows[0][0], 0x99990002)
        self.assertEqual(t[mm3(b'SEG_TerminalZone26')].rows[0], (0x99990003, 26))   # renamed with its region

    def test_region_tables_moved_in_a_chain_keep_both(self):
        # its 24 goes to 25 and its 25 to 26: the table named 24 becomes 25 while the one named 25 becomes 26, whatever
        # order their names sort in (the three region templates' names for 24 and 25 sort both ways)
        for tmpl in (b'SEG_TerminalZone', b'MNU_SegMapInfoZone', b'SEG_ProbeListZone'):
            m = game() + [Table(mm3(b'%s%d' % (tmpl, z)), [(0x1B, 9), (H_ZONE, 1)], [(0x99990000 + z, z)]) for z in (24, 25)]
            ids = {'c': [{'kind': 'region', 'name': 'x', 'local': 24, 'number': 25},
                         {'kind': 'region', 'name': 'y', 'local': 25, 'number': 26}]}
            t, notes = self.merge(game(), [('c', m, None)], ids)
            self.assertEqual(t[mm3(tmpl + b'25')].rows[0], (0x99990000 + 24, 25), (tmpl, notes))
            self.assertEqual(t[mm3(tmpl + b'26')].rows[0], (0x99990000 + 25, 26), (tmpl, notes))
            self.assertNotIn(mm3(tmpl + b'24'), t)
            self.assertFalse([n for n in notes if 'renamed with its region' in n], notes)

    # ---- what can't be moved: made visible

    def test_a_spare_game_row_two_mods_take_is_the_higher_ones_whole(self):
        a, b = region_mod(b'a', slot=3), region_mod(b'b', slot=3)
        b[4].rows[3] = (b[4].rows[3][0], 800, 24)
        ids = {'a': ids_of(), 'b': ids_of(given=((26, 27), 0xC002, 0x8004, 33, 24))}
        t, notes = self.merge(game(), [('a', a, None), ('b', b, None)], ids)
        self.assertEqual(t[SLOT].rows[3][1:], (700, 24))
        self.has(notes, 'left out: b: table F63C8049 row 4', 'a takes this row too')

    def test_a_value_two_mods_added_rows_share_is_a_caveat(self):
        g = game()
        a, b = game(), game()
        a[3].rows.append((0x1234, 2, 99))   # flag_index 99, not declared by either
        b[3].rows.append((0x5678, 2, 99))
        t, notes = self.merge(g, [('a', a, None), ('b', b, None)], {'a': [], 'b': []})
        self.has(notes, 'caveat: b: table FA12D19B column 2: a row it adds has 99', "as one of a's does")

    # ---- the world raster

    @unittest.skipUnless('c' in engines()[0], 'the raster is combined by the C side only')
    def test_two_region_mods_rasters_combine_with_their_numbers(self):
        head = b'zatr' + bytes(28)
        g = bytearray(head + bytes([1]) * 100)
        a, b = bytearray(g), bytearray(g)
        a[32 + 10:32 + 20] = bytes([24]) * 10       # a's land, region 24
        b[32 + 15:32 + 30] = bytes([24]) * 15       # b's land, its own 24 (given 26); overlaps a on 15..19
        d = self.dir / 'raster'
        d.mkdir(exist_ok=True)
        for nm, data in (('game', g), ('a', a), ('b', b)):
            (d / (nm + '.atr')).write_bytes(bytes(data))
        (d / 'ids.json').write_text(json.dumps({'a': [{'kind': 'region', 'name': 'x', 'local': 24, 'number': 24}],
                                                'b': [{'kind': 'region', 'name': 'y', 'local': 24, 'number': 26}]}))
        r = subprocess.run(self.cmds['c'] + ['raster', str(d / 'game.atr'), str(d / 'out.atr'), 'a=' + str(d / 'a.atr'),
                                             'b=' + str(d / 'b.atr'), '--ids=' + str(d / 'ids.json')],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        out = (d / 'out.atr').read_bytes()[32:]
        self.assertEqual(out[10:20], bytes([24]) * 10)      # a is higher: its cells win where both paint
        self.assertEqual(out[20:30], bytes([26]) * 10)      # b's own cells, with the number it was given
        self.assertEqual(out[:10], bytes([1]) * 10)
        self.assertIn('left out: b: 5 of its raster cells', r.stdout)

    # ---- FrontierNav sites: a site's number is its FnetVeinList row; its location and probe list rows are appended

    @staticmethod
    def site_game(nloc=20):
        """The site tables as the game's: 110 sites (row 47 unused), nloc locations and their menu rows, 6 probe list
        rows, a hexagon table, map marks."""
        vein = Table(VEIN, [(0x21, 9), (V_LOC, 2), (V_SITE, 2), (V_LINK0, 1), (V_LINK1, 1), (V_LINK2, 1), (V_LINK3, 1)],
                     [(0xA000 + r, 0 if r == 47 else 100 + r, r, (r + 1) % 110, 0, 0, 0) for r in range(110)], base=0)
        loc = Table(LOCATIONS, [(0x22, 9), (L_ID, 2), (L_FLAG, 2), (L_SPOT, 1)],
                    [(0xA200 + r, r + 1, 6400 + r, r % 3) for r in range(nloc)])
        menu = Table(LOC_MENU, [(0x23, 9), (0x24, 2)], [(0xA300 + r, r) for r in range(nloc)])
        probe = Table(PROBES, [(0x25, 9), (P_SITE, 2), (0x26, 1)], [(0xA400 + r, r + 1, 0) for r in range(6)])
        hexes = Table(mm3(b'SEG_Terminal1501'), [(0x27, 9), (X_TYPE, 1), (X_SEG, 2), (X_LOC, 2)],
                      [(0xA500 + r, 1 + r % 2, r + 1, r + 1) for r in range(8)])
        marks = Table(MAPINFO, [(0x28, 9), (M_LOC, 3)], [(0xA600 + r, r + 1) for r in range(5)])
        other = Table(mm3(b'Some_OtherTable'), [(0x29, 9), (X_LOC, 2)], [(0xA700 + r, r) for r in range(5)])
        return [vein, loc, menu, probe, hexes, marks, other]

    @classmethod
    def site_mod(cls, tag, n=2):
        """A mod with n sites as its author writes them: vein rows 110.., location rows 21.. (ids = row ids), menu
        rows, probe list rows 7.., a probe hexagon each (and a normal hexagon whose segment id happens to be 7), map
        marks; each site linked to the game's site 1."""
        t = cls.site_game()
        vein, loc, menu, probe, hexes, marks, other = t
        other.rows.append((mm3(b'%s other' % tag), 21))       # a column named like a hexagon's, in another table
        for i in range(n):
            vein.rows.append((mm3(b'%s vein %d' % (tag, i)), 21 + i, 110 + i, 1, 0, 0, 0))
            loc.rows.append((mm3(b'%s loc %d' % (tag, i)), 21 + i, 0xC000 + i, 110 + i))
            menu.rows.append((mm3(b'%s menu %d' % (tag, i)), 900 + i))
            probe.rows.append((mm3(b'%s probe %d' % (tag, i)), 110 + i, 0))
            hexes.rows.append((mm3(b'%s hex %d' % (tag, i)), 1, 7 + i, 21 + i))
            marks.rows.append((mm3(b'%s mark %d' % (tag, i)), 21 + i))
        hexes.rows.append((mm3(b'%s normal hex' % tag), 2, 7, 0))
        r = list(vein.rows[1]); r[4] = 110; vein.rows[1] = tuple(r)      # the game's site 1 links to its first
        return t

    @staticmethod
    def site_ids(n=2, sites=None, locs=None, probes=None):
        out = []
        for i in range(n):
            out.append({'kind': 'site', 'name': 'fn/%d' % i, 'local': 110 + i, 'number': (sites or [110, 111])[i]})
            out.append({'kind': 'row', 'name': 'loc/%d' % i, 'table': '<%08X>' % LOCATIONS, 'local': 21 + i,
                        'number': (locs or [21, 22])[i]})
            out.append({'kind': 'row', 'name': 'probe/%d' % i, 'table': '<%08X>' % PROBES, 'local': 7 + i,
                        'number': (probes or [7, 8])[i]})
        return out

    def test_two_site_mods_with_the_same_numbers_get_sites_of_their_own(self):
        a, b = self.site_mod(b'a'), self.site_mod(b'b')
        ids = {'a': self.site_ids(), 'b': self.site_ids(sites=[112, 113], locs=[23, 24], probes=[9, 10])}
        t, notes = self.merge(self.site_game(), [('a', a, None), ('b', b, None)], ids)
        vein, loc, menu, probe = t[VEIN], t[LOCATIONS], t[LOC_MENU], t[PROBES]
        hexes, marks = t[mm3(b'SEG_Terminal1501')], t[MAPINFO]
        self.assertEqual(len(vein.rows), 114)
        self.assertEqual([r[1:3] for r in vein.rows[110:]], [(21, 110), (22, 111), (23, 112), (24, 113)])
        # both linked their first site to the game's site 1 in its first free link: both links kept
        self.assertEqual(vein.rows[1][3:7], (2, 110, 112, 0))
        self.assertFalse([n for n in notes if 'left out' in n], notes)
        self.assertEqual([r[1:] for r in loc.rows[20:]], [(21, 0xC000, 110), (22, 0xC001, 111),
                                                          (23, 0xC000, 112), (24, 0xC001, 113)])
        self.assertEqual([r[1] for r in menu.rows[20:]], [900, 901, 900, 901])   # the menu rows went with them
        self.assertEqual([r[1] for r in probe.rows[6:]], [110, 111, 112, 113])
        # probe hexagons follow their probe rows and locations; a normal hexagon's segment id 7 is not a probe row's
        self.assertEqual(sorted(r[1:] for r in hexes.rows[8:]),
                         [(1, 7, 21), (1, 8, 22), (1, 9, 23), (1, 10, 24), (2, 7, 0), (2, 7, 0)])
        self.assertEqual(sorted(r[1] for r in marks.rows[5:]), [21, 22, 23, 24])
        # a table outside the hexagon tables keeps its value, though its column has the hexagons' name
        self.assertEqual(sorted(r[1] for r in t[mm3(b'Some_OtherTable')].rows[5:]), [21, 21])

    def test_a_site_gap_of_a_mod_that_is_off_has_no_location(self):
        b = self.site_mod(b'b', n=1)
        ids = {'b': self.site_ids(n=1, sites=[112], locs=[21], probes=[7])}
        t, notes = self.merge(self.site_game(), [('b', b, None)], ids)
        vein = t[VEIN]
        self.assertEqual(len(vein.rows), 113)
        self.assertEqual([r[1:3] for r in vein.rows[110:112]], [(0, 110), (0, 111)])   # gaps: no location
        self.assertEqual(vein.rows[112][1:3], (21, 112))
        self.assertEqual(len({r[0] for r in vein.rows}), 113)

    def test_a_location_gap_is_a_copy_of_the_unused_camp_slot(self):
        g = self.site_game(nloc=420)
        m = self.site_game(nloc=420)
        m[1].rows.append((mm3(b'm loc'), 422, 0xC000, 0))
        m[2].rows.append((mm3(b'm menu'), 77))
        ids = {'m': [{'kind': 'row', 'name': 'loc', 'table': '<%08X>' % LOCATIONS, 'local': 421, 'number': 422}]}
        t, notes = self.merge(g, [('m', m, None)], ids)
        loc, menu = t[LOCATIONS], t[LOC_MENU]
        self.assertEqual(len(loc.rows), 422)
        # the gap: CAMP_1501_10's place, not the last row, with its own id (row id 421)
        self.assertEqual(loc.rows[420][1:], (421,) + g[1].rows[399][2:])
        self.assertEqual(menu.rows[420][1:], g[2].rows[399][1:])
        self.assertEqual(loc.rows[421][1:], (422, 0xC000, 0))
        self.assertEqual(menu.rows[421][1], 77)

    def test_a_site_number_among_the_games_rows_is_refused(self):
        m = self.site_mod(b'm', n=1)
        ids = {'m': [{'kind': 'site', 'name': 'fn/0', 'local': 47, 'number': 110}]}
        _, notes = self.merge(self.site_game(), [('m', m, None)], ids)
        self.has(notes, 'left out: m', 'its site "fn/0" (row 47 of table F63C8049): not a row it adds there')

    # ---- the map's placement files (gimmick/<zone>/arrangement_*.gimk)

    @staticmethod
    def gimk(recs, labels, esz=16, magic=b'GMLD'):
        """A placement file: recs = [(instance id, x, label index)], labels = [entry bytes]."""
        import struct
        body = b''.join(struct.pack('<II', i, 0) + struct.pack('<f', x) + bytes(40) + struct.pack('<I', lab)
                        for i, x, lab in recs)
        ents = b''.join(e.ljust(esz, b'\0')[:esz] for e in labels)
        return (b'SPT ' + bytes(12) + b'GIMK' + struct.pack('<HHII', 1, len(recs), 16 + len(body), 0) + body +
                magic + struct.pack('<HHII', 1, len(labels), 16 + len(ents), 0) + ents)

    @staticmethod
    def read_gimk(b):
        import struct
        n, size = struct.unpack_from('<HI', b, 0x16)
        recs = [struct.unpack_from('<I', b, 0x20 + 56 * i)[0:1] + struct.unpack_from('<f', b, 0x20 + 56 * i + 8) +
                struct.unpack_from('<I', b, 0x20 + 56 * i + 52) for i in range(n)]
        s2 = 0x10 + size
        m, size2 = struct.unpack_from('<HI', b, s2 + 6)
        es = (size2 - 16) // m if m else 0
        labels = [b[s2 + 16 + es * k:s2 + 16 + es * (k + 1)].rstrip(b'\0') for k in range(m)]
        assert s2 + size2 == len(b)
        return [(i, round(x, 3), lab) for i, x, lab in recs], labels

    def run_gimk(self, game, mods):
        d = self.dir / 'gimk'
        d.mkdir(exist_ok=True)
        (d / 'game.gimk').write_bytes(game)
        args = []
        for nm, data in mods:
            (d / (nm + '.gimk')).write_bytes(data)
            args.append(nm + '=' + str(d / (nm + '.gimk')))
        r = subprocess.run(self.cmds['c'] + ['gimk', str(d / 'game.gimk'), str(d / 'out.gimk')] + args,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        return self.read_gimk((d / 'out.gimk').read_bytes()), r.stdout

    @unittest.skipUnless('c' in engines()[0], 'placement files are combined by the C side only')
    def test_placement_files_of_two_mods_combine_label_by_label(self):
        labs = [b'LOC_A', b'LOC_B', b'CAMP_07']
        game = self.gimk([(0, 1.0, 0), (0, 2.0, 1), (0, 3.0, 2), (0, 3.5, 2)], labs)
        # a: moves LOC_A, adds SITE_A (two boxes); b: moves LOC_A too (left out), removes CAMP_07's boxes, adds SITE_B
        a = self.gimk([(0, 10.0, 0), (0, 2.0, 1), (0, 3.0, 2), (0, 3.5, 2), (0, 50.0, 3), (0, 51.0, 3)],
                      labs + [b'SITE_A'])
        b = self.gimk([(0, 11.0, 0), (0, 2.0, 1), (0, 60.0, 3)], labs + [b'SITE_B'])
        (recs, labels), notes = self.run_gimk(game, [('a', a), ('b', b)])
        self.assertEqual(labels, labs + [b'SITE_A', b'SITE_B'])
        self.assertEqual(recs, [(0, 10.0, 0), (0, 2.0, 1), (0, 50.0, 3), (0, 51.0, 3), (0, 60.0, 4)])
        self.assertIn('left out: b: its changes to label "LOC_A" 0: a changes it too', notes)
        self.assertIn("gimmick: a: 1 label(s) changed, 1 added, 0 record(s) added to the game's labels, 0 left out", notes)
        self.assertIn("gimmick: b: 1 label(s) changed, 1 added, 0 record(s) added to the game's labels, 1 left out", notes)

    @unittest.skipUnless('c' in engines()[0], 'placement files are combined by the C side only')
    def test_placement_files_added_records_get_instance_ids_of_their_own(self):
        labs = [b'Se_1501_02', b'Se_FN_01']
        game = self.gimk([(100001, 1.0, 0), (100002, 2.0, 1)], labs)
        a = self.gimk([(100001, 1.0, 0), (100002, 2.0, 1), (100003, 70.0, 1)], labs)     # each adds an FN sound
        b = self.gimk([(100001, 1.0, 0), (100002, 2.0, 1), (100003, 80.0, 1)], labs)     # with the same next id
        (recs, labels), notes = self.run_gimk(game, [('a', a), ('b', b)])
        self.assertEqual(labels, labs)
        # records only added under a game label (every site's sound is under Se_FN_01): each mod's kept, the second
        # one's instance id made its own
        self.assertEqual(recs, [(100001, 1.0, 0), (100002, 2.0, 1), (100003, 70.0, 1), (100004, 80.0, 1)])
        self.assertNotIn('left out:', notes)
        self.assertIn('gimmick: b: 0 label(s) changed, 0 added, 1 record(s) added to the game\'s labels', notes)
        # a mod that moves the game's record under that label owns it; another's additions there are still kept
        c = self.gimk([(100001, 1.0, 0), (100002, 9.0, 1)], labs)
        (recs, labels), notes = self.run_gimk(game, [('c', c), ('b', b)])
        self.assertEqual(recs, [(100001, 1.0, 0), (100002, 9.0, 1), (100003, 80.0, 1)])
        # a lower mod that also edits that game record loses the edit, not the records it adds there
        d = self.gimk([(100001, 1.0, 0), (100002, 7.0, 1), (100003, 90.0, 1)], labs)
        (recs, labels), notes = self.run_gimk(game, [('c', c), ('d', d)])
        self.assertEqual(recs, [(100001, 1.0, 0), (100002, 9.0, 1), (100003, 90.0, 1)])
        self.assertIn('left out: d: its changes to label "Se_FN_01" 1: c changes it too', notes)
        # the top mod's file not the game's layout: no combine (its whole copy is used, as without a merge)
        bad = self.gimk([(100001, 1.0, 0)], labs[:1])
        d2 = self.dir / 'gimk'
        (d2 / 'game.gimk').write_bytes(game); (d2 / 'bad.gimk').write_bytes(bad); (d2 / 'd.gimk').write_bytes(d)
        r = subprocess.run(self.cmds['c'] + ['gimk', str(d2 / 'game.gimk'), str(d2 / 'out2.gimk'), 'bad=' + str(d2 / 'bad.gimk'),
                                             'd=' + str(d2 / 'd.gimk')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2, r.stdout)
        self.assertIn("the top mod's file is not the game's layout", r.stdout)
        # as a new label each, both kept, the second record renumbered
        a = self.gimk([(100001, 1.0, 0), (100002, 2.0, 1), (100003, 70.0, 2)], labs + [b'Se_FN_A'])
        b = self.gimk([(100001, 1.0, 0), (100002, 2.0, 1), (100003, 80.0, 2)], labs + [b'Se_FN_B'])
        (recs, labels), notes = self.run_gimk(game, [('a', a), ('b', b)])
        self.assertEqual(recs, [(100001, 1.0, 0), (100002, 2.0, 1), (100003, 70.0, 2), (100004, 80.0, 3)])
        self.assertIn('1 added record(s) given instance ids of their own', notes)

    @unittest.skipUnless('c' in engines()[0], 'placement files are combined by the C side only')
    def test_placement_files_the_same_label_added_alike_is_one_copy(self):
        labs = [b'LOC_A']
        game = self.gimk([(0, 1.0, 0)], labs)
        a = self.gimk([(0, 1.0, 0), (0, 5.0, 1)], labs + [b'SHARED'])
        b = self.gimk([(0, 1.0, 0), (0, 5.0, 1)], labs + [b'SHARED'])
        c = self.gimk([(0, 1.0, 0), (0, 6.0, 1)], labs + [b'SHARED'])
        (recs, labels), notes = self.run_gimk(game, [('a', a), ('b', b), ('c', c)])
        self.assertEqual(labels, [b'LOC_A', b'SHARED'])
        self.assertEqual(recs, [(0, 1.0, 0), (0, 5.0, 1)])
        self.assertIn('left out: c: its added label "SHARED": a adds it too, with other records', notes)
        self.assertNotIn('left out: b', notes)
        # entries that aren't names (an enemy spawn group: 152 bytes of settings) alike, records not: both kept
        grp = bytes([7, 0, 0, 0]) + bytes(148)
        game = self.gimk([(0, 1.0, 0)], [bytes(152)], esz=152, magic=b'GMEP')
        a = self.gimk([(0, 1.0, 0), (0, 5.0, 1)], [bytes(152), grp], esz=152, magic=b'GMEP')
        c = self.gimk([(0, 1.0, 0), (0, 6.0, 1)], [bytes(152), grp], esz=152, magic=b'GMEP')
        (recs, labels), notes = self.run_gimk(game, [('a', a), ('c', c)])
        self.assertEqual(recs, [(0, 1.0, 0), (0, 5.0, 1), (0, 6.0, 2)])
        self.assertFalse([n for n in notes if n.startswith('left out')], notes)

    # ---- C and the reference agree

    @unittest.skipUnless(len(engines()) == 2, 'needs both clang and XCX_BDAT_MERGE_PY')
    def test_c_and_script_agree(self):
        rnd = random.Random(7)
        for case in range(40):
            mods, ids = [], {}
            n = rnd.randint(1, 3)
            for k in range(n):
                name = 'm%d' % k
                regs = tuple(rnd.choice((24, 25, 26)) + i for i in range(rnd.randint(1, 2)))
                kw = dict(regions=regs, flag=rnd.choice((0xC000, 0xC004)), reward=rnd.choice((0x8000, 0x8010)),
                          text0=rnd.choice((31, 33)), cond0=rnd.choice((22, 23)),
                          slot=rnd.choice((None, 2, 5)))
                t = region_mod(name.encode(), **kw)
                if rnd.random() < 0.5:
                    t.append(Table(mm3(b'SEG_TerminalZone%d' % regs[0]), [(0x1B, 9), (H_ZONE, 1)], [(rnd.randint(1, 9), regs[0])]))
                shift = 4 * k * rnd.randint(0, 1)
                given = (tuple(r + shift for r in regs), kw['flag'] + shift, kw['reward'] + shift,
                         kw['text0'] + shift, kw['cond0'] + shift)
                mods.append((name, t, None))
                ids[name] = ids_of(regs, kw['flag'], kw['reward'], kw['text0'], kw['cond0'], given)
            outs = [self.run_merge(self.cmds[e], game(), mods, ids, tag='%s%d' % (e, case)) for e in ('c', 'py')]
            self.assertEqual(outs[0][1], outs[1][1], 'case %d notes' % case)
            self.assertEqual(outs[0][0], outs[1][0], 'case %d bytes' % case)
        for case in range(30):          # FrontierNav sites: numbers, gaps, locations and probe rows moved
            mods, ids, used = [], {}, set()
            for k in range(rnd.randint(1, 3)):
                name = 's%d' % k
                n = rnd.randint(1, 2)
                sites = [rnd.choice([s for s in range(110, 118) if s not in used]) for _ in range(n)]
                if len(set(sites)) < n:
                    sites = sites[:1]
                    n = 1
                used.update(sites)
                locs = [21 + 2 * k + i for i in range(n)] if rnd.random() < 0.5 else [21 + i for i in range(n)]
                probes = [7 + 2 * k + i for i in range(n)]
                mods.append((name, self.site_mod(name.encode(), n), None))
                ids[name] = self.site_ids(n, sites + [0], locs + [0], probes + [0])[:3 * n]
                if rnd.random() < 0.3:                    # a mod that is off: its numbers leave gaps
                    mods.pop()
            if not mods:
                continue
            outs = [self.run_merge(self.cmds[e], self.site_game(), mods, ids, tag='%ss%d' % (e, case))
                    for e in ('c', 'py')]
            self.assertEqual(outs[0][1], outs[1][1], 'site case %d notes' % case)
            self.assertEqual(outs[0][0], outs[1][0], 'site case %d bytes' % case)


if __name__ == '__main__':
    unittest.main()
