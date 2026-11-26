"""Map mods as pieces (docs/map-mods.md): turn an edited world map into the pieces runtime/map_overlay.c serves, check
them, and combine several mods the way the game will.

The world map is two header + data pairs: map/ma9990.wismhd + .wismda (props, terrain, textures, lighting) and
map/ma9990.winvhe + .winvda (the enemy navmesh). A map mod made the usual way (append edited chunks to a copy of the
data file and repoint the header) is a whole header plus a data file of ~1 GB. Pieces are what it changed, keyed by
header table and entry, with the data those entries point at as files of their own:

  python tools/map_pieces.py make OUT --game GAME [--map HD SRC] [--nav HD SRC]
      OUT: the mod's pieces folder (<mod>/romfs/map/ma9990.pieces). GAME: a folder with the game's own ma9990.wismhd,
      .wismda, .winvhe and .winvda (unpacked; map__ma9990.* names work too). HD: the edited header; SRC: the edited
      data file, or a folder of chunk-<offset>.xbc1 / .bin files holding everything past the game's end.
  python tools/map_pieces.py verify OUT --game GAME [--map HD SRC] [--nav HD SRC]
      builds the header from the game's and the pieces (as the game will), and checks every entry against the edited
      header and every piece's bytes against the edited data. Prints the built headers' xxh64, which the game logs.
  python tools/map_pieces.py combine --game GAME MOD... [--out DIR]
      MOD: a pieces folder, the top (winning) mod first. Prints what each mod keeps and loses, and the headers' xxh64;
      --out writes the built headers and each data file's layout (piece offsets), never the data itself.

No game data is in this file: only where the tables are (header words) and their entry sizes.
"""
import argparse
import os
import struct
import sys
import zlib

MAP = 'ma9990'
FILES = ('wismhd', 'winvhe')
DATA = {'wismhd': 'wismda', 'winvhe': 'winvda'}
ALIGN = 4096

# name, header, kind, grows, link, entry size, a, b, chunk pointer fields (byte offsets of (offset, size) pairs)
#   words: count at header word a, offset at word b; desc: descriptor a of the list at header word b (count +8,
#   offset +12); cells: the cell table of nav grid a (grid words 9/10), u16 per cell
TABLES = [
    ('terrain', 'wismhd', 'words', 1, None, 76, 6, 7, (52,)),
    ('group', 'wismhd', 'words', 1, None, 64, 8, 9, (52,)),
    ('t10', 'wismhd', 'words', 1, None, 88, 10, 11, ()),
    ('t12', 'wismhd', 'words', 1, None, 60, 12, 13, (52,)),
    ('propvd', 'wismhd', 'words', 1, None, 8, 20, 21, (0,)),
    ('texture', 'wismhd', 'words', 1, None, 20, 22, 23, (0, 8)),
    ('t25', 'wismhd', 'words', 1, None, 60, 25, 26, (52,)),
    ('placement', 'wismhd', 'words', 1, None, 8, 27, 28, (0,)),
    ('t29', 'wismhd', 'words', 1, None, 8, 29, 30, (0,)),
    ('light', 'wismhd', 'words', 1, None, 56, 34, 35, (24,)),
    ('t36', 'wismhd', 'words', 1, None, 8, 36, 37, (0,)),
    ('t44', 'wismhd', 'words', 1, None, 8, 44, 45, (0,)),
    ('t46', 'wismhd', 'words', 1, None, 72, 46, 47, (40,)),
    ('t49', 'wismhd', 'words', 1, None, 8, 49, 50, (0,)),
    ('terrainvd', 'wismhd', 'words', 1, None, 8, 51, 52, (0,)),
    ('texture2', 'wismhd', 'desc', 0, 'texture', 20, 0, 56, (0, 8)),
    ('texture3', 'wismhd', 'desc', 0, 'texture', 20, 1, 56, (0, 8)),
    ('tile', 'winvhe', 'words', 1, None, 16, 3, 2, (4,)),
    ('grid', 'winvhe', 'words', 0, None, 64, 5, 4, ()),
    ('cells0', 'winvhe', 'cells', 0, None, 2, 0, 0, ()),
    ('cells1', 'winvhe', 'cells', 0, None, 2, 1, 0, ()),
    ('cells2', 'winvhe', 'cells', 0, None, 2, 2, 0, ()),
]
T = {t[0]: dict(zip(('name', 'file', 'kind', 'grows', 'link', 'esize', 'a', 'b', 'refs'), t)) for t in TABLES}


