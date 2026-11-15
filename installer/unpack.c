// Reads RAR (4 and 5) archives for the mod manager, which handles zips itself (installer/mods.py, archive.py).
// Compiled with the bundled clang the first time it is needed, against the part of libarchive in deps/libarchive.
//
//     unpack list ARCHIVE              one JSON object: {"format": ..., "entries": [{"name", "type", "size"}]}
//     unpack extract ARCHIVE DIR LIMIT writes the regular files under DIR; {"files": N, "bytes": N}
//
// On failure both print {"error": ID, "detail": TEXT} and exit 1. The ids are the manager's: archive.unsafe_name,
// archive.encrypted, archive.too_big, archive.bad (libarchive could not read it), archive.write (DIR could not be
// written). extract checks every name again, as installer/archive.py does when it lists: no absolute paths, drive
// letters or '..', and nothing but regular files and folders (no links, devices or sparse holes).
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"
#include "archive_entry.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static void json_string(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static int fail(const char *id, const char *detail) {
    fputs("{\"error\": ", stdout);
    json_string(id);
    fputs(", \"detail\": ", stdout);
    json_string(detail ? detail : "");
    fputs("}\n", stdout);
    return 1;
}

static struct archive *open_archive(const char *path, int *ok) {
    struct archive *a = archive_read_new();
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    *ok = archive_read_open_filename(a, path, 1 << 16) == ARCHIVE_OK;
    return a;
}

// The entry's name with '\' as '/', or NULL when it is not one the manager may write: empty, absolute, a drive letter,
// a '..' or '.' part, a control character. out holds at least 4096 bytes.
static const char *safe_name(struct archive_entry *e, char *out) {
    const char *name = archive_entry_pathname_utf8(e);
    if (!name) name = archive_entry_pathname(e);
    if (!name || !*name || strlen(name) >= 4096) return NULL;
    size_t n = 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == ':') return NULL;
        out[n++] = c == '\\' ? '/' : (char)c;
    }
    out[n] = 0;
    while (n && out[n - 1] == '/') out[--n] = 0;   // folders may end in '/'
    if (!n || out[0] == '/') return NULL;
    for (char *part = out;;) {
        char *slash = strchr(part, '/');
        size_t len = slash ? (size_t)(slash - part) : strlen(part);
        if (len == 0 || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) return NULL;
        if (!slash) break;
        part = slash + 1;
    }
    return out;
}

static const char *type_of(struct archive_entry *e) {
    if (archive_entry_hardlink(e) || archive_entry_symlink(e)) return "link";
    switch (archive_entry_filetype(e)) {
    case AE_IFREG: return "file";
    case AE_IFDIR: return "dir";
    case AE_IFLNK: return "link";
    default: return "other";
    }
}

static int list(const char *path) {
    int ok;
    struct archive *a = open_archive(path, &ok);
    if (!ok) {
        int r = fail("archive.bad", archive_error_string(a));
        archive_read_free(a);
        return r;
    }
    struct archive_entry *e;
    int first = 1, r;
    char name[4096];
    printf("{\"entries\": [");
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        const char *raw = archive_entry_pathname_utf8(e);
        if (!raw) raw = archive_entry_pathname(e);
        printf(first ? "\n" : ",\n");
        first = 0;
        printf(" {\"name\": ");
        json_string(raw ? raw : "");
        printf(", \"type\": \"%s\", \"size\": %lld, \"safe\": %s, \"encrypted\": %s}", type_of(e),
               (long long)archive_entry_size(e), safe_name(e, name) ? "true" : "false",
               archive_entry_is_encrypted(e) ? "true" : "false");
        archive_read_data_skip(a);
    }
    if (r != ARCHIVE_EOF) {
        // The entries so far are printed; end the object with the error so the reader gets one JSON value.
        printf("\n], \"error\": \"archive.bad\", \"detail\": ");
        json_string(archive_error_string(a) ? archive_error_string(a) : "");
        printf("}\n");
        archive_read_free(a);
        return 1;
    }
    printf("\n], \"format\": ");
    json_string(archive_format_name(a) ? archive_format_name(a) : "");
    printf("}\n");
    archive_read_free(a);
    return 0;
}

