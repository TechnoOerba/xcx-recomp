// runtime/map_overlay.c on its own, for tests/map_overlay_test.py: the game's archive is two host files (GAME/sts.arh
// and GAME/sts.ard, made by the test), the mods folder is MODS (its romfs-enabled.txt gives the order). Opens the four
// map files the way the game does (rom:/map/...), reads each through in uneven reads that cross the game's end and the
// pieces' edges, and writes what it read to OUT/<file>, then the overlay's state and Mods tab lines to stdout:
//   state <n>
//   line <text>          (line! for a line about something left out)
// The test compares the files with tools/map_pieces.py's combine.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/map_overlay.c"

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "log: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(3);
}
void rt_mods_shutdown(void) {}
void mem_thaw(uint64_t a, uint64_t n) { (void)a; (void)n; }
const char *cfg_get(const char *name) { (void)name; return NULL; }
const RtLoaded *ldr_module(int i) { (void)i; return NULL; }
int ldr_count(void) { return 0; }
const char *ldr_describe(uint64_t a, char *buf, int n) { (void)a; (void)n; return buf; }
Cpu *kern_cpu(void) { return NULL; }

// The game's headers in the test archive are stored unpacked; nothing here is zstd.
size_t ZSTD_decompress(void *dst, size_t cap, const void *src, size_t n) { (void)dst; (void)cap; (void)src; (void)n; return (size_t)-1; }
unsigned ZSTD_isError(size_t code) { return code == (size_t)-1; }

// ---- the RomFS: rom:/sts.arh and rom:/sts.ard from GAME
static PlatFile *rom_files[8];
static int64_t rom_sizes[8];
static int rom_opens;   // RomFS handles open now
const char *romfs_mount_name(void) { return "rom"; }
uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    (void)mode;
    const char *name = strrchr(path, '/');
    char host[2048];
    snprintf(host, sizeof host, "%s/%s", getenv("GAME"), name ? name + 1 : path);
    for (int j = 0; j < 8; j++)
        if (!rom_files[j]) {
            if (!(rom_files[j] = plat_file_open(host))) return FS_R_PATH_NOT_FOUND;
            rom_sizes[j] = plat_file_size(host);
            *out = ROMFS_HANDLE_BASE + 1 + (uint64_t)j;
            rom_opens++;
            return 0;
        }
    return FS_R_INVALID;
}
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    int j = (int)(h - ROMFS_HANDLE_BASE - 1);
    if (offset < 0 || offset > rom_sizes[j]) return FS_R_INVALID;
    if (size > (uint64_t)(rom_sizes[j] - offset)) size = (uint64_t)(rom_sizes[j] - offset);
    int64_t got = size ? plat_file_pread(rom_files[j], buffer, (uint32_t)size, (uint64_t)offset) : 0;
    if (got < 0) return FS_R_INVALID;
    *out = (uint64_t)got;
    return 0;
}
uint32_t romfs_size(int64_t *out, uint64_t h) {
    *out = rom_sizes[h - ROMFS_HANDLE_BASE - 1];
    return 0;
}
void romfs_close(uint64_t h) {
    int j = (int)(h - ROMFS_HANDLE_BASE - 1);
    plat_file_close(rom_files[j]);
    rom_files[j] = NULL;
    rom_opens--;
}

// ---- the mods folder
const char *romfs_mods_dir(void) { return getenv("MODS"); }
int romfs_mods_priority(char (*ids)[65], int cap) {
    char path[2048];
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", getenv("MODS"));
    FILE *f = fopen(path, "r");
    int n = 0;
    char line[200];
    while (f && n < cap && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#') snprintf(ids[n++], 65, "%s", line);
    }
    if (f) fclose(f);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *names[5] = {"wismhd", "winvhe", "wismda", "winvda", "ma00a_grd"};
    // not a map file: never claimed, and nothing built for it
    int other = map_overlay_claims("map/ma9990.wiefp") || map_overlay_claims("bdat/common.bdat");
    printf("other %d built %d\n", other, map_overlay_state());
    printf("claims %d\n", map_overlay_claims("map/ma9990.wismda"));
    printf("state %d\n", map_overlay_state());
    printf("claims-coli %d %d\n", map_overlay_claims("map/mapcoli/ma00a_grd.wihkar"),
           map_overlay_claims("map/mapcoli/ma02a_grd.wihkar"));
    for (int k = 0; k < 5; k++) {
        char path[64], out[2048];
        if (k < 4) snprintf(path, sizeof path, "rom:/map/ma9990.%s", names[k]);
        else snprintf(path, sizeof path, "rom:/Map/MapColi/MA00A_GRD.wihkar");   // any case, as the game may ask
        uint64_t h;
        if (!map_overlay_open(path, &h)) {
            printf("closed %s\n", names[k]);
            continue;
        }
        int64_t size = 0;
        map_overlay_size(&size, h);
        uint8_t *all = calloc(1, (size_t)size + 1);
        // uneven reads: 1, 2, 3, ... 4093 bytes and again, so reads start and end anywhere
        uint64_t at = 0, step = 1;
        while (at < (uint64_t)size) {
            uint64_t got = 0;
            if (map_overlay_read(&got, h, (int64_t)at, all + at, step)) {
                printf("FAIL read %s at %llu\n", names[k], (unsigned long long)at);
                return 1;
            }
            if (!got) break;
            at += got;
            step = step % 4093 + 1 + (step * 7) % 13;
        }
        uint64_t past = 9;
        uint8_t byte;
        map_overlay_read(&past, h, size, &byte, 1);   // at the end: nothing, no failure
        if (past) printf("FAIL read past the end of %s\n", names[k]);
        // and one read of the whole file must give the same bytes
        uint8_t *once = calloc(1, (size_t)size + 1);
        uint64_t got = 0;
        map_overlay_read(&got, h, 0, once, (uint64_t)size + 100);
        if (got != (uint64_t)size || memcmp(all, once, (size_t)size)) printf("FAIL whole read of %s\n", names[k]);
        map_overlay_close(h);
        if (k < 4) snprintf(out, sizeof out, "%s/ma9990.%s", argv[1], names[k]);
        else snprintf(out, sizeof out, "%s/%s.wihkar", argv[1], names[k]);
        plat_file_write(out, all, (size_t)size);
        printf("size %s %lld\n", names[k], (long long)size);
        free(all);
        free(once);
    }
    char line[400];
    int out = 0;
    for (int i = 0; map_overlay_line(i, line, sizeof line, &out); i++) printf("line%s %s\n", out ? "!" : "", line);
    printf("peak %d open\n", handles_peak);
    printf("changed %d\n", map_overlay_changed());
    const char *touch = getenv("TOUCH");   // a pieces.txt changed while the game runs: applies at the next start
    if (touch) {
        FILE *f = fopen(touch, "a");
        if (f) {
            fputs("# changed while the game runs, and longer now\n", f);
            fclose(f);
        }
        printf("changed-after %d\n", map_overlay_changed());
    }
    printf("rom-opens %d\n", rom_opens);
    return 0;
}
