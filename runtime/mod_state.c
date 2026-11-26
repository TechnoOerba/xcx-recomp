// Mod state: what game-file mods add that must last, in a file of its own beside the game's save, never in it.
//
// The game's save holds a fixed set of flags and marks, and every bit of it already means something to the game. A
// mod that borrowed some (a spare flag, a claimed-reward bit past the game's) would clash with another mod borrowing
// the same, could run out (the claimed-reward block has 320 bits), and would leave traces in the save that the game
// without the mod reads as its own. So mod state lives here instead (docs/mod-state.md):
//
// - Flags: every flag id from MOD_FLAG_FIRST up (but MOD_FLAG_NONE), of any type, belongs to mods. The game's flag
//   test and set (main+0xc96d0, main+0xc94a0; recomp/mod_state.py) ask mod_flag_get / mod_flag_set first; for such an
//   id the value comes from and goes to this state, and the game's flag storage is never read or written for it.
// - Survey rewards: a SEG_ExploreReward row whose flag_index is MOD_REWARD_FIRST or more keeps its claimed mark here
//   (the grant, main+0x561b90, tests and sets it through mod_reward_test / mod_reward_claim).
// - Names, not numbers: each mod says, in mod.json at the top of its folder, which of those ids it uses and what they
//   are called; the state stores (mod id, name) -> value, so the numbers may change between versions of a mod:
//     { "id": "ashen-shoal", "version": "1", "regions": [24, 25],
//       "flags":   [ { "name": "discovered/ashen-reef", "type": 1, "id": 49153 }, ... ],
//       "rewards": [ { "name": "ashen-reef/5", "index": 32788 }, ... ] }
//   An id in the mods' range that no enabled mod declares is kept under the owner "_undeclared".
// - The file: <save>/mods/state-<slot>.json, one per save slot (gamedata01, ..., gamedataa), written into the save's
//   working copy right before each Commit for every slot the game wrote since the last one (fs_host.c), so it is
//   committed, backed up and copied with the save files themselves. It records the save's own time (the game data
//   object, +0x95688); when the game loads a slot, its file is used once the loaded game data holds that same time,
//   so a file left from an older save in that slot is never applied. JSON with keys in a fixed order and a checksum;
//   a file whose checksum fails is not used.
// - A mod that is off keeps its part of the file untouched (its section is copied into every later file), so turning
//   it on again brings its progress back; this holds when mods are switched while the game runs, too. Parts of the
//   file this runtime doesn't know are kept as they are.
// - A save time that changes with no load or save behind it (a New Game) starts an empty state; values the game set
//   after the change began are kept.
// - Leaving a mod's land: the file records the player's region and the last position outside every mod region
//   ("regions" in mod.json). A save made on a mod's land and loaded with that mod off would put the player in the
//   sea or a void; the party is moved to that last position instead.
//
// - For the runtime (mod_state.h, "Runtime values and features"): a mod's mod.json may name features ("features":
//   ["example.extra-rules"]) that runtime code turns on while that mod is enabled, and runtime code keeps
//   numbers of its own per save ("values" in a section): under such a mod's id (dormant with the mod, like its flags),
//   or under a runtime owner that is always read (mod_state_owner).
//
// XCX_MOD_STATE=0 turns it all off (mod ids then reach the game's storage, as without this file).
#include "mod_state.h"
#include "cfg.h"
#include "dispatch.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "romfs_mods.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_BASE 0x7000000000ull
#define GAMEDATA_PTR (MAIN_BASE + 0x1DC1408ull)   // the game data object (null until a save is loaded)
#define GD_SAVE_TIME 0x95688u                     // the save's time, Unix seconds (u32)
#define GD_POSITION 0x95560u                      // the party's positions, vec4 16 bytes apart; the first is the player
#define REGION_PTR (MAIN_BASE + 0x1df5968ull)     // the current region: *(this) + 0xb0
#define LEADER_POS (MAIN_BASE + 0x1f97ea0ull)     // a copy of the party leader's position, rewritten every frame
#define STATE_FORMAT 1
#define LOAD_WAIT_MS 60000    // how long a slot the game read may wait for the loaded game data to hold its save time
#define NEW_GAME_MS 2000      // how long a save time must differ, with no save behind it, to count as another save
#define SAMPLE_MS 500         // between samples of the spot outside mod land, and checks of the enabled mods
#define MAX_REGIONS 16      // regions per mod in mod.json
#define MAX_FEATURES 8      // features per mod in mod.json
#define MAX_OWNERS 8        // runtime owners (mod_state_owner)

enum { K_FLAG, K_REWARD, K_VALUE };
typedef struct { char owner[64], name[96]; uint8_t kind; uint32_t type, id; } Decl;   // a mod.json entry
typedef struct {
    char id[64], version[32], features[MAX_FEATURES][64];
    uint32_t regions[MAX_REGIONS];
    int nregions, nfeatures;
} Mod;
typedef struct { char owner[64], name[96]; uint32_t value, gen; uint8_t runtime; } Value;   // runtime: a K_VALUE
typedef struct { char owner[64]; char *raw; } Raw;                                    // a section kept as it is

static PlatMutex lock = PLAT_MUTEX_INIT;
static int off = -1, ready;
static Mod *mods;
static int nmods, capmods;
static Decl *decls;
static int ndecls, capdecls;
static Value *vals;
static int nvals, capvals;
static Raw *raws;               // sections of mods that are off, and parts this runtime doesn't know (owner "")
static int nraws, capraws;
static char *enabled_list;      // romfs-enabled.txt as the mods were last read from it
static uint32_t stamp;          // the save time the state belongs to
static uint32_t gen;            // bumped when the state may be replaced; values set later survive the replacement
static int have_player;
static uint32_t player_region;
static char player_owner[64];
static float last_outside[3];
static int have_outside;
static char written[8][MOD_STATE_SLOT_MAX];     // slots written since the last Commit
static int nwritten;
static char pending[MOD_STATE_SLOT_MAX];        // a slot the game read whose state waits for the game data to hold its save time
static int pending_known;       // its file was read: pending_want = its save time (0: no usable file)
static uint32_t list_gen;       // romfs_mods_generation() the enabled mods were last read at
static uint32_t pending_want;
static uint64_t load_until;     // plat_now_ms until which a slot the game read may still load (load_gen: its generation)
static uint32_t load_gen;
static uint64_t differ_since;   // plat_now_ms at which the game data's save time began to differ from stamp (0: equal)
static uint64_t next_sample;
static uint32_t differ_gen;
static int held;                // the state is the one of the save the game holds
static int pending_title;       // the slot was read at the title screen (a load), not in the world (the save screen)
static uint32_t applied_world;  // romfs_mods_world_loads() when a slot's state was last applied
static int sample_outside;      // the last sample was outside mod land, at sample_pos
static float sample_pos[3];
static char save_dir[1100];
static char runtime_owners[MAX_OWNERS][64];   // mod_state_owner: sections always read, whatever mods are on
static int nruntime_owners;
static uint32_t loads;          // bumped whenever the state is replaced (mod_state_loads)
static _Thread_local uint32_t cur_reward;   // the mod reward row this thread's grant tested last (0: a game row)

