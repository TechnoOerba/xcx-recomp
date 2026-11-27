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


def group_chunk(texture, vd=(), links=()):
    """A prop group chunk the tool and the overlay understand: base 64, stream table at base + 40 (list A: vertex
    data 1, list B: texture `texture`), texture list at 200 (two entries: global `texture`, and 0xffff = none), the
    model -> vertex data list at 224 (u32 each, words 10/11) and the links to placement chunks at 256 (32 bytes each,
    the placement entry first; words 14/15)."""
    d = bytearray(256 + 32 * len(links))
    struct.pack_into('<I', d, 24, 64)                       # base
    struct.pack_into('<II', d, 32, 200, 2)                  # texture list
    struct.pack_into('<I', d, 64 + 56, 40)                  # stream table, from base
    struct.pack_into('<4I', d, 64 + 40, 30, 1, 34, 1)       # A at base+30 (1), B at base+34 (1)
    struct.pack_into('<H', d, 64 + 30, 1)
    struct.pack_into('<H', d, 64 + 34, texture)
    struct.pack_into('<4H', d, 200, 0xffff, 0xffff, texture, 2)
    struct.pack_into('<4H', d, 208, 0xffff, 0xffff, 0xffff, 2)
    if vd:
        struct.pack_into('<II', d, 40, 224, len(vd))
        struct.pack_into('<%dI' % len(vd), d, 224, *vd)
    if links:
        struct.pack_into('<II', d, 56, 256, len(links))
        for i, p in enumerate(links):
            struct.pack_into('<I', d, 256 + 32 * i, p)
    return xbc1(bytes(d), 'group')


def terrain_chunk(vds):
    """A terrain set chunk as the tool reads it: groups block at 64 (word 14), its groups at +8 (36 bytes each,
    terrain vertex data per level of detail at +24 and +28)."""
    d = bytearray(64 + 8 + 36 * len(vds))
    struct.pack_into('<I', d, 56, 64)
    struct.pack_into('<II', d, 64, 8, len(vds))
    for i, (a, b) in enumerate(vds):
        struct.pack_into('<II', d, 64 + 8 + 36 * i + 24, a, b)
    return xbc1(bytes(d), 'terrain')


def tag_sec(tag, flags, payload):
    return struct.pack('>I4s', (8 + len(payload)) | (flags << 30), tag.encode()) + payload


def coli_file(data, items, patches, entries):
    """A made-up region collision file: a '1RAS' archive whose first entry is a tagfile (SDKV, DATA, TYPE, INDX) of
    data, items [(type, offset, count)] and patches [(type, [offsets])]; entries: the archive's other entries, each
    padded to pad bytes as given [(bytes, pad)]."""
    item = b''.join(struct.pack('<III', *i) for i in items)
    ptch = b''.join(struct.pack('<II', t, len(p)) + struct.pack('<%dI' % len(p), *p) for t, p in patches)
    tag = tag_sec('TAG0', 0, tag_sec('SDKV', 0, b'20230200') + tag_sec('DATA', 1, data) +
                  tag_sec('TYPE', 0, b'made-up types, never read') +
                  tag_sec('INDX', 0, tag_sec('ITEM', 0, item) + tag_sec('PTCH', 0, ptch)))
    blobs = [(tag, 128)] + entries
    head = bytearray(160 + 64 * len(blobs))
    head[:16] = b'1RAS' + struct.pack('<III', 0, 0x101, len(blobs))
    head += bytes(-len(head) % 128)
    body, pos = bytearray(), len(head)
    for i, (b, pad) in enumerate(blobs):
        b = b + bytes(-len(b) % pad)
        struct.pack_into('<II', head, 160 + 64 * i, pos, len(b))
        head[168 + 64 * i:176 + 64 * i] = (b'ent%d' % i).ljust(8, b'\0')
        body += b
        pos += len(b)
    body += bytes(-pos % 2048)
    struct.pack_into('<I', head, 4, len(head) + len(body))
    return bytes(head + body)


