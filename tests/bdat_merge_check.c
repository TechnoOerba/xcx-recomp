// Command-line driver for runtime/bdat_merge.c, for tests/bdat_merge_test.py and for checks against real files.
//   bdat_merge_check rewrite FILE...                  rebuild every table, print how many differ from the file's own
//   bdat_merge_check merge BASE OUT MOD...            merge (mods highest priority first), print the notes and counts
// Exit status: 0 done, 2 the files can't be merged (the reason printed), 1 a usage or I/O error.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdat_merge.h"

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return p;
}

static void print_note(void *ctx, const char *line) {
    (void)ctx;
    printf("note %s\n", line);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "rewrite")) {
        for (int i = 2; i < argc; i++) {
            size_t n;
            uint8_t *p = slurp(argv[i], &n);
            if (!p) return 1;
            printf("rewrite %s %d\n", argv[i], bdat_rewrite_check(p, n));
            free(p);
        }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "merge")) {
        BdatInput base, mods[64];
        int n = argc - 4;
        if (n > 64) return 1;
        base.name = "game";
        if (!(base.data = slurp(argv[2], &base.size))) return 1;
        for (int k = 0; k < n; k++) {
            const char *p = argv[4 + k], *eq = strchr(p, '=');   // name=path, or the path as the name
            mods[k].name = p;
            if (eq) {
                char *name = malloc((size_t)(eq - p) + 1);
                memcpy(name, p, (size_t)(eq - p));
                name[eq - p] = 0;
                mods[k].name = name;
                p = eq + 1;
            }
            if (!(mods[k].data = slurp(p, &mods[k].size))) return 1;
        }
        size_t out_n = 0;
        BdatSummary s;
        char why[512];
        uint8_t *out = bdat_merge(&base, mods, n, &out_n, &s, print_note, NULL, why, sizeof why);
        if (!out) {
            printf("unmergeable %s\n", why);
            return 2;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f || fwrite(out, 1, out_n, f) != out_n) return 1;
        fclose(f);
        char file[100] = "merged";
        if (s.file_mode != BDAT_FILE_MERGED)
            snprintf(file, sizeof file, "%s:%s", s.file_mode == BDAT_FILE_WHOLE ? "whole" : "copied",
                     mods[s.file_mod].name);
        printf("summary changed=%d merged=%d whole=%d priority=%d leftout=%d caveat=%d file=%s\n", s.tables_changed,
               s.tables_merged, s.tables_whole, s.priority, s.left_out, s.caveats, file);
        return 0;
    }
    fprintf(stderr, "usage: bdat_merge_check rewrite FILE... | merge BASE OUT MOD...\n");
    return 1;
}
