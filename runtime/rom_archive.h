// The game's own copy of a file in its archive (sts.arh + sts.ard on the mounted RomFS), read host-side
// (runtime/rom_archive.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

// key: the path as the archive device gets it (lowercase, no mount, no leading '/'). A new buffer with the file
// (unpacked) and its size in *n; NULL when the RomFS isn't mounted, the archive has no such file or it can't be read.
uint8_t *rom_archive_file(const char *key, size_t *n);
