"""Map mods as pieces (runtime/map_overlay.c, tools/map_pieces.py) on a small made-up map: no game data.

The test writes a game archive (sts.arh + sts.ard) holding a tiny world map, edits it the old way (chunks appended to
a copy of the data file, the header repointed, a texture added), turns that into pieces with map_pieces.py make,
checks them with verify, and adds a second mod by hand that clashes with the first on one entry, adds a texture of
its own (numbered after the first mod's, so its prop group chunk is served with the number fixed) and a nav tile with
a cell pointing at it. tests/map_overlay_check.c then serves the four map files from runtime/map_overlay.c, and every byte
it serves must equal map_pieces.py combine's headers and data layout. Also: no mod with pieces (the overlay stays
off), a mod made for another game version and one with a bad line (both left out), and a pieces.txt changed after
the start (applies at the next start).

    python tests/map_overlay_test.py        (needs clang: MSYS2 UCRT64, or XCX_CLANG=<path to clang>)
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import map_pieces as mp  # noqa: E402


def clang():
    found = shutil.which(os.environ.get('XCX_CLANG', '')) or os.environ.get('XCX_CLANG') or shutil.which('clang') or \
        ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    return found if found and Path(found).is_file() else None


def xbc1(data, name='test'):
    z = zlib.compress(data, 9)
    return b'xbc1' + struct.pack('<III', 1, len(data), len(z)) + b'\0' * 4 + name.encode().ljust(28, b'\0') + z


def group_chunk(texture):
    """A prop group chunk the tool and the overlay understand: base 64, stream table at base + 40 (list A: vertex
    data 1, list B: texture `texture`), texture list at 200 (two entries: global `texture`, and 0xffff = none)."""
    d = bytearray(256)
    struct.pack_into('<I', d, 24, 64)                       # base
    struct.pack_into('<II', d, 32, 200, 2)                  # texture list
    struct.pack_into('<I', d, 64 + 56, 40)                  # stream table, from base
    struct.pack_into('<4I', d, 64 + 40, 30, 1, 34, 1)       # A at base+30 (1), B at base+34 (1)
    struct.pack_into('<H', d, 64 + 30, 1)
    struct.pack_into('<H', d, 64 + 34, texture)
    struct.pack_into('<4H', d, 200, 0xffff, 0xffff, texture, 2)
    struct.pack_into('<4H', d, 208, 0xffff, 0xffff, 0xffff, 2)
    return xbc1(bytes(d), 'group')


class Game:
    """The made-up map: map header + data and nav header + data, and the archive holding them."""

    def __init__(self, folder):
        self.dir = folder
        folder.mkdir(parents=True)
        # map data: chunks at 4 KB, then a tail that is not 4 KB long
        da, self.chunk = bytearray(), {}
        for name in ('terrain0', 'terrain1', 'group0', 'group1', 'pv0', 'pv1', 'tex0', 'tex1', 'tex2'):
            da += bytes(-len(da) % 4096)
            self.chunk[name] = (len(da), 1000 + len(name) * 17)
            da += xbc1(name.encode() * 60, name) if not name.startswith('group') else group_chunk(1)
        da += b'tail' * 25
        self.da = bytes(da)
        h = bytearray(800)
        struct.pack_into('<4s', h, 0, b'DMSM')

        def table(cw, ow, at, entries):
            struct.pack_into('<II', h, 4 * cw, len(entries), at)
            for i, e in enumerate(entries):
                h[at + i * len(e):at + (i + 1) * len(e)] = e

        def ref_entry(size, at, chunk, extra=0):
            e = bytearray(size)
            o, n = self.chunk[chunk]
            struct.pack_into('<II', e, at, o, 1000)
            struct.pack_into('<f', e, 0, 1.5 + extra)
            return bytes(e)
        table(6, 7, 256, [ref_entry(76, 52, 'terrain0'), ref_entry(76, 52, 'terrain1', 1)])
        table(8, 9, 416, [ref_entry(64, 52, 'group0'), ref_entry(64, 52, 'group1')])
        table(20, 21, 544, [struct.pack('<II', self.chunk['pv0'][0], 1000), struct.pack('<II', self.chunk['pv1'][0], 1000)])
        tex = [struct.pack('<5I', self.chunk['tex%d' % i][0], 1000, 0, 0, 32) for i in range(3)]
        table(22, 23, 560, tex)
        struct.pack_into('<II', h, 4 * 56, 624, 2)
        struct.pack_into('<4I', h, 624, 0, 0x31535453, 3, 656)
        struct.pack_into('<4I', h, 640, 1, 0x31535453, 3, 716)
        for k, at in enumerate((656, 716)):
            for i in range(3):
                h[at + 20 * i:at + 20 * (i + 1)] = tex[i] if k == 0 or i != 2 else struct.pack('<5I', *struct.unpack(
                    '<5I', tex[i])[:4], 33)   # copy 3 differs, as the game's does
        h[780:800] = b'not in any table....'
        self.hd = bytes(h)
        # nav: 3 tiles of 2048 bytes, 3 grids, cells
        nd = b''.join(bytes([65 + i]) * 2048 for i in range(3)) + b'navtail'
        nh = bytearray(320)
        struct.pack_into('<4sI', nh, 0, b'NVMS', 0x1013)
        struct.pack_into('<IIII', nh, 8, 64, 3, 112, 3)
        for i in range(3):
            struct.pack_into('<4I', nh, 64 + 16 * i, 3, 2048 * i, 2048, 1024)
        for g, (at, n) in enumerate(((304, 4), (312, 1), (314, 1))):
            struct.pack_into('<3I', nh, 112 + 64 * g, 2, 2, 300)
            struct.pack_into('<II', nh, 112 + 64 * g + 36, at, n)
        struct.pack_into('<4H', nh, 304, 1, 2, 3, 0)
        self.vd, self.vh = nd, bytes(nh)
        files = {'wismhd': self.hd, 'wismda': self.da, 'winvhe': self.vh, 'winvda': self.vd}
        for ext, b in files.items():
            (folder / ('ma9990.' + ext)).write_bytes(b)
        # the archive: an entry nobody asks for first, then the four; 16-byte alignment
        entries = [('/other/file.bin', b'x' * 33, 33)] + [('/map/ma9990.' + e, b, len(b) if e.endswith('hd') or
                                                          e.endswith('he') else 0) for e, b in files.items()]
        arh = bytearray(b'arh2' + struct.pack('<III', len(entries), 16, 0))
        ard = bytearray()
        for path, data, un in entries:
            arh += struct.pack('<QII', mp.xxh64(path.encode()), len(data), un)
            ard += data + bytes(-len(data) % 16)
        (folder / 'sts.arh').write_bytes(arh)
        (folder / 'sts.ard').write_bytes(ard)


def edited_top(game, out):
    """The first mod, made the old way: terrain 0 repointed and its bounds moved, a texture added (all three tables
    moved to the header's end, as the texture tool does) and prop group 1 using it, nav tile 1 replaced."""
    h, da = bytearray(game.hd), bytearray(game.da)

    def append(blob):
        da.extend(bytes(-len(da) % 4096))
        at = len(da)
        da.extend(blob)
        return at
    t0 = xbc1(b'our terrain' * 99)
    struct.pack_into('<II', h, 256 + 52, append(t0), 1089)
    struct.pack_into('<f', h, 256 + 4, 99.0)
    tex = xbc1(b'our texture' * 50)
    at_tex = append(tex)
    for k, (cw, ow) in enumerate(((88, 92), (624 + 8, 624 + 12), (640 + 8, 640 + 12))):
        n, o = struct.unpack_from('<II', h, cw)
        h.extend(bytes(-len(h) % 16))
        new = len(h)
        h += h[o:o + 20 * n] + struct.pack('<5I', at_tex, 550, 0, 0, 32)
        struct.pack_into('<II', h, cw, n + 1, new)
    struct.pack_into('<II', h, 416 + 64 + 52, append(group_chunk(3)), 256)
    vh, vd = bytearray(game.vh), bytearray(game.vd)
    vd += bytes(-len(vd) % 64)
    at = len(vd)
    vd += b'T' * 2048
    struct.pack_into('<4I', vh, 64 + 16, 3, at, 2048, 1000)
    for name, b in (('hd', h), ('da', da), ('vh', vh), ('vd', vd)):
        (out / name).write_bytes(b)


LOW = """xcx-map-pieces 1
made-from wismhd {hd_n} {hd_h:016x}
made-from winvhe {vh_n} {vh_h:016x}
# terrain 0 is the top mod's too: left out
set wismhd terrain 0 4 u32 77
chunk wismhd propvd 1 0 pv.xbc1 1199
add wismhd texture mine
chunk wismhd texture +mine 0 tex.xbc1 594
chunk wismhd texture2 +mine 0 tex.xbc1 594
chunk wismhd texture3 +mine 0 tex.xbc1 594
chunk wismhd group 0 52 g0.xbc1 256
fix g0.xbc1 98 u16 texture +mine 3
fix g0.xbc1 204 u16 texture +mine 3
add winvhe tile far
chunk winvhe tile +far 4 tile.bin 2048
set winvhe tile +far 0 u32 3
set winvhe tile +far 12 u32 1024
set winvhe cells0 3 0 u16 @tile:+far 1
"""

# Mods each left out whole for one reason (after low, so low's numbers don't depend on them), and the line's end
MADE = "xcx-map-pieces 1\nmade-from wismhd {hd_n} {hd_h:016x}\n"
EDGES = [
    ('past-end', MADE + "set wismhd terrain 9 0 u32 5\n", {}, "pieces.txt line 3: no such entry in this game's terrain"),
    ('no-file', MADE + "chunk wismhd propvd 0 0 missing.xbc1 100\n", {}, 'missing.xbc1 is missing or not a file'),
    ('no-made', "xcx-map-pieces 1\nset wismhd terrain 1 0 u32 5\n", {}, 'pieces.txt has no made-from line for wismhd'),
    ('device', MADE + "chunk wismhd propvd 0 0 aux.xbc1 100\n", {}, 'pieces.txt line 3: file aux.xbc1: not a plain path'),
    ('wrong-size', MADE + "chunk wismhd propvd 0 0 c.xbc1 999\n", {'c.xbc1': 'chunk'},
     "pieces.txt line 3: c.xbc1 is not an xbc1 chunk of the line's unpacked size"),
    ('too-big', MADE + "add wismhd texture x\nset wismhd texture 1 16 u8 @texture:+x 300\n", {},
     "pieces.txt line 4: entry number 305 doesn't fit in 1 bytes"),
    ('newer', "xcx-map-pieces 2\n", {}, 'pieces.txt line 1: map pieces of a newer format (version 2)'),
]


class MapOverlayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.exe = None
        if not clang():
            return
        exe = cls.tmp / 'map_overlay_check.exe'
        plat = 'win32' if os.name == 'nt' else 'linux'
        libs = ['-lwinmm', '-lbcrypt'] if os.name == 'nt' else ['-lpthread', '-lm', '-ldl']
        rt = ROOT / 'runtime'
        stub = cls.tmp / 'stub'
        (stub / 'SDL3').mkdir(parents=True)
        (stub / 'zstd.h').write_text('#include <stddef.h>\nsize_t ZSTD_decompress(void *dst, size_t cap, const void '
                                     '*src, size_t n);\nunsigned ZSTD_isError(size_t code);\n')
        extra = []
        if os.name != 'nt':
            (stub / 'SDL3' / 'SDL_clipboard.h').write_text('#include <stdbool.h>\nbool SDL_SetClipboardText(const char '
                                                           '*text);\n')
            (stub / 'sdl_stub.c').write_text('#include <stdbool.h>\nbool SDL_SetClipboardText(const char *text) '
                                             '{ (void)text; return 0; }\n')
            extra = [str(stub / 'sdl_stub.c')]
        subprocess.run([clang(), '-std=gnu11', '-O1', '-Wall',
                        f'-I{stub}', f'-I{rt}', f'-I{rt / "platform"}', *extra, str(ROOT / 'tests/map_overlay_check.c'),
                        str(rt / 'mod_zip.c'), str(rt / f'platform/{plat}.c'), str(rt / 'platform/common.c'), *libs,
                        '-o', str(exe)], check=True)
        cls.exe = exe
        cls.game = Game(cls.tmp / 'game')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_check(self, mods, out, touch=None):
        out.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, GAME=str(self.game.dir), MODS=str(mods))
        if touch:
            env['TOUCH'] = str(touch)
        r = subprocess.run([str(self.exe), str(out)], env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn('FAIL', r.stdout, r.stdout + r.stderr)
        return r.stdout

    def setUp(self):
        if not self.exe:
            self.skipTest('needs clang (MSYS2 UCRT64, or XCX_CLANG)')

    def test_no_pieces_leaves_the_map_alone(self):
        mods = self.tmp / 'none'
        (mods / 'plain' / 'romfs' / 'bdat').mkdir(parents=True)
        (mods / 'romfs-enabled.txt').write_text('plain\n')
        out = self.run_check(mods, self.tmp / 'none-out')
        self.assertIn('other 0 built 0', out)
        self.assertIn('claims 0', out)
        self.assertIn('state -1', out)
        for f in ('wismhd', 'winvhe', 'wismda', 'winvda'):
            self.assertIn('closed ' + f, out)
        self.assertIn('rom-opens 0', out)   # the archive was not even opened

    def test_two_mods_combine(self):
        g = self.game
        mods = self.tmp / 'mods'
        edited = self.tmp / 'edited'
        edited.mkdir()
        edited_top(g, edited)
        top = mods / 'top' / 'romfs' / 'map' / 'ma9990.pieces'
        top.mkdir(parents=True)
        r = subprocess.run([sys.executable, str(ROOT / 'tools/map_pieces.py'), 'make', str(top), '--game', str(g.dir),
                            '--map', str(edited / 'hd'), str(edited / 'da'), '--nav', str(edited / 'vh'),
                            str(edited / 'vd')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        text = (top / 'pieces.txt').read_text()
        self.assertIn('add wismhd texture n0', text)
        self.assertIn('fix group-1-52.xbc1 204 u16 texture +n0 3', text)   # the texture list
        self.assertIn('fix group-1-52.xbc1 98 u16 texture +n0 3', text)    # stream list B (base 64 + 34)
        self.assertNotIn('65535', text)
        r = subprocess.run([sys.executable, str(ROOT / 'tools/map_pieces.py'), 'verify', str(top), '--game', str(g.dir),
                            '--map', str(edited / 'hd'), str(edited / 'da'), '--nav', str(edited / 'vh'),
                            str(edited / 'vd')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('ok:', r.stdout)
        # in folders named with other case: found as romfs_mods.c finds mod files (it matters on Linux)
        low = mods / 'low' / 'romfs' / 'Map' / 'MA9990.pieces'
        low.mkdir(parents=True)
        (low / 'pieces.txt').write_text(LOW.format(hd_n=len(g.hd), hd_h=mp.xxh64(g.hd), vh_n=len(g.vh),
                                                   vh_h=mp.xxh64(g.vh)))
        (low / 'pv.xbc1').write_bytes(xbc1(b'vertex data' * 109))
        (low / 'tex.xbc1').write_bytes(xbc1(b'low texture' * 54))
        (low / 'g0.xbc1').write_bytes(group_chunk(3))
        (low / 'tile.bin').write_bytes(b'F' * 2048)
        bad = mods / 'bad' / 'romfs' / 'map' / 'ma9990.pieces'
        bad.mkdir(parents=True)
        (bad / 'pieces.txt').write_text('xcx-map-pieces 1\nchunk wismhd terrain 1 52 ../../escape.xbc1 10\n')
        old = mods / 'old' / 'romfs' / 'map' / 'ma9990.pieces'
        old.mkdir(parents=True)
        (old / 'pieces.txt').write_text('xcx-map-pieces 1\nmade-from wismhd %d 0123456789abcdef\n'
                                        'set wismhd terrain 1 0 u32 5\n' % len(g.hd))
        edges = []
        for name, text, files, _ in EDGES:
            d = mods / name / 'romfs' / 'map' / 'ma9990.pieces'
            d.mkdir(parents=True)
            (d / 'pieces.txt').write_text(text.format(hd_n=len(g.hd), hd_h=mp.xxh64(g.hd)))
            for fn in files:
                (d / fn).write_bytes(xbc1(b'x' * 300))
            edges.append(d)
        whole = mods / 'whole' / 'romfs' / 'map'
        whole.mkdir(parents=True)
        (whole / 'ma9990.wismhd').write_bytes(g.hd)
        order = ['top', 'bad', 'low', 'old'] + [e[0] for e in EDGES] + ['whole']
        (mods / 'romfs-enabled.txt').write_text(''.join(m + '\n' for m in order))
        out = self.run_check(mods, self.tmp / 'out', touch=low / 'pieces.txt')
        self.assertIn('state 1', out)
        self.assertIn('other 0', out)
        # the reference gets every enabled mod with pieces, in the same order, and must leave out the same ones
        heads, layout, report, gsize = mp.combine(str(g.dir), [str(top), str(bad), str(low), str(old)] +
                                                  [str(e) for e in edges])
        out_ref = sorted(r.split(':')[0] for r in report if 'map pieces left out' in r)
        self.assertEqual(out_ref, sorted(['bad', 'old'] + [e[0] for e in EDGES]), report)
        lost = "low: terrain 0 is top's (a higher mod); low's change to it is left out"
        self.assertEqual([r for r in report if 'higher mod' in r], [lost], report)
        for name, _, _, why in EDGES:   # each twin says the same about each
            self.assertTrue(any(r.startswith('%s: map pieces left out: %s' % (name, why)) for r in report),
                            (name, report))
            self.assertIn('line! %s: map pieces left out: %s' % (name, why), out)
        served = self.tmp / 'out'
        for f in ('wismhd', 'winvhe'):
            self.assertEqual((served / ('ma9990.' + f)).read_bytes(), heads[f], f)
            data = bytearray(mp.readb(mp.game_file(str(g.dir), mp.DATA[f])))
            for p, n, m, fn, blob, _ in layout[f]:
                data.extend(bytes(p - len(data)))
                data[p:p + n] = blob
            self.assertEqual((served / ('ma9990.' + mp.DATA[f])).read_bytes(), bytes(data), mp.DATA[f])
        # the second mod's texture is numbered after the first's (too-big's was given back), and its chunk says so
        h = heads['wismhd']
        self.assertEqual(struct.unpack_from('<I', h, 88)[0], 5)
        g0 = [blob for p, n, m, fn, blob, patched in layout['wismhd'] if fn == 'g0.xbc1' and patched][0]
        d = zlib.decompress(g0[48:])
        self.assertEqual(struct.unpack_from('<H', d, 204)[0], 4)
        self.assertEqual(struct.unpack_from('<H', d, 98)[0], 4)
        # the nav cell points at the added tile (1-based: tile 3 is 4)
        vh = heads['winvhe']
        self.assertEqual(struct.unpack_from('<H', vh, 304 + 6)[0], 4)
        self.assertIn('line! ' + lost, out)
        self.assertIn('line! bad: map pieces left out: pieces.txt line 2', out)
        self.assertIn('line! old: map pieces left out: pieces.txt line 2: made for another game version', out)
        self.assertIn('line! whole: its whole map/ma9990.wismhd is left out', out)
        self.assertIn('line low: 7 map entries (2 added), 1 lost to higher mods', out)
        self.assertIn('changed 0', out)
        self.assertIn('changed-after 1', out)


if __name__ == '__main__':
    unittest.main()
