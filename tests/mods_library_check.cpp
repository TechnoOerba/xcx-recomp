// The F1 Mods tab's library list on its own (runtime/ui/mods_page.cpp, library), drawn headless by ImGui from a made-up
// view of the mods folder, for tests/mods_library_test.py: an entry's parts sit under one row that starts shut and
// opens on a click, they show without the entry's name in front, and the list scrolls in a box that takes at most
// its share of the page. Prints "ok <n>" or one "FAIL <check>" per failure; the exit code is the failure count.
#define XCX_MOD_UI_TESTING
#include "../runtime/ui/mods_page.cpp"
#include <cstdlib>

// What mods_page.cpp calls besides ImGui and the menu text, as the game would answer with no Lua mods and nothing
// merged. Weak, so the file links whichever of these the page calls.
static ModLibView test_view;
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
__attribute__((weak)) int romfs_mods_priority(char (*)[65], int) { return 0; }
__attribute__((weak)) int romfs_mods_move(const char *, int) { return 0; }
__attribute__((weak)) int romfs_mods_tables_changed(void) { return 0; }
__attribute__((weak)) int bdat_merge_status(int, char *, size_t, int *) { return 0; }
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

static void click(ImVec2 low, ImVec2 high) {
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((low.x + high.x) / 2, (low.y + high.y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
}

static void row(const char *id, const char *name, int on, const char *group = "", const char *group_name = "") {
    ModLibRow &r = test_view.rows[test_view.count++];
    memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "%s", id);
    snprintf(r.name, sizeof r.name, "%s", name);
    strcpy(r.kind, "romfs");
    r.installed = 1;
    r.enabled = on;
    snprintf(r.group, sizeof r.group, "%s", group);
    snprintf(r.group_name, sizeof r.group_name, "%s", group_name);
}

static bool has_row(const char *label) {
    for (int i = 0; i < lib_row_count; i++)
        if (!strcmp(lib_rows[i], label)) return true;
    return false;
}

int main() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    io.IniFilename = nullptr;
    io.Fonts->Build();

    // three mods of their own and an entry in six parts, two of them on; one part's library.ini predates group_name
    row("alpha", "Alpha", 1);
    static const char *const parts[] = {"Probe tweak", "Treasure sensor up", "Earlier missions", "More multiguns",
                                        "Target appendages more", "Time attack and support mission tweaks"};
    for (int i = 0; i < 6; i++) {
        char id[65], name[96];
        snprintf(id, sizeof id, "tweaks.p%d", i);
        snprintf(name, sizeof name, "Tweaks mod: %s", parts[i]);
        row(id, name, i < 2, "tweaks", i == 5 ? "" : "Tweaks mod");
    }
    row("beta", "Beta", 0);
    row("gamma", "Gamma", 1);
    test_view.state = MODLIB_IDLE;
    test_view.generation = 1;
    frame();
    frame();
    verify(lib_group_count == 1, "the parts make one entry row");
    verify(lib_group_count && !strcmp(lib_groups[0], "Tweaks mod"), "the entry row is called by the entry's name");
    verify(lib_group_count && !strcmp(lib_group_state[0], "2 of 6 parts on"), "the entry row counts the parts on");
    verify(lib_row_count == 3, "the entry starts shut: only the mods of their own are rows");
    verify(!has_row("Probe tweak") && has_row("Alpha") && has_row("Gamma"), "shut, the parts are not drawn");

    click(lib_group_low[0], lib_group_high[0]);
    verify(lib_row_count == 9, "a click opens the entry on its six parts");
    verify(has_row("Probe tweak") && has_row("Time attack and support mission tweaks"),
           "the parts show without the entry's name in front, by group_name or by the name before \": \"");
    verify(!has_row("Tweaks mod: Probe tweak"), "no part keeps the entry's name in its row");
    click(lib_group_low[0], lib_group_high[0]);
    verify(lib_row_count == 3, "a second click shuts it");

    // a short list takes only its rows; a long one scrolls in a box no taller than its share of the page
    float short_box = lib_box_height;
    verify(short_box > 0 && short_box < lib_box_max, "a short list's box fits its rows");
    for (int i = 0; i < 40; i++) {
        char id[65], name[96];
        snprintf(id, sizeof id, "mod%02d", i);
        snprintf(name, sizeof name, "Mod %02d", i);
        row(id, name, 0);
    }
    test_view.generation++;
    frame();
    frame();
    verify(lib_box_height <= lib_box_max + 1 && lib_box_height > short_box, "a long list's box stops at its share");
    verify(lib_box_max <= 720 * 0.7f + 1, "the box's share is at most about two thirds of the page");

    ImGui::DestroyContext();
    if (!failures) printf("ok %d\n", checks);
    return failures;
}