def readb(path):
    with open(path, 'rb') as f:
        return f.read()


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def table_words(h, name):
    t = T[name]
    if t['kind'] == 'words':
        return 4 * t['a'], 4 * t['b']
    if t['kind'] == 'desc':
        d = u32(h, 4 * t['b']) + 16 * t['a']
        return d + 8, d + 12
    gc, go = table_words(h, 'grid')
    g = u32(h, go) + 64 * t['a']
    return g + 40, g + 36


def table_at(h, name):
    cw, ow = table_words(h, name)
    return u32(h, cw), u32(h, ow)


def file_tables(f):
    return [n for n, t in T.items() if t['file'] == f]


# ---- xxh64, seed 0 (what the game logs for a built header)
M = (1 << 64) - 1
P1, P2, P3, P4, P5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def _round(acc, x):
    return _rotl((acc + x * P2) & M, 31) * P1 & M


def xxh64(d):
    n, i = len(d), 0
    if n >= 32:
        v = [(P1 + P2) & M, P2, 0, (-P1) & M]
        q = struct.unpack_from('<%dQ' % (n // 8), d)
        while i + 32 <= n:
            k = i // 8
            v = [_round(v[0], q[k]), _round(v[1], q[k + 1]), _round(v[2], q[k + 2]), _round(v[3], q[k + 3])]
            i += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & M
        for x in v:
            h = ((h ^ _round(0, x)) * P1 + P4) & M
    else:
        h = P5
    h = (h + n) & M
    while i + 8 <= n:
        h = (_rotl(h ^ _round(0, struct.unpack_from('<Q', d, i)[0]), 27) * P1 + P4) & M
        i += 8
    if i + 4 <= n:
        h = (_rotl(h ^ (u32(d, i) * P1 & M), 23) * P2 + P3) & M
        i += 4
    while i < n:
        h = _rotl(h ^ (d[i] * P5 & M), 11) * P1 & M
        i += 1
    h ^= h >> 33
    h = h * P2 & M
    h ^= h >> 29
    h = h * P3 & M
    return h ^ (h >> 32)


# ---- inputs
def game_file(game, ext):
    for name in ('%s.%s' % (MAP, ext), 'map__%s.%s' % (MAP, ext)):
        p = os.path.join(game, name)
        if os.path.exists(p):
            return p
    sys.exit('no %s.%s in %s' % (MAP, ext, game))


class Source:
    """The edited data: a whole data file, or a folder of chunk-<offset> files (everything past the game's end)."""

    def __init__(self, path):
        self.path = path
        self.parts = []
        if os.path.isdir(path):
            for n in os.listdir(path):
                if n.startswith('chunk-') and n.endswith(('.xbc1', '.bin')):
                    self.parts.append((int(n[6:n.rindex('.')]), os.path.getsize(os.path.join(path, n)), n))
            self.parts.sort()

    def read(self, off, n):
        if not self.parts:
            with open(self.path, 'rb') as f:
                f.seek(off)
                b = f.read(n)
        else:
            b = None
            for at, size, name in self.parts:
                if at <= off and off + n <= at + size:
                    with open(os.path.join(self.path, name), 'rb') as f:
                        f.seek(off - at)
                        b = f.read(n)
                    break
        if b is None or len(b) != n:
            sys.exit('%s has no %d bytes at %d' % (self.path, n, off))
        return b


def blob_at(src, off, file, entry, field):
    """The bytes a pointer at off points at: a whole xbc1 chunk (its header and packed data), or a nav tile (its
    length field)."""
    if file == 'wismhd':
        head = src.read(off, 48)
        if head[:4] != b'xbc1':
            sys.exit('no xbc1 chunk at %d' % off)
        return src.read(off, 48 + u32(head, 12))
    return src.read(off, u32(entry, field + 4))


# ---- fix-ups: numbers inside a prop group's chunk that name entries of other tables
def group_numbers(blob):
    """(byte offset in the unpacked chunk, table) for each u16 holding a global number: the group's texture list
    (chunk words 8/9: offset, count; 8 bytes each, the global texture at +4) and its stream table (base = word 6,
    table at base + base word 14: list A = prop vertex data, list B = textures)."""
    if blob[:4] != b'xbc1' or u32(blob, 4) != 1:
        return None, []
    d = zlib.decompress(blob[48:48 + u32(blob, 12)])
    out = []
    wo, wc = u32(d, 32), u32(d, 36)
    out += [(wo + 8 * i + 4, 'texture') for i in range(wc)]
    base = u32(d, 24)
    t = base + u32(d, base + 56)
    a_o, a_n, b_o, b_n = struct.unpack_from('<4I', d, t)
    out += [(base + a_o + 2 * i, 'propvd') for i in range(a_n)]
    out += [(base + b_o + 2 * i, 'texture') for i in range(b_n)]
    return d, out


# ---- make
def make_file(f, gh, gsize, mh, src, lines, files):
    tabs = file_tables(f)
    covered = bytearray(len(mh))    # mod header bytes a table (or its words) accounts for
    added = {}                      # table -> number of added entries
    for name in tabs:
        cw, ow = table_words(mh, name)
        for w in (cw, ow):
            covered[w:w + 4] = b'\1' * 4
        gc, go = table_at(gh, name)
        mc, mo = table_at(mh, name)
        if mc < gc:
            sys.exit('%s: table %s has fewer entries than the game\'s' % (f, name))
        if mc > gc:
            t = T[name]
            if t['link']:
                if mc - gc != added.get(t['link'], -1):
                    sys.exit('%s: %s grew by %d, %s by %d: the copies must grow together' % (f, name, mc - gc,
                             t['link'], added.get(t['link'], 0)))
            elif not t['grows']:
                sys.exit('%s: table %s can\'t grow' % (f, name))
            else:
                added[name] = mc - gc
                for k in range(mc - gc):
                    lines.append('add %s %s n%d' % (f, name, k))
        covered[mo:mo + mc * T[name]['esize']] = b'\1' * (mc * T[name]['esize'])
        if (mo, mc) != (go, gc):
            covered[go:go + gc * T[name]['esize']] = b'\1' * (gc * T[name]['esize'])   # the game's copy, unused
    for i in range(len(gh)):
        if not covered[i] and mh[i] != gh[i]:
            sys.exit('%s: the edited header changes byte %d, outside every table the tool knows' % (f, i))
    # past the game's end, bytes no table uses are earlier copies of tables that grew more than once (each texture
    # added one at a time can copy all three tables again): nothing points at them, so they are left behind
    dead = sum(1 for i in range(len(gh), len(mh)) if not covered[i] and mh[i])
    if dead:
        print('%s: %d bytes past the end of the game header belong to no table (old copies of grown tables): left out' %
              (f, dead))
    names = {}                      # (table, index) -> "+nK" for added entries
    for name, n in added.items():
        gc = table_at(gh, name)[0]
        for k in range(n):
            names[(name, gc + k)] = '+n%d' % k
            for t in T.values():
                if t['link'] == name:
                    names[(t['name'], gc + k)] = '+n%d' % k

    def entry_name(name, i):
        return names.get((name, i), str(i))

    def index_ref(name, v, bias=0):   # a number naming an entry: added ones by name
        n = names.get((name, v - bias))
        return ('@%s:%s' % (name, n) + (' %d' % bias if bias else '')) if n else None

    fixes = []
    for name in tabs:
        t = T[name]
        gc, go = table_at(gh, name)
        mc, mo = table_at(mh, name)
        es = t['esize']
        for i in range(mc):
            e = mh[mo + i * es:mo + (i + 1) * es]
            g = gh[go + i * es:go + (i + 1) * es] if i < gc else bytes(es)
            if e == g:
                continue
            en = entry_name(name, i)
            skip = set()
            for r in t['refs']:
                o, s = struct.unpack_from('<II', e, r)
                skip |= {r, r + 4}
                if (o, s) == struct.unpack_from('<II', g, r):
                    continue
                if o < gsize:     # pointed at another place of the game's own data
                    lines.append('set %s %s %s %d u32 %d' % (f, name, en, r, o))
                    lines.append('set %s %s %s %d u32 %d' % (f, name, en, r + 4, s))
                    continue
                blob = blob_at(src, o, f, e, r)
                fn = '%s-%s-%d.%s' % (name, en.lstrip('+'), r, 'xbc1' if f == 'wismhd' else 'bin')
                files[fn] = blob
                lines.append('chunk %s %s %s %d %s %d' % (f, name, en, r, fn, s))
                if name == 'group':
                    d, nums = group_numbers(blob)
                    for at, tt in nums:
                        v = struct.unpack_from('<H', d, at)[0]
                        ref = names.get((tt, v))   # a number naming an added entry (0xffff = none)
                        if ref:
                            fixes.append('fix %s %d u16 %s %s %d' % (fn, at, tt, ref, v))
            if t['kind'] == 'cells':
                v = struct.unpack_from('<H', e)[0]
                ref = index_ref('tile', v, 1) if v else None
                lines.append('set %s %s %s 0 u16 %s' % (f, name, en, ref or v))
                continue
            for w in range(0, es, 4):
                if w in skip or e[w:w + 4] == g[w:w + 4]:
                    continue
                lines.append('set %s %s %s %d u32 %d' % (f, name, en, w, u32(e, w)))
    lines += fixes
    return added


def cmd_make(a):
    os.makedirs(a.out, exist_ok=True)
    lines = ['xcx-map-pieces 1', '# made by tools/map_pieces.py']
    files = {}
    for f, opt in (('wismhd', a.map), ('winvhe', a.nav)):
        if not opt:
            continue
        gh = readb(game_file(a.game, f))
        gsize = os.path.getsize(game_file(a.game, DATA[f]))
        mh = readb(opt[0])
        lines.append('made-from %s %d %016x' % (f, len(gh), xxh64(gh)))
        make_file(f, gh, gsize, mh, Source(opt[1]), lines, files)
    # each file written beside its place and moved over it: a game that has the old one open keeps reading the old one
    # (until its next start), never a file half rewritten
    def put(fn, data):
        tmp = os.path.join(a.out, fn + '.new')
        with open(tmp, 'wb') as w:
            w.write(data)
        os.replace(tmp, os.path.join(a.out, fn))
    for fn, blob in files.items():
        put(fn, blob)
    put('pieces.txt', ('\n'.join(lines) + '\n').encode('ascii'))
    for old in os.listdir(a.out):
        if old.endswith(('.xbc1', '.bin')) and old not in files:
            os.remove(os.path.join(a.out, old))
    print('%s: %d lines, %d files, %d bytes of pieces' % (a.out, len(lines), len(files),
                                                        sum(len(b) for b in files.values())))


# ---- reading pieces and combining (the same rules as runtime/map_overlay.c)
WIDTH = {'u8': 1, 'u16': 2, 'u32': 4}
MAX_HEADER = 64 << 20
DEVICES = {'con', 'prn', 'aux', 'nul'} | {'com%d' % i for i in range(1, 10)} | {'lpt%d' % i for i in range(1, 10)}


def number(s):
    """A decimal number (digits only), as runtime/map_overlay.c number() reads them; None otherwise."""
    return int(s) if s and len(s) <= 19 and all('0' <= c <= '9' for c in s) else None


def safe_path(p):
    """A plain path inside the pieces folder (runtime/map_overlay.c safe_path): ASCII names joined by '/', none
    empty, '.', '..', ending in a dot or space, or a Windows device name."""
    if not p or p.startswith('/') or len(p) >= 200:
        return False
    for seg in p.split('/'):
        if not seg or seg in ('.', '..') or seg[-1] in '. ':
            return False
        if any(ord(c) < 32 or ord(c) > 126 or c in ':\\*?"<>|' for c in seg):
            return False
        if seg.split('.')[0].lower() in DEVICES:
            return False
    return True


def safe_name(s):
    return 0 < len(s) < 48 and all(c.isascii() and (c.isalnum() or c in '_.-') for c in s)


def find_ci(base, rel):
    """base/rel with each name matched case-insensitively when the exact one isn't there (map_overlay.c find_ci)."""
    out = base
    for seg in rel.split('/'):
        nxt = os.path.join(out, seg)
        if not os.path.exists(nxt):
            try:
                hit = [n for n in os.listdir(out) if n.lower() == seg.lower()]
            except OSError:
                return None
            if not hit:
                return None
            nxt = os.path.join(out, hit[0])
        out = nxt
    return out


class Left(Exception):
    """A mod's pieces left out, with the reason the game gives."""


def tokens(line):
    return line.replace('\t', ' ').replace('\r', ' ').split(' ')


def parse(folder, game_hash):
    """(ops, adds) of a pieces folder, checked as runtime/map_overlay.c parse checks them; Left('pieces.txt line N:
    ...') for the first bad line. game_hash: {header: (size, xxh64)} of the game's headers."""
    ops, adds, names, made, uses = [], [], set(), set(), set()
    raw = readb(os.path.join(folder, 'pieces.txt'))
    first = True
    for n, line in enumerate(raw.split(b'\n'), 1):
        def bad(why):
            raise Left('pieces.txt line %d: %s' % (n, why))
        try:
            line = line.decode('ascii')
        except UnicodeDecodeError:
            line = line.decode('latin-1')   # the C reads bytes: a non-ASCII token fails its own check below
        tok = [t for t in tokens(line.split('#')[0]) if t]
        if not tok:
            continue

        def entry_ok(table, e):
            link = T[table]['link'] or table
            if e.startswith('+'):
                return (link, e[1:]) in names
            v = number(e)
            return v is not None and v <= 0x7fffffff
        if first:
            if len(tok) != 2 or tok[0] != 'xcx-map-pieces':
                bad('not "xcx-map-pieces 1"')
            if tok[1] != '1':
                bad('map pieces of a newer format (version %s); this build reads version 1' % tok[1][:20])
            first = False
        elif tok[0] == 'made-from':
            if len(tok) != 4 or tok[1] not in FILES or number(tok[2]) is None or len(tok[3]) != 16 or \
                    any(c not in '0123456789abcdefABCDEF' for c in tok[3]):
                bad('made-from <hd> <size> <xxh64>')
            if (number(tok[2]), int(tok[3], 16)) != game_hash[tok[1]]:
                bad("made for another game version (its %s differs from this game's)" % tok[1])
            made.add(tok[1])
        elif tok[0] == 'add':
            if len(tok) != 4 or tok[2] not in T or T[tok[2]]['file'] != tok[1] or not T[tok[2]]['grows']:
                bad("add <hd> <table> <name>: no such table, or one that can't grow")
            if not safe_name(tok[3]) or (tok[2], tok[3]) in names:
                bad('added name: [A-Za-z0-9_.-], under 48, once')
            names.add((tok[2], tok[3]))
            adds.append((tok[1], tok[2], tok[3]))
            uses.add(tok[1])
        elif tok[0] in ('set', 'chunk'):
            chunk = tok[0] == 'chunk'
            if not (len(tok) == 7 or (len(tok) == 8 and not chunk)):
                bad('%s: wrong number of fields' % tok[0])
            if tok[2] not in T or T[tok[2]]['file'] != tok[1]:
                bad('no table %s in %s' % (tok[2][:40], tok[1][:10]))
            uses.add(tok[1])
            es = T[tok[2]]['esize']
            if not entry_ok(tok[2], tok[3]):
                bad('entry %s: not a number or an added name' % tok[3][:48])
            at = number(tok[4])
            if at is None or at >= es:
                bad('field %s outside the entry' % tok[4][:20])
            if chunk:
                if at + 8 > es:
                    bad("the pair at %d doesn't fit in the entry" % at)
                if not safe_path(tok[5]):
                    bad('file %s: not a plain path in the pieces folder' % tok[5][:60])
                if number(tok[6]) is None or number(tok[6]) > 0xffffffff:
                    bad('size %s' % tok[6][:20])
            else:
                w = WIDTH.get(tok[5])
                if not w or at + w > es:
                    bad('width %s at %d' % (tok[5][:8], at))
                if tok[6].startswith('@'):
                    vt, colon, ve = tok[6][1:].partition(':')
                    if not colon:
                        bad('value %s: @<table>:<entry>' % tok[6][:60])
                    if vt not in T or T[vt]['file'] != tok[1] or not entry_ok(vt, ve):
                        bad('value: no entry %s' % ve[:48])
                    if len(tok) == 8 and (number(tok[7]) is None or number(tok[7]) > 0xffff):
                        bad('bias %s (0 to 65535)' % tok[7][:20])
                else:
                    if len(tok) != 7 or number(tok[6]) is None:
                        bad('value %s' % tok[6][:20])
                    if number(tok[6]) >> (8 * w):
                        bad('value %s too big for %s' % (tok[6][:20], tok[5]))
            ops.append(tuple(tok + [n]))
        elif tok[0] == 'fix':
            if len(tok) != 7 or tok[4] not in T:
                bad('fix <file> <at> <u16|u32> <table> <entry> <recorded>')
            if not safe_path(tok[1]):
                bad('file %s' % tok[1][:60])
            if number(tok[2]) is None or number(tok[2]) > 0xffffffff or tok[3] not in ('u16', 'u32'):
                bad('at %s / width %s' % (tok[2][:20], tok[3][:8]))
            rec = number(tok[6])
            if not entry_ok(tok[4], tok[5]) or rec is None or rec >> (8 * WIDTH[tok[3]]):
                bad('entry %s / recorded %s' % (tok[5][:48], tok[6][:20]))
            ops.append(tuple(tok + [n]))
        else:
            bad('unknown line "%s"' % tok[0][:20])
    if first:
        raise Left('pieces.txt is empty')
    for f in FILES:
        if f in uses and f not in made:
            raise Left('pieces.txt has no made-from line for %s' % f)
    return ops, adds


def check_files(folder, ops):
    """{file: info} for every file the lines name, checked as runtime/map_overlay.c check_files checks them; Left
    otherwise."""
    info = {}
    for op in ops:
        name = op[5] if op[0] == 'chunk' else op[1] if op[0] == 'fix' else None
        if name is None or name in info:
            continue
        path = find_ci(folder, name)
        if not path or not os.path.isfile(path):
            raise Left('%s is missing or not a file' % name)
        data = readb(path)
        if not data:
            raise Left("%s is empty or can't be read" % name)
        x = {'path': path, 'data': data, 'xbc1': False, 'zlib': False, 'un': 0}
        if len(data) >= 48 and data[:4] == b'xbc1':
            if 48 + u32(data, 12) > len(data):
                raise Left('%s: its xbc1 header says more data than the file has' % name)
            x.update(xbc1=True, zlib=u32(data, 4) == 1, un=u32(data, 8))
        info[name] = x
    chunked = {op[5] for op in ops if op[0] == 'chunk'}
    for op in ops:
        why = None
        if op[0] == 'chunk':
            x = info[op[5]]
            if op[1] == 'wismhd' and (not x['xbc1'] or x['un'] != int(op[6])):
                why = "is not an xbc1 chunk of the line's unpacked size"
            elif op[1] == 'winvhe' and int(op[6]) > len(x['data']):
                why = "is shorter than the line's size"
            name = op[5]
        elif op[0] == 'fix':
            x, name = info[op[1]], op[1]
            if name not in chunked:
                why = 'has a fix line but no chunk line'
            elif int(op[2]) + WIDTH[op[3]] > (x['un'] if x['xbc1'] else len(x['data'])):
                why = 'has a fix line past its end'
            elif x['xbc1'] and not x['zlib']:
                why = 'has a fix line but is not a zlib xbc1 chunk'
        if why:
            raise Left('pieces.txt line %d: %s %s' % (op[-1], name, why))
    return info


def mod_id(folder):
    """<mod>/romfs/map/ma9990.pieces -> <mod> (the id the game's lines name)."""
    return os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(folder)))))


