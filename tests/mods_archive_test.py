"""Tests of mod archives: the layout planner (installer/mod_layout.py), the game-file check (installer/game_paths.py),
manual library entries, and RAR through xcx-unpack (installer/unpack.c, built by tools/build_unpack.py from
deps/libarchive; its test archives come from libarchive's own test suite).

    python tests/mods_archive_test.py         (the RAR tests need clang: XCX_CLANG, else clang on PATH or MSYS2's)
"""
import binascii
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
from installer import archive, mod_layout, mods  # noqa: E402
from installer.archive import Member  # noqa: E402
from installer.game_paths import GamePaths, arh_hashes, xxh64  # noqa: E402

FIXTURES = ROOT / 'deps' / 'libarchive' / 'pristine' / 'libarchive' / 'test'


def clang():
    found = (shutil.which(os.environ.get('XCX_CLANG', '')) or os.environ.get('XCX_CLANG') or shutil.which('clang')
             or ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None))
    return found if found and Path(found).is_file() else None


def members(*names):
    return [Member(n.rstrip('/'), 'dir' if n.endswith('/') else 'file', 1, True) for n in names]


def game(*paths):
    """A GamePaths that knows paths as sts.arh hashes, as an install's would."""
    return GamePaths(hashes={xxh64(('/' + p).encode()) for p in paths})


def fixture(name, out):
    """A libarchive test archive (uuencoded in its test suite), decoded to out/name."""
    data = bytearray()
    for line in (FIXTURES / (name + '.uu')).read_text().splitlines()[1:]:
        if line in ('end', '`', ''):
            break
        data += binascii.a2b_uu(line)
    path = Path(out) / name
    path.write_bytes(data)
    return path


class HashTest(unittest.TestCase):
    def test_xxh64(self):
        # reference values of XXH64 with seed 0
        self.assertEqual(xxh64(b''), 0xEF46DB3751D8E999)
        self.assertEqual(xxh64(b'a'), 0xD24EC4F1A98C6E5B)
        self.assertEqual(xxh64(b'abc'), 0x44BC2CF5AD770999)
        self.assertEqual(xxh64(b'Nobody inspects the spammish repetition'), 0xFBCEA83C8A378BF1)

    def test_arh_and_install(self):
        rows = [xxh64(b'/bdat/common.bdat'), xxh64(b'/chr/np/a.wimdo')]
        arh = struct.pack('<4sIII', b'arh1', len(rows), 16, 0) + b''.join(struct.pack('<QII', h, 1, 1) for h in rows)
        self.assertEqual(arh_hashes(arh), set(rows))
        with tempfile.TemporaryDirectory() as d:
            romfs = Path(d) / 'romfs'
            romfs.mkdir()
            (romfs / 'romfs.bin').write_bytes(b'\0' * 7 + arh)
            (romfs / 'index.json').write_text(json.dumps({'files': [
                {'path': 'sts.arh', 'offset': 7, 'size': len(arh)}, {'path': 'movie/a.webm', 'offset': 0, 'size': 1}]}))
            g = GamePaths.of_install(d)
            self.assertIn('bdat/common.bdat', g)
            self.assertIn('BDAT\\Common.bdat', g)
            self.assertIn('movie/a.webm', g)
            self.assertNotIn('bdat/other.bdat', g)
            self.assertFalse(GamePaths.of_install(Path(d) / 'missing'))