# the made-up collision: a root (item 1) pointing at the body array (item 2: two 32-byte bodies, each pointing at its
# shape, item 3, and the name "ground", item 4), the shape after the array
COLI_ITEMS = [(0, 0, 0), (2 | 0x10 << 24, 0, 1), (5 | 0x20 << 24, 16, 2), (7 | 0x10 << 24, 80, 1),
              (8 | 0x20 << 24, 112, 6)]
COLI_PATCHES = [(2, [0]), (5, [16, 48]), (8, [24, 56])]
COLI_ENTRIES = [(b'IDM\0' + b'm' * 36, 64), (b'IDV\0' + b'v' * 124, 128)]


def coli_data():
    d = bytearray(128)
    struct.pack_into('<Q', d, 0, 2)
    for i in range(2):
        struct.pack_into('<QQ4f', d, 16 + 32 * i, 3, 4, 1.0 + i, 2.0, 3.0, 0.0)
    d[80:112] = struct.pack('<I', 16) + b'game shape' + bytes(18)
    d[112:118] = b'ground'
    return bytes(d)


def coli_add(shapes, extra=None):
    """The made-up collision with one body per shape (32 bytes each, a copy of body 0 pointing at its shape), made
    the way the collision tool adds land: the body array grows in place, everything after it moves, the shapes go to
    the end with an item each. extra: (patch type, offset in the first shape, item) of one more pointer field."""
    d, items, patches = bytearray(coli_data()), [list(i) for i in COLI_ITEMS], [[t, list(p)] for t, p in COLI_PATCHES]
    ins, delta = 16 + 32 * 2, 32 * len(shapes)
    recs = b''.join(bytes(d[16:48]) for _ in shapes)
    d = d[:ins] + recs + d[ins:]
    for it in items[1:]:
        if it[1] >= ins:
            it[1] += delta
    items[2][2] += len(shapes)
    for p in patches:
        p[1] = [o + delta if o >= ins else o for o in p[1]]
    d += bytes(-len(d) % 16)
    for j, sh in enumerate(shapes):
        at = len(d)
        d += sh + bytes(-len(sh) % 16)
        struct.pack_into('<Q', d, ins + 32 * j, len(items))
        items.append([7 | 0x10 << 24, at, 1])
        patches[1][1].append(ins + 32 * j)
        patches[2][1].append(ins + 32 * j + 8)
        if extra and j == 0:
            t, off, target = extra
            struct.pack_into('<Q', d, at + off, target)
            patches.append([t, [at + off]])
    for p in patches:
        p[1].sort()
    return coli_file(bytes(d), [tuple(i) for i in items], [(t, p) for t, p in patches],
                     [(b, 128) for b, _ in COLI_ENTRIES])


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
        self.coli = coli_file(coli_data(), COLI_ITEMS, COLI_PATCHES, COLI_ENTRIES)
        (folder / 'ma00a_grd.wihkar').write_bytes(self.coli)
        # the archive: an entry nobody asks for first, then the four, then the collision; 16-byte alignment
        entries = [('/other/file.bin', b'x' * 33, 33)] + [('/map/ma9990.' + e, b, len(b) if e.endswith('hd') or
                                                          e.endswith('he') else 0) for e, b in files.items()]
        entries.append(('/map/mapcoli/ma00a_grd.wihkar', self.coli, 0))
        arh = bytearray(b'arh2' + struct.pack('<III', len(entries), 16, 0))
        ard = bytearray()
        for path, data, un in entries:
            arh += struct.pack('<QII', mp.xxh64(path.encode()), len(data), un)
            ard += data + bytes(-len(data) % 16)
        (folder / 'sts.arh').write_bytes(arh)
        (folder / 'sts.ard').write_bytes(ard)


