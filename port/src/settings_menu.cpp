#include "settings_menu.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "config.h"
#include "hle/videoout.h"
#include "linker.h"
#include "memory.h"

namespace {

// engine functions (eboot offsets)
constexpr u64 kClonePlaceable = 0x251ca0;  // (project, source placeable, parent): clone into the project
constexpr u64 kSetFocus = 0x24be10;        // (project, placeable, controller)
constexpr u64 kSetText = 0x9cad70;         // (text, placeable): igGuiPlaceable::_displayText
constexpr u64 kStringPool = 0x26ee240;     // global: the igString pool
constexpr u64 kInternString = 0x14a74b0;   // (pool, text) -> pooled string (one reference)
constexpr u64 kSelectorAdd = 0x11a1b90;   // (selector, &pooled string, placeable): append a value
constexpr u64 kSelectorInit = 0x11a13b0;  // (selector): current = default, show value and name
constexpr u64 kSelectorSet = 0x11a11c0;   // (selector, index, placeable for animations or 0, direction)
// behaviour vtables
constexpr u64 kNavItemVtable = 0x2558220;         // CGuiBehaviorOctaneNavigationItem
constexpr u64 kGlobalSelectorVtable = 0x2544780;  // CGuiBehaviorOctaneGlobalOptionSelector

// igGuiPlaceable
constexpr u64 PL_BITS = 0xc, PL_PROJECT = 0x20, PL_PARENT = 0x40, PL_CHILDREN = 0x48, PL_POSITION = 0x60,
              PL_SCALE = 0x84, PL_BEHAVIORS = 0xe8;
constexpr u32 PL_HIDDEN = 0x400, PL_DIRTY = 0x1000, PL_FINAL_HIDDEN = 0x2000;
// CGuiBehaviorNavigationItem
constexpr u64 NAV_ITEM_TEXT = 0xe8, NAV_TEXT_PLACEABLE = 0xf0, NAV_TEXT2_PLACEABLE = 0xf8, NAV_UP = 0x110,
              NAV_DOWN = 0x118;

typedef u64(SYSV* CloneFn)(u64, u64, u64);
typedef void(SYSV* SetFocusFn)(u64, u64, int);
typedef void(SYSV* SetTextFn)(const char*, u64);
typedef u64(SYSV* InternFn)(u64, const char*);
typedef void(SYSV* SelectorAddFn)(u64, u64*, u64);
typedef void(SYSV* SelectorInitFn)(u64);
typedef u64(SYSV* SelectorSetFn)(u64, int, u64, bool);
// CGuiBehaviorBaseSettingSelector (a navigation item) / CGuiBehaviorOctaneGlobalOptionSelector
constexpr u64 SEL_OPTION_NAME = 0x148, SEL_DEFAULT_INDEX = 0x190, SEL_CURRENT_INDEX = 0x198, SEL_SETTINGS = 0x1a0,
              SEL_ENABLED = 0x1a8, GSEL_VARIABLE = 0x1c0, GSEL_PLAYER_SPECIFIC = 0x1c4;

u64 Base() { return (u64)g_linker.eboot->base; }

template <class T>
bool Get(u64 addr, T* v) {
    return addr && GuestTryCopy(v, (const void*)addr, sizeof(T));
}
template <class T>
T Read(u64 addr) {
    T v{};
    Get(addr, &v);
    return v;
}

int ListItems(u64 list, u64* items, int max) {  // igObjectList: count +0xc, data +0x20
    int count = 0;
    u64 data = 0;
    if (!Get(list + 0xc, &count) || !Get(list + 0x20, &data) || !data) return 0;
    count = count < max ? count : max;
    for (int i = 0; i < count; i++)
        if (!Get(data + i * 8, &items[i])) return 0;
    return count;
}

std::string Str(u64 p) {
    char buf[64] = {};
    if (!p) return {};
    for (size_t i = 0; i + 1 < sizeof(buf); i++)
        if (!GuestTryCopy(&buf[i], (const void*)(p + i), 1) || !buf[i]) break;
    return buf;
}

// the placeable's behaviour of the given class (vtable offset), 0 if none
u64 Behavior(u64 pl, u64 vtable) {
    u64 list[8];
    for (int i = 0, n = ListItems(Read<u64>(pl + PL_BEHAVIORS), list, 8); i < n; i++)
        if (Read<u64>(list[i]) == Base() + vtable) return list[i];
    return 0;
}

u64 Intern(const char* text) {
    return ((InternFn)(Base() + kInternString))(Read<u64>(Base() + kStringPool), text);
}

void SetText(u64 placeable, const char* text) {
    if (placeable) ((SetTextFn)(Base() + kSetText))(text, placeable);
}

// ------------------------------------------------------------------ Options menu
struct Button {
    u64 placeable, nav;
    std::string text;
};

// The buttons of the game's Options menu (Gameplay, Audio, Controls, Language Select, Extras) in
// navigation order from the focused one's list, if `focus` is in it.
bool OptionsMenuButtons(u64 focus, std::vector<Button>* out) {
    u64 kids[16];
    int n = ListItems(Read<u64>(Read<u64>(focus + PL_PARENT) + PL_CHILDREN), kids, 16);
    bool gameplay = false, extras = false, ours = false;
    out->clear();
    for (int i = 0; i < n; i++) {
        Button b{kids[i], Behavior(kids[i], kNavItemVtable), {}};
        if (!b.nav) continue;
        b.text = Str(Read<u64>(b.nav + NAV_ITEM_TEXT));
        gameplay |= b.text == "Gameplay";
        extras |= b.text == "Extras";
        ours |= b.text == "Video";
        out->push_back(b);
    }
    return gameplay && extras && !ours;
}

u64 g_options_project = 0, g_video_button = 0;

// "Video" goes between Gameplay and Audio: the buttons from Audio on move one place down.
void AddVideoButton(u64 project, const std::vector<Button>& buttons) {
    // the visual order: down links from Gameplay
    std::vector<const Button*> order;
    const Button* b = nullptr;
    for (const Button& x : buttons)
        if (x.text == "Gameplay") b = &x;
    while (b && order.size() < buttons.size()) {
        order.push_back(b);
        u64 down = Read<u64>(b->nav + NAV_DOWN);
        b = nullptr;
        for (const Button& x : buttons)
            if (x.placeable == down) b = &x;
        if (b == order.front()) break;
    }
    if (order.size() < 3) return;
    std::vector<std::array<float, 3>> pos(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        GuestTryCopy(pos[i].data(), (const void*)(order[i]->placeable + PL_POSITION), sizeof(pos[i]));
        LOG("settings: options button '%s' at %.0f,%.0f", order[i]->text.c_str(), pos[i][0], pos[i][1]);
    }
    const Button& gameplay = *order[0];
    const Button& audio = *order[1];
    const u64 parent = Read<u64>(gameplay.placeable + PL_PARENT);
    const u64 clone = ((CloneFn)(Base() + kClonePlaceable))(project, gameplay.placeable, parent);
    if (!clone) {
        LOG("settings: cloning the Gameplay button failed");
        return;
    }
    // positions: Video takes Audio's place, every later button the next one's, the last one a
    // step further down the column
    auto place = [](u64 pl, const std::array<float, 3>& p) {
        memcpy((void*)(pl + PL_POSITION), p.data(), sizeof(p));
        *(u32*)(pl + PL_BITS) |= PL_DIRTY;
    };
    const size_t n = order.size();
    std::array<float, 3> after_last = pos[n - 1];
    for (int k = 0; k < 3; k++) after_last[k] = 2 * pos[n - 1][k] - pos[n - 2][k];
    for (size_t i = n - 1; i >= 1; i--) place(order[i]->placeable, i + 1 < n ? pos[i + 1] : after_last);
    place(clone, pos[1]);
    const u64 nav = Behavior(clone, kNavItemVtable);
    if (nav) {
        *(u64*)(nav + NAV_ITEM_TEXT) = Intern("Video");
        SetText(Read<u64>(nav + NAV_TEXT_PLACEABLE), "Video");
        SetText(Read<u64>(nav + NAV_TEXT2_PLACEABLE), "Video");
        *(u64*)(nav + NAV_UP) = gameplay.placeable;
        *(u64*)(nav + NAV_DOWN) = audio.placeable;
        *(u64*)(gameplay.nav + NAV_DOWN) = clone;
        *(u64*)(audio.nav + NAV_UP) = clone;
    }
    g_options_project = project;
    g_video_button = clone;
    LOG("settings: added the Video button %llx (nav %llx) to project %llx list %llx", (unsigned long long)clone,
        (unsigned long long)nav, (unsigned long long)project, (unsigned long long)parent);
}

// ------------------------------------------------------------------ Video page
// The options page project (one page at a time: Gameplay, Audio, Controls) shows our rows instead
// of the Gameplay ones while it was opened with the Video button.

// A row: its values, the value index of the current settings, and what choosing one does.
// `restart`: takes effect at the next start (the value then reads "... (RESTART)").
struct Setting {
    const char* name;
    std::vector<std::string> labels;
    std::function<int()> current;
    std::function<void(int)> apply;
    bool restart;
};

int IndexOf(const std::vector<int>& values, int v) {  // exact, else the nearest
    int best = 0;
    for (int i = 0; i < (int)values.size(); i++)
        if (std::abs(values[i] - v) < std::abs(values[best] - v)) best = i;
    return best;
}

const std::vector<Setting>& VideoSettings() {
    static const std::vector<Setting> settings = [] {
        static const std::vector<int> heights = {720, 900, 1080, 1440, 1800, 2160, 2880, 4320};
        static const std::vector<int> fps = {30, 60, 120, 144, 165, 240, 0};
        static const std::vector<int> aniso = {1, 2, 4, 8, 16};
        std::vector<std::string> resolutions;
        for (int h : heights) resolutions.push_back(std::to_string(h * 16 / 9) + "x" + std::to_string(h));
        return std::vector<Setting>{
            {"Display Mode", {"Windowed", "Fullscreen"}, [] { return ConfigInt("Fullscreen", 0) ? 1 : 0; },
             [](int i) { ConfigSet("Fullscreen", i), VideoOutSetFullscreen(i != 0); }, false},
            // internal resolution (RenderScale, percent of 1080p); the picture is scaled to the window
            {"Resolution", resolutions,
             [] { return IndexOf(heights, ConfigInt("RenderScale", 200) * 1080 / 100); },
             [](int i) { ConfigSet("RenderScale", (heights[i] * 100 + 540) / 1080); }, true},
            {"Aspect Ratio", {"Auto", "16:9", "21:9", "32:9"},
             [] { return std::clamp(ConfigInt("AspectRatio", 0), 0, 3); }, [](int i) { ConfigSet("AspectRatio", i); },
             true},
            {"Frame Rate Limit", {"30", "60", "120", "144", "165", "240", "Unlimited", "VSync"},
             [] { return ConfigInt("VSync", 0) ? 7 : IndexOf(fps, ConfigInt("FpsLimit", 60)); },
             [](int i) {
                 ConfigSet("VSync", i == 7);
                 if (i < 7) ConfigSet("FpsLimit", fps[i]);
             },
             false},
            {"Shadow Quality", {"Low", "Medium", "High"},
             [] { return std::clamp(ConfigInt("ShadowQuality", 1), 0, 2); },
             [](int i) { ConfigSet("ShadowQuality", i); }, false},
            {"Texture Filtering", {"Off", "2x", "4x", "8x", "16x"},
             [] { return IndexOf(aniso, ConfigInt("Anisotropy", 16)); },
             [](int i) { ConfigSet("Anisotropy", aniso[i]); }, false},
            {"Motion Blur", {"Off", "On"}, [] { return ConfigInt("MotionBlur", 1) ? 1 : 0; },
             [](int i) { ConfigSet("MotionBlur", i); }, false},
        };
    }();
    return settings;
}

struct RowState {
    u64 placeable, selector, nav;
    int last;     // value index last seen
    int started;  // value index of the running settings (restart rows)
    bool marked;  // the value text carries the restart note
};
u64 g_page_project = 0;
std::vector<RowState> g_rows;
bool g_video_page = false;

bool IsOurRow(u64 pl) {
    for (const RowState& r : g_rows)
        if (r.placeable == pl) return true;
    return false;
}

void SetHidden(u64 pl, bool hidden) {
    u32 bits = Read<u32>(pl + PL_BITS);
    const u32 want = hidden ? (bits | PL_HIDDEN) : (bits & ~PL_HIDDEN);
    if (want != bits) *(u32*)(pl + PL_BITS) = want | PL_DIRTY;
}

// the option page's row list: the focused row's siblings, with a template row (a global option
// selector: Preferred Boost Bar on the Gameplay page, the control scheme on the Controls page)
bool FindPage(u64 focus, u64* list_parent, u64* template_row) {
    const u64 parent = Read<u64>(focus + PL_PARENT);
    u64 kids[48];
    const int n = ListItems(Read<u64>(parent + PL_CHILDREN), kids, 48);
    *template_row = 0;
    for (int i = 0; i < n; i++) {
        if (IsOurRow(kids[i]) || !Behavior(kids[i], kGlobalSelectorVtable)) continue;
        const bool shown = !(Read<u32>(kids[i] + PL_BITS) & (PL_HIDDEN | PL_FINAL_HIDDEN));
        if (!*template_row || shown) *template_row = kids[i];
    }
    *list_parent = parent;
    return *template_row != 0;
}

// the settings as they were when the game started (what the restart rows compare with)
int StartedIndex(size_t row) {
    static const std::vector<int> started = [] {
        std::vector<int> v;
        for (const Setting& s : VideoSettings()) v.push_back(s.current());
        return v;
    }();
    return started[row];
}

void BuildVideoPage(u64 project, u64 parent, u64 tmpl) {
    g_rows.clear();
    g_page_project = project;
    // the visible game rows give the column: first position and spacing; ours are packed closer
    // and a bit smaller to fit eight rows
    u64 kids[48];
    const int n = ListItems(Read<u64>(parent + PL_CHILDREN), kids, 48);
    std::vector<std::array<float, 3>> shown;
    for (int i = 0; i < n; i++) {
        if (!Behavior(kids[i], kNavItemVtable) || (Read<u32>(kids[i] + PL_BITS) & (PL_HIDDEN | PL_FINAL_HIDDEN)))
            continue;
        std::array<float, 3> p{};
        GuestTryCopy(p.data(), (const void*)(kids[i] + PL_POSITION), sizeof(p));
        shown.push_back(p);
    }
    std::sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) { return a[1] < b[1]; });
    std::array<float, 3> p0{}, step{0, 140, 0};
    GuestTryCopy(p0.data(), (const void*)(tmpl + PL_POSITION), sizeof(p0));
    if (shown.size() >= 2) {
        p0 = shown[0];
        for (int k = 0; k < 3; k++) step[k] = shown[1][k] - shown[0][k];
    }
    constexpr float kPack = 100.0f / 140.0f, kRowScale = 0.85f;
    for (int k = 0; k < 3; k++) step[k] *= kPack;
    p0[0] -= step[0] * 0.5f, p0[1] -= step[1] * 0.5f;
    const std::vector<Setting>& settings = VideoSettings();
    for (size_t i = 0; i < settings.size(); i++) {
        const u64 pl = ((CloneFn)(Base() + kClonePlaceable))(project, tmpl, parent);
        const u64 sel = pl ? Behavior(pl, kGlobalSelectorVtable) : 0;
        if (!sel) {
            LOG("settings: cloning an option row failed");
            break;
        }
        std::array<float, 3> p;
        for (int k = 0; k < 3; k++) p[k] = p0[k] + step[k] * i;
        memcpy((void*)(pl + PL_POSITION), p.data(), sizeof(p));
        const float scale[3] = {kRowScale, kRowScale, 1.0f};
        memcpy((void*)(pl + PL_SCALE), scale, sizeof(scale));
        SetHidden(pl, false);
        // detached from the game option, filled with our values
        *(u8*)(sel + GSEL_PLAYER_SPECIFIC) = 0;
        *(int*)(Read<u64>(sel + SEL_SETTINGS) + 0xc) = 0;
        *(int*)(Read<u64>(sel + SEL_ENABLED) + 0xc) = 0;
        for (const std::string& v : settings[i].labels) {
            u64 str = Intern(v.c_str());
            ((SelectorAddFn)(Base() + kSelectorAdd))(sel, &str, pl);
        }
        const int index = settings[i].current();
        *(u64*)(sel + SEL_OPTION_NAME) = Intern(settings[i].name);
        *(int*)(sel + SEL_DEFAULT_INDEX) = index;
        ((SelectorInitFn)(Base() + kSelectorInit))(sel);
        g_rows.push_back({pl, sel, Behavior(pl, kNavItemVtable), index, StartedIndex(i), false});
    }
    // navigation: a ring of our rows
    for (size_t i = 0; i < g_rows.size(); i++) {
        const u64 up = g_rows[(i + g_rows.size() - 1) % g_rows.size()].placeable;
        const u64 down = g_rows[(i + 1) % g_rows.size()].placeable;
        for (u64 b : {g_rows[i].selector, g_rows[i].nav})
            if (b) *(u64*)(b + NAV_UP) = up, *(u64*)(b + NAV_DOWN) = down;
    }
    LOG("settings: Video page with %zu rows", g_rows.size());
}