static int enabled(void) {
    int o = __atomic_load_n(&off, __ATOMIC_RELAXED);
    if (o < 0) {
        const char *v = cfg_get("XCX_MOD_STATE");
        o = v && !strcmp(v, "0");
        __atomic_store_n(&off, o, __ATOMIC_RELAXED);
    }
    return !o;
}

static void *grow(void *p, int *cap, int need, size_t size) {
    if (need <= *cap) return p;
    int c = *cap ? *cap * 2 : 16;
    while (c < need) c *= 2;
    void *q = realloc(p, (size_t)c * size);
    if (!q) rt_fatal("mod state: out of memory");
    *cap = c;
    return q;
}

// ---- a small JSON reader (what mod.json and our own files hold)
#define JSON_DEPTH 32   // nesting the reader follows; deeper is a format error (no stack overflow on hostile files)
typedef struct { const char *p, *end; int bad, depth; } J;
static void jws(J *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++;
}
static int jlit(J *j, char ch) {
    jws(j);
    if (j->p < j->end && *j->p == ch) { j->p++; return 1; }
    return 0;
}
// A string: [*s, *s + *n) are its raw bytes (escapes as written: names are compared as written).
static int jstr(J *j, const char **s, int *n) {
    jws(j);
    if (j->p >= j->end || *j->p != '"') return j->bad = 1, 0;
    const char *b = ++j->p;
    while (j->p < j->end && *j->p != '"') j->p += *j->p == '\\' && j->p + 1 < j->end ? 2 : 1;
    if (j->p >= j->end) return j->bad = 1, 0;
    *s = b, *n = (int)(j->p - b);
    j->p++;
    return 1;
}
static void jskip(J *j);
static void jskip_body(J *j, char close) {
    if (++j->depth > JSON_DEPTH) { j->bad = 1; return; }
    if (jlit(j, close)) { j->depth--; return; }
    do {
        if (close == '}') {
            const char *s; int n;
            if (!jstr(j, &s, &n) || !jlit(j, ':')) { j->bad = 1; return; }
        }
        jskip(j);
    } while (!j->bad && jlit(j, ','));
    if (!jlit(j, close)) j->bad = 1;
    j->depth--;
}
static void jskip(J *j) {
    jws(j);
    if (j->p >= j->end) { j->bad = 1; return; }
    char ch = *j->p;
    if (ch == '"') { const char *s; int n; jstr(j, &s, &n); }
    else if (ch == '{') { j->p++; jskip_body(j, '}'); }
    else if (ch == '[') { j->p++; jskip_body(j, ']'); }
    else {
        const char *b = j->p;
        while (j->p < j->end && !strchr(",}] \n\r\t", *j->p)) j->p++;
        if (j->p == b) j->bad = 1;
    }
}
// A whole number from 0 to 0xffffffff (all ours are); anything else is a format error.
static uint32_t jnum(J *j) {
    jws(j);
    uint64_t v = 0;
    const char *b = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9' && v <= 0xffffffffull) v = v * 10 + (uint64_t)(*j->p++ - '0');
    if (j->p == b || v > 0xffffffffull || (j->p < j->end && strchr(".eE-+", *j->p))) { j->bad = 1; return 0; }
    return (uint32_t)v;
}
// A float as written by putf("%.3f").
static float jfloat(J *j) {
    jws(j);
    char buf[48];
    int n = 0;
    while (j->p < j->end && n < 47 && strchr("+-0123456789.eE", *j->p)) buf[n++] = *j->p++;
    buf[n] = 0;
    char *e;
    double v = strtod(buf, &e);
    if (!n || *e) j->bad = 1;
    return (float)v;
}
// Copies a string's bytes; 0 (and the copy cut) when it doesn't fit.
static int jcopy(char *out, size_t cap, const char *s, int n) {
    int fits = n < (int)cap;
    if (!fits) n = (int)cap - 1;
    memcpy(out, s, (size_t)n);
    out[n] = 0;
    return fits;
}
// A name or id the file can hold as written: no control characters, quotes or backslashes (UTF-8 is fine), not empty.
static int plain(const char *s, int n) {
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == '"' || s[i] == '\\' || s[i] == 0x7f) return 0;
    return 1;
}
// A mod id: the rule of the mods folder's ids (runtime/romfs_mods.c): lower case letters, digits, '.', '_', '-'.
static int mod_id_ok(const char *s, int n) {
    if (n <= 0 || n > 63 || s[0] < 'a' || s[0] > 'z') return 0;
    for (int i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || strchr("._-", s[i]))) return 0;
    return 1;
}
static int jeq(const char *s, int n, const char *lit) { return (int)strlen(lit) == n && !memcmp(s, lit, (size_t)n); }
// Iterates an object's members: call with j->p just after '{'; *key/*kn is set, j->p at the value. 0 at the end.
static int jmember(J *j, int *first, const char **key, int *kn) {
    if (*first) {
        *first = 0;
        if (jlit(j, '}')) return 0;
    } else if (!jlit(j, ',')) {
        if (!jlit(j, '}')) j->bad = 1;
        return 0;
    }
    return jstr(j, key, kn) && jlit(j, ':');
}

