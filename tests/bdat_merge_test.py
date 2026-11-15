"""runtime/bdat_merge.c on made-up BDAT files: cell merges, priority, appended rows, the tables taken whole from the top
mod, refused files and the writer's round trip. No game data: the tables are written here.

    python tests/bdat_merge_test.py        (needs clang: MSYS2 UCRT64, or XCX_CLANG=<path to clang>)

With XCX_BDAT_FILES=<a.bdat;b.bdat;...> it also checks that the writer rebuilds every table of those files (mods'
copies of bdat/common.bdat, for example) byte for byte.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FMT = {1: 'B', 2: 'H', 3: 'I', 4: 'b', 5: 'h', 6: 'i', 7: 'I', 8: 'f', 9: 'I', 10: 'B', 11: 'I', 12: 'B', 13: 'H'}
STR = (7, 11)


def clang():
    found = os.environ.get('XCX_CLANG') or shutil.which('clang') or \
        ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    return found if found and Path(found).is_file() else None


class Table:
    """name hash, [(column name hash, type)], base id, rows as tuples (strings as bytes), the string table's kept word,
    and debug=True for a block of debug names before the columns, as the game's own tables have."""

    def __init__(self, name, cols, rows, base=1, extra=0, debug=False):
        self.name, self.cols, self.rows, self.base, self.extra, self.debug = name, cols, rows, base, extra, debug

    def copy(self):
        return Table(self.name, list(self.cols), [tuple(r) for r in self.rows], self.base, self.extra, self.debug)


def write_table(t):
    st = bytearray(b'\0') + struct.pack('<II', t.name, t.extra)
    names = []
    for h, _ in t.cols:
        names.append(len(st))
        st += struct.pack('<I', h)
    seen = {b'': 0}
    rows = []
    for row in t.rows:
        row = list(row)
        for i, (_, ty) in enumerate(t.cols):
            if ty in STR:
                if row[i] not in seen:
                    seen[row[i]] = len(st)
                    st += row[i] + b'\0'
                row[i] = seen[row[i]]
        rows.append(row)
    fmt = '<' + ''.join(FMT[ty] for _, ty in t.cols)
    index = {}
    if t.cols[0][1] == 9:
        for r, row in enumerate(t.rows):
            index.setdefault(row[0], r)
    hidx = b''.join(struct.pack('<II', h, r) for h, r in sorted(index.items()))
    dbg = b'debug names\0' * 3 if t.debug else b''
    co = 48 + len(dbg)
    ho = co + 3 * len(t.cols)
    ro = ho + len(hidx)
    so = ro + struct.calcsize(fmt) * len(rows)
    out = bytearray(struct.pack('<4s11I', b'BDAT', 0x3004, len(t.cols), len(rows), t.base, 0, co, ho, ro,
                                struct.calcsize(fmt), so, len(st)))
    out += dbg
    for (h, ty), no in zip(t.cols, names):
        out += struct.pack('<BH', ty, no)
    out += hidx
    for row in rows:
        out += struct.pack(fmt, *row)
    out += st
    out += b'\0' * (-len(out) % 4)
    return bytes(out)


def write_file(tables):
    parts = [write_table(t) for t in tables]
    offs, o = [], 16 + 4 * len(parts)
    for p in parts:
        offs.append(o)
        o += len(p)
    return struct.pack('<4sIII', b'BDAT', 0x01001004, len(parts), o) + struct.pack('<%dI' % len(offs), *offs) + \
        b''.join(parts)


def read_file(b):
    n = struct.unpack_from('<I', b, 8)[0]
    offs = list(struct.unpack_from('<%dI' % n, b, 16)) + [len(b)]
    out = []
    for i in range(n):
        t = offs[i]
        _, _, cc, rc, base, _, co, ho, ro, rl, so, sl = struct.unpack_from('<4s11I', b, t)
        S = t + so
        name, extra = struct.unpack_from('<II', b, S + 1)
        cols = []
        for k in range(cc):
            ty, no = struct.unpack_from('<BH', b, t + co + 3 * k)
            cols.append((struct.unpack_from('<I', b, S + no)[0], ty))
        fmt = '<' + ''.join(FMT[ty] for _, ty in cols)
        rows = []
        for r in range(rc):
            v = list(struct.unpack_from(fmt, b, t + ro + r * rl))
            for k, (_, ty) in enumerate(cols):
                if ty in STR:
                    v[k] = b[S + v[k]:b.index(b'\0', S + v[k])]
            rows.append(tuple(v))
        out.append(Table(name, cols, rows, base, extra))
    return out


