"""The generator's hook modules (recomp/mod_state.py): every site a hook changes is checked.

A hook module adds C at chosen addresses of one build. Its verified() compares an instruction word at each listed
address with the build's, so a hook on a moved or different instruction stops the generation instead of running.
These tests need no game files: a stand-in module answers each listed word, and every address a module's tables name
is asked for hook code; each address that gets some must be in the module's WORDS (for an entry hook: the function's
first instruction).
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))

import mod_state  # noqa: E402

BASE = 0x7000000000


class FakeNso:
    def __init__(self, hooks):
        self.build_id = hooks.BUILD
        self.source_sha256 = hooks.SHA256


class FakeModule:
    """The verified build as far as a hook module looks: its identity and the listed words."""

    def __init__(self, hooks, words=None, name='main'):
        self.name, self.base, self.nso = name, BASE, FakeNso(hooks)
        self.words = dict(hooks.WORDS if words is None else words)

    def word(self, address):
        return self.words.get(address - BASE, 0xD503201F)   # nop where the module lists nothing


def addresses(hooks):
    """Every main offset a hook module's tables name (ints, dict keys and values, tuple items)."""
    found = set()

    def walk(v):
        if isinstance(v, bool):
            return
        if isinstance(v, int) and 0x1000 <= v < 0x2000000:
            found.add(v)
        elif isinstance(v, dict):
            for k, x in v.items():
                walk(k)
                walk(x)
        elif isinstance(v, (tuple, list, set, frozenset)):
            for x in v:
                walk(x)
    for name, v in vars(hooks).items():
        if name.isupper():
            walk(v)
    return found


class HookSites(unittest.TestCase):
    def check(self, hooks):
        mod = FakeModule(hooks)
        self.assertTrue(hooks.verified(mod))
        hooked = set()
        for a in addresses(hooks):
            if hooks.entry(mod, BASE + a) or hooks.after(mod, BASE + a):
                hooked.add(a)
            if hasattr(hooks, 'call'):
                w = hooks.WORDS.get(a)
                if w is not None and w >> 26 == 0x25:   # bl: the target it encodes
                    imm = w & 0x3FFFFFF
                    target = BASE + a + 4 * (imm - (1 << 26) if imm >> 25 else imm)
                else:
                    target = BASE + 0x100000
                try:
                    if hooks.call(mod, BASE + a, target):
                        hooked.add(a)
                except AssertionError:   # a call hook for another target: still a hooked site
                    hooked.add(a)
        self.assertTrue(hooked)
        unchecked = sorted(hex(a) for a in hooked - set(hooks.WORDS))
        self.assertEqual(unchecked, [], f'{hooks.__name__}: hooked sites without an expected instruction word')

    def mismatch(self, hooks):
        # any listed word that differs stops the generation; another build is left alone
        a = sorted(hooks.WORDS)[0]
        with self.assertRaises(ValueError):
            hooks.verified(FakeModule(hooks, {**hooks.WORDS, a: hooks.WORDS[a] ^ 1}))
        other = FakeModule(hooks)
        other.nso.build_id = '0' * 40
        self.assertFalse(hooks.verified(other))
        self.assertFalse(hooks.verified(FakeModule(hooks, name='subsdk0')))

    def test_mod_state(self):
        self.check(mod_state)
        self.mismatch(mod_state)


if __name__ == '__main__':
    unittest.main()
