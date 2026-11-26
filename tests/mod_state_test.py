"""runtime/mod_state.c: mods' flags and survey reward marks kept beside the save (docs/mod-state.md).

tests/mod_state_check.c plays the game's side (flag test and set, the reward grant, Commits, loads, the game data)
from a script; these tests check the answers and the files. Needs clang (MSYS2 UCRT64 on Windows).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAG = 0xC000 + 1           # the first mod flag id used here
REWARD = 0x8000 + 1         # a mod survey reward row's flag_index


def clang():
    found = shutil.which('clang') or ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    return found if found and Path(found).is_file() else None


@unittest.skipUnless(clang(), 'needs clang (MSYS2 UCRT64)')
class ModState(unittest.TestCase):
    exe = None

    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        rt = ROOT / 'runtime'
        plat = 'win32' if os.name == 'nt' else 'linux'
        libs = ['-lwinmm', '-lbcrypt'] if os.name == 'nt' else ['-lpthread', '-lm']
        cls.exe = Path(cls.build.name) / ('mod_state_check' + ('.exe' if os.name == 'nt' else ''))
        subprocess.run([clang(), '-O1', '-Wall', '-Wextra', '-Werror=implicit-function-declaration', f'-I{rt}',
                        f'-I{rt / "platform"}', str(ROOT / 'tests/mod_state_check.c'), str(rt / f'platform/{plat}.c'),
                        str(rt / 'platform/common.c'), *libs, '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.mods, self.committed, self.working = t / 'mods', t / 'save', t / 'work'
        for d in (self.mods, self.committed, self.working):
            d.mkdir()
        self.mod('isle', {'id': 'isle', 'version': '2', 'regions': [24],
                          'flags': [{'name': 'seen', 'type': 1, 'id': FLAG}, {'name': 'boss', 'type': 1, 'id': FLAG + 1}],
                          'rewards': [{'name': 'cache', 'index': REWARD}]})
        self.mod('reef', {'id': 'reef', 'version': '1', 'regions': [25],
                          'flags': [{'name': 'found', 'type': 1, 'id': FLAG + 10}]})
        self.enable('isle', 'reef')

    def tearDown(self):
        self.tmp.cleanup()

    def mod(self, folder, manifest):
        (self.mods / folder).mkdir(exist_ok=True)
        (self.mods / folder / 'mod.json').write_text(json.dumps(manifest) if isinstance(manifest, dict) else manifest)

    def enable(self, *ids):
        (self.mods / 'romfs-enabled.txt').write_text(''.join(f'{i}\n' for i in ids))

    def run_script(self, *lines, env=None):
        """Runs one game session; returns its output lines."""
        e = dict(os.environ, XCX_MOD_DIR=str(self.mods))
        e.pop('XCX_MOD_STATE', None)
        e.update(env or {})
        r = subprocess.run([str(self.exe), str(self.committed), str(self.working)], input='\n'.join(lines) + '\n',
                           capture_output=True, text=True, env=e, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.splitlines()

    def answers(self, out, kind):
        return [line.split()[1:] for line in out if line.startswith(kind + ' ')]

    def state(self, slot='gamedata01', where=None):
        return json.loads(((where or self.committed) / 'mods' / f'state-{slot}.json').read_text())

    def save(self, slot='gamedata01', time=1000):
        return [f'time {time}', f'written {slot}', 'commit']

    def load(self, slot='gamedata01', time=1000):
        # the game reads the slot, then the loaded game data holds its time a few presents later
        return ['title', 'present 5', f'read {slot}', 'present 10', f'time {time}', 'present 5', f'read {slot}',
                'present 5', 'world', 'present 5']   # the game reads the slot more than once while it loads

    # ---- the hooks

    def test_flags_are_answered_from_the_mod_state(self):
        out = self.run_script(f'get 1 {FLAG}', f'set 1 {FLAG} 7', f'get 1 {FLAG}', f'set 1 {FLAG} 9',
                              'get 1 100', 'set 1 100 1', f'get 1 {0xFFFF}', f'get 1 {0xC000 - 1}')
        self.assertEqual(self.answers(out, 'get'), [['1', '0'], ['1', '7'], ['0', '1'], ['0', '1'], ['0', '1']])
        self.assertEqual(self.answers(out, 'set'), [['1', '0'], ['1', '7'], ['0', '0']])   # x8 = the old value

    def test_a_flag_of_another_type_is_its_own(self):
        out = self.run_script(f'set 1 {FLAG} 5', f'get 2 {FLAG}', f'set 2 {FLAG} 3', f'get 1 {FLAG}', *self.save())
        self.assertEqual(self.answers(out, 'get'), [['1', '0'], ['1', '5']])
        self.assertEqual(self.state()['mods']['_undeclared']['flags'], {f'flag/2/{FLAG}': 3})

    def test_survey_rewards(self):
        out = self.run_script(f'reward {REWARD} 0', f'reward {REWARD} 0', 'reward 5 0', 'reward 5 4', *self.save())
        # the mark of a mod row comes from the mod state; the game's word is never changed for it; game rows untouched
        self.assertEqual(self.answers(out, 'reward'), [['0'], ['4'], ['0'], ['4']])
        self.assertEqual(self.answers(out, 'claim'), [['0'], ['0'], ['4'], ['4']])
        self.assertEqual(self.state()['mods']['isle']['rewards'], {'cache': 1})

    def test_off_switch(self):
        out = self.run_script(f'set 1 {FLAG} 7', f'get 1 {FLAG}', f'reward {REWARD} 0', *self.save(),
                              env={'XCX_MOD_STATE': '0'})
        self.assertEqual(self.answers(out, 'set'), [['0', '0']])
        self.assertEqual(self.answers(out, 'get'), [['0', '1']])
        self.assertEqual(self.answers(out, 'reward'), [['0']])
        self.assertEqual(self.answers(out, 'claim'), [['4']])
        self.assertFalse((self.committed / 'mods').exists())

    # ---- files

    def test_save_writes_the_slots_written_and_load_reads_them_back(self):
        self.run_script(f'set 1 {FLAG} 7', f'set 1 {FLAG + 10} 2', f'reward {REWARD} 0', *self.save('gamedata02', 1000),
                        f'set 1 {FLAG} 8', *self.save('gamedataa', 2000))
        self.assertEqual(sorted(p.name for p in (self.committed / 'mods').iterdir()),
                         ['state-gamedata02.json', 'state-gamedataa.json'])
        s = self.state('gamedata02')
        self.assertEqual((s['save_time'], s['slot'], s['format']), (1000, 'gamedata02', 1))
        self.assertEqual(s['mods']['isle']['flags'], {'seen': 7})
        out = self.run_script(*self.load('gamedata02', 1000), f'get 1 {FLAG}', f'get 1 {FLAG + 10}', f'reward {REWARD} 0')
        self.assertEqual(self.answers(out, 'get'), [['1', '7'], ['1', '2']])
        self.assertEqual(self.answers(out, 'reward'), [['4']])
        out = self.run_script(*self.load('gamedataa', 2000), f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '8']])

    def test_a_big_file_reads_back(self):
        # big enough that growing the read buffer by one byte moves it (a pointer taken before that went stale once)
        sets = [f'set 1 {FLAG + 100 + i} {i + 1}' for i in range(3000)]
        self.run_script(*sets, *self.save())
        self.assertGreater((self.working / 'mods' / 'state-gamedata01.json').stat().st_size, 50000)
        out = self.run_script(*self.load(), f'get 1 {FLAG + 100}', f'get 1 {FLAG + 3099}')
        self.assertEqual(self.answers(out, 'get'), [['1', '1'], ['1', '3000']])
        self.assertFalse(any('checksum does not match' in line for line in out))

    def test_a_commit_without_a_slot_writes_nothing(self):
        self.run_script(f'set 1 {FLAG} 7', 'time 1000', 'commit')
        self.assertFalse((self.committed / 'mods').exists())

    def test_nothing_of_any_mod_writes_no_file(self):
        self.enable()
        self.run_script('get 1 100', *self.save())
        self.assertFalse((self.committed / 'mods').exists())

    def test_the_file_is_stable(self):
        self.run_script(f'set 1 {FLAG + 1} 1', f'set 1 {FLAG} 2', *self.save())
        first = (self.committed / 'mods' / 'state-gamedata01.json').read_text()
        self.run_script(*self.load(), *self.save())
        self.assertEqual((self.committed / 'mods' / 'state-gamedata01.json').read_text(), first)
        self.assertEqual(list(self.state()['mods']['isle']['flags']), ['boss', 'seen'])   # sorted

    def test_a_file_from_an_older_save_in_the_slot_is_not_used(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save('gamedata01', 1000))
        # the slot now holds a save made without this runtime (time 3000): its old state file must not apply
        out = self.run_script(*self.load('gamedata01', 3000), 'present 200', f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '0']])

    def test_a_damaged_file_is_not_used(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save())
        p = self.working / 'mods' / 'state-gamedata01.json'   # where the game reads its save
        p.write_bytes(p.read_bytes().replace(b'"seen": 7', b'"seen": 8'))
        out = self.run_script(*self.load(), 'present 200', f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '0']])
        self.assertTrue(any('checksum does not match' in line for line in out))

    def test_reading_a_slot_without_loading_it_changes_nothing(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save('gamedata01', 1000))
        self.run_script(f'set 1 {FLAG} 5', *self.save('gamedata02', 2000))
        # in the game of slot 2, the game reads slot 1's file (a load menu) but stays in slot 2's game
        out = self.run_script(*self.load('gamedata02', 2000), 'read gamedata01', 'present 1000', f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '5']])

    def test_the_save_screen_reading_the_held_slot_changes_nothing(self):
        # in play after a load, a flag changes; the save screen then reads the same slot whole: the newer value stays
        self.run_script(f'set 1 {FLAG} 7', *self.save())
        out = self.run_script(*self.load(), f'set 1 {FLAG} 9', 'read gamedata01', 'present 200', f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '9']])
        self.assertEqual(sum('loaded: its mod state read' in line for line in out), 1)

    def test_loading_the_held_save_again_from_the_title_drops_unsaved_values(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save())
        out = self.run_script(*self.load(), f'set 1 {FLAG} 9', *self.load(), f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '7']])

    def test_a_new_game_starts_empty_and_keeps_what_it_set(self):
        self.run_script(f'set 1 {FLAG} 7', f'set 1 {FLAG + 1} 1', *self.save())
        out = self.run_script(*self.load(), f'get 1 {FLAG}',
                              'time 0', 'present 10', f'set 1 {FLAG + 1} 4', 'present 200', f'get 1 {FLAG}', f'get 1 {FLAG + 1}')
        self.assertEqual(self.answers(out, 'get'), [['1', '7'], ['1', '0'], ['1', '4']])

    def test_the_new_game_wait_is_time_not_frames(self):
        # at 240 fps, 400 presents are 1.7 s: still the old save's state; 600 more make it 4.2 s: another save
        self.run_script(f'set 1 {FLAG} 7', *self.save())
        out = self.run_script(*self.load(), 'fps 240', 'time 0', 'present 400', f'get 1 {FLAG}', 'present 600',
                              f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '7'], ['1', '0']])

    def test_a_save_in_progress_is_not_a_new_game(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save())
        # the game sets the new save time, then writes the file for longer than the new-game wait, then commits
        out = self.run_script(*self.load(), 'time 1500', 'busy 1', 'present 300', 'written gamedata01', 'busy 0',
                              'present 300', 'commit', 'present 300', f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '7']])
        self.assertEqual(self.state()['save_time'], 1500)

    def test_a_save_loaded_without_a_state_file_starts_empty(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save('gamedata01', 1000))
        out = self.run_script(*self.load('gamedata01', 1000), *self.load('gamedata03', 5000), 'present 200', f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '0']])

    # ---- mods that are off, and mods switched while the game runs

    def test_a_mod_that_is_off_keeps_its_section_as_it_was(self):
        self.run_script(f'set 1 {FLAG + 10} 2', f'set 1 {FLAG} 7', *self.save())
        section = self.state()['mods']['reef']
        self.enable('isle')
        out = self.run_script(*self.load(), f'get 1 {FLAG + 10}', f'set 1 {FLAG} 8', *self.save())
        self.assertEqual(self.answers(out, 'get'), [['1', '0']])   # nobody declares it now: kept apart, not answered
        s = self.state()
        self.assertEqual(s['mods']['reef'], section)
        self.assertEqual(s['mods']['isle']['flags'], {'seen': 8})
        self.enable('isle', 'reef')
        out = self.run_script(*self.load(), f'get 1 {FLAG + 10}')
        self.assertEqual(self.answers(out, 'get'), [['1', '2']])

    def test_switching_mods_while_the_game_runs(self):
        self.run_script(f'set 1 {FLAG + 10} 2', f'set 1 {FLAG} 7', *self.save())
        out = self.run_script(*self.load(), 'present 30',
                              f'get 1 {FLAG + 10}')
        self.assertEqual(self.answers(out, 'get'), [['1', '2']])
        # reef off, a save, reef on again, a save: its value survives both, and is answered again once it is on
        lines = [*self.load(), 'present 30', 'off reef', 'present 30', f'set 1 {FLAG} 8', *self.save(time=1100),
                 'on reef', 'present 30', f'get 1 {FLAG + 10}', *self.save(time=1200)]
        out = self.run_switching(lines)
        self.assertEqual(self.answers(out, 'get'), [['1', '2']])
        s = self.state()
        self.assertEqual((s['mods']['reef']['flags'], s['mods']['isle']['flags']), ({'found': 2}, {'seen': 8}))

    def run_switching(self, lines):
        """run_script with 'off <id>' / 'on <id>' lines that change romfs-enabled.txt between presents."""
        out = []
        on = {'isle', 'reef'}
        env = dict(os.environ, XCX_MOD_DIR=str(self.mods))
        env.pop('XCX_MOD_STATE', None)
        proc = subprocess.Popen([str(self.exe), str(self.committed), str(self.working)], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True, env=env)
        for line in lines:
            if line.startswith(('off ', 'on ')):
                proc.stdin.write('sync\n')   # every line before has run: now the list changes
                proc.stdin.flush()
                while (got := proc.stdout.readline().rstrip('\n')) != 'sync':
                    self.assertTrue(got, 'the check program ended early')
                    out.append(got)
                verb, mid = line.split()
                (on.discard if verb == 'off' else on.add)(mid)
                self.enable(*sorted(on))
                continue
            proc.stdin.write(line + '\n')
        stdout, _ = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 0, stdout)
        return out + stdout.splitlines()

    # ---- leaving a mod's land

    def test_a_save_on_land_of_a_mod_that_is_off_moves_the_party_out(self):
        # outside: sampled while walking; then on the island (region 24) the game saves
        self.run_script('time 1', 'region 2', 'leader 100 5 200', 'present 60', 'region 24', 'leader 900 1 900',
                        'present 60', *self.save())
        s = self.state()
        self.assertEqual((s['player']['region'], s['player']['owner']), (24, 'isle'))
        self.assertEqual(s['player']['last_outside'], [100.0, 5.0, 200.0])
        self.enable('reef')
        out = self.run_script('leader 900 1 900', *self.load(), 'pos')
        self.assertEqual(self.answers(out, 'pos'), [['100.0', '5.0', '200.0']])
        self.enable('isle', 'reef')   # with the mod on, the party stays where it saved
        out = self.run_script('leader 900 1 900', *self.load(), 'pos')
        self.assertEqual(self.answers(out, 'pos'), [['900.0', '1.0', '900.0']])

    def test_a_warp_onto_mod_land_is_not_taken_for_outside(self):
        # after a warp the position is the new one while the region is still the old one for a moment; at 2 fps each
        # present is one sample: one sample sees the island's position with the old region
        self.run_script('fps 2', 'time 1', 'region 2', 'leader 100 5 200', 'present 3', 'leader 900 1 900', 'present 1',
                        'region 24', 'present 3', *self.save())
        self.assertEqual(self.state()['player']['last_outside'], [100.0, 5.0, 200.0])

    def test_a_load_that_reads_its_slot_twice_applies_it_once(self):
        self.enable('isle')
        self.run_script('time 1', 'region 2', 'leader 100 5 200', 'present 90', 'region 24', 'leader 900 1 900',
                        'present 90', *self.save())
        self.enable('reef')
        out = self.run_script('leader 900 1 900', *self.load())
        self.assertEqual(sum('loaded: its mod state read' in line for line in out), 1)
        self.assertEqual(sum('the party goes to' in line for line in out), 1)

    # ---- mod.json

    def test_manifest_problems_are_reported_and_left_out(self):
        self.mod('bad', '{"id": "bad", "flags": [ {"name": "x", "type": 1, "id": 49153 ')
        self.mod('dup', {'id': 'dup', 'flags': [{'name': 'mine', 'type': 1, 'id': FLAG},
                                                {'name': 'low', 'type': 1, 'id': 100}]})
        self.mod('long', {'id': 'long', 'flags': [{'name': 'n' * 200, 'type': 1, 'id': FLAG + 50}]})
        self.mod('twin', {'id': 'isle'})
        self.enable('isle', 'reef', 'bad', 'dup', 'long', 'twin')
        out = self.run_script(f'set 1 {FLAG} 3', *self.save())
        logs = '\n'.join(out)
        self.assertIn('bad/mod.json: not readable as JSON', logs)
        self.assertIn("dup's mine uses number", logs)           # isle declared it first
        self.assertIn('flag "low" (100) left out', logs)
        self.assertIn('long/mod.json: a name or id is longer', logs)
        self.assertIn("id isle is another enabled mod's", logs)
        self.assertEqual(self.state()['mods']['isle']['flags'], {'seen': 3})

    def test_hostile_manifests_are_refused(self):
        self.mod('deep', '{"id": "deep", "x": ' + '[' * 200000 + ']' * 200000 + '}')
        self.mod('quote', {'id': 'quote', 'flags': [{'name': 'a"b', 'type': 1, 'id': FLAG + 70}]})
        self.mod('upper', {'id': 'Upper', 'flags': [{'name': 'a', 'type': 1, 'id': FLAG + 71}]})
        self.mod('ctrl', {'id': 'ctrl', 'flags': [{'name': 'a\nb', 'type': 1, 'id': FLAG + 72}]})
        self.enable('isle', 'deep', 'quote', 'upper', 'ctrl')
        out = self.run_script(f'set 1 {FLAG} 3', f'set 1 {FLAG + 70} 1', *self.save())
        logs = '\n'.join(out)
        self.assertIn('deep/mod.json: not readable as JSON', logs)
        for folder in ('quote', 'upper', 'ctrl'):
            self.assertIn(f'{folder}/mod.json: an id that is not lower case', logs)
        s = self.state()   # the file reads back: every name in it is plain
        self.assertEqual(s['mods']['isle']['flags'], {'seen': 3})
        self.assertEqual(s['mods']['_undeclared']['flags'], {f'flag/1/{FLAG + 70}': 1})

    def test_a_file_from_a_newer_format_is_set_aside_not_overwritten(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save())
        p = self.working / 'mods' / 'state-gamedata01.json'
        body = p.read_bytes().decode().replace('"format": 1', '"format": 2')
        body = body[:body.index('  "checksum"')]
        h = 0xcbf29ce484222325
        for b in body.encode():
            h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
        p.write_bytes((body + f'  "checksum": "{h:016x}"\n}}\n').encode())
        out = self.run_script(*self.load(), f'get 1 {FLAG}')
        self.assertEqual(self.answers(out, 'get'), [['1', '0']])
        self.assertTrue(any('newer version of the mod state' in line for line in out))
        self.assertTrue((self.working / 'mods' / 'state-gamedata01.json.bad').is_file())

    def test_unknown_parts_are_kept(self):
        self.run_script(f'set 1 {FLAG} 7', *self.save())
        p = self.working / 'mods' / 'state-gamedata01.json'   # where the game reads its save
        text = p.read_bytes().decode().replace('  "player"', '  "future": {"a": [1, 2]},\n  "player"')
        body = text[:text.index('  "checksum"')]
        h = 0xcbf29ce484222325
        for b in body.encode():   # the checksum covers everything before its own line
            h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
        p.write_bytes((body + f'  "checksum": "{h:016x}"\n}}\n').encode())   # bytes: no newline translation
        self.run_script(*self.load(), *self.save())
        self.assertEqual(self.state()['future'], {'a': [1, 2]})
        self.assertEqual(self.state()['mods']['isle']['flags'], {'seen': 7})


if __name__ == '__main__':
    sys.exit(unittest.main())