COLS = [(0x11111111, 9), (0x22222222, 1), (0x33333333, 5), (0x44444444, 8), (0x55555555, 7), (0x66666666, 13)]


def make_rows(n, seed):
    return [(0x1000 * (seed + 1) + r, r & 0xff, -r, r / 2, b'name%d' % (r % 3) if r % 4 else b'', r * 7)
            for r in range(n)]


def game():
    return [Table(0xA0000001, COLS, make_rows(8, 0), debug=True, extra=0x12345678),
            Table(0xA0000002, COLS, make_rows(6, 1), debug=True, extra=0x9abcdef0),
            Table(0xA0000003, [(0x77777777, 9), (0x88888888, 2)], [(0x5000 + r, r) for r in range(5)], base=10,
                  debug=True),
            Table(0xA0000004, COLS[:3], [(0x6000 + r, r, r) for r in range(4)], debug=True)]


def mod_of(tables):
    """A mod tool's copy: the same tables without the debug block and with the kept word cleared."""
    out = []
    for t in tables:
        c = t.copy()
        c.debug, c.extra = False, 0
        out.append(c)
    return out


def edit(t, r, c, v):
    row = list(t.rows[r])
    row[c] = v
    t.rows[r] = tuple(row)


@unittest.skipUnless(clang(), 'needs clang (MSYS2 UCRT64, or XCX_CLANG)')
class BdatMerge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.exe = cls.dir / ('bdat_merge_check' + ('.exe' if os.name == 'nt' else ''))
        rt = ROOT / 'runtime'
        subprocess.run([clang(), '-O1', '-Wall', '-Werror', f'-I{rt}', str(ROOT / 'tests/bdat_merge_check.c'),
                        str(rt / 'bdat_merge.c'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def merge(self, base, *mods, expect=0):
        """mods: (name, tables) highest priority first. Returns (tables, notes, summary dict)."""
        args = [str(self.exe), 'merge', self.put('game.bdat', write_file(base)), str(self.dir / 'out.bdat')]
        for name, tables in mods:
            args.append('%s=%s' % (name, self.put(name + '.bdat', write_file(tables))))
        r = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(r.returncode, expect, r.stdout + r.stderr)
        lines = r.stdout.splitlines()
        if expect:
            return None, lines, None
        notes = [x[5:] for x in lines if x.startswith('note ')]
        summary = dict(kv.split('=') for kv in next(x for x in lines if x.startswith('summary ')).split()[1:])
        return read_file((self.dir / 'out.bdat').read_bytes()), notes, {k: int(v) for k, v in summary.items()}

    def put(self, name, data):
        p = self.dir / name
        p.write_bytes(data)
        return str(p)

    def test_round_trip(self):
        tables = mod_of(game())
        edit(tables[0], 2, 4, b'')
        path = self.put('rt.bdat', write_file(tables))
        out = subprocess.run([str(self.exe), 'rewrite', path], capture_output=True, text=True, check=True).stdout
        self.assertEqual(out.split()[-1], '0')

    def test_cells_merge_and_the_top_mod_wins_a_clash(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 1, 2, 100)            # both set table 1 row 1 column 2
        edit(b[0], 1, 2, 200)
        edit(b[0], 2, 3, 9.5)            # only B
        edit(a[0], 3, 4, b'from a')      # only A, a string
        edit(b[0], 4, 4, b'name1')       # B reuses a string that is already there
        for order, winner in (((('a', a), ('b', b)), 100), ((('b', b), ('a', a)), 200)):
            out, notes, s = self.merge(g, *order)
            t = out[0]
            self.assertEqual(t.rows[1][2], winner)
            self.assertEqual(t.rows[2][3], 9.5)
            self.assertEqual(t.rows[3][4], b'from a')
            self.assertEqual(t.rows[4][4], b'name1')
            self.assertEqual(t.rows[5], g[0].rows[5])
            self.assertEqual(s, {'changed': 1, 'merged': 1, 'whole': 0, 'overridden': 1})
            self.assertEqual(len(notes), 1)
            self.assertIn("%s's value, not %s's" % (order[0][0], order[1][0]), notes[0])
            self.assertEqual([x.rows for x in out[1:]], [x.rows for x in g[1:]])

    def test_tables_keep_their_bytes(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[1], 0, 1, 7)
        del b[2].rows[4]                 # one mod only: any change is fine, its own table is used
        raw = write_file(g)
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        got = (self.dir / 'out.bdat').read_bytes()
        self.assertEqual(s['changed'], 2)
        self.assertEqual(s['merged'], 0)
        self.assertEqual(out[2].rows, b[2].rows)
        self.assertEqual(out[1].rows, a[1].rows)
        # unchanged tables are the game's bytes, debug block included
        self.assertIn(write_table(g[0]), got)
        self.assertIn(write_table(g[3]), got)
        self.assertIn(write_table(a[1]), got)
        self.assertIn(write_table(b[2]), got)
        self.assertNotIn(write_table(g[1]), got)
        self.assertLess(len(got), len(raw))

    def test_appended_rows(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 0, 1, 50)
        b[0].rows += [(0x9001, 1, 2, 3.0, b'new row', 4), (0x9002, 5, 6, 7.0, b'', 8)]
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(out[0].rows[:-2], a[0].rows)
        self.assertEqual(out[0].rows[-2:], b[0].rows[-2:])
        self.assertEqual(s['merged'], 1)
        a[0].rows += b[0].rows[-2:]      # the same rows from both: merged once
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(len(out[0].rows), len(g[0].rows) + 2)
        self.assertEqual(s['merged'], 1)
        a[0].rows[-1] = (0x9003,) + a[0].rows[-1][1:]   # different rows: the top mod's whole table
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(out[0].rows, a[0].rows)
        self.assertEqual(s['whole'], 1)
        self.assertIn('two mods add different rows', notes[0])

    def test_whole_tables(self):
        cases = {
            'rows removed or renumbered': lambda t: t.rows.pop(),
            'other columns': lambda t: (setattr(t, 'cols', t.cols[:-1]), setattr(t, 'rows', [r[:-1] for r in t.rows])),
            'rows reordered': lambda t: t.rows.__setitem__(slice(0, 2), [t.rows[1], t.rows[0]]),
        }
        for reason, change in cases.items():
            with self.subTest(reason):
                g = game()
                a, b = mod_of(g), mod_of(g)
                edit(a[1], 2, 2, 77)
                change(b[1])
                for (top, tt), (other, ot) in ((('a', a), ('b', b)), (('b', b), ('a', a))):
                    out, notes, s = self.merge(g, (top, tt), (other, ot))
                    self.assertEqual(out[1].rows, tt[1].rows)
                    self.assertEqual(out[1].cols, tt[1].cols)
                    self.assertEqual(s['whole'], 1)
                    self.assertEqual(len(notes), 1)
                    self.assertIn("%s's whole table is used (b: %s); left out: %s" % (top, reason, other), notes[0])

    def test_a_renamed_key_is_an_edit(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 3, 0, 0xdeadbeef)     # a new key: an edit, not a reorder
        edit(b[0], 3, 1, 9)
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(out[0].rows[3][:2], (0xdeadbeef, 9))
        self.assertEqual(s['merged'], 1)

    def test_refused(self):
        g = game()
        a = mod_of(g)
        _, lines, _ = self.merge(g, ('a', a), ('b', mod_of(g)[:3]), expect=2)
        self.assertIn('unmergeable b: its list of tables differs', lines[-1])
        good = write_file(a)
        t0 = struct.unpack_from('<I', good, 16)[0]
        ro = struct.unpack_from('<I', good, t0 + 32)[0]
        bad_string = bytearray(good)   # row 0's string cell (column 4, at byte 11 of the row) points past the strings
        struct.pack_into('<I', bad_string, t0 + ro + 11, 0xffffff00)
        for name, data in (('short', good[:100]), ('magic', b'XDAT' + good[4:]),
                           ('offset', good[:16] + struct.pack('<I', len(good) + 8) + good[20:]),
                           ('string', bytes(bad_string))):
            with self.subTest(name):
                path = self.put('bad.bdat', data)
                r = subprocess.run([str(self.exe), 'merge', self.put('game.bdat', write_file(g)),
                                    str(self.dir / 'out.bdat'), 'a=' + path], capture_output=True, text=True)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertIn('unmergeable a:', r.stdout)

    def test_real_files_round_trip(self):
        files = [f for f in os.environ.get('XCX_BDAT_FILES', '').split(';') if f]
        if not files:
            self.skipTest('XCX_BDAT_FILES not set')
        out = subprocess.run([str(self.exe), 'rewrite', *files], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            self.assertTrue(line.endswith(' 0'), line)


if __name__ == '__main__':
    unittest.main()