// ---- writing JSON
typedef struct { char *b; size_t n, cap; } Out;
static void put(Out *o, const char *s, size_t n) {
    if (o->n + n + 1 > o->cap) {
        o->cap = (o->n + n + 1) * 2 + 1024;
        o->b = realloc(o->b, o->cap);
        if (!o->b) rt_fatal("mod state: out of memory");
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
    o->b[o->n] = 0;
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }
static void putf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(Out *o, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) put(o, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}
// Names and ids hold no quote, backslash or control character (plain, mod_id_ok), so they are written as they are.
static void put_str(Out *o, const char *s) {
    puts_(o, "\"");
    puts_(o, s);
    puts_(o, "\"");
}
static uint64_t fnv64(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x100000001b3ull;
    return h;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// ---- mods and what they declare
static const Mod *mod_on(const char *id) {
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].id, id)) return &mods[i];
    return NULL;
}
static int runtime_owner(const char *id) {
    for (int i = 0; i < nruntime_owners; i++)
        if (!strcmp(runtime_owners[i], id)) return 1;
    return 0;
}
// An owner whose section is read as values: an enabled mod, or a runtime owner.
static int owner_on(const char *id) { return mod_on(id) || runtime_owner(id); }
static const Decl *decl_of(uint8_t kind, uint32_t type, uint32_t id) {
    for (int i = 0; i < ndecls; i++)
        if (decls[i].kind == kind && decls[i].id == id && (kind == K_REWARD || decls[i].type == type)) return &decls[i];
    return NULL;
}

static void read_manifest(const char *folder) {
    char path[1300];
    if (snprintf(path, sizeof path, "%s/%s/mod.json", romfs_mods_dir(), folder) >= (int)sizeof path) return;
    uint64_t size = 0;
    uint8_t *text = plat_file_read(path, &size);
    if (!text) return;   // a mod without mod.json: nothing of its own in the mod state
    Mod m;
    memset(&m, 0, sizeof m);
    snprintf(m.id, sizeof m.id, "%s", folder);
    J j = {(const char *)text, (const char *)text + size, 0, 0};
    const char *k, *s; int kn, n, first = 1, before = ndecls, long_name = 0, bad_name = 0;
    if (!jlit(&j, '{')) j.bad = 1;
    while (!j.bad && jmember(&j, &first, &k, &kn)) {
        if (jeq(k, kn, "id") && jstr(&j, &s, &n)) {
            bad_name |= !mod_id_ok(s, n);
            long_name |= !jcopy(m.id, sizeof m.id, s, n);
        } else if (jeq(k, kn, "version") && jstr(&j, &s, &n)) {
            bad_name |= n && !plain(s, n);
            long_name |= !jcopy(m.version, sizeof m.version, s, n);
        }
        else if (jeq(k, kn, "regions") && jlit(&j, '[')) {
            if (!jlit(&j, ']')) {
                do {
                    uint32_t v = jnum(&j);
                    if (m.nregions < MAX_REGIONS) m.regions[m.nregions++] = v;
                    else rt_log("mods: %s/mod.json: region %u left out (%d regions at most)", folder, v, MAX_REGIONS);
                } while (!j.bad && jlit(&j, ','));
                if (!j.bad && !jlit(&j, ']')) j.bad = 1;
            }
        } else if (jeq(k, kn, "features") && jlit(&j, '[')) {
            if (jlit(&j, ']')) continue;
            do {
                if (!jstr(&j, &s, &n)) break;
                bad_name |= !mod_id_ok(s, n);
                if (m.nfeatures < MAX_FEATURES) long_name |= !jcopy(m.features[m.nfeatures++], sizeof m.features[0], s, n);
                else rt_log("mods: %s/mod.json: a feature left out (%d features at most)", folder, MAX_FEATURES);
            } while (!j.bad && jlit(&j, ','));
            if (!j.bad && !jlit(&j, ']')) j.bad = 1;
        } else if ((jeq(k, kn, "flags") || jeq(k, kn, "rewards")) && jlit(&j, '[')) {
            int reward = jeq(k, kn, "rewards");
            if (jlit(&j, ']')) continue;
            do {
                Decl d = {{0}, {0}, (uint8_t)(reward ? K_REWARD : K_FLAG), 0, 0};
                const char *k2; int kn2, f2 = 1;
                if (!jlit(&j, '{')) { j.bad = 1; break; }
                while (!j.bad && jmember(&j, &f2, &k2, &kn2)) {
                    if (jeq(k2, kn2, "name") && jstr(&j, &s, &n)) {
                        bad_name |= !plain(s, n);
                        long_name |= !jcopy(d.name, sizeof d.name, s, n);
                    }
                    else if (jeq(k2, kn2, "type")) d.type = jnum(&j);
                    else if (jeq(k2, kn2, reward ? "index" : "id")) d.id = jnum(&j);
                    else jskip(&j);
                }
                if (j.bad) break;
                int range_ok = reward ? d.id >= MOD_REWARD_FIRST && d.id <= 0xffff : MOD_IS_FLAG(d.id);
                if (d.name[0] && range_ok) {
                    snprintf(d.owner, sizeof d.owner, "%s", m.id);
                    decls = grow(decls, &capdecls, ndecls + 1, sizeof *decls);
                    decls[ndecls++] = d;
                } else {
                    rt_log("mods: %s/mod.json: %s \"%s\" (%u) left out: no name, or a number outside the mods' range",
                           folder, reward ? "reward" : "flag", d.name, d.id);
                }
            } while (!j.bad && jlit(&j, ','));
            if (!j.bad && !jlit(&j, ']')) j.bad = 1;
        } else {
            jskip(&j);
        }
    }
    free(text);
    if (j.bad || long_name || bad_name) {
        rt_log("mods: %s/mod.json: %s; the mod's saved state is kept but not used", folder,
               j.bad ? "not readable as JSON" : long_name ? "a name or id is longer than the mod state keeps"
                     : "an id that is not lower case letters, digits, '.', '_' or '-', or a name with a quote, "
                       "backslash or control character");
        ndecls = before;
        return;
    }
    // Two mods (or two entries) with one number: the first keeps it.
    for (int i = before; i < ndecls; i++)
        for (int k2 = 0; k2 < i; k2++)
            if (decls[k2].kind == decls[i].kind && decls[k2].id == decls[i].id &&
                (decls[i].kind == K_REWARD || decls[k2].type == decls[i].type)) {
                rt_log("mods: %s's %s uses number %u, already %s's %s; left out", decls[i].owner, decls[i].name,
                       decls[i].id, decls[k2].owner, decls[k2].name);
                decls[i] = decls[--ndecls];
                i--;
                break;
            }
    if (mod_on(m.id)) {
        rt_log("mods: %s/mod.json: id %s is another enabled mod's; this one's state is left out", folder, m.id);
        ndecls = before;
        return;
    }
    rt_log("mods: %s (%s, version %s): %d flag(s) and reward(s) in the mod state, %d region(s), %d feature(s)", m.id,
           folder, m.version[0] ? m.version : "-", ndecls - before, m.nregions, m.nfeatures);
    mods = grow(mods, &capmods, nmods + 1, sizeof *mods);
    mods[nmods++] = m;
}