def plan(gh, gsize, mods, parsed, report):
    """One attempt, as runtime/map_overlay.c plan(): (count, off, extra, owner, placed, blobs) or Left(k, why) naming
    the mod to leave out before trying again."""
    count = {}
    for n in T:
        try:
            count[n] = table_at(gh[T[n]['file']], n)
        except struct.error:
            count[n] = None
    extra, index = {n: 0 for n in T}, {}
    for k, (ops, adds, info) in enumerate(parsed):
        for f, name, nm in adds:
            if count[name] is None or any(t['link'] == name and (count[t['name']] or (None,))[0] != count[name][0]
                                          for t in T.values()):
                raise Left(k, "adds to %s, which this game's header can't grow" % name)
            index[(k, name, nm)] = count[name][0] + extra[name]
            extra[name] += 1
        for f in FILES:
            size = len(gh[f])
            for n in file_tables(f):
                e = extra[T[n]['link']] if T[n]['link'] else extra[n]
                if e:
                    size += (-size % 16) + (count[n][0] + e) * T[n]['esize']
            if size > MAX_HEADER:
                raise Left(k, 'its added entries would make the %s header too big' % f)
    for n, t in T.items():
        if t['link']:
            extra[n] = extra[t['link']]

    def idx(k, name, e):
        if e.startswith('+'):
            return index[(k, T[name]['link'] or name, e[1:])]
        return int(e)
    owner, told = {}, set()
    for k, (ops, _, _) in enumerate(parsed):
        for op in ops:
            if op[0] not in ('set', 'chunk'):
                continue
            ref = op[0] == 'set' and op[6].startswith('@')
            vt, _, ve = op[6][1:].partition(':') if ref else (None, None, None)
            if count[op[2]] is None or (not op[3].startswith('+') and int(op[3]) >= count[op[2]][0]) or \
                    (ref and not ve.startswith('+') and int(ve) >= count[vt][0]):
                raise Left(k, "pieces.txt line %d: no such entry in this game's %s" % (
                    op[-1], vt if ref and not ve.startswith('+') and int(ve) >= count[vt][0] else op[2]))
            if op[3].startswith('+'):
                continue
            key = (op[2], int(op[3]))
            if owner.setdefault(key, k) != k and (key, k) not in told:
                told.add((key, k))
                report.append("%s: %s %s is %s's (a higher mod); %s's change to it is left out" % (
                    mod_id(mods[k]), op[2], op[3], mod_id(mods[owner[key]]), mod_id(mods[k])))
    end = dict(gsize)
    placed, blobs = {}, {}
    for k, (ops, _, info) in enumerate(parsed):
        def kept(op):
            return op[3].startswith('+') or owner[(op[2], int(op[3]))] == k
        for op in ops:
            ref = op[0] == 'fix' or (op[0] == 'set' and op[6].startswith('@') and kept(op))
            if not ref:
                continue
            if op[0] == 'fix':
                vt, ve, w, bias = op[4], op[5], WIDTH[op[3]], 0
            else:
                (vt, _, ve), w = op[6][1:].partition(':'), WIDTH[op[5]]
                bias = int(op[7]) if len(op) > 8 else 0
            if not ve.startswith('+') and int(ve) >= count[vt][0]:
                raise Left(k, "pieces.txt line %d: no such entry in this game's %s" % (op[-1], vt))
            v = idx(k, vt, ve) + bias
            if v >= (0xffffffff if w == 4 else (1 << (8 * w)) - 1):
                raise Left(k, "pieces.txt line %d: entry number %d doesn't fit in %d bytes" % (op[-1], v, w))
        fixes = [op for op in ops if op[0] == 'fix']
        for op in ops:
            if op[0] != 'chunk' or not kept(op) or (k, op[1], op[5]) in placed:
                continue
            fn = op[5]
            mine = [x for x in fixes if x[1] == fn]
            blob, patched = info[fn]['data'], False
            if any(idx(k, x[4], x[5]) != int(x[6]) for x in mine):
                try:
                    blob = patch(blob, [(int(x[2]), x[3], idx(k, x[4], x[5])) for x in mine])
                except (zlib.error, struct.error):
                    raise Left(k, "%s can't be read or unpacked" % fn)
                patched = True
            p = end[op[1]] + (-end[op[1]] % ALIGN)
            if p + len(blob) > 0xffffffff:
                raise Left(k, 'its pieces would take the %s data past 4 GB' % op[1])
            placed[(k, op[1], fn)] = p
            end[op[1]] = p + len(blob)
            blobs[(k, op[1], fn)] = (blob, patched)
    return count, extra, idx, owner, placed, blobs