// Shows the current settings again (they may have changed since: F11, the settings file).
void RefreshRows() {
    const std::vector<Setting>& settings = VideoSettings();
    for (size_t i = 0; i < g_rows.size(); i++) {
        RowState& r = g_rows[i];
        const int index = settings[i].current();
        if (index != Read<int>(r.selector + SEL_CURRENT_INDEX))
            ((SelectorSetFn)(Base() + kSelectorSet))(r.selector, index, 0, false);
        r.last = index;
        r.marked = false;
    }
}

void UpdatePage(u64 project, u64 parent, u64 tmpl) {
    const bool video = g_video_button && Read<u64>(g_options_project + 0x50) == g_video_button &&
                       Read<u64>(g_video_button + PL_PROJECT) == g_options_project;
    if (video && project != g_page_project) BuildVideoPage(project, parent, tmpl);
    if (project != g_page_project) return;
    // our rows or the game's, whichever page this is
    u64 kids[48];
    const int n = ListItems(Read<u64>(parent + PL_CHILDREN), kids, 48);
    for (int i = 0; i < n; i++)
        if (IsOurRow(kids[i])) SetHidden(kids[i], !video);
        else if (video && Behavior(kids[i], kNavItemVtable)) SetHidden(kids[i], true);
    if (video && !g_video_page && !g_rows.empty()) {
        RefreshRows();
        ((SetFocusFn)(Base() + kSetFocus))(project, g_rows[0].placeable, 0xff);
    }
    g_video_page = video;
    if (!video) return;
    const std::vector<Setting>& settings = VideoSettings();
    for (size_t i = 0; i < g_rows.size(); i++) {
        RowState& r = g_rows[i];
        const int index = Read<int>(r.selector + SEL_CURRENT_INDEX);
        if (index != r.last && index >= 0 && index < (int)settings[i].labels.size()) {
            settings[i].apply(index);
            r.last = index;
            r.marked = false;
        }
        // restart rows: say so while the chosen value is not the running one
        if (settings[i].restart && index == r.last && index != r.started && !r.marked) {
            const std::string text = settings[i].labels[index] + " (restart)";
            SetText(Read<u64>(r.selector + NAV_TEXT_PLACEABLE), text.c_str());
            SetText(Read<u64>(r.selector + NAV_TEXT2_PLACEABLE), text.c_str());
            r.marked = true;
        }
    }
}

}  // namespace

void SettingsMenuUpdate(u64 gui_context) {
    static u64 last_ms = 0;
    const u64 now = GetTickCount64();
    if (now - last_ms < 15) return;  // the GUI polls its input several times a frame
    last_ms = now;
    u64 projects[64];
    const int n = ListItems(Read<u64>(gui_context + 0x58), projects, 64);
    for (int i = 0; i < n; i++) {
        const u64 project = projects[i];
        const u64 focus = Read<u64>(project + 0x50);
        if (!focus) continue;
        std::vector<Button> buttons;
        u64 list = 0, tmpl = 0;
        if (OptionsMenuButtons(focus, &buttons)) AddVideoButton(project, buttons);
        else if (FindPage(focus, &list, &tmpl)) UpdatePage(project, list, tmpl);
    }
}