// The enabled mods, as runtime/romfs_mods.c reads romfs-enabled.txt (the same ids, the same rules), one per line,
// then the mods' generation (bumped when one is updated in place): the key the manifests were read for.
static char *read_enabled_list(void) {
    static char ids[ROMFS_MODS_MAX][65];
    int n = romfs_mods_enabled(ids, ROMFS_MODS_MAX);
    Out o = {0};
    for (int i = 0; i < n; i++) {
        puts_(&o, ids[i]);
        puts_(&o, "\n");
    }
    putf(&o, "#%u", romfs_mods_generation());
    return o.b;
}

static void load_manifests(const char *list) {
    nmods = ndecls = 0;
    if (!list) return;
    char *copy = strdup(list), *save = NULL;
    if (!copy) rt_fatal("mod state: out of memory");
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (*line != '#') read_manifest(line);
    free(copy);
    romfs_mods_watch_scene(nmods > 0);
}

static void init_once(void) {
    if (ready) return;
    ready = 1;
    enabled_list = read_enabled_list();
    load_manifests(enabled_list);
}

// ---- stored values
// The stored value of (owner, name); NULL when none.
static Value *val_of(const char *owner, const char *name) {
    for (int i = 0; i < nvals; i++)
        if (!strcmp(vals[i].owner, owner) && !strcmp(vals[i].name, name)) return &vals[i];
    return NULL;
}
// g: the generation it was set in (the game's sets: the current one; values read from a file: 0, older than any load).
static void val_put(const char *owner, const char *name, uint32_t v, uint32_t g) {
    Value *e = val_of(owner, name);
    if (!e) {
        vals = grow(vals, &capvals, nvals + 1, sizeof *vals);
        e = &vals[nvals++];
        snprintf(e->owner, sizeof e->owner, "%s", owner);
        snprintf(e->name, sizeof e->name, "%s", name);
        e->runtime = 0;
    }
    e->value = v;
    e->gen = g;
}
static void val_set(const char *owner, const char *name, uint32_t v) { val_put(owner, name, v, gen); }

// The (owner, name) of a mod id: its declaration, or "_undeclared" with the number, so nothing a mod sets is lost.
static void name_of(uint8_t kind, uint32_t type, uint32_t id, char owner[64], char name[96]) {
    const Decl *d = decl_of(kind, type, id);
    if (d) {
        snprintf(owner, 64, "%s", d->owner);
        snprintf(name, 96, "%s", d->name);
        return;
    }
    snprintf(owner, 64, "_undeclared");
    if (kind == K_REWARD) snprintf(name, 96, "reward/%u", id);
    else snprintf(name, 96, "flag/%u/%u", type, id);
    static int logged;
    if (logged < 8) {
        logged++;
        rt_log("mods: %s is in the mods' range but no enabled mod declares it (kept under _undeclared)", name);
    }
}

static void keep_raw(const char *owner, const char *s, size_t n) {
    raws = grow(raws, &capraws, nraws + 1, sizeof *raws);
    raws[nraws].raw = malloc(n + 1);
    if (!raws[nraws].raw) rt_fatal("mod state: out of memory");
    memcpy(raws[nraws].raw, s, n);
    raws[nraws].raw[n] = 0;
    snprintf(raws[nraws].owner, sizeof raws[nraws].owner, "%s", owner);
    nraws++;
}
static void drop_raw(int i) {
    free(raws[i].raw);
    raws[i] = raws[--nraws];
}

// Drops the stored values older than generation g, and the kept sections.
static void clear_state(uint32_t g) {
    int n = 0;
    for (int i = 0; i < nvals; i++)
        if (vals[i].gen >= g) vals[n++] = vals[i];
    nvals = n;
    while (nraws) drop_raw(nraws - 1);
    have_player = 0;   // the spot outside mod land stays: it is where the player was this session, not the save's
    player_owner[0] = 0;
}

// Anything of any mod: a mod with a mod.json, a stored value or a kept section (the lock held, or the present thread).
static int active(void) { return nmods || nvals || nraws; }

// ---- the game's hooks
int mod_flag_get(Cpu *c) {
    uint32_t id = (uint32_t)c->x[1];
    if (!MOD_IS_FLAG(id) || !enabled()) return 0;
    char owner[64], name[96];
    plat_mutex_lock(&lock);
    init_once();
    name_of(K_FLAG, (uint32_t)c->x[0], id, owner, name);
    Value *v = val_of(owner, name);
    c->x[0] = v ? v->value : 0;
    plat_mutex_unlock(&lock);
    return 1;
}

int mod_flag_set(Cpu *c) {
    uint32_t id = (uint32_t)c->x[1];
    if (!MOD_IS_FLAG(id) || !enabled()) return 0;
    char owner[64], name[96];
    plat_mutex_lock(&lock);
    init_once();
    name_of(K_FLAG, (uint32_t)c->x[0], id, owner, name);
    Value *v = val_of(owner, name);
    c->x[8] = v ? v->value : 0;   // the game's function leaves the old value in x8
    val_set(owner, name, (uint32_t)c->x[2]);
    plat_mutex_unlock(&lock);
    return 1;
}

void mod_reward_begin(void) { cur_reward = 0; }

uint64_t mod_reward_test(uint64_t index, uint64_t word, uint64_t mask) {
    cur_reward = 0;
    if (index < MOD_REWARD_FIRST || index > 0xffff || !enabled()) return word;
    cur_reward = (uint32_t)index;
    char owner[64], name[96];
    plat_mutex_lock(&lock);
    init_once();
    name_of(K_REWARD, 0, cur_reward, owner, name);
    Value *v = val_of(owner, name);
    uint64_t r = v && v->value ? mask : 0;
    plat_mutex_unlock(&lock);
    return r;
}