// ---- writing

#ifdef _WIN32
static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) { free(w); w = NULL; }
    return w;
}
static int make_dir(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return -1;
    int r = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
    free(w);
    return r;
}
static FILE *create(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return NULL;
    FILE *f = _wfopen(w, L"wbx");   // never over an existing file: two entries of one name are refused
    free(w);
    return f;
}
#else
static int make_dir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1; }
static FILE *create(const char *path) { return fopen(path, "wbx"); }
#endif

// Makes every folder of rel (a safe name) below dir; with file set the last part is a file and is left out.
static int make_parents(const char *dir, const char *rel, int file, char *full, size_t cap) {
    if ((size_t)snprintf(full, cap, "%s/%s", dir, rel) >= cap) return -1;
    char *p = full + strlen(dir) + 1;
    for (;;) {
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = 0;
        int r = make_dir(full);
        *slash = '/';
        if (r) return -1;
        p = slash + 1;
    }
    return file ? 0 : make_dir(full);
}

static int extract(const char *path, const char *dir, unsigned long long limit) {
    int ok;
    struct archive *a = open_archive(path, &ok);
    if (!ok) {
        int r = fail("archive.bad", archive_error_string(a));
        archive_read_free(a);
        return r;
    }
    static char buf[1 << 16];
    char name[4096], full[8192];
    struct archive_entry *e;
    unsigned long long total = 0, files = 0;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        const char *rel = safe_name(e, name);
        const char *type = type_of(e);
        const char *shown = archive_entry_pathname_utf8(e) ? archive_entry_pathname_utf8(e) : "";
        if (!rel || (strcmp(type, "file") && strcmp(type, "dir")) || archive_entry_sparse_count(e)) {
            archive_read_free(a);
            return fail("archive.unsafe_name", shown);
        }
        if (archive_entry_is_encrypted(e)) {
            archive_read_free(a);
            return fail("archive.encrypted", shown);
        }
        if (!strcmp(type, "dir")) {
            if (make_parents(dir, rel, 0, full, sizeof full)) { archive_read_free(a); return fail("archive.write", rel); }
            continue;
        }
        if (make_parents(dir, rel, 1, full, sizeof full)) { archive_read_free(a); return fail("archive.write", rel); }
        FILE *f = create(full);
        if (!f) { archive_read_free(a); return fail("archive.write", rel); }
        la_ssize_t n;
        while ((n = archive_read_data(a, buf, sizeof buf)) > 0) {
            total += (unsigned long long)n;
            if (total > limit) { fclose(f); archive_read_free(a); return fail("archive.too_big", rel); }
            if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); archive_read_free(a); return fail("archive.write", rel); }
        }
        if (fclose(f) || n < 0) {
            int bad = n < 0;
            char detail[600];
            snprintf(detail, sizeof detail, "%s: %s", rel, bad && archive_error_string(a) ? archive_error_string(a) : "");
            archive_read_free(a);
            return fail(bad ? "archive.bad" : "archive.write", detail);
        }
        files++;
    }
    if (r != ARCHIVE_EOF) {
        int ret = fail("archive.bad", archive_error_string(a));
        archive_read_free(a);
        return ret;
    }
    archive_read_free(a);
    printf("{\"files\": %llu, \"bytes\": %llu}\n", files, total);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "list")) return list(argv[2]);
    if (argc == 5 && !strcmp(argv[1], "extract")) return extract(argv[2], argv[3], strtoull(argv[4], NULL, 10));
    fputs("usage: unpack list ARCHIVE | unpack extract ARCHIVE DIR LIMIT\n", stderr);
    return 2;
}
