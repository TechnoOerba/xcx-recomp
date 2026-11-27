"""runtime/region_layout.c: rows for mod regions added to a panel's menu layout, and a tall panel fitted on the screen.

The game's layouts are game files, so these tests build a small layout of the same shape (LAGP version 10003: a
placement node section and a component section) with three rows, the last one special as Region Intel's Mira row is
(its placement node has three tracks and a two-key position track, its frame flags differ), and check what
tests/region_layout_check.c (the layout code alone) makes of it. Needs clang (XCX_CLANG, else clang on PATH, else MSYS2
UCRT64 on Windows). Run: python tests/region_layout_test.py -v
"""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STEP = 0.024          # the rows' step
MIRA_GAP = 0.03       # the special last row sits a bigger step below the one before it
WIN = (-0.1, -0.05, 0.2, 0.1)   # the window rect: x, y, w, h
LGAP = 0.01           # a divider line sits this far above its row (Region Intel's rect_LineNN)


def clang():
    # XCX_CLANG may be a path or a name on PATH (CI's Linux job gives just 'clang')
    found = shutil.which(os.environ.get('XCX_CLANG', '')) or os.environ.get('XCX_CLANG') or shutil.which('clang') or         ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    return found if found and Path(found).is_file() else None


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def row_y(r, rows):
    """The y of row r (1-based) in a built layout: the last one is special (Mira's gap)."""
    return (rows - 2) * STEP + MIRA_GAP - STEP if r == rows else (r - 2) * STEP