class PlanTest(unittest.TestCase):
    G = game('bdat/common.bdat', 'bdat/us/common_ms.bdat', 'chr/np/a.wimdo')

    def test_romfs_layouts_as_before(self):
        p = mod_layout.plan(members('romfs/chr/np/a.wimdo', 'exefs/main.npdm', 'readme.txt'), self.G)
        self.assertEqual((p.how, p.files), ('romfs', {'romfs/chr/np/a.wimdo': 'chr/np/a.wimdo'}))
        self.assertEqual(sorted(w for _, w in p.left_out), ['doc', 'exefs'])
        self.assertFalse(mod_layout.needs_confirm(p))
        p = mod_layout.plan(members('My Mod/romfs/mod/bdat/common.bdat'), self.G)
        self.assertEqual((p.how, list(p.files.values())), ('loader', ['bdat/common.bdat']))

    def test_folder_named_after_the_mod(self):
        # the Nexus Mods "Tweaks mod" layout: no romfs folder, the game's folders inside one named after the mod
        names = ('tweaks mod/', 'tweaks mod/bdat/', 'tweaks mod/bdat/common.bdat', 'tweaks mod/bdat/us/common_ms.bdat')
        p = mod_layout.plan(members(*names), self.G)
        self.assertEqual((p.how, p.top, p.matched), ('matched', 'tweaks mod', 2))
        self.assertEqual(sorted(p.files.values()), ['bdat/common.bdat', 'bdat/us/common_ms.bdat'])
        self.assertTrue(p.sure and mod_layout.needs_confirm(p))
        p = mod_layout.plan(members(*names), None)   # game files unknown: a guess from the folder names
        self.assertEqual((p.how, p.top), ('guessed', 'tweaks mod'))
        self.assertFalse(p.sure)

    def test_loose_and_added_files(self):
        p = mod_layout.plan(members('bdat/common.bdat', 'README.md'), self.G)
        self.assertEqual((p.how, p.top, p.left_out), ('matched', '', [('README.md', 'doc')]))
        p = mod_layout.plan(members('m/chr/np/a.wimdo', 'm/chr/np/new.wimdo', 'm/preview.png'), self.G)
        self.assertEqual((p.top, p.added, p.left_out), ('m', ['chr/np/new.wimdo'], [('m/preview.png', 'doc')]))

    def test_several_versions(self):
        names = members('Lite/bdat/common.bdat', 'Full/bdat/common.bdat', 'readme.txt')
        for g in (self.G, None):
            with self.assertRaises(mod_layout.LayoutError) as e:
                mod_layout.plan(names, g)
            self.assertEqual((e.exception.id, e.exception.params['tops']), ('layout.several', ['Full', 'Lite']))
        p = mod_layout.plan(names, self.G, top='Lite')
        self.assertEqual((p.files, p.left_out[-1]), ({'Lite/bdat/common.bdat': 'bdat/common.bdat'},
                                                     ('readme.txt', 'doc')))

    def test_refusals(self):
        cases = ((members('readme.txt'), 'mods.zip_no_romfs'), ([], 'archive.empty'),
                 ([Member('romfs/x', 'link', 0, True)], 'mods.zip_unsafe'),
                 ([Member('../romfs/x', 'file', 1, False)], 'mods.zip_unsafe'),
                 ([Member('romfs/x', 'file', 1, True, True)], 'archive.encrypted'),
                 (members('romfs/A.bin', 'romfs/a.bin'), 'mods.zip_duplicate'),
                 (members('0100000000010000/romfs/x'), 'mods.zip_other_game'))
        for m, error in cases:
            with self.assertRaises(mod_layout.LayoutError) as e:
                mod_layout.plan(m, self.G)
            self.assertEqual(e.exception.id, error)

    def test_describe_uses_every_line_the_catalog_has(self):
        en = json.loads((ROOT / 'installer' / 'text' / 'en.json').read_text(encoding='utf-8'))
        ru = json.loads((ROOT / 'installer' / 'text' / 'ru.json').read_text(encoding='utf-8'))
        used = []
        cat = lambda i, **k: used.append(i) or en[i].format(**k)
        for m, g in ((members('m/chr/np/a.wimdo', 'm/chr/np/b', 'exefs/x', 'r.txt', 'other/y'), self.G),
                     (members(*(f'romfs/f{i}' for i in range(20))), None),
                     (members('romfs/mod/a'), None), (members('m/bdat/a'), None)):
            mod_layout.describe(mod_layout.plan(m, g), cat)
        self.assertTrue(set(used) <= set(en) and set(used) <= set(ru))
        ids = {i for i in en if i.startswith(('layout.', 'archive.'))} | {i for i in en if i.startswith('mods.')}
        self.assertEqual(sorted(ids - set(ru)), [])


class ManualTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = Path(self.tmp.name) / 'XCX'
        self.dest.mkdir()
        self.zip = Path(self.tmp.name) / 'all tweaks mod-10-1-0-1745514276.zip'
        with zipfile.ZipFile(self.zip, 'w') as z:
            z.writestr('tweaks mod/bdat/common.bdat', b'one')
        data = self.zip.read_bytes()
        self.entry = {'id': 'tweaks', 'kind': 'romfs', 'download': 'manual', 'name': 'Tweaks', 'version': '1',
                      'page': 'https://example.invalid/files', 'file': self.zip.name, 'size': len(data),
                      'sha256': hashlib.sha256(data).hexdigest()}

    def tearDown(self):
        self.tmp.cleanup()

    def test_install_from_the_players_download(self):
        with self.assertRaises(mods.ModError) as e:
            mods.install(self.dest, self.entry)
        self.assertEqual(e.exception.id, 'mods.manual_needs_file')
        other = Path(self.tmp.name) / 'other.zip'
        shutil.copy(self.zip, other)
        with open(other, 'ab') as f:
            f.write(b'x')
        with self.assertRaises(mods.ModError) as e:
            mods.install(self.dest, self.entry, file=other)
        self.assertEqual((e.exception.id, e.exception.params['picked']), ('mods.file_mismatch', 'other.zip'))
        self.assertFalse((self.dest / 'mods' / 'tweaks').exists())
        mods.install(self.dest, self.entry, file=self.zip)
        self.assertTrue((self.dest / 'mods' / 'tweaks' / 'romfs' / 'bdat' / 'common.bdat').is_file())

    def test_index_keeps_complete_manual_entries(self):
        index = Path(self.tmp.name) / 'index.json'
        broken = dict(self.entry, id='broken', page='http://example.invalid')
        index.write_text(json.dumps({'format': 2, 'mods': [self.entry, broken]}))
        self.assertEqual([e['id'] for e in mods.fetch_index(str(index))], ['tweaks'])

    def test_names_from_downloads(self):
        self.assertEqual(mods.archive_name(self.zip), 'all tweaks mod')
        self.assertEqual(mods.archive_name('Tatsu Potato v1.zip'), 'Tatsu Potato v1')
        self.assertEqual(mods.archive_name('Mod-1-2.zip'), 'Mod-1-2')
        self.assertEqual(mods.add_archive(self.dest, self.zip), 'all-tweaks-mod')