uint64_t mod_reward_claim(uint64_t word, uint64_t old) {
    if (!cur_reward) return word;
    char owner[64], name[96];
    plat_mutex_lock(&lock);
    name_of(K_REWARD, 0, cur_reward, owner, name);
    val_set(owner, name, 1);
    plat_mutex_unlock(&lock);
    rt_log("mods: survey reward %s/%s claimed (kept in the mod state)", owner, name);
    cur_reward = 0;
    return old;   // the game's block stays as it was
}

// ---- writing a state file
static int is_reward_value(const char *owner, const char *name) {
    for (int d = 0; d < ndecls; d++)
        if (!strcmp(decls[d].owner, owner) && !strcmp(decls[d].name, name)) return decls[d].kind == K_REWARD;
    return !strcmp(owner, "_undeclared") && !strncmp(name, "reward/", 7);
}

static void write_values(Out *o, const char *owner, uint8_t kind, int indent) {
    // names in order; a value is a flag or a reward by its name's declaration, else by its form
    char **names = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < nvals; i++) {
        if (strcmp(vals[i].owner, owner) || vals[i].runtime != (kind == K_VALUE)) continue;
        if (kind != K_VALUE && is_reward_value(owner, vals[i].name) != (kind == K_REWARD)) continue;
        names = grow(names, &cap, n + 1, sizeof *names);
        names[n++] = vals[i].name;
    }
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    puts_(o, "{");
    for (int i = 0; i < n; i++) {
        putf(o, "%s\n%*s", i ? "," : "", indent + 2, "");
        put_str(o, names[i]);
        putf(o, ": %u", val_of(owner, names[i])->value);
    }
    if (n) putf(o, "\n%*s", indent, "");
    puts_(o, "}");
    free(names);
}

// One mod's section: { "flags": {...}, "rewards": {...}, "version": "..." }, at the indent of its key.
static void write_section(Out *o, const char *owner, const char *version, int indent) {
    putf(o, "{\n%*s\"flags\": ", indent + 2, "");
    write_values(o, owner, K_FLAG, indent + 2);
    putf(o, ",\n%*s\"rewards\": ", indent + 2, "");
    write_values(o, owner, K_REWARD, indent + 2);
    int any = 0;   // "values" only when there are some: a mod without them gets the file it always had
    for (int i = 0; i < nvals && !any; i++) any = vals[i].runtime && !strcmp(vals[i].owner, owner);
    if (any) {
        putf(o, ",\n%*s\"values\": ", indent + 2, "");
        write_values(o, owner, K_VALUE, indent + 2);
    }
    putf(o, ",\n%*s\"version\": ", indent + 2, "");
    put_str(o, version);
    putf(o, "\n%*s}", indent, "");
}

// The owners with something to write, sorted: enabled mods, stored values' owners, kept sections.
static int owners(char ***out) {
    char **own = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < nmods + nvals + nraws; i++) {
        const char *o = i < nmods ? mods[i].id : i < nmods + nvals ? vals[i - nmods].owner : raws[i - nmods - nvals].owner;
        int seen = !*o;
        for (int k = 0; k < n && !seen; k++) seen = !strcmp(own[k], o);
        if (seen) continue;
        own = grow(own, &cap, n + 1, sizeof *own);
        own[n++] = (char *)o;
    }
    qsort(own, (size_t)n, sizeof *own, cmp_str);
    *out = own;
    return n;
}

static char *state_text(uint32_t save_time, const char *slot) {
    Out o = {0};
    putf(&o, "{\n  \"format\": %d,\n  \"mods\": {", STATE_FORMAT);
    char **own;
    int n = owners(&own);
    for (int i = 0; i < n; i++) {
        const Raw *kept = NULL;
        for (int r = 0; r < nraws; r++)
            if (!strcmp(raws[r].owner, own[i])) kept = &raws[r];
        puts_(&o, i ? ",\n    " : "\n    ");
        put_str(&o, own[i]);
        puts_(&o, ": ");
        const Mod *m = mod_on(own[i]);
        if (kept && !owner_on(own[i])) puts_(&o, kept->raw);   // a mod that is off: its section as it was
        else write_section(&o, own[i], m ? m->version : "", 4);
    }
    free(own);
    puts_(&o, n ? "\n  },\n" : "},\n");
    puts_(&o, "  \"player\": {");
    if (have_player) {
        putf(&o, "\"region\": %u, \"owner\": ", player_region);
        put_str(&o, player_owner);
        if (have_outside)
            putf(&o, ", \"last_outside\": [%.3f, %.3f, %.3f]", last_outside[0], last_outside[1], last_outside[2]);
    }
    puts_(&o, "},\n");
    putf(&o, "  \"save_time\": %u,\n  \"slot\": ", save_time);
    put_str(&o, slot);
    puts_(&o, ",\n");
    for (int r = 0; r < nraws; r++)
        if (!raws[r].owner[0]) {   // a top-level part this runtime doesn't know, kept
            puts_(&o, "  ");
            puts_(&o, raws[r].raw);
            puts_(&o, ",\n");
        }
    puts_(&o, "  \"written_by\": ");
    put_str(&o, "xcx-recomp mod state");
    puts_(&o, ",\n");
    putf(&o, "  \"checksum\": \"%016llx\"\n}\n", (unsigned long long)fnv64(o.b, o.n));
    return o.b;
}

static int is_mod_region(uint32_t region, char owner[64]) {
    for (int i = 0; i < nmods; i++)
        for (int k = 0; k < mods[i].nregions; k++)
            if (mods[i].regions[k] == region) {
                if (owner) snprintf(owner, 64, "%s", mods[i].id);
                return 1;
            }
    return 0;
}

static uint32_t current_region(void) {
    uint64_t p = LD64(REGION_PTR);
    return p ? LD32(p + 0xb0) : 0;
}

int mod_state_slot_file(const char *name) {
    return !strncmp(name, "gamedata", 8) && strlen(name) < sizeof written[0] && !strchr(name, '.') &&
           !strchr(name, '/') && !strchr(name, '\\');
}

void mod_state_slot_written(const char *name) {
    if (!mod_state_slot_file(name)) return;
    plat_mutex_lock(&lock);
    int seen = 0;
    for (int i = 0; i < nwritten && !seen; i++) seen = !strcmp(written[i], name);
    if (!seen && nwritten < (int)(sizeof written / sizeof *written)) snprintf(written[nwritten++], sizeof written[0], "%s", name);
    plat_mutex_unlock(&lock);
}

