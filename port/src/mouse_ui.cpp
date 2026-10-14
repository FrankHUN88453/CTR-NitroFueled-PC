#include "mouse_ui.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include "linker.h"
#include "memory.h"
#include "settings_menu.h"

namespace {

// CGuiInput (the game's igGuiInput) vtable: slot 22 is the per-frame signal query
// (this, signal, controller mask) the GUI polls; hooking it gives us the GUI's own thread and
// its igGuiContext (CGuiInput::_context).
constexpr u64 kGuiInputVtable = 0x23411c0;
// igGuiPlaceable "is the point inside" test: (placeable, &pos, &padding, ignore hidden)
constexpr u64 kPlaceableContains = 0x24da50;

typedef float(SYSV* SignalFn)(u64, int, int);
typedef bool(SYSV* ContainsFn)(u64, const float*, const float*, bool);
SignalFn g_signal_orig;
u64 g_gui_input = 0;  // CGuiInput, captured on the GUI thread

SYSV float GuiSignal(u64 self, int signal, int mask) {
    g_gui_input = self;
    return g_signal_orig(self, signal, mask);
}

u64 Base() { return (u64)g_linker.eboot->base; }

template <class T>
bool Get(u64 addr, T* v) {
    return addr && GuestTryCopy(v, (const void*)addr, sizeof(T));
}

// igObjectList: count at +0xc, items at +0x20
int ListItems(u64 list, u64* items, int max) {
    int count = 0;
    u64 data = 0;
    if (!Get(list + 0xc, &count) || !Get(list + 0x20, &data) || !data) return 0;
    count = count < max ? count : max;
    for (int i = 0; i < count; i++)
        if (!Get(data + i * 8, &items[i])) return 0;
    return count;
}

struct Item {
    u64 placeable;
    float x0, y0, x1, y1;  // GUI pixels (the viewer size: 1920x1080, 3840x2160 in Pro mode; y down)
};

// Placeable bounds from its runtime state: world matrix, then local min / max at +0x40 / +0x4c.
bool Bounds(u64 pl, Item* it) {
    u64 st = 0;
    float s[21];
    if (!Get(pl + 0x100, &st) || !st || !GuestTryCopy(s, (const void*)st, sizeof(s))) return false;
    it->placeable = pl;
    it->x0 = s[12] + s[16] * s[0], it->x1 = s[12] + s[19] * s[0];
    it->y0 = s[13] + s[17] * s[5], it->y1 = s[13] + s[20] * s[5];
    if (it->x1 < it->x0) std::swap(it->x0, it->x1);
    if (it->y1 < it->y0) std::swap(it->y0, it->y1);
    return it->x1 - it->x0 > 1 && it->y1 - it->y0 > 1;
}

bool Hidden(u64 pl) {
    u32 bits = 0;
    return !Get(pl + 0xc, &bits) || (bits & 0x2000);
}

// The focused GUI item of controller 1 and the visible items next to it (its siblings): a list or
// grid when there are at least two, else count is 0 (a prompt or dialog with one choice).
struct Menu {
    u64 focus = 0;
    int count = 0;
    Item items[32];
};

bool FindMenu(Menu* m) {
    u64 ctx = 0, projects = 0, list[64];
    if (!Get(g_gui_input + 0x80, &ctx) || !Get(ctx + 0x58, &projects)) return false;
    int n = ListItems(projects, list, 64);
    int best_prio = -1;
    for (int i = 0; i < n; i++) {
        u64 pr = list[i], focus = 0, parent = 0, kids = 0;
        u8 bits = 0;
        int prio = 0, control = 0, disabled = 0;
        Get(pr + 0x34, &bits);
        Get(pr + 0x20, &prio);
        Get(pr + 0x44, &control);
        Get(pr + 0x48, &disabled);
        // enabled, can have focus, not deactivated, listens to controller 1
        if ((bits & 0x21) != 0x21 || (bits & 0x10) || disabled > 0 || !(control & 1)) continue;
        if (!Get(pr + 0x50, &focus) || !focus || prio < best_prio) continue;
        Menu cand;
        cand.focus = focus;
        u64 sib[32];
        int k = Get(focus + 0x40, &parent) && parent && Get(parent + 0x48, &kids) ? ListItems(kids, sib, 32) : 0;
        Item f;
        if (!Bounds(focus, &f)) k = 0;
        // the other entries of the list or grid: visible siblings about the size of the focused one
        const float fw = f.x1 - f.x0, fh = f.y1 - f.y0;
        for (int j = 0; j < k; j++) {
            Item& it = cand.items[cand.count];
            if (Hidden(sib[j]) || !Bounds(sib[j], &it)) continue;
            if (std::fabs(it.x1 - it.x0 - fw) <= fw * 0.3f && std::fabs(it.y1 - it.y0 - fh) <= fh * 0.3f) cand.count++;
        }
        if (cand.count < 2) cand.count = 0;
        if (prio == best_prio && m->count >= cand.count) continue;  // prefer the project with a list
        *m = cand;
        best_prio = prio;
    }
    return best_prio >= 0;
}

// Item under the point. Neighbouring item boxes overlap (the visible bars sit in their upper
// part), so the item whose top-left corner is the closest one above / left of the point wins.
const Item* ItemAt(const Menu& m, float x, float y) {
    const ContainsFn contains = (ContainsFn)(Base() + kPlaceableContains);
    static const float kNoPadding[2] = {0, 0};
    const float pos[2] = {x, y};
    const Item* best = nullptr;
    for (int i = 0; i < m.count; i++) {
        const Item& it = m.items[i];
        if (!contains(it.placeable, pos, kNoPadding, false)) continue;
        if (!best || it.x0 + it.y0 > best->x0 + best->y0) best = &it;
    }
    return best;
}

const Item* Find(const Menu& m, u64 pl) {
    for (int i = 0; i < m.count; i++)
        if (m.items[i].placeable == pl) return &m.items[i];
    return nullptr;
}

// navigation state
u64 g_target = 0;  // item the mouse wants focused
bool g_click_pending = false;
int g_steps = 0, g_wait = 0, g_confirm = 0, g_stuck = 0;
u64 g_last_focus = 0;
float g_last_x = -1, g_last_y = -1;
bool g_last_button = false;


}  // namespace