class PartsTest(unittest.TestCase):
    """An entry in parts: a full version and optional pieces, each its own download, as on Nexus Mods."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.dest = t / 'XCX'
        self.dest.mkdir()
        self.files = {}
        for pid, members in (('all', {'tweaks mod/bdat/common.bdat': b'all'}),
                             ('probe', {'probe tweak/bdat/common.bdat': b'probe'}),
                             ('sensor', {'sensor/bdat/common.bdat': b'sensor'})):
            path = t / f'{pid} part-10-1-0-1745514623.zip'
            with zipfile.ZipFile(path, 'w') as z:
                for n, d in members.items():
                    z.writestr(n, d)
            self.files[pid] = path
        self.stranger = t / 'other.zip'
        with zipfile.ZipFile(self.stranger, 'w') as z:
            z.writestr('romfs/x.bin', b'x')

        def part(pid, includes=()):
            data = self.files[pid].read_bytes()
            return {'id': pid, 'name': pid.title(), 'page': 'https://example.invalid/files', 'file': self.files[pid].name,
                    'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data), 'includes': list(includes)}
        self.entry = {'id': 'tweaks', 'kind': 'romfs', 'download': 'manual', 'name': 'Tweaks', 'version': '1',
                      'author': 'A', 'homepage': 'https://example.invalid',
                      'parts': [part('all', ['probe', 'sensor']), part('probe'), part('sensor')]}

    def tearDown(self):
        self.tmp.cleanup()

    def test_picks_are_matched_by_hash(self):
        # any order, any names: the hash says which part a file is; a stranger is reported, not installed
        picked = [self.stranger, self.files['sensor'], self.files['all']]
        matched, unknown = mods.match_files(self.entry, picked)
        self.assertEqual(sorted(matched), ['tweaks.all', 'tweaks.sensor'])
        self.assertEqual(unknown, [self.stranger])
        result = mods.install(self.dest, self.entry, files=picked)
        self.assertEqual((sorted(result['installed']), result['unknown']),
                         (['tweaks.all', 'tweaks.sensor'], [self.stranger]))
        info = mods.read_info(self.dest / 'mods' / 'tweaks.all')
        self.assertEqual((info['name'], info['group'], info['part'], info['includes']),
                         ('Tweaks: All', 'tweaks', 'all', 'probe,sensor'))
        self.assertTrue((self.dest / 'mods' / 'tweaks.sensor' / 'romfs' / 'bdat' / 'common.bdat').is_file())
        with self.assertRaises(mods.ModError) as e:
            mods.install(self.dest, self.entry, files=[self.stranger])
        self.assertEqual(e.exception.id, 'mods.file_mismatch')

    def test_status_and_overlaps(self):
        mods.install(self.dest, self.entry, files=[self.files['all'], self.files['probe']])
        mods.set_enabled(self.dest, 'tweaks.all', True)
        row = next(r for r in mods.status(self.dest, [self.entry]) if r['id'] == 'tweaks')
        self.assertEqual([(p['mid'], p['installed'], p['enabled']) for p in row['parts']],
                         [('tweaks.all', True, True), ('tweaks.probe', True, False), ('tweaks.sensor', False, False)])
        self.assertTrue(row['installed'] and row['enabled'])
        # the parts are not listed again as mods outside the library
        self.assertEqual([r['id'] for r in mods.status(self.dest, [self.entry])], ['tweaks'])
        self.assertEqual(mods.overlaps(row, 'tweaks.probe'), ['All'])     # all contains probe, and all is on
        self.assertEqual(mods.overlaps(row, 'tweaks.sensor'), ['All'])
        mods.set_enabled(self.dest, 'tweaks.probe', True)
        row = next(r for r in mods.status(self.dest, [self.entry]) if r['id'] == 'tweaks')
        self.assertEqual(mods.overlaps(row, 'tweaks.all'), ['Probe'])     # the other way round

    def test_index_checks_parts(self):
        index = Path(self.tmp.name) / 'index.json'
        bad = json.loads(json.dumps(self.entry))
        bad['id'] = 'bad'
        bad['parts'][1]['page'] = 'http://example.invalid'
        long = json.loads(json.dumps(self.entry))
        long['id'] = 'x' * 62
        index.write_text(json.dumps({'format': 2, 'mods': [self.entry, bad, long]}))
        self.assertEqual([e['id'] for e in mods.fetch_index(str(index))], ['tweaks'])


@unittest.skipUnless(clang(), 'needs clang (XCX_CLANG, PATH or MSYS2 UCRT64)')
class RarTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import build_unpack
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        build_unpack.build(clang(), cls.root / 'bin' / archive.UNPACK)
        cls.layout = SimpleNamespace(dev=False, root=cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_reads_and_unpacks(self):
        for name, file, size in (('test_read_format_rar5_stored.rar', 'helloworld.txt', 29),
                                 ('test_read_format_rar5_compressed.rar', 'test.bin', 1200)):
            path = fixture(name, self.root)
            self.assertEqual(archive.kind_of(path), 'rar')
            self.assertEqual([(m.name, m.type, m.size) for m in archive.members(path, self.layout)],
                             [(file, 'file', size)])
            out = self.root / ('out-' + name)
            self.assertEqual(archive.extract(path, self.layout, {file: 'bdat/' + file}, out, 1 << 20), 1)
            self.assertEqual((out / 'bdat' / file).stat().st_size, size)

    def test_refuses_links_and_passwords(self):
        for name, error in (('test_read_format_rar5_symlink.rar', 'mods.zip_unsafe'),
                            ('test_read_format_rar5_hardlink.rar', 'mods.zip_unsafe'),
                            ('test_read_format_rar.rar', 'mods.zip_unsafe'),
                            ('test_read_format_rar5_unicode.rar', 'mods.zip_unsafe'),
                            ('test_read_format_rar5_encrypted.rar', 'archive.encrypted')):
            path = fixture(name, self.root)
            with self.assertRaises(mod_layout.LayoutError) as e:
                mod_layout.plan(archive.members(path, self.layout))
            self.assertEqual(e.exception.id, error, name)
            # xcx-unpack refuses them as well, so a plan that skipped the check still writes nothing
            out = self.root / ('x-' + name)
            out.mkdir()
            with self.assertRaises(archive.ArchiveError):
                archive.run_tool(self.root / 'bin' / archive.UNPACK, 'extract', path, out, 1 << 20)

    def test_limit_and_broken_archives(self):
        path = fixture('test_read_format_rar5_compressed.rar', self.root)
        out = self.root / 'small'
        out.mkdir()
        with self.assertRaises(archive.ArchiveError) as e:
            archive.run_tool(self.root / 'bin' / archive.UNPACK, 'extract', path, out, 100)
        self.assertEqual(e.exception.id, 'mods.zip_too_big')
        with self.assertRaises(archive.ArchiveError) as e:
            archive.members(fixture('test_read_format_rar_invalid1.rar', self.root), self.layout)
        self.assertEqual(e.exception.id, 'archive.bad')

    def test_7z_and_unknown_files(self):
        seven = self.root / 'a.7z'
        seven.write_bytes(b"7z\xbc\xaf'\x1c" + b'\0' * 26)
        other = self.root / 'a.bin'
        other.write_bytes(b'hello')
        for path, error in ((seven, 'archive.no_7z'), (other, 'archive.unknown')):
            with self.assertRaises(archive.ArchiveError) as e:
                archive.members(path, self.layout)
            self.assertEqual(e.exception.id, error)


if __name__ == '__main__':
    unittest.main()