def combine(game, mods):
    """mods: pieces folders, top first. The same rules as runtime/map_overlay.c: a mod is used whole or left out (a bad
    line, a file that is missing or doesn't match its line, an entry this game's header lacks, a number too big for its
    field, a size limit), an entry two mods change is the higher one's. Returns (headers {f: bytes}, layout {f:
    [(offset, len, folder, file, bytes, patched)]}, report lines, game data sizes)."""
    gh = {f: bytearray(readb(game_file(game, f))) for f in FILES}
    gsize = {f: os.path.getsize(game_file(game, DATA[f])) for f in FILES}
    ghash = {f: (len(gh[f]), xxh64(bytes(gh[f]))) for f in FILES}
    report, parsed, used = [], [], []
    for m in mods:
        try:
            ops, adds = parse(m, ghash)
            parsed.append((ops, adds, check_files(m, ops)))
            used.append(m)
        except Left as e:
            report.append('%s: map pieces left out: %s' % (mod_id(m), e.args[0]))
    mods = used
    while True:
        lost = []
        try:
            count, extra, idx, owner, placed, blobs = plan(gh, gsize, mods, parsed, lost)
            break
        except Left as e:
            k, why = e.args
            report.append('%s: map pieces left out: %s' % (mod_id(mods[k]), why))
            del mods[k]
            del parsed[k]
    report += lost
    heads, off = {}, {n: (count[n] or (0, 0))[1] for n in T}
    for f in FILES:
        h = bytearray(gh[f])
        for n in file_tables(f):
            if not extra[n]:
                continue
            h += bytes(-len(h) % 16)
            at = len(h)
            c, o = count[n]
            es = T[n]['esize']
            h += gh[f][o:o + c * es] + bytes(extra[n] * es)
            cw, ow = table_words(h, n)
            struct.pack_into('<I', h, cw, c + extra[n])
            struct.pack_into('<I', h, ow, at)
            off[n] = at
        heads[f] = h
    layout = {f: [] for f in FILES}
    for k, (ops, _, _) in enumerate(parsed):
        for op in ops:
            if op[0] not in ('set', 'chunk'):
                continue
            f, name, e = op[1], op[2], op[3]
            if not e.startswith('+') and owner[(name, int(e))] != k:
                continue
            base = off[name] + idx(k, name, e) * T[name]['esize']
            at = int(op[4])
            if op[0] == 'set':
                v = op[6]
                if v.startswith('@'):
                    vt, _, ve = v[1:].partition(':')
                    v = idx(k, vt, ve) + (int(op[7]) if len(op) > 8 else 0)
                else:
                    v = int(v)
                w = WIDTH[op[5]]
                struct.pack_into({1: '<B', 2: '<H', 4: '<I'}[w], heads[f], base + at, v)
                continue
            struct.pack_into('<II', heads[f], base + at, placed[(k, f, op[5])], int(op[6]))
    for (k, f, fn), p in sorted(placed.items(), key=lambda kv: kv[1]):
        blob, patched = blobs[(k, f, fn)]
        layout[f].append((p, len(blob), mods[k], fn, blob, patched))
    return {f: bytes(h) for f, h in heads.items()}, layout, report, gsize


