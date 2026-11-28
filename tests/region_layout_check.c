// Driver for tests/region_layout_test.py: runtime/region_layout.c's layout code (built with REGION_LAYOUT_CORE_ONLY, so
// without the file serving) on a layout file. Usage: region_layout_check IN OUT ROWS MOVE_UP WIDEN [FIT] [RECT...]
// adds ROWS rows (the first one WIDEN wider, RECTs moving with its columns; an argument band=B: the window's header
// band grows B per unit of its scale, so the rows move down by that; cursor=W,H: the share of the cursor rects its
// picture leaves empty, so a scaled panel's cursor rects grow); FIT 1 then scales the panel into the
// screen band, FIT 2 adds as many of ROWS as fit at half size or more (region_layout_grow_fit). Prints
// "rows <before> added <n> after <rows> fit <scale> lines <divider lines>".
#include "region_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    double band = 0, cw = 0, ch = 0;
    for (int i = 7, a = 0; i < argc && a < 7; i++) {
        if (!strncmp(argv[i], "band=", 5)) band = atof(argv[i] + 5);
        else if (!strncmp(argv[i], "cursor=", 7)) sscanf(argv[i] + 7, "%lf,%lf", &cw, &ch);
        else also[a++] = argv[i];
    }
    RegionLayoutKind k = {REGION_PANEL_INTEL, 0, atoi(argv[4]), atof(argv[5]), also, band, cw, ch};
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