def edited_top(game, out):
    """The first mod, made the old way: terrain 0 repointed and its bounds moved, a texture and a vertex data entry
    added (the tables moved to the header's end, as the texture tool does) and prop group 1 using both (its texture
    lists and its model -> vertex data list), nav tile 1 replaced."""
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
    n, o = struct.unpack_from('<II', h, 80)   # vertex data (words 20/21): one more entry, the table moved
    h.extend(bytes(-len(h) % 16))
    new = len(h)
    h += h[o:o + 8 * n] + struct.pack('<II', append(xbc1(b'our vertex data' * 30)), 450)
    struct.pack_into('<II', h, 80, n + 1, new)
    g1 = group_chunk(3, vd=[1, 2])
    struct.pack_into('<II', h, 416 + 64 + 52, append(g1), 256)
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
add wismhd propvd pvx
chunk wismhd propvd +pvx 0 pv2.xbc1 640
chunk wismhd group 0 52 g0.xbc1 256
fix g0.xbc1 98 u16 texture +mine 3
fix g0.xbc1 204 u16 texture +mine 3
fix g0.xbc1 228 u32 propvd +pvx 2
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
# Collision: mods left out whole for one reason (each with a copy of low's bodies file, b.bin, unless it brings its own)
COLI_MADE = "xcx-map-pieces 1\nmade-from mapcoli/ma00a_grd {cn} {ch:016x}\n"
COLI_EDGES = [
    ('coli-old', "xcx-map-pieces 1\nmade-from mapcoli/ma00a_grd {cn} 0123456789abcdef\nbodies mapcoli/ma00a_grd b.bin {bn}\n",
     None, "made for another game version (its mapcoli/ma00a_grd differs from this game's)"),
    ('coli-size', COLI_MADE + "bodies mapcoli/ma00a_grd b.bin 7\n", None, "pieces.txt line 3: b.bin is not the line's size"),
    ('coli-magic', COLI_MADE + "bodies mapcoli/ma00a_grd b.bin 40\n", b'XCXA' + bytes(36),
     'b.bin: not a collision bodies file (XCXB version 1)'),
    ('coli-none', "xcx-map-pieces 1\nmade-from mapcoli/ma09a_grd 1 0123456789abcdef\nbodies mapcoli/ma09a_grd b.bin {bn}\n",
     None, 'this game has no map/mapcoli/ma09a_grd.wihkar it can read'),
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
        if not self.exe and self._testMethodName != 'test_chunk_numbers':
            self.skipTest('needs clang (MSYS2 UCRT64, or XCX_CLANG)')

    def test_chunk_numbers(self):
        # where make looks for numbers of other tables inside a chunk (needs no clang)
        d, nums = mp.chunk_numbers('group', group_chunk(5, vd=[7, 8], links=[3, 4]))
        self.assertEqual(sorted(nums), sorted([(204, 'texture', 2), (212, 'texture', 2), (94, 'propvd', 2),
                                               (98, 'texture', 2), (224, 'propvd', 4), (228, 'propvd', 4),
                                               (256, 'placement', 4), (288, 'placement', 4)]))
        d, nums = mp.chunk_numbers('terrain', terrain_chunk([(1, 2), (3, 4)]))
        self.assertEqual(nums, [(96, 'terrainvd', 4), (132, 'terrainvd', 4), (100, 'terrainvd', 4),
                                (136, 'terrainvd', 4)])
        self.assertEqual([struct.unpack_from('<I', d, at)[0] for at, _, _ in nums], [1, 3, 2, 4])
        # lists that don't fit the chunk give nothing; other tables' chunks are not read
        bad = bytearray(zlib.decompress(group_chunk(5, vd=[7])[48:]))
        struct.pack_into('<I', bad, 44, 10 ** 6)
        self.assertNotIn('propvd', [t for _, t, w in mp.chunk_numbers('group', xbc1(bytes(bad)))[1] if w == 4])
        self.assertEqual(mp.chunk_numbers('light', group_chunk(5))[1], [])

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
        # collision: the top mod adds two bodies, the low one a third whose shape also points at the root (a patch
        # type the game's file doesn't have)
        (edited / 'top' / 'mapcoli').mkdir(parents=True)
        (edited / 'top' / 'mapcoli' / 'ma00a_grd.wihkar').write_bytes(coli_add([b'top shape A'.ljust(32, b'.'),
                                                                                b'top shape B'.ljust(48, b'.')]))
        (edited / 'low' / 'mapcoli').mkdir(parents=True)
        (edited / 'low' / 'mapcoli' / 'MA00A_GRD.wihkar').write_bytes(coli_add([b'low shape'.ljust(32, b'.')],
                                                                               extra=(9, 16, 1)))
        tool = [sys.executable, str(ROOT / 'tools/map_pieces.py')]
        top = mods / 'top' / 'romfs' / 'map' / 'ma9990.pieces'
        top.mkdir(parents=True)
        edits = ['--game', str(g.dir), '--map', str(edited / 'hd'), str(edited / 'da'), '--nav', str(edited / 'vh'),
                 str(edited / 'vd'), '--coli', str(edited / 'top' / 'mapcoli' / 'ma00a_grd.wihkar')]
        r = subprocess.run(tool + ['make', str(top)] + edits, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        text = (top / 'pieces.txt').read_text()
        self.assertIn('add wismhd texture n0', text)
        self.assertIn('fix group-1-52.xbc1 204 u16 texture +n0 3', text)   # the texture list
        self.assertIn('fix group-1-52.xbc1 98 u16 texture +n0 3', text)    # stream list B (base 64 + 34)
        self.assertNotIn('65535', text)
        self.assertIn('fix group-1-52.xbc1 228 u32 propvd +n0 2', text)   # the model -> vertex data list
        self.assertIn('bodies mapcoli/ma00a_grd bodies-ma00a_grd.bin', text)
        r = subprocess.run(tool + ['verify', str(top)] + edits, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('ok:', r.stdout)
        self.assertIn('mapcoli/ma00a_grd: built', r.stdout)
        # a collision change that isn't only added bodies can't be pieces, and make says so
        changed = bytearray(g.coli)
        changed[changed.index(b'game shape')] ^= 1
        (edited / 'ma00a_grd.wihkar').write_bytes(bytes(changed))
        r = subprocess.run(tool + ['make', str(self.tmp / 'nope'), '--game', str(g.dir), '--coli',
                                   str(edited / 'ma00a_grd.wihkar')], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('ship this collision file whole', r.stderr)
        # the low mod's bodies, made by the tool on their own; its other pieces are written by hand below
        lowcoli = self.tmp / 'lowcoli'
        r = subprocess.run(tool + ['make', str(lowcoli), '--game', str(g.dir), '--coli',
                                   str(edited / 'low' / 'mapcoli' / 'MA00A_GRD.wihkar')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        coli_lines = [ln for ln in (lowcoli / 'pieces.txt').read_text().splitlines() if 'mapcoli/' in ln]
        bodies = (lowcoli / 'bodies-ma00a_grd.bin').read_bytes()
        # in folders named with other case: found as romfs_mods.c finds mod files (it matters on Linux)
        low = mods / 'low' / 'romfs' / 'Map' / 'MA9990.pieces'
        low.mkdir(parents=True)
        (low / 'pieces.txt').write_text(LOW.format(hd_n=len(g.hd), hd_h=mp.xxh64(g.hd), vh_n=len(g.vh),
                                                   vh_h=mp.xxh64(g.vh)) + '\n'.join(coli_lines) + '\n')
        (low / 'bodies-ma00a_grd.bin').write_bytes(bodies)
        (low / 'pv.xbc1').write_bytes(xbc1(b'vertex data' * 109))
        (low / 'tex.xbc1').write_bytes(xbc1(b'low texture' * 54))
        (low / 'g0.xbc1').write_bytes(group_chunk(3, vd=[0, 2]))
        (low / 'pv2.xbc1').write_bytes(xbc1(b'more vertex data' * 40))
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
        cn, ch = len(g.coli), mp.xxh64(g.coli)
        for name, text, own, _ in COLI_EDGES:
            d = mods / name / 'romfs' / 'map' / 'ma9990.pieces'
            d.mkdir(parents=True)
            (d / 'pieces.txt').write_text(text.format(cn=cn, ch=ch, bn=len(bodies)))
            (d / 'b.bin').write_bytes(own if own is not None else bodies)
            edges.append(d)
        whole = mods / 'whole' / 'romfs' / 'map'
        (whole / 'mapcoli').mkdir(parents=True)
        (whole / 'ma9990.wismhd').write_bytes(g.hd)
        (whole / 'mapcoli' / 'ma00a_grd.wihkar').write_bytes(g.coli)
        (whole / 'mapcoli' / 'ma02a_grd.wihkar').write_bytes(g.coli)   # no pieces for it: loads as before
        names = [e[0] for e in EDGES] + [e[0] for e in COLI_EDGES]
        order = ['top', 'bad', 'low', 'old'] + names + ['whole']
        (mods / 'romfs-enabled.txt').write_text(''.join(m + '\n' for m in order))
        out = self.run_check(mods, self.tmp / 'out', touch=low / 'pieces.txt')
        self.assertIn('state 1', out)
        self.assertIn('other 0', out)
        self.assertIn('claims-coli 1 0', out)   # ma00a_grd served, ma02a_grd (no bodies for it) not claimed
        # the reference gets every enabled mod with pieces, in the same order, and must leave out the same ones
        heads, layout, report, gsize, built = mp.combine(str(g.dir), [str(top), str(bad), str(low), str(old)] +
                                                         [str(e) for e in edges])
        out_ref = sorted(r.split(':')[0] for r in report if 'map pieces left out' in r)
        self.assertEqual(out_ref, sorted(['bad', 'old'] + names), report)
        lost = "low: terrain 0 is top's (a higher mod); low's change to it is left out"
        self.assertEqual([r for r in report if 'higher mod' in r], [lost], report)
        for name, _, _, why in EDGES + COLI_EDGES:   # each twin says the same about each
            self.assertTrue(any(r.startswith('%s: map pieces left out: %s' % (name, why)) for r in report),
                            (name, report))
            self.assertIn('line! %s: map pieces left out: %s' % (name, why), out)
        served = self.tmp / 'out'
        # collision: the game's two bodies, then top's two, then low's; every byte as the tool builds it
        self.assertEqual((served / 'ma00a_grd.wihkar').read_bytes(), built['ma00a_grd'][0])
        self.assertEqual(built['ma00a_grd'][1:], (3, 2))
        v = mp.coli_view(built['ma00a_grd'][0])
        self.assertEqual(v['items'][2][2], 5)
        bodies_at = v['items'][2][1]
        firsts = [struct.unpack_from('<Q', v['data'], bodies_at + 32 * i)[0] for i in range(5)]
        self.assertEqual(firsts, [3, 3, 5, 6, 7])   # the game's shape, top's two new items, low's one
        shape = v['items'][7]
        self.assertEqual(v['data'][shape[1]:shape[1] + 9], b'low shape')
        self.assertEqual(struct.unpack_from('<Q', v['data'], shape[1] + 16)[0], 1)   # low's own pointer field
        self.assertEqual(v['patches'][-1], [9, [shape[1] + 16]])
        self.assertIn('line low: 8 map entries (3 added), 1 lost to higher mods; 1 collision body', out)
        self.assertIn('line top: ', out)
        self.assertIn('2 collision bodies', out)
        self.assertIn('line! whole: its whole map/mapcoli/ma00a_grd.wihkar is left out', out)
        self.assertNotIn('ma02a_grd', out)
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
        self.assertEqual(struct.unpack_from('<2I', d, 224), (0, 3))   # vertex data: after the game's 2 and top's 1
        # the nav cell points at the added tile (1-based: tile 3 is 4)
        vh = heads['winvhe']
        self.assertEqual(struct.unpack_from('<H', vh, 304 + 6)[0], 4)
        self.assertIn('line! ' + lost, out)
        self.assertIn('line! bad: map pieces left out: pieces.txt line 2', out)
        self.assertIn('line! old: map pieces left out: pieces.txt line 2: made for another game version', out)
        self.assertIn('line! whole: its whole map/ma9990.wismhd is left out', out)
        self.assertIn('line low: 8 map entries (3 added), 1 lost to higher mods', out)
        self.assertIn('changed 0', out)
        self.assertIn('changed-after 1', out)


if __name__ == '__main__':
    unittest.main()