def patch(blob, fixes):
    """The file with its fix lines applied (an xbc1 chunk unpacked and stored again, as runtime/map_overlay.c)."""
    xb = blob[:4] == b'xbc1'
    d = bytearray(zlib.decompress(blob[48:48 + u32(blob, 12)]) if xb else blob)
    for at, w, v in fixes:
        struct.pack_into('<H' if w == 'u16' else '<I', d, at, v)
    if not xb:
        return bytes(d)
    z = bytearray(b'\x78\x01')
    i = 0
    while True:
        k = min(65535, len(d) - i)
        z += bytes([1 if i + k == len(d) else 0]) + struct.pack('<HH', k, k ^ 0xffff) + d[i:i + k]
        i += k
        if i == len(d):
            break
    z += struct.pack('>I', zlib.adler32(bytes(d)))
    head = bytearray(blob[:48])
    struct.pack_into('<II', head, 4, 1, len(d))
    struct.pack_into('<I', head, 12, len(z))
    out = bytes(head) + bytes(z)
    return out + bytes(-len(out) % 16)


def cmd_verify(a):
    heads, layout, report, gsize = combine(a.game, [a.out])
    bad = 0
    for f, opt in (('wismhd', a.map), ('winvhe', a.nav)):
        if not opt:
            continue
        mh, src, h = readb(opt[0]), Source(opt[1]), heads[f]
        by_at = {p: blob for p, n, m, fn, blob, _ in layout[f]}
        for name in file_tables(f):
            t = T[name]
            c1, o1 = table_at(h, name)
            c2, o2 = table_at(mh, name)
            if c1 != c2:
                print('FAIL %s %s: %d entries, the edited header has %d' % (f, name, c1, c2))
                bad += 1
                continue
            es = t['esize']
            for i in range(c1):
                e1, e2 = h[o1 + i * es:o1 + (i + 1) * es], mh[o2 + i * es:o2 + (i + 1) * es]
                for r in t['refs']:
                    p1, s1 = struct.unpack_from('<II', e1, r)
                    p2, s2 = struct.unpack_from('<II', e2, r)
                    if s1 != s2 or (p1 < gsize[f]) != (p2 < gsize[f]) or (p1 < gsize[f] and p1 != p2):
                        print('FAIL %s %s %d: pointer (%d, %d), edited (%d, %d)' % (f, name, i, p1, s1, p2, s2))
                        bad += 1
                    elif p1 >= gsize[f]:
                        blob = by_at.get(p1)
                        if blob is None or blob != blob_at(src, p2, f, e2, r):
                            print('FAIL %s %s %d: the piece at %d differs from the edited data at %d' % (f, name, i,
                                                                                                         p1, p2))
                            bad += 1
                    e1 = e1[:r] + bytes(8) + e1[r + 8:]
                    e2 = e2[:r] + bytes(8) + e2[r + 8:]
                if e1 != e2:
                    print('FAIL %s %s %d: fields differ' % (f, name, i))
                    bad += 1
        print('%s: built %d bytes, xxh64 %016x; data %d + %d bytes in %d pieces' % (
            f, len(h), xxh64(h), gsize[f], sum(n for _, n, *_ in layout[f]), len(layout[f])))
    for r in report:
        print(r)
    if bad:
        sys.exit('%d differences' % bad)
    print('ok: every entry and every piece matches the edited map')