void mod_state_before_commit(const char *work_root) {
    if (!enabled()) return;
    uint64_t gd = LD64(GAMEDATA_PTR);
    plat_mutex_lock(&lock);
    init_once();
    if (!gd || !nwritten || (!nmods && !nvals && !nraws)) {   // no slot written, or nothing of any mod: no file
        nwritten = 0;
        plat_mutex_unlock(&lock);
        return;
    }
    uint32_t t = LD32(gd + GD_SAVE_TIME);
    float pos[3];
    for (int i = 0; i < 3; i++) {
        uint32_t bits = LD32(gd + GD_POSITION + 4 * (uint32_t)i);
        memcpy(&pos[i], &bits, 4);
    }
    player_region = current_region();
    player_owner[0] = 0;
    int pos_ok = pos[0] == pos[0] && pos[1] == pos[1] && pos[2] == pos[2];
    if (!is_mod_region(player_region, player_owner) && pos_ok) {   // (else: the last spot sampled outside)
        memcpy(last_outside, pos, sizeof pos);
        have_outside = 1;
    }
    have_player = 1;
    stamp = t;
    held = 1;
    differ_since = 0;
    // The texts under the lock, the files after it. Written plainly (no flush): this is the working copy, which the
    // Commit then copies and flushes itself (fs_host.c), and a flush here would hold up the game's save.
    char slots[8][MOD_STATE_SLOT_MAX], *texts[8];
    int n = nwritten, on_land = player_owner[0] != 0;
    for (int i = 0; i < n; i++) {
        memcpy(slots[i], written[i], sizeof slots[i]);
        texts[i] = state_text(t, written[i]);
    }
    nwritten = 0;
    plat_mutex_unlock(&lock);
    char dir[1200], path[1300];
    snprintf(dir, sizeof dir, "%s/mods", work_root);
    plat_file_mkdir_p(dir);
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/state-%s.json", dir, slots[i]);
        int ok = plat_file_write(path, texts[i], strlen(texts[i]));
        free(texts[i]);
        rt_log("mods: state of %s (save of %u) %s%s", slots[i], t, ok ? "written" : "NOT written (write failed)",
               on_land ? "; the player is on a mod's land" : "");
    }
}

// ---- reading a state file
static void read_values(J *j, const char *owner, int runtime) {
    const char *k; int kn, first = 1;
    if (!jlit(j, '{')) { j->bad = 1; return; }
    while (!j->bad && jmember(j, &first, &k, &kn)) {
        char name[96];
        if (!jcopy(name, sizeof name, k, kn)) { j->bad = 1; return; }
        uint32_t v = jnum(j);
        if (!j->bad && !val_of(owner, name)) {   // what the game set since the load wins
            val_put(owner, name, v, 0);
            val_of(owner, name)->runtime = (uint8_t)runtime;
        }
    }
}

// A mod's section { "flags": ..., "rewards": ..., ... } into stored values.
static void read_section(J *j, const char *owner) {
    const char *fk; int fkn, first = 1;
    if (!jlit(j, '{')) { j->bad = 1; return; }
    while (!j->bad && jmember(j, &first, &fk, &fkn)) {
        if (jeq(fk, fkn, "flags") || jeq(fk, fkn, "rewards")) read_values(j, owner, 0);
        else if (jeq(fk, fkn, "values")) read_values(j, owner, 1);
        else jskip(j);   // version, and parts a newer runtime added: rebuilt per mod
    }
}

// The file's text when its checksum checks, else NULL; *time = its save_time (0 when absent).
static char *checked_text(const char *path, uint64_t *size, uint32_t *time) {
    char *text = (char *)plat_file_read(path, size);
    if (!text) return NULL;
    char *t = realloc(text, *size + 1);   // NUL-terminated for sscanf (before any pointer into it is taken)
    if (!t) rt_fatal("mod state: out of memory");
    text = t;
    text[*size] = 0;
    const char *ck = NULL;
    for (const char *p = text; p + 11 <= text + *size; p++)
        if (!memcmp(p, "\"checksum\":", 11)) ck = p;
    unsigned long long want = 0;
    const char *fmt = strstr(text, "\n  \"format\": ");
    if (!ck || sscanf(ck, "\"checksum\": \"%16llx\"", &want) != 1 || ck < text + 2 ||
        fnv64(text, (size_t)(ck - 2 - text)) != want || !fmt || strtoul(fmt + 13, NULL, 10) > STATE_FORMAT) {
        // Damaged (a hand edit voids the checksum) or written by a newer runtime: set aside, never overwritten.
        char bad[1400];
        snprintf(bad, sizeof bad, "%s.bad", path);
        int kept = plat_file_replace(path, bad);
        rt_log("mods: %s: %s; not used%s", path, fmt && strtoul(fmt + 13, NULL, 10) > STATE_FORMAT
               ? "written by a newer version of the mod state" : "the checksum does not match",
               kept ? " (kept as .bad)" : "");
        free(text);
        return NULL;
    }
    *time = 0;
    const char *st = strstr(text, "\n  \"save_time\": ");
    if (st) *time = (uint32_t)strtoul(st + 16, NULL, 10);
    return text;
}

