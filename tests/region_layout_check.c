// Driver for tests/region_layout_test.py: runtime/region_layout.c's layout code (built with REGION_LAYOUT_CORE_ONLY, so
// without the file serving) on a layout file. Usage: region_layout_check IN OUT ROWS MOVE_UP WIDEN [FIT] [RECT...]
// adds ROWS rows (the first one WIDEN wider, RECTs moving with its columns); FIT 1 then scales the panel into the
// screen band, FIT 2 adds as many of ROWS as fit at half size or more (region_layout_grow_fit). Prints
// "rows <before> added <n> after <rows> fit <scale> lines <divider lines>".
#include "region_layout.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 6) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n ? n : 1);
    if (!d || fread(d, 1, n, f) != n) return 2;
    fclose(f);
    const char *also[8] = {0};
    for (int i = 7; i < argc && i - 7 < 7; i++) also[i - 7] = argv[i];
    RegionLayoutKind k = {REGION_PANEL_INTEL, 0, atoi(argv[4]), atof(argv[5]), also};
    int before = region_layout_rows(d, n), added = 0, mode = argc > 6 ? atoi(argv[6]) : 0;
    double fit = 0;
    if (before >= 0 && mode == 2) {
        added = region_layout_grow_fit(&d, &n, atoi(argv[3]), &k, &fit);
    } else if (before >= 0) {
        added = region_layout_grow(&d, &n, atoi(argv[3]), &k);
        fit = region_layout_fit(d, n, mode == 1);
    }
    printf("rows %d added %d after %d fit %.4f lines %d\n", before, added, before >= 0 ? region_layout_rows(d, n) : -1,
           fit, region_layout_lines(d, n));
    f = fopen(argv[2], "wb");
    if (!f) return 2;
    fwrite(d, 1, n, f);
    fclose(f);
    free(d);
    return 0;
}
