// runtime/mod_state.c on its own, for tests/mod_state_test.py: the game's side (its flag test and set, the survey
// reward grant, the save's Commit and loads, the game data and the party's position) is played by a script on stdin,
// one command per line, and every answer is printed. The mods folder is XCX_MOD_DIR; the save folders are argv[1]
// (the committed save, a copy the test reads its files from) and argv[2] (the working copy, where the game reads its
// save and a Commit writes).
//
//   get <type> <id>          the flag test: "get <handled> <value>"
//   set <type> <id> <value>  the flag set: "set <handled> <old value>"
//   reward <index> <word>    the grant's claimed-mark test (mask 4): "reward <word>", then "claim <word>" from its claim
//   written <slot>           the game opened a slot's data file for writing
//   commit                   the save's Commit (the working copy's state files are then copied to the committed save)
//   read <slot>              the game read a slot's data file to its end
//   time <t>                 the game data's save time
//   title                    the title screen's files load (at boot and at each return to the title)
//   world                    the world's files load (after a load or a new game)
//   nogame                   the game data pointer is null (before the first load)
//   region <r>               the current region
//   leader <x> <y> <z>       the party leader's position
//   busy <0|1>               a save file is open for writing
//   present <n>              n presents, each 1/fps seconds on the clock mod_state.c reads
//   fps <n>                  the presents' rate (60 at the start)
//   sync                     prints "sync" (the test waits for it before it changes the mods folder)
//   pos                      the party's positions in the game data: "pos x y z" (the first; all four must agree)
//   owner <id>               a runtime owner (mod_state_owner)
//   vset <owner> <name> <v>  a runtime value set (mod_state_set_value)
//   vget <owner> <name>      a runtime value: "vget <found> <value>"
//   feature <name>           "feature <found> <owner>" (mod_state_feature)
//   loads                    "loads <mod_state_loads()> <mod_state_current()>"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#define usable_size _msize
#else
#include <malloc.h>
#define usable_size malloc_usable_size
#endif