// Replaces the state with the file's (values the game set since generation g stay). 1 when it was read.
static int read_state(const char *path, uint32_t g) {
    uint64_t size = 0;
    uint32_t t;
    char *text = checked_text(path, &size, &t);
    if (!text) return 0;
    clear_state(g);
    J j = {text, text + size, 0, 0};   // the checksum covers what comes before its own line
    const char *k; int kn, first = 1;
    if (!jlit(&j, '{')) j.bad = 1;
    while (!j.bad && jmember(&j, &first, &k, &kn)) {
        const char *start = k - 1;
        if (jeq(k, kn, "mods")) {
            const char *mk; int mkn, mfirst = 1;
            if (!jlit(&j, '{')) { j.bad = 1; break; }
            while (!j.bad && jmember(&j, &mfirst, &mk, &mkn)) {
                char owner[64];
                if (!jcopy(owner, sizeof owner, mk, mkn)) { j.bad = 1; break; }
                jws(&j);
                const char *body = j.p;
                if (!owner_on(owner) && strcmp(owner, "_undeclared")) {   // off: kept as it is
                    jskip(&j);
                    if (!j.bad) keep_raw(owner, body, (size_t)(j.p - body));
                    continue;
                }
                read_section(&j, owner);
            }
        } else if (jeq(k, kn, "player")) {
            const char *pk, *s; int pkn, n, pfirst = 1;
            if (!jlit(&j, '{')) { j.bad = 1; break; }
            while (!j.bad && jmember(&j, &pfirst, &pk, &pkn)) {
                if (jeq(pk, pkn, "region")) player_region = jnum(&j), have_player = 1;
                else if (jeq(pk, pkn, "owner") && jstr(&j, &s, &n)) jcopy(player_owner, sizeof player_owner, s, n);
                else if (jeq(pk, pkn, "last_outside") && jlit(&j, '[')) {
                    for (int i = 0; i < 3 && !j.bad; i++) {
                        if (i && !jlit(&j, ',')) j.bad = 1;
                        last_outside[i] = jfloat(&j);
                    }
                    if (!jlit(&j, ']')) j.bad = 1;
                    have_outside = !j.bad;
                } else jskip(&j);
            }
        } else if (jeq(k, kn, "format") || jeq(k, kn, "save_time") || jeq(k, kn, "slot") ||
                   jeq(k, kn, "written_by") || jeq(k, kn, "checksum")) {
            jskip(&j);
        } else {
            jskip(&j);
            if (!j.bad) keep_raw("", start, (size_t)(j.p - start));
        }
    }
    free(text);
    if (j.bad) {
        rt_log("mods: %s: not readable; not used", path);
        clear_state(g);
        return 0;
    }
    return 1;
}

// A slot the game read: its state file is applied once the game data holds the file's save time (the load has
// filled it). Reading a slot without loading it (or a file from an older save in that slot) never matches, and the
// candidate is dropped after LOAD_WAIT_MS.
static void apply_pending(uint64_t gd) {
    uint32_t now = LD32(gd + GD_SAVE_TIME);
    char path[1300];
    snprintf(path, sizeof path, "%s/mods/state-%s.json", save_dir, pending);
    if (!pending_known) {   // the file's save time, read once per load
        pending_known = 1;
        pending_want = 0;
        uint64_t size;
        char *text = save_dir[0] && plat_file_exists(path) ? checked_text(path, &size, &pending_want) : NULL;
        if (!text) pending_want = 0;
        free(text);
    }
    if (!pending_want) {   // no usable file: nothing to wait for (a save without one starts empty, below)
        pending[0] = 0;
        return;
    }
    if (now != pending_want) {
        if (plat_now_ms() >= load_until) pending[0] = 0;
        return;
    }
    if (held && now == stamp && !(pending_title && romfs_mods_world_loads() != applied_world)) {
        // The save the game already holds: read in the world (its save screen lists the slots by reading them whole),
        // or read again while the same load is still at the title. The state in memory is the newer; nothing to load.
        pending[0] = 0;
        return;
    }
    held = 1;
    applied_world = romfs_mods_world_loads();
    int ok = read_state(path, load_gen);
    if (!ok) clear_state(load_gen);
    loads++;
    stamp = now;
    differ_since = 0;
    int nmod = 0;
    for (int i = 0; i < nvals; i++) nmod += strcmp(vals[i].owner, "_undeclared") != 0;
    rt_log("mods: %s (save of %u) loaded: %s (%d value(s), %d kept part(s))", pending, now,
           ok ? "its mod state read" : "its mod state file is unreadable; none used", nmod, nraws);
    // On the land of a mod that is off now: to the last position outside every mod region.
    if (ok && have_player && player_owner[0] && !mod_on(player_owner)) {
        if (have_outside) {
            for (int p = 0; p < 4; p++)
                for (int i = 0; i < 3; i++) {
                    uint32_t bits;
                    memcpy(&bits, &last_outside[i], 4);
                    ST32(gd + GD_POSITION + 16 * (uint32_t)p + 4 * (uint32_t)i, bits);
                }
            rt_log("mods: the save was made on %s's land (region %u) and that mod is off: the party goes to the last "
                   "spot outside mod land, %.1f %.1f %.1f", player_owner, player_region, last_outside[0],
                   last_outside[1], last_outside[2]);
        } else {
            rt_log("mods: the save was made on %s's land (region %u), that mod is off, and no spot outside mod land "
                   "is known", player_owner, player_region);
        }
    }
    pending[0] = 0;
}

// The enabled mods changed while the game runs (list = romfs-enabled.txt now): a mod switched off keeps its section
// as it was, one switched on gets its kept section back as values.
static void follow_enabled_list(char *list) {
    if ((!list && !enabled_list) || (list && enabled_list && !strcmp(list, enabled_list))) {
        free(list);
        return;
    }
    Out o = {0};
    for (int i = 0; i < nmods; i++) {   // the mods on until now: each one's section, in case it went off
        o.n = 0;
        write_section(&o, mods[i].id, mods[i].version, 4);
        keep_raw(mods[i].id, o.b, o.n);
    }
    free(o.b);
    free(enabled_list);
    enabled_list = list;
    load_manifests(list);
    // One kept section per owner, the newest (the sections just written come last).
    for (int i = 0; i < nraws; i++)
        for (int k = i + 1; k < nraws; k++)
            if (raws[i].owner[0] && !strcmp(raws[k].owner, raws[i].owner)) {
                free(raws[i].raw);
                memmove(&raws[i], &raws[i + 1], (size_t)(nraws - i - 1) * sizeof *raws);
                nraws--;
                i--;
                break;
            }
    for (int i = 0; i < nraws; i++) {
        if (!raws[i].owner[0]) continue;
        if (!owner_on(raws[i].owner)) {   // off: its values now live in the kept section only
            int n = 0;
            for (int v = 0; v < nvals; v++)
                if (strcmp(vals[v].owner, raws[i].owner)) vals[n++] = vals[v];
            nvals = n;
            continue;
        }
        char owner[64];   // on: the kept section back to values
        snprintf(owner, sizeof owner, "%s", raws[i].owner);
        J j = {raws[i].raw, raws[i].raw + strlen(raws[i].raw), 0, 0};
        read_section(&j, owner);
        free(raws[i].raw);
        memmove(&raws[i], &raws[i + 1], (size_t)(nraws - i - 1) * sizeof *raws);
        nraws--;
        i--;
    }
    loads++;   // a feature may have come or gone with its values
    rt_log("mods: the enabled mods changed: %d with a mod.json", nmods);
}