def build(rows=3, kid=False, top_frame=True, lines=False):
    """A layout: components top, mc_Top, mc_ListNN..01 (each optionally followed by a child component mc_Kid), the
    window rect in mc_Top, placement nodes for top (2 position + 2 scale keys), the window and each row. lines: a
    divider line above each row as Region Intel has them (rects rect_LineNN, the last one on a special node)."""
    # ---- placement nodes: table at +64, arrays after it
    nnodes = 2 + rows + (rows if lines else 0)
    arrays = bytearray()
    base = 64 + 64 * nnodes

    def arr(floats):
        o = base + len(arrays)
        arrays.extend(struct.pack('<%df' % len(floats), *floats))
        return o

    nodes = []
    pos = arr([0.2, 0.25, 0.21, 0.26]); scale = arr([1, 1, 0.85, 0.85]); rest = arr([0, 0])
    nodes.append([10, 0, pos, scale, rest, 0, rest + 8, 0, 0, 0, 0, 0, 0, 0, 0, 0])            # 0: top
    pos = arr([0, 0]); scale = arr([1, 1]); end = arr([0])
    nodes.append([1, 0, pos, scale, 0, 0, end, 0, 0, 0, 0, 0, 0, 0, 0, 0])                    # 1: the window
    for r in range(rows):                                                                    # 2..: mc_List01..
        if r == rows - 1:
            y = (rows - 2) * STEP + MIRA_GAP - STEP
            pos = arr([0.01, y, 0.01, y - 0.001]); nxt = arr([0, 0])
            nodes.append([3, 0, pos, nxt, 0, 0, nxt + 8, 0, 0, 0, 0, 0, 0, 0, 0, 0])
        else:
            pos = arr([0.01, (r - 1) * STEP]); nxt = arr([0])
            nodes.append([1, 0, pos, 0, 0, 0, nxt, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    for r in range(1, rows + 1) if lines else ():                                            # then rect_Line01..
        y = row_y(r, rows) - LGAP
        if r == rows:
            pos = arr([0.01, y, 0.01, y - 0.001]); nxt = arr([0, 0])
            nodes.append([3, 0, pos, nxt, 0, 0, nxt + 8, 0, 0, 0, 0, 0, 0, 0, 0, 0])
        else:
            pos = arr([0.01, y]); nxt = arr([0])
            nodes.append([1, 0, pos, 0, 0, 0, nxt, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    u4 = bytearray(struct.pack('<16I', 0, 64, nnodes, *[0] * 13))
    for n in nodes:
        u4 += struct.pack('<16I', *n)
    u4 += arrays

    # ---- components
    names = ['top', 'mc_Top']
    for r in range(rows, 0, -1):
        names.append('mc_List%02d' % r)
        if kid:
            names.append('mc_Kid')
    comp_of_row = {r: names.index('mc_List%02d' % r) for r in range(1, rows + 1)}
    u13 = bytearray(64)
    blocks, ents = {}, []

    def put(blob, align=4):
        while len(u13) % align:
            u13.append(0)
        o = len(u13)
        u13.extend(blob)
        return o

    def display(items, lists):
        body = b''.join(struct.pack('<IHHI', *it) for it in items)
        lb = [struct.pack('<%dH' % len(x), *x) for x in lists]
        off = [64 + len(body)]
        for x in lb[:-1]:
            off.append(off[-1] + len(x))
        return struct.pack('<8I', len(items), 64, len(lists[0]), off[0], len(lists[1]), off[1], len(lists[2]),
                           off[2] if lists[2] else 0) + bytes(32) + body + b''.join(lb)

    for i, name in enumerate(names):
        start = len(u13) + (-len(u13) % 4)
        if name == 'top':
            fr = put(struct.pack('<2I', 8, 1) + struct.pack('<2I', 1, 0 << 16 | 0xffff) if top_frame else
                     struct.pack('<2I', 8, 0))
            ents.append([0, fr, 0, put(display([], [[], [], []])), put(struct.pack('<2I', 0, 8)), start, 0])
        elif name == 'mc_Top':
            items = [(1, 0, 0, 0)] + [(0, 1 + k, comp_of_row[r], k) for k, r in enumerate(range(rows, 0, -1))]
            frames = [(1, 1 << 16 | 0xffff)] + [(0x201 if r == rows else 0x600, (1 + r) << 16 | 0xffff)
                                                for r in range(rows, 0, -1)]
            line_rows = list(range(rows, 0, -1)) if lines else []          # rect 1 + k = rect_Line(line_rows[k])
            items += [(1, 1 + rows + k, 1 + k, 0) for k in range(len(line_rows))]
            frames += [(0x201 if r == rows else 0x600, (1 + rows + r) << 16 | 0xffff) for r in line_rows]
            fr = put(struct.pack('<2I', 8, len(frames)) + b''.join(struct.pack('<2I', *f) for f in frames))
            dl = put(display(items, [list(range(1, rows + 1)), [0] + [1 + rows + k for k in range(len(line_rows))],
                                     []]))
            rc = put(struct.pack('<2I', 1 + len(line_rows), 8) + struct.pack('<2I4f2I', 0, 1, *WIN, 0, 0) +
                     b''.join(struct.pack('<2I4f2I', 0, 1 + rows + r, -0.1, -0.002, 0.2, 0.004, 0, 0)
                              for r in line_rows))
            rect_name_at = rc + 8                    # named below, with the components
            ents.append([0, fr, 0, dl, rc, start, 0])
        elif name.startswith('mc_List'):
            kids = [(0, 0, i + 1, 0)] if kid else []
            fr = put(struct.pack('<2I', 8, len(kids)) + b''.join(struct.pack('<2I', 0, 0 << 16 | 0xffff) for _ in kids))
            dl = put(display(kids, [[0] if kid else [], [], []]))
            ents.append([0, fr, 0, dl, put(struct.pack('<2I', 0, 8)), start, 0])
        else:
            fr = put(struct.pack('<2I', 8, 0))
            ents.append([0, fr, 0, put(display([], [[], [], []])), put(struct.pack('<2I', 0, 8)), start, 0])
        blocks[name] = start
    for i, name in enumerate(names):
        ents[i][6] = put(name.encode() + b'\0', 1)
    struct.pack_into('<I', u13, rect_name_at, put(b'rect_Window\0', 1))
    for k, r in enumerate(range(rows, 0, -1) if lines else ()):
        struct.pack_into('<I', u13, rect_name_at + 32 * (1 + k), put(b'rect_Line%02d\0' % r, 1))
    ct = put(b''.join(struct.pack('<7I', *e) for e in ents))
    # tree: item i = component i + 1; mc_Top's children are the rows (slots 1..rows), a row's children its kid
    n = len(names)
    items = [[0] * 8 for _ in range(n - 1)]
    rows_slots = [names.index('mc_List%02d' % r) for r in range(rows, 0, -1)]
    items[0] = [rows, 1, n - 2, 0, 0, 0, 0, 0]
    idx = [1] + rows_slots
    for c in rows_slots:
        items[c - 1] = [1 if kid else 0, 0, 1 if kid else 0, 1, 0, 0, 0, 0]
    if kid:
        for c in rows_slots:
            items[c - 1][1] = len(idx)
            items[c] = [0, len(idx) + 1, 0, c, 0, 0, 0, 0]
            idx.append(c + 1)
    for c in rows_slots:
        if not kid:
            items[c - 1][1] = len(idx)
    idx.append(0)
    tr = put(struct.pack('<4I', 0, 0, n - 1, 0) + bytes(16) + b''.join(struct.pack('<8I', *t) for t in items))
    ix = put(struct.pack('<%dH' % len(idx), *idx))
    struct.pack_into('<4I', u13, 0, ct, n, tr, ix)

    head = bytearray(0x40)
    head[:4] = b'LAGP'
    struct.pack_into('<I', head, 4, 10003)
    u4_at = 0x40
    u13_at = u4_at + len(u4) + (-len(u4) % 16)
    struct.pack_into('<I', head, 0x18, u4_at)
    struct.pack_into('<I', head, 0x3c, u13_at)
    return bytes(head + u4 + bytes(u13_at - u4_at - len(u4)) + u13)


class Layout:
    """Reads back what the tests check."""

    def __init__(self, d):
        self.d = d
        self.u4, self.u13 = struct.unpack_from('<I', d, 0x18)[0], struct.unpack_from('<I', d, 0x3c)[0]

    def u32(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def node(self, i):
        off, cnt = self.u32(self.u4 + 4), self.u32(self.u4 + 8)
        assert i < cnt
        return struct.unpack_from('<16I', self.d, self.u4 + off + 64 * i)

    def floats(self, rel, n):
        return struct.unpack_from('<%df' % n, self.d, self.u4 + rel)

    def comps(self):
        off, cnt = self.u32(self.u13), self.u32(self.u13 + 4)
        out = []
        for i in range(cnt):
            e = struct.unpack_from('<7I', self.d, self.u13 + off + 28 * i)
            s = self.u13 + e[6]
            out.append((self.d[s:self.d.index(b'\0', s)].decode(), e))
        return out

    def top_rows(self):
        """{row name: (frame flags, placement node)} from mc_Top's display items and frames."""
        comps = self.comps()
        e = dict(comps)['mc_Top']
        b = self.u13 + e[3]
        cnt, at = self.u32(b), self.u32(b + 4)
        items = [struct.unpack_from('<IHHI', self.d, b + at + 12 * j) for j in range(cnt)]
        fb = self.u13 + e[1]
        foff, fcnt = self.u32(fb), self.u32(fb + 4)
        frames = [struct.unpack_from('<2I', self.d, fb + foff + 8 * j) for j in range(fcnt)]
        self.assert_eq(len(items), len(frames))
        return {comps[it[2]][0]: (f[0], f[1] >> 16) for it, f in zip(items, frames) if it[0] == 0}

    def top_lines(self):
        """{line name: (frame flags, placement node)} from mc_Top's kind 1 items and its rect table."""
        e = dict(self.comps())['mc_Top']
        b = self.u13 + e[3]
        cnt, at = self.u32(b), self.u32(b + 4)
        items = [struct.unpack_from('<IHHI', self.d, b + at + 12 * j) for j in range(cnt)]
        fb = self.u13 + e[1]
        foff = self.u32(fb)
        frames = [struct.unpack_from('<2I', self.d, fb + foff + 8 * j) for j in range(cnt)]
        rb = self.u13 + e[4]
        rcnt, roff = self.u32(rb), self.u32(rb + 4)
        rects = [struct.unpack_from('<2I', self.d, rb + roff + 32 * j) for j in range(rcnt)]
        out = {}
        for it, f in zip(items, frames):
            if it[0] != 1:
                continue
            s = self.u13 + rects[it[2]][0]
            name = self.d[s:self.d.index(b'\0', s)].decode()
            if name.startswith('rect_Line'):
                self.assert_eq(rects[it[2]][1], f[1] >> 16)        # the rect and its frame name the same node
                out[name] = (f[0], f[1] >> 16)
        return out

    @staticmethod
    def assert_eq(a, b):
        assert a == b, (a, b)


@unittest.skipUnless(clang(), 'needs clang (MSYS2 UCRT64)')
class RegionLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        rt = ROOT / 'runtime'
        cls.exe = Path(cls.build.name) / ('region_layout_check' + ('.exe' if os.name == 'nt' else ''))
        subprocess.run([clang(), '-O1', '-Wall', '-Wextra', '-Werror', '-DREGION_LAYOUT_CORE_ONLY', f'-I{rt}',
                        str(ROOT / 'tests/region_layout_check.c'), str(rt / 'region_layout.c'), '-lm', '-o',
                        str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_check(self, data, rows, move_up=0, widen=0.0, fit=0, also=()):
        t = Path(self.build.name)
        src, out = t / 'in.wilay', t / 'out.wilay'
        src.write_bytes(data)
        r = subprocess.run([str(self.exe), str(src), str(out), str(rows), str(move_up), str(widen), str(fit), *also],
                           capture_output=True, text=True, check=True)
        w = r.stdout.split()
        return {w[i]: float(w[i + 1]) for i in range(0, len(w), 2)}, out.read_bytes()

    def test_rows_added_and_original_bytes_kept(self):
        data = build()
        res, out = self.run_check(data, 2)
        self.assertEqual((res['rows'], res['added'], res['after']), (3, 2, 5))
        L = Layout(out)
        self.assertEqual(sorted(n for n, _ in L.comps() if n.startswith('mc_List')),
                         ['mc_List01', 'mc_List02', 'mc_List03', 'mc_List04', 'mc_List05'])
        # everything grown is written past the original bytes: the component section is the same apart from its header
        u13 = Layout(data).u13
        self.assertGreater(len(out), len(data))
        self.assertEqual(out[u13 + 16:len(data)], data[u13 + 16:])

    def test_special_last_row_stays_last(self):
        res, out = self.run_check(build(), 2)
        L = Layout(out)
        rows = L.top_rows()
        flags = {n: f for n, (f, _) in rows.items()}
        self.assertEqual(flags, {'mc_List01': 0x600, 'mc_List02': 0x600, 'mc_List03': 0x600, 'mc_List04': 0x600,
                                 'mc_List05': 0x201})
        ys = {}
        for name, (_, nd) in rows.items():
            f = L.node(nd)
            keys = 2 if name == 'mc_List05' else 1
            self.assertEqual(f[0], 3 if name == 'mc_List05' else 1, name)
            ys[name] = L.floats(f[2], 2 * keys)[1::2]
        # ordinary rows one step apart; the special row keeps its own gap and both of its position keys
        for r in range(1, 5):
            self.assertAlmostEqual(ys['mc_List%02d' % r][0], f32((r - 2) * STEP), places=5)
        last = (5 - 2) * STEP + MIRA_GAP - STEP
        self.assertAlmostEqual(ys['mc_List05'][0], last, places=5)
        self.assertAlmostEqual(ys['mc_List05'][1], last - 0.001, places=5)

    def test_window_grows_by_the_rows(self):
        res, out = self.run_check(build(), 3)
        L = Layout(out)
        f = L.node(1)
        sx, sy = L.floats(f[3], 2)
        px, py = L.floats(f[2], 2)
        self.assertAlmostEqual(sx, 1.0, places=5)
        self.assertAlmostEqual(sy, 1 + 3 * STEP / WIN[3], places=4)
        self.assertAlmostEqual(py, 3 * STEP / 2, places=5)

    def test_move_up_and_widen(self):
        res, out = self.run_check(build(), 2, move_up=1, widen=0.016)
        L = Layout(out)
        top = L.node(0)
        self.assertAlmostEqual(L.floats(top[2], 2)[1], 0.25 - 2 * STEP, places=5)   # one row up per row
        win = L.node(1)
        self.assertAlmostEqual(L.floats(win[3], 2)[0], 1 + 0.016 / WIN[2], places=5)  # wider once, not per row

    def test_rows_with_child_components(self):
        res, out = self.run_check(build(kid=True), 2)
        self.assertEqual((res['added'], res['after']), (2, 5))
        L = Layout(out)
        names = [n for n, _ in L.comps()]
        self.assertEqual(names.count('mc_Kid'), 5)   # each new row got its own copy of the child

    def test_fit_scales_a_tall_panel(self):
        res, _ = self.run_check(build(), 2)
        self.assertEqual(res['fit'], 1.0)            # a short panel fits as it is
        res, out = self.run_check(build(), 30, fit=1)
        self.assertLess(res['fit'], 1.0)
        L = Layout(out)
        top = L.node(0)
        sx, sy = L.floats(top[3], 2)
        self.assertAlmostEqual(sx, res['fit'], places=3)
        self.assertAlmostEqual(sy, res['fit'], places=3)
        # the window lies inside the band afterwards: top edge at or below 0.05, bottom at or above 0.525
        win = L.node(1)
        wpy, wsy = L.floats(win[2], 2)[1], L.floats(win[3], 2)[1]
        tpy = L.floats(top[2], 2)[1]
        top_edge = tpy + sy * (wpy + WIN[1] * wsy)
        bottom = tpy + sy * (wpy + (WIN[1] + WIN[3]) * wsy)
        self.assertGreaterEqual(top_edge, 0.05 - 1e-4)
        self.assertLessEqual(bottom, 0.525 + 1e-4)

    def test_rows_stop_at_half_size(self):
        res, out = self.run_check(build(), 60, fit=2)
        n = int(res['added'])
        self.assertGreater(n, 3)
        self.assertLess(n, 60)
        self.assertGreaterEqual(res['fit'], 0.5)
        # n rows fit at half size or more, one more would not
        self.assertGreaterEqual(self.run_check(build(), n)[0]['fit'], 0.5)
        self.assertLess(self.run_check(build(), n + 1)[0]['fit'], 0.5)
        self.assertEqual(res['after'], 3 + n)
        res2, _ = self.run_check(build(), 2, fit=2)   # a few rows: all of them, no scaling
        self.assertEqual((res2['added'], res2['fit']), (2, 1.0))

    def test_failed_row_leaves_the_file_as_it_was(self):
        # without a frame for `top` the move up fails after the window was already grown: the row is undone whole
        data = build(top_frame=False)
        res, out = self.run_check(data, 2, move_up=1)
        self.assertEqual((res['added'], res['after']), (0, 3))
        self.assertEqual(out, data)

    def test_divider_lines_follow_the_rows(self):
        # Region Intel: each new row gets a line just above it; the special line stays above the special last row
        res, out = self.run_check(build(lines=True), 2)
        self.assertEqual((res['added'], res['after'], res['lines']), (2, 5, 5))
        L = Layout(out)
        rows, lines = L.top_rows(), L.top_lines()
        self.assertEqual(sorted(lines), ['rect_Line%02d' % r for r in range(1, 6)])
        for r in range(1, 6):
            fr, nr = rows['mc_List%02d' % r]
            fl, nl = lines['rect_Line%02d' % r]
            self.assertEqual(fl, fr, r)                              # special flags with the special row only
            self.assertEqual(L.node(nl)[0], L.node(nr)[0], r)        # and its kind of node
            ry, ly = L.floats(L.node(nr)[2], 2)[1], L.floats(L.node(nl)[2], 2)[1]
            self.assertAlmostEqual(ry - ly, LGAP, places=5, msg=r)
        res, _ = self.run_check(build(), 2)                          # no lines in the layout: none made
        self.assertEqual(res['lines'], 0)

    def test_header_band_moves_the_rows_down(self):
        # Map Shortcuts: the window's header band stretches with it, so each row moves the rows down by its growth d
        band, h = 0.01, WIN[3]
        d = band * STEP / (h - band)
        res, out = self.run_check(build(), 2, also=('band=%g' % band,))
        self.assertEqual(res['added'], 2)
        L = Layout(out)
        rows = L.top_rows()
        for r in range(1, 5):                                        # the old rows and the new ones, once each
            y = L.floats(L.node(rows['mc_List%02d' % r][1])[2], 2)[1]
            self.assertAlmostEqual(y, (r - 2) * STEP + 2 * d, places=5, msg=r)
        last = (5 - 2) * STEP + MIRA_GAP - STEP + 2 * d             # the special row, kept last
        self.assertAlmostEqual(L.floats(L.node(rows['mc_List05'][1])[2], 2)[1], last, places=5)
        sy = L.floats(L.node(1)[3], 2)[1]
        self.assertAlmostEqual(sy, 1 + 2 * (STEP + d) / h, places=4)
        res, out = self.run_check(build(), 2, move_up=1, also=('band=%g' % band,))   # as Map Shortcuts runs
        self.assertAlmostEqual(Layout(out).floats(Layout(out).node(0)[2], 2)[1], 0.25 - 2 * (STEP + d), places=5)
        _, plain = self.run_check(build(), 2)                        # no band: the rows stay where they were
        self.assertAlmostEqual(Layout(plain).floats(Layout(plain).node(Layout(plain).top_rows()['mc_List01'][1])[2], 2)[1],
                               -STEP, places=5)

    def test_not_a_layout(self):
        res, out = self.run_check(b'NOTALAYOUT' * 10, 2)
        self.assertEqual(res['rows'], -1)
        self.assertEqual(out, b'NOTALAYOUT' * 10)


if __name__ == '__main__':
    unittest.main()