// mod_state.c's realloc always moves the block and poisons the old one, so a pointer kept across a realloc fails here
// every time instead of only when the allocator happens to move the block.
static void *moving_realloc(void *p, size_t n) {
    void *q = malloc(n ? n : 1);
    if (!q) return NULL;
    if (p) {
        size_t old = usable_size(p);
        memcpy(q, p, old < n ? old : n);
        memset(p, 0xAA, old);
        free(p);
    }
    return q;
}
#define realloc moving_realloc
// The clock: mod_state.c's waits are in time, and the presents here move a clock of their own.
#define plat_now_ns test_now_ns
static uint64_t fake_ns = 1000000000ull, step_ns = 16666667ull;
uint64_t test_now_ns(void) { return fake_ns; }
#include "../runtime/mod_state.c"
#include "../runtime/mod_ids.c"
#undef realloc
#undef plat_now_ns

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int busy;
static char committed[1100], working[1100];

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("log ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("fatal ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(3);
}
void rt_mods_shutdown(void) {}   // the platform layer's exit path
const char *cfg_get(const char *name) { return getenv(name); }
int fs_save_busy(void) { return busy; }
const char *romfs_mods_dir(void) { return getenv("XCX_MOD_DIR"); }
// runtime/mod_ids.c's profile (its records file) and bdat_merge.c's name hash
const RtConfig *rt_config(void) {
    static RtConfig c;
    c.profile = getenv("XCX_PROFILE_DIR");
    return &c;
}
uint32_t bdat_merge_name_hash(const char *s, size_t n) {
    uint32_t h = 0, k;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        memcpy(&k, s + i, 4);
        k *= 0xcc9e2d51u;
        k = (k << 15 | k >> 17) * 0x1b873593u;
        h ^= k;
        h = (h << 13 | h >> 19) * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (n & 3) {
    case 3: k ^= (uint32_t)(uint8_t)s[i + 2] << 16; /* fall through */
    case 2: k ^= (uint32_t)(uint8_t)s[i + 1] << 8; /* fall through */
    case 1:
        k ^= (uint8_t)s[i];
        k *= 0xcc9e2d51u;
        k = (k << 15 | k >> 17) * 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}
// romfs_mods.c's list of enabled mods, read the way it reads it (one id per line), and its generation: here a hash of
// the list file, so a changed file is a new generation as a rebuild would make it.
static char *list_text(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", getenv("XCX_MOD_DIR"));
    uint64_t n;
    return (char *)plat_file_read(path, &n);
}
int romfs_mods_enabled(char (*ids)[65], int cap) {
    char *t = list_text(), *save = NULL;
    int n = 0;
    for (char *l = t ? strtok_r(t, "\r\n", &save) : NULL; l && n < cap; l = strtok_r(NULL, "\r\n", &save))
        if (*l && *l != '#' && strlen(l) < 65) snprintf(ids[n++], 65, "%s", l);
    free(t);
    return n;
}
uint32_t romfs_mods_generation(void) {
    char *t = list_text();
    uint32_t h = 2166136261u;
    for (const char *p = t ? t : ""; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    free(t);
    return h;
}
void romfs_mods_watch_scene(int on) { (void)on; }
static int at_title = 1;
static uint32_t world_loads;
int romfs_mods_at_title(void) { return at_title; }
uint32_t romfs_mods_world_loads(void) { return world_loads; }

// The guest addresses mod_state.c reads: the game data pointer, the region pointer and the leader's position, in one
// mapping at their real addresses; the game data and the region object are host memory they point to.
#define LOW (GAMEDATA_PTR & ~0xffffull)
#define HIGH ((LEADER_POS + 0x10000) & ~0xffffull)
static uint8_t gamedata[0x96000], region_obj[0x100];

static void map_guest(void) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)LOW, HIGH - LOW, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)LOW, HIGH - LOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (p != (void *)(uintptr_t)LOW) {
        printf("fatal cannot map the guest addresses\n");
        exit(2);
    }
    *(uint64_t *)(uintptr_t)GAMEDATA_PTR = (uint64_t)(uintptr_t)gamedata;
    *(uint64_t *)(uintptr_t)REGION_PTR = (uint64_t)(uintptr_t)region_obj;
}