def cmd_combine(a):
    heads, layout, report, gsize = combine(a.game, a.mods)
    for r in report:
        print(r)
    for f in FILES:
        for p, n, m, fn, blob, patched in layout[f]:
            if patched:
                print('%s: %s served with its numbers fixed (%d bytes, xxh64 %016x)' % (mod_id(m), fn, n,
                                                                                         xxh64(blob)))
        print('%s: %d bytes, xxh64 %016x; data %d + %d bytes in %d pieces' % (
            f, len(heads[f]), xxh64(heads[f]), gsize[f], sum(n for _, n, *_ in layout[f]), len(layout[f])))
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        for f in FILES:
            with open(os.path.join(a.out, '%s.%s' % (MAP, f)), 'wb') as w:
                w.write(heads[f])
            with open(os.path.join(a.out, '%s.layout.txt' % DATA[f]), 'w', newline='\n') as w:
                for p, n, m, fn, _, _ in layout[f]:
                    w.write('%d %d %s/%s\n' % (p, n, m, fn))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for c in ('make', 'verify'):
        p = sub.add_parser(c)
        p.add_argument('out')
        p.add_argument('--game', required=True)
        p.add_argument('--map', nargs=2, metavar=('HD', 'SRC'))
        p.add_argument('--nav', nargs=2, metavar=('HD', 'SRC'))
    p = sub.add_parser('combine')
    p.add_argument('--game', required=True)
    p.add_argument('mods', nargs='+')
    p.add_argument('--out')
    a = ap.parse_args()
    {'make': cmd_make, 'verify': cmd_verify, 'combine': cmd_combine}[a.cmd](a)


if __name__ == '__main__':
    main()