void MouseUiInstall() {
    u64* slot = (u64*)(Base() + kGuiInputVtable + 22 * 8);
    g_signal_orig = (SignalFn)*slot;
    DWORD old;
    VirtualProtect(slot, 8, PAGE_READWRITE, &old);
    *slot = (u64)&GuiSignal;
    VirtualProtect(slot, 8, old, &old);
    LOG("mouse: menu mouse support installed");
}

void MouseUiUpdate(const MouseUiInput& in, MouseUiOutput* out) {
    *out = {};
    const bool pressed = in.button && !g_last_button;
    g_last_button = in.button;
    if (g_confirm > 0) {  // hold the confirm for a few reads so the GUI sees a press
        out->buttons |= kMouseUiConfirm;
        out->ui = true;
        g_confirm--;
        return;
    }
    if (u64 gctx = 0; Get(g_gui_input + 0x80, &gctx)) SettingsMenuUpdate(gctx);
    Menu menu;
    out->ui = g_gui_input && FindMenu(&menu);
    const int state = out->ui ? 1 + menu.count : 0;
    static int last_state = -1, changes = 0;
    if (state != last_state && changes++ < 60) {
        last_state = state;
        LOG("mouse: %s", !out->ui     ? "no focused GUI item (gameplay)"
                         : menu.count ? ("menu with " + std::to_string(menu.count) + " items").c_str()
                                      : "single focused GUI item");
    }
    if (!out->ui || !menu.count || !in.cursor) {
        g_target = 0, g_click_pending = false;
        if (out->ui && pressed) {  // a prompt or dialog with one choice: any click is ✕
            out->buttons |= kMouseUiConfirm;
            g_confirm = 2;
        }
        return;  // gameplay: the mouse buttons act as bound (item, drift)
    }
    // GUI coordinates: the context's viewer size (1920x1080, 3840x2160 in PS4 Pro mode)
    float viewer[2] = {1920.0f, 1080.0f};
    u64 gctx = 0;
    if (!Get(g_gui_input + 0x80, &gctx) || !GuestTryCopy(viewer, (const void*)(gctx + 0x28), sizeof(viewer)) ||
        viewer[0] < 1 || viewer[1] < 1)
        viewer[0] = 1920.0f, viewer[1] = 1080.0f;
    const float x = in.x * viewer[0], y = in.y * viewer[1];
    const bool moved = std::fabs(x - g_last_x) + std::fabs(y - g_last_y) > 2.0f;
    g_last_x = x, g_last_y = y;
    const Item* hover = (moved || pressed) ? ItemAt(menu, x, y) : nullptr;
    if (hover && hover->placeable != g_target) {
        static int logged = 0;
        if (logged++ < 50)
            LOG("mouse: pointing at item %d of %d (%.0f,%.0f-%.0f,%.0f)", (int)(hover - menu.items) + 1, menu.count,
                hover->x0, hover->y0, hover->x1, hover->y1);
        g_target = hover->placeable;
        g_steps = 0, g_stuck = 0, g_wait = 0;
        g_click_pending = false;
    }
    if (pressed && hover) g_click_pending = true;
    if (!g_target) return;
    const Item* target = Find(menu, g_target);
    const Item* focus = Find(menu, menu.focus);
    if (!target || !focus) {
        g_target = 0, g_click_pending = false;
        return;
    }
    if (target == focus) {
        if (g_click_pending) {
            out->buttons |= kMouseUiConfirm;
            g_confirm = 2;
        }
        g_target = 0, g_click_pending = false;
        return;
    }
    if (g_wait > 0) {  // let the GUI react to the last step
        g_wait--;
        return;
    }
    // one d-pad step towards the target; give up when the focus stops following
    g_stuck = menu.focus == g_last_focus ? g_stuck + 1 : 0;
    g_last_focus = menu.focus;
    if (g_stuck > 3 || ++g_steps > 24) {
        g_target = 0, g_click_pending = false;
        return;
    }
    const float dx = (target->x0 + target->x1 - focus->x0 - focus->x1) * 0.5f;
    const float dy = (target->y0 + target->y1 - focus->y0 - focus->y1) * 0.5f;
    if (std::fabs(dy) >= std::fabs(dx) * 0.5f)
        out->buttons |= dy > 0 ? kMouseUiDown : kMouseUiUp;
    else
        out->buttons |= dx > 0 ? kMouseUiRight : kMouseUiLeft;
    g_wait = 2;
}