static void copy_states(void) {   // the Commit's copy of the working copy's mods folder into the committed save
    char from[1300], to[1300];
    const char *slots[] = {"gamedata01", "gamedata02", "gamedata03", "gamedataa"};
    for (int i = 0; i < 4; i++) {
        snprintf(from, sizeof from, "%s/mods/state-%s.json", working, slots[i]);
        uint64_t n;
        uint8_t *d = plat_file_read(from, &n);
        if (!d) continue;
        snprintf(to, sizeof to, "%s/mods", committed);
        plat_file_mkdir_p(to);
        snprintf(to, sizeof to, "%s/mods/state-%s.json", committed, slots[i]);
        plat_file_write_atomic(to, d, n);
        free(d);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(committed, sizeof committed, "%s", argv[1]);
    snprintf(working, sizeof working, "%s", argv[2]);
    map_guest();
    mod_state_set_save_dir(working);   // the game reads its save from the working copy
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = "", a[128] = "", b[128] = "";
        unsigned long u1 = 0, u2 = 0, u3 = 0;
        float f1, f2, f3;
        Cpu c;
        memset(&c, 0, sizeof c);
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "get") && sscanf(line, "%*s %lu %lu", &u1, &u2) == 2) {
            c.x[0] = u1, c.x[1] = u2;
            int h = mod_flag_get(&c);
            printf("get %d %llu\n", h, (unsigned long long)c.x[0]);
        } else if (!strcmp(cmd, "set") && sscanf(line, "%*s %lu %lu %lu", &u1, &u2, &u3) == 3) {
            c.x[0] = u1, c.x[1] = u2, c.x[2] = u3;
            int h = mod_flag_set(&c);
            printf("set %d %llu\n", h, (unsigned long long)c.x[8]);
        } else if (!strcmp(cmd, "reward") && sscanf(line, "%*s %lu %lu", &u1, &u2) == 2) {
            uint64_t w = mod_reward_test(u1, u2, 4);
            printf("reward %llu\n", (unsigned long long)w);
            printf("claim %llu\n", (unsigned long long)mod_reward_claim(u2 | 4, u2));
        } else if (!strcmp(cmd, "written") && sscanf(line, "%*s %127s", a) == 1) {
            mod_state_slot_written(a);
        } else if (!strcmp(cmd, "commit")) {
            mod_state_before_commit(working);
            copy_states();
        } else if (!strcmp(cmd, "read") && sscanf(line, "%*s %127s", a) == 1) {
            mod_state_save_read(a);
        } else if (!strcmp(cmd, "time") && sscanf(line, "%*s %lu", &u1) == 1) {
            *(uint64_t *)(uintptr_t)GAMEDATA_PTR = (uint64_t)(uintptr_t)gamedata;
            memcpy(gamedata + GD_SAVE_TIME, &(uint32_t){(uint32_t)u1}, 4);
        } else if (!strcmp(cmd, "title")) {
            at_title = 1;
        } else if (!strcmp(cmd, "world")) {
            if (at_title) world_loads++;
            at_title = 0;
        } else if (!strcmp(cmd, "nogame")) {
            *(uint64_t *)(uintptr_t)GAMEDATA_PTR = 0;
        } else if (!strcmp(cmd, "region") && sscanf(line, "%*s %lu", &u1) == 1) {
            memcpy(region_obj + 0xb0, &(uint32_t){(uint32_t)u1}, 4);
        } else if (!strcmp(cmd, "leader") && sscanf(line, "%*s %f %f %f", &f1, &f2, &f3) == 3) {
            float v[3] = {f1, f2, f3};
            memcpy((void *)(uintptr_t)LEADER_POS, v, sizeof v);
            for (int p = 0; p < 4; p++) memcpy(gamedata + GD_POSITION + 16 * p, v, sizeof v);
        } else if (!strcmp(cmd, "busy") && sscanf(line, "%*s %lu", &u1) == 1) {
            busy = (int)u1;
        } else if (!strcmp(cmd, "present") && sscanf(line, "%*s %lu", &u1) == 1) {
            for (unsigned long i = 0; i < u1; i++) {
                fake_ns += step_ns;
                mod_state_update();
            }
        } else if (!strcmp(cmd, "fps") && sscanf(line, "%*s %lu", &u1) == 1 && u1) {
            step_ns = 1000000000ull / u1;
        } else if (!strcmp(cmd, "sync")) {
            printf("sync\n");
        } else if (!strcmp(cmd, "owner") && sscanf(line, "%*s %127s", a) == 1) {
            mod_state_owner(a);
        } else if (!strcmp(cmd, "vset") && sscanf(line, "%*s %127s %127s %lu", a, b, &u1) == 3) {
            mod_state_set_value(a, b, (uint32_t)u1);
        } else if (!strcmp(cmd, "vget") && sscanf(line, "%*s %127s %127s", a, b) == 2) {
            uint32_t v = 0;
            int ok = mod_state_value(a, b, &v);
            printf("vget %d %u\n", ok, v);
        } else if (!strcmp(cmd, "feature") && sscanf(line, "%*s %127s", a) == 1) {
            char o[64] = "-";
            int ok = mod_state_feature(a, o, sizeof o);
            printf("feature %d %s\n", ok, o);
        } else if (!strcmp(cmd, "loads")) {
            printf("loads %u %d\n", mod_state_loads(), mod_state_current());
        } else if (!strcmp(cmd, "pos")) {
            float v[4][3];
            for (int p = 0; p < 4; p++) memcpy(v[p], gamedata + GD_POSITION + 16 * p, sizeof v[p]);
            int same = 1;
            for (int p = 1; p < 4; p++) same &= !memcmp(v[p], v[0], sizeof v[0]);
            printf("pos %.1f %.1f %.1f%s\n", v[0][0], v[0][1], v[0][2], same ? "" : " (party apart)");
        } else {
            printf("fatal unknown command: %s", line);
            return 2;
        }
    }
    return 0;
}
