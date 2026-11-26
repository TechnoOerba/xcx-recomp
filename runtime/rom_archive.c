// The game's copy of a file stored in its archive (sts.arh index + sts.ard data on the RomFS), read host-side: the
// index entry is the XXH64 of "/<path>", entries are laid out in index order at the index's alignment, and an entry is
// either the file itself or an xbc1 container (type 3: zstd).
#include "rom_archive.h"
#include "fs.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull
static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t round64(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
static uint64_t merge64(uint64_t h, uint64_t v) { return (h ^ round64(0, v)) * P1 + P4; }

static uint64_t xxh64(const uint8_t *d, size_t n) {
    size_t i = 0;
    uint64_t h;
    if (n >= 32) {
        uint64_t v[4] = {P1 + P2, P2, 0, (uint64_t)0 - P1};
        for (; i + 32 <= n; i += 32)
            for (int k = 0; k < 4; k++) v[k] = round64(v[k], rd64(d + i + 8 * k));
        h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
        for (int k = 0; k < 4; k++) h = merge64(h, v[k]);
    } else {
        h = P5;
    }
    h += n;
    for (; i + 8 <= n; i += 8) h = rotl(h ^ round64(0, rd64(d + i)), 27) * P1 + P4;
    if (i + 4 <= n) {
        h = rotl(h ^ (uint64_t)rd32(d + i) * P1, 23) * P2 + P3;
        i += 4;
    }
    for (; i < n; i++) h = rotl(h ^ d[i] * P5, 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    return h ^ (h >> 32);
}

// n bytes at off of RomFS file name (all of it for n == UINT64_MAX) into a new buffer; NULL on failure.
static uint8_t *rom_read(const char *mount, const char *name, uint64_t off, uint64_t n, uint64_t *got) {
    char path[512];
    uint64_t h;
    snprintf(path, sizeof path, "%s:/%s", mount, name);
    if (romfs_open(&h, path, FS_OPEN_READ)) return NULL;
    int64_t size = 0;
    uint8_t *p = NULL;
    if (!romfs_size(&size, h) && size >= 0 && off <= (uint64_t)size) {
        if (n == UINT64_MAX || n > (uint64_t)size - off) n = (uint64_t)size - off;
        uint64_t done = 0;
        if (n < (1ull << 31) && (p = malloc(n ? n : 1)) && romfs_read(&done, h, (int64_t)off, p, n) == 0 && done == n) {
            *got = n;
        } else {
            free(p);
            p = NULL;
        }
    }
    romfs_close(h);
    return p;
}

static PlatMutex lock = PLAT_MUTEX_INIT;
static uint8_t *arh;   // the index, read once (it never changes while the game runs)
static uint64_t arh_size;
static int arh_state;  // 0 not read, 1 good, -1 unusable

uint8_t *rom_archive_file(const char *key, size_t *n) {
    const char *mount = romfs_mount_name();
    if (!key || !mount || !*mount) return NULL;
    plat_mutex_lock(&lock);
    if (!arh_state) {
        arh = rom_read(mount, "sts.arh", 0, UINT64_MAX, &arh_size);
        arh_state = arh && arh_size >= 16 && !memcmp(arh, "arh2", 4) && 16 + 16ull * rd32(arh + 4) <= arh_size &&
                    rd32(arh + 8) && !(rd32(arh + 8) & (rd32(arh + 8) - 1)) ? 1 : -1;
    }
    int good = arh_state > 0;
    plat_mutex_unlock(&lock);
    if (!good) return NULL;
    char path[1100];
    int len = snprintf(path, sizeof path, "/%s", key);
    if (len <= 0 || len >= (int)sizeof path) return NULL;
    uint64_t want = xxh64((const uint8_t *)path, (size_t)len), off = 0, align = rd32(arh + 8);
    uint32_t count = rd32(arh + 4);
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *e = arh + 16 + 16ull * k;
        uint32_t stored = rd32(e + 8);
        if (rd64(e) != want) {
            off += (stored + align - 1) & ~(align - 1);
            continue;
        }
        uint64_t got = 0;
        uint8_t *p = rom_read(mount, "sts.ard", off, stored, &got);
        if (!p || got != stored) {
            free(p);
            return NULL;
        }
        if (got < 48 || memcmp(p, "xbc1", 4)) {
            *n = got;
            return p;
        }
        uint32_t type = rd32(p + 4), un = rd32(p + 8), packed = rd32(p + 12);
        uint8_t *out = type == 3 && packed <= got - 48 && un < (1u << 30) ? malloc(un ? un : 1) : NULL;
        size_t r = out ? ZSTD_decompress(out, un, p + 48, packed) : 0;
        free(p);
        if (!out || ZSTD_isError(r) || r != un) {
            free(out);
            return NULL;
        }
        *n = un;
        return out;
    }
    return NULL;
}
