// The F1 Mods tab's Mod priority section on its own (runtime/ui/mods_page.cpp, mod_priority), drawn headless by ImGui
// from a made-up mod list and merge status, for tests/mods_priority_test.py: the mods listed by their names, Up moving
// one, the merge lines in their looks (left out, caveat, grey for mods that add nothing or are partly alike) and the
// restart line once the list differs from what the game merged at start-up. Prints "ok <n>" or one "FAIL <check>" per
// failure; the exit code is the failure count.
#define XCX_MOD_UI_TESTING
#include "../runtime/ui/mods_page.cpp"
#include <cstdlib>

// What mods_page.cpp calls besides ImGui and the menu text. Weak, so the file links whichever of these the page calls.
static ModLibView test_view;
static char test_ids[8][65];
static int test_n, test_changed;
static const char *test_status[8];
static int test_kind[8], test_nstatus;
static char moved[65];
static int moved_up = -1;
extern "C" {
__attribute__((weak)) void modlib_snapshot(ModLibView *v) { *v = test_view; }
__attribute__((weak)) void modlib_refresh(void) {}
__attribute__((weak)) void modlib_install(const char *) {}
__attribute__((weak)) void modlib_remove(const char *) {}
__attribute__((weak)) void modlib_set_enabled(const char *, int) {}
__attribute__((weak)) void mod_ui_snapshot(ModSnapshot *out) { memset(out, 0, sizeof *out); }
__attribute__((weak)) void mod_ui_interaction(int) {}
__attribute__((weak)) int rt_mods_ui_submit(const ModAction *) { return 0; }
__attribute__((weak)) int rt_mods_ui_intent(const struct ModIntent *) { return 0; }
__attribute__((weak)) int romfs_mods_priority(char (*ids)[65], int cap) {
    for (int i = 0; i < test_n && i < cap; i++) memcpy(ids[i], test_ids[i], 65);
    return test_n;
}
__attribute__((weak)) int romfs_mods_move(const char *id, int up) {
    snprintf(moved, sizeof moved, "%s", id);
    moved_up = up;
    return 1;
}
__attribute__((weak)) int romfs_mods_tables_changed(void) { return test_changed; }
__attribute__((weak)) int bdat_merge_status(int i, char *out, size_t cap, int *kind) {
    if (i < 0 || i >= test_nstatus) return 0;
    snprintf(out, cap, "%s", test_status[i]);
    if (kind) *kind = test_kind[i];
    return 1;
}
const char *cfg_value(const char *) { return "en"; }
unsigned cfg_version(void) { return 1; }
const char *ov_language(void) { return "en-US"; }
const char *plat_ui_language(void) { return "en"; }
}

static int checks, failures;
static void verify(bool yes, const char *what) {
    ++checks;
    if (!yes) { ++failures; printf("FAIL %s\n", what); }
}

static void frame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Mods", nullptr, ImGuiWindowFlags_NoDecoration);
    mods_page(1.0f, 0, false);
    ImGui::End();
    ImGui::Render();
}

static void refresh() {   // the page re-reads the list and the status when the library view changes
    test_view.generation++;
    frame();
    frame();
}

static void row(const char *id, const char *name) {
    ModLibRow &r = test_view.rows[test_view.count++];
    memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "%s", id);
    snprintf(r.name, sizeof r.name, "%s", name);
    strcpy(r.kind, "romfs");
    r.installed = r.enabled = 1;
}

static int find(char tag, const char *text) {   // the index of a drawn line, -1 when there is none
    for (int i = 0; i < prio_count; i++)
        if (prio_tag[i] == tag && (!text || strstr(prio_lines[i], text))) return i;
    return -1;
}

int main() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 2000);   // tall enough that the whole section is on the page
    io.DeltaTime = 1.0f / 60;
    io.IniFilename = nullptr;
    io.Fonts->Build();
    row("whole", "Whole mod");
    row("whole.piece", "Whole mod: Piece");
    row("other", "Other mod");
    test_view.state = MODLIB_IDLE;

    // one mod on and nothing merged: no section
    strcpy(test_ids[0], "whole");
    test_n = 1;
    refresh();
    verify(prio_count == 0, "one mod and no merge: no Mod priority section");

    // three mods, merged with one of each kind of line
    strcpy(test_ids[0], "whole.piece");
    strcpy(test_ids[1], "whole");
    strcpy(test_ids[2], "other");
    test_n = 3;
    static const char *const lines[] = {"bdat/common.bdat: 3 mods merged", "bdat/common.bdat: left out: other: x",
                                        "bdat/common.bdat: caveat: other: y",
                                        "bdat/common.bdat: adds nothing: Whole mod: Piece: z",
                                        "bdat/common.bdat: partly alike: Whole mod: w"};
    for (int i = 0; i < 5; i++) {
        test_status[i] = lines[i];
        test_kind[i] = i;
    }
    test_nstatus = 5;
    refresh();
    verify(find('P', "Whole mod: Piece") == 0 && find('P', "Other mod") == 2, "the mods are listed by name, in order");
    verify(find('H', nullptr) >= 0, "the merge lines have their heading");
    verify(find('N', "3 mods merged") >= 0, "the summary line is plain");
    verify(find('O', "left out") >= 0 && find('Y', "caveat") >= 0, "left out and caveat lines have their colours");
    verify(find('G', "adds nothing") >= 0 && find('G', "partly alike") >= 0, "adds-nothing and partly-alike are grey");
    verify(find('R', nullptr) < 0, "no restart line while the list is what was merged");

    // Up on the third mod asks romfs_mods.c to move it
    io.AddMousePosEvent((prio_up_low[2].x + prio_up_high[2].x) / 2, (prio_up_low[2].y + prio_up_high[2].y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    verify(!strcmp(moved, "other") && moved_up == 1, "Up moves that mod up");

    // the list differs from what was merged at start-up: the restart line, ahead of the merge lines
    test_changed = 1;
    refresh();
    int r = find('R', nullptr);
    verify(r >= 0 && r == find('H', nullptr) + 1 && r < find('N', nullptr), "the restart line comes first");
    verify(r >= 0 && strstr(prio_lines[r], "restart"), "the restart line says to restart");

    // nothing merged and one mod left, but the list changed since start-up: the section still says so
    test_n = 1;
    test_nstatus = 0;
    refresh();
    verify(find('R', nullptr) >= 0 && find('P', nullptr) < 0, "a changed list shows the restart line on its own");

    ImGui::DestroyContext();
    if (!failures) printf("ok %d\n", checks);
    return failures;
}