void mod_state_set_save_dir(const char *root) {
    plat_mutex_lock(&lock);
    snprintf(save_dir, sizeof save_dir, "%s", root);
    plat_mutex_unlock(&lock);
}

void mod_state_save_read(const char *name) {
    if (!mod_state_slot_file(name) || !enabled()) return;
    plat_mutex_lock(&lock);
    snprintf(pending, sizeof pending, "%s", name);
    pending_title = romfs_mods_at_title();
    pending_known = 0;
    load_until = plat_now_ms() + LOAD_WAIT_MS;
    load_gen = ++gen;   // values the game sets from now on belong to the save it loads
    int log = active();
    plat_mutex_unlock(&lock);
    if (log) rt_log("mods: %s read to its end (its mod state applies when the game holds that save's time)", name);
}

void mod_state_update(void) {
    if (!enabled()) return;
    uint64_t now_ms = plat_now_ms();
    int sample = now_ms >= next_sample;
    if (sample) next_sample = now_ms + SAMPLE_MS;
    uint32_t g = romfs_mods_generation();   // the enabled mods changed (romfs_mods.c read the list): read them again
    char *list = sample && g != list_gen ? read_enabled_list() : NULL;   // outside the lock the flag calls wait on
    if (list) list_gen = g;
    plat_mutex_lock(&lock);
    init_once();
    uint64_t gd = LD64(GAMEDATA_PTR);
    if (list) follow_enabled_list(list);
    if (sample && active()) {
        // Twice a second in the world: the leader's position while outside every mod region, the spot to go back to.
        // A sample counts once the next one is outside too: right after a warp or a fast travel the position is the
        // new one while the current region is still the old one.
        int outside = 0;
        float pos[3];
        if (gd && nmods && !is_mod_region(current_region(), NULL)) {
            for (int i = 0; i < 3; i++) {
                uint32_t bits = LD32(LEADER_POS + 4 * (uint64_t)i);
                memcpy(&pos[i], &bits, 4);
            }
            outside = pos[0] == pos[0] && pos[1] == pos[1] && pos[2] == pos[2] && (pos[0] != 0 || pos[2] != 0);
        }
        if (outside && sample_outside) {
            memcpy(last_outside, sample_pos, sizeof sample_pos);
            have_outside = 1;
        }
        sample_outside = outside;
        if (outside) memcpy(sample_pos, pos, sizeof pos);
    }
    if (gd && pending[0]) apply_pending(gd);
    if (gd) {
        // A save time that differs with no save behind it: the game holds a save this state isn't for (one loaded
        // without a mod state file, or a New Game). After NEW_GAME_MS of that, without a save being
        // written, the state starts empty; what the game set since the load began (or since the difference began)
        // stays. A save the game is writing ends the difference at its Commit.
        uint32_t now = LD32(gd + GD_SAVE_TIME);
        // (a load at the title whose file waits for the game data to hold its save time holds this off: a slow load
        // must not clear the state the file then brings)
        if (now == stamp || fs_save_busy() || nwritten || (pending[0] && pending_title)) {
            differ_since = 0;
        } else if (!differ_since) {
            differ_since = now_ms ? now_ms : 1;
            differ_gen = now_ms < load_until ? load_gen : ++gen;
        } else if (now_ms - differ_since >= NEW_GAME_MS) {
            int had = active();
            clear_state(differ_gen);
            loads++;
            if (had) rt_log("mods: the game holds another save (time %u, the state was for %u): the mod state starts empty", now,
                   stamp);
            stamp = now;
            held = 1;
            differ_since = 0;
        }
    }
    plat_mutex_unlock(&lock);
}

// ---- runtime values and features (mod_state.h)
int mod_state_on(void) { return enabled(); }

void mod_state_owner(const char *id) {
    if (!id || !mod_id_ok(id, (int)strlen(id))) return;   // (may run before the settings are read)
    plat_mutex_lock(&lock);
    if (!runtime_owner(id) && nruntime_owners < MAX_OWNERS)
        snprintf(runtime_owners[nruntime_owners++], sizeof runtime_owners[0], "%s", id);
    plat_mutex_unlock(&lock);
}

int mod_state_feature(const char *feature, char *owner, size_t cap) {
    if (!enabled() || !feature) return 0;
    int found = 0;
    plat_mutex_lock(&lock);
    init_once();
    for (int i = 0; i < nmods && !found; i++)   // mods in priority order: the top one that names it
        for (int k = 0; k < mods[i].nfeatures && !found; k++)
            if (!strcmp(mods[i].features[k], feature)) {
                found = 1;
                if (owner && cap) snprintf(owner, cap, "%s", mods[i].id);
            }
    plat_mutex_unlock(&lock);
    return found;
}

int mod_state_value(const char *owner, const char *name, uint32_t *v) {
    if (!enabled()) return 0;
    plat_mutex_lock(&lock);
    const Value *e = val_of(owner, name);
    int ok = e && e->runtime;
    if (ok && v) *v = e->value;
    plat_mutex_unlock(&lock);
    return ok;
}

void mod_state_set_value(const char *owner, const char *name, uint32_t v) {
    if (!enabled() || !plain(name, (int)strlen(name)) || strlen(name) >= 96) return;
    plat_mutex_lock(&lock);
    init_once();
    if (owner_on(owner)) {
        Value *e = val_of(owner, name);
        if (!e || e->value != v || !e->runtime) {
            val_set(owner, name, v);
            val_of(owner, name)->runtime = 1;
        }
    }
    plat_mutex_unlock(&lock);
}

uint32_t mod_state_loads(void) {
    plat_mutex_lock(&lock);
    uint32_t n = loads;
    plat_mutex_unlock(&lock);
    return n;
}

int mod_state_current(void) {
    if (!enabled()) return 0;
    uint64_t gd = LD64(GAMEDATA_PTR);
    plat_mutex_lock(&lock);
    int cur = gd && held && LD32(gd + GD_SAVE_TIME) == stamp && !(pending[0] && pending_title) && !differ_since;
    plat_mutex_unlock(&lock);
    return cur;
}
