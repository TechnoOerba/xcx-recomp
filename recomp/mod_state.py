"""Mod state: game flags and survey reward marks that belong to mods, kept outside the save, for the verified XCX DE
2.0.0 main.

The game reads and writes its flags through one test and one set function (w0 = flag type, w1 = flag id):
  main+0xc96d0  test: returns the value in w0
  main+0xc94a0  set: w2 = value, w3 = notify; leaves the old value in x8
At the entry of each the generated C asks runtime/mod_state.c first; for an id in the mods' range it answers from the
mod state and returns, so the game's flag storage is never read or written for it.

The survey reward grant (main+0x561b90) keeps a claimed mark per reward row in a 320-bit block of the save, indexed by
the row's flag_index (SEG_ExploreReward). Two sites:
  main+0x561cd0  ldr w8, [x24, x27, lsl #2]   the word holding the row's mark (x0 = flag_index, w23 = its bit)
  main+0x561f78  orr w9, w9, w23              the word with the mark set, stored to [x24, x8] next
After each the generated C passes the word through runtime/mod_state.c: for a row in the mods' range the test answers
from the mod state and the claim keeps the game's word as it was, so the block is never changed for mod rows. (For a
mod row the game still loads, and stores back unchanged, the word its index would select: x24 + (flag_index >> 5) * 4,
4 to 8 KiB past the 40-byte block, other save data. The store writes back the value loaded a few instructions
before, so a write by another thread in those few instructions would be undone; which other code writes that word
was not traced.) The grant's entry (main+0x561b90) forgets the row the
last grant tested.
"""

BUILD = 'ec4ed46bbc4a9ebbe7ce0af7a4a8ebbc09e5724a'
SHA256 = '0a9dbf1f3816dd0fab0a08883d538c74d9b9fd7d939cb29aaf0947d71a201026'
FLAG_GET = 0xc96d0
FLAG_SET = 0xc94a0
GRANT = 0x561b90
ENTRIES = {FLAG_GET: 'if (mod_flag_get(c)) return;',
           FLAG_SET: 'if (mod_flag_set(c)) return;',
           GRANT: 'mod_reward_begin();'}
AFTER = {0x561cd0: 'c->x[8] = mod_reward_test(c->x[0], c->x[8], c->x[23]);',
         0x561f78: 'c->x[9] = mod_reward_claim(c->x[9], LD32(c->x[24] + c->x[8]));'}
WORDS = {FLAG_GET: 0xf000f649,   # adrp x9, <the flag tables' page>: the first instruction of both functions
         FLAG_SET: 0xf000f649,
         GRANT: 0xd10383ff,      # sub sp, sp, #0xe0
         0x561cd0: 0xb87b7b08,   # ldr w8, [x24, x27, lsl #2]
         0x561f78: 0x2a170129,   # orr w9, w9, w23
         0x561f7c: 0xb8286b09}   # str w9, [x24, x8]: the claim's store reads x24 + x8 as the hook does


def verified(mod):
    if mod.name != 'main' or mod.nso.build_id != BUILD:
        return False
    if mod.nso.source_sha256 != SHA256 or mod.base != 0x7000000000:
        raise ValueError('mod state: build identity mismatch')
    for offset, word in WORDS.items():
        if mod.word(mod.base + offset) != word:
            raise ValueError(f'mod state: instruction mismatch at main+{offset:#x}')
    return True


def entry(mod, address):
    """C to run at the entry of the function at address, or None."""
    return ENTRIES.get(address - mod.base)


def after(mod, address):
    """C to run after the instruction at address, or None."""
    return AFTER.get(address - mod.base)
