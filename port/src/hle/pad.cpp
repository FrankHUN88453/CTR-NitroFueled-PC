// libScePad on the PC: XInput controllers plus keyboard and mouse.
//
// Keyboard / mouse bindings come from %APPDATA%/CTRNF/Controls.sav (INI text, written with the
// defaults below on first start): "<PS4 control> = <key>, <key>, ...". Keys and mouse buttons
// only count while the game window has the focus.
//
// PC conventions on top of the bindings: while a video plays, Space and the left mouse button
// skip it (Triangle; the prompt reads "Click to skip"); on the title screen they begin (Cross;
// "Click to begin"); in menus and dialogs the mouse points and clicks (mouse_ui.cpp) and its right
// button goes back (Circle). Only while driving do the mouse buttons act as bound (by default the
// left one uses the item, the right one jumps / drifts). The device used last picks the button
// prompt style.
//
// Local multiplayer (split screen): PS4 users 1-4 are players 1-4 and a user's pad handle is its
// user id. Player 1 is logged in from the start and plays with the keyboard and mouse plus the
// first controller (or with the keyboard and mouse alone: SeparateKeyboardPlayer in Controls.sav);
// every further XInput controller logs in the next player, whom the game then lets join. A player
// whose controller is unplugged stays logged in and reads as disconnected until one comes back.
#include "pad.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../config.h"
#include "../gfx/gfx.h"
#include "../hle.h"
#include "../mouse_ui.h"
#include "../prompts.h"
#include "savedata.h"
#include "videoout.h"

namespace {

enum : u32 {
    L3 = 0x2, R3 = 0x4, OPTIONS = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
    L2 = 0x100, R2 = 0x200, L1 = 0x400, R1 = 0x800, TRIANGLE = 0x1000, CIRCLE = 0x2000,
    CROSS = 0x4000, SQUARE = 0x8000, TOUCH_PAD = 0x100000,
};

#pragma pack(push, 1)
struct PadData {
    u32 buttons;
    u8 lx, ly, rx, ry;
    u8 l2, r2, pad0[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    u8 touch_num, touch_reserve[3];
    u32 touch_reserve1;
    u8 touch[2][8];
    u8 connected;
    u8 pad1[3];
    u64 timestamp;
    u8 extension[26];
    u8 connected_count;
    u8 reserve[2];
    u8 unique_len;
    u8 unique[12];
    u8 pad2[6];
};
#pragma pack(pop)
static_assert(offsetof(PadData, connected) == 76);
static_assert(offsetof(PadData, timestamp) == 80);
static_assert(sizeof(PadData) == 136);

struct ControllerInfo {
    float touch_pixel_density;
    u16 touch_res_x, touch_res_y;
    u8 dead_zone_left, dead_zone_right;
    u8 connection_type;
    u8 connected_count;
    u8 connected;
    u8 pad[3];
    u32 device_class;
    u8 reserve[8];
};

// ------------------------------------------------------------------ bindings
enum Action {
    A_CROSS, A_CIRCLE, A_SQUARE, A_TRIANGLE, A_L1, A_R1, A_L2, A_R2, A_L3, A_R3, A_OPTIONS, A_TOUCHPAD,
    A_DUP, A_DDOWN, A_DLEFT, A_DRIGHT, A_LUP, A_LDOWN, A_LLEFT, A_LRIGHT, A_RUP, A_RDOWN, A_RLEFT,
    A_RRIGHT, A_COUNT
};
const char* const kActionNames[A_COUNT] = {
    "Cross",       "Circle",      "Square",       "Triangle",      "L1",           "R1",
    "L2",          "R2",          "L3",           "R3",            "Options",      "TouchPad",
    "DpadUp",      "DpadDown",    "DpadLeft",     "DpadRight",     "LeftStickUp",  "LeftStickDown",
    "LeftStickLeft", "LeftStickRight", "RightStickUp", "RightStickDown", "RightStickLeft", "RightStickRight"};

constexpr int kWheelUp = 0x10001, kWheelDown = 0x10002;  // pseudo keys

struct KeyName {
    const char* name;
    int vk;
};
const KeyName kKeys[] = {
    {"Space", VK_SPACE},     {"Enter", VK_RETURN},       {"Escape", VK_ESCAPE},     {"Esc", VK_ESCAPE},
    {"Backspace", VK_BACK},  {"Tab", VK_TAB},            {"Shift", VK_SHIFT},       {"LShift", VK_LSHIFT},
    {"RShift", VK_RSHIFT},   {"Ctrl", VK_CONTROL},       {"LCtrl", VK_LCONTROL},    {"RCtrl", VK_RCONTROL},
    {"Alt", VK_MENU},        {"LAlt", VK_LMENU},         {"RAlt", VK_RMENU},        {"Up", VK_UP},
    {"Down", VK_DOWN},       {"Left", VK_LEFT},          {"Right", VK_RIGHT},       {"Insert", VK_INSERT},
    {"Delete", VK_DELETE},   {"Home", VK_HOME},          {"End", VK_END},           {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT},   {"CapsLock", VK_CAPITAL},   {"Comma", VK_OEM_COMMA},   {"Period", VK_OEM_PERIOD},
    {"Slash", VK_OEM_2},     {"Semicolon", VK_OEM_1},    {"Apostrophe", VK_OEM_7},  {"LBracket", VK_OEM_4},
    {"RBracket", VK_OEM_6},  {"Backslash", VK_OEM_5},    {"Minus", VK_OEM_MINUS},   {"Equals", VK_OEM_PLUS},
    {"Grave", VK_OEM_3},     {"NumpadEnter", VK_RETURN}, {"NumpadPlus", VK_ADD},    {"NumpadMinus", VK_SUBTRACT},
    {"MouseLeft", VK_LBUTTON}, {"MouseRight", VK_RBUTTON}, {"MouseMiddle", VK_MBUTTON},
    {"MouseX1", VK_XBUTTON1}, {"MouseX2", VK_XBUTTON2},  {"WheelUp", kWheelUp},     {"WheelDown", kWheelDown},
};

const char kDefaultControls[] =
    "; Crash Team Racing Nitro-Fueled - keyboard and mouse controls\n"
    "; <PS4 control> = keys, comma separated. Key names: A-Z, 0-9, F1-F12, Space, Enter, Escape,\n"
    "; Backspace, Tab, Shift/LShift/RShift, Ctrl/LCtrl/RCtrl, Alt/LAlt/RAlt, Up, Down, Left, Right,\n"
    "; Insert, Delete, Home, End, PageUp, PageDown, Numpad0-Numpad9, Comma, Period, Slash, Semicolon,\n"
    "; Apostrophe, LBracket, RBracket, Backslash, Minus, Equals, Grave,\n"
    "; MouseLeft, MouseRight, MouseMiddle, MouseX1, MouseX2, WheelUp, WheelDown\n"
    "; In menus the mouse points and clicks and its right button goes back, whatever is bound here.\n"
    "[Controls]\n"
    "; accelerate / confirm\n"
    "Cross = W, Enter\n"
    "; brake / reverse\n"
    "Square = S\n"
    "; use item / back\n"
    "Circle = MouseLeft, LShift, Backspace\n"
    "; jump / power slide (drift)\n"
    "R1 = MouseRight, Space\n"
    "L1 = E\n"
    "R2 = R\n"
    "L2 = Q\n"
    "; rear view / menu options\n"
    "Triangle = F, MouseMiddle\n"
    "; pause\n"
    "Options = Escape\n"
    "; adventure map\n"
    "TouchPad = Tab\n"
    "L3 = C\n"
    "R3 = V\n"
    "DpadUp = Up, WheelUp\n"
    "DpadDown = Down, WheelDown\n"
    "DpadLeft = Left, A\n"
    "DpadRight = Right, D\n"
    "LeftStickUp = Up\n"
    "LeftStickDown = Down\n"
    "LeftStickLeft = Left, A\n"
    "LeftStickRight = Right, D\n"
    "RightStickUp = I\n"
    "RightStickDown = K\n"
    "RightStickLeft = J\n"
    "RightStickRight = L\n";

// added to the files of earlier builds too
const char kSplitScreenControls[] =
    "\n"
    "[SplitScreen]\n"
    "; 0 = the keyboard and mouse play together with the first controller as player 1,\n"
    "; 1 = the keyboard and mouse are player 1 on their own and every controller is a further player\n"
    "SeparateKeyboardPlayer = 0\n";

int ParseKey(std::string k) {
    while (!k.empty() && isspace((u8)k.back())) k.pop_back();
    size_t b = 0;
    while (b < k.size() && isspace((u8)k[b])) b++;
    k = k.substr(b);
    if (k.empty()) return 0;
    for (const KeyName& n : kKeys)
        if (_stricmp(n.name, k.c_str()) == 0) return n.vk;
    if (k.size() == 1 && isalnum((u8)k[0])) return toupper((u8)k[0]);
    if ((k[0] == 'F' || k[0] == 'f') && k.size() <= 3 && isdigit((u8)k[1])) {
        int n = atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (_strnicmp(k.c_str(), "Numpad", 6) == 0 && k.size() == 7 && isdigit((u8)k[6])) return VK_NUMPAD0 + (k[6] - '0');
    LOG("controls: unknown key '%s'", k.c_str());
    return 0;
}

std::vector<int> g_bind[A_COUNT];

enum Device { kGamepad, kKeyboardMouse };
std::atomic<int> g_device{-1};  // used last; -1 until the first read
std::atomic<bool> g_menu_context{true};
bool g_separate_keyboard = false;  // SeparateKeyboardPlayer

bool IsMouseButton(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

void LoadControls() {
    namespace fs = std::filesystem;
    fs::path p = UserRoot() / "Controls.sav";
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::ofstream(p) << kDefaultControls;
    }
    std::ifstream f(p);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    // the first defaults had the mouse buttons on Cross / Circle; now the left one uses the item and
    // the right one drifts (in menus the mouse works on its own): update untouched files
    static const char* const kOld[] = {"Cross = W, Enter, MouseLeft\n", "Circle = LShift, Backspace, MouseRight\n",
                                       "R1 = Space\n"};
    if (text.find(kOld[0]) != std::string::npos && text.find(kOld[1]) != std::string::npos &&
        text.find(kOld[2]) != std::string::npos) {
        text = kDefaultControls;
        std::ofstream(p) << text;
        LOG("controls: updated %s to the new mouse defaults (left click: item, right click: drift)",
            p.string().c_str());
    }
    if (text.empty()) text = kDefaultControls;
    if (text.find("SeparateKeyboardPlayer") == std::string::npos) {
        text += kSplitScreenControls;
        std::ofstream(p) << text;
    }
    std::istringstream in(text);
    std::string line;
    int bound = 0;
    while (std::getline(in, line)) {
        size_t c = line.find(';');
        if (c != std::string::npos) line.resize(c);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        while (!name.empty() && isspace((u8)name.back())) name.pop_back();
        while (!name.empty() && isspace((u8)name[0])) name.erase(0, 1);
        if (_stricmp(name.c_str(), "SeparateKeyboardPlayer") == 0) {
            g_separate_keyboard = atoi(line.c_str() + eq + 1) != 0;
            continue;
        }
        int a = -1;
        for (int i = 0; i < A_COUNT; i++)
            if (_stricmp(kActionNames[i], name.c_str()) == 0) a = i;
        if (a < 0) {
            LOG("controls: unknown control '%s'", name.c_str());
            continue;
        }
        g_bind[a].clear();
        std::istringstream keys(line.substr(eq + 1));
        std::string k;
        while (std::getline(keys, k, ','))
            if (int vk = ParseKey(k)) g_bind[a].push_back(vk), bound++;
    }
    LOG("controls: %d keyboard/mouse bindings from %s%s", bound, p.string().c_str(),
        g_separate_keyboard ? " (the keyboard and mouse are a player of their own)" : "");
}

// ------------------------------------------------------------------ test scripts
// time of the test scripts, from the first input read
double TestClock() {
    static const ULONGLONG start = GetTickCount64();
    return (GetTickCount64() - start) / 1000.0;
}

// test hook: CTR_TEST_MOUSE="<seconds>:<x>,<y>[,c];..." puts the cursor at GUI pixel x, y (1920x1080)
// from that time on; ",c" clicks there (held 100 ms)
void TestMouse(MouseUiInput* mi) {
    struct Step {
        double t;
        float x, y;
        bool click;
    };
    static const std::vector<Step> steps = [] {
        std::vector<Step> v;
        char buf[1024] = {};
        if (!GetEnvironmentVariableA("CTR_TEST_MOUSE", buf, sizeof(buf))) return v;
        for (char* tok = strtok(buf, ";"); tok; tok = strtok(nullptr, ";")) {
            Step st{};
            char c = 0;
            if (sscanf(tok, "%lf:%f,%f,%c", &st.t, &st.x, &st.y, &c) >= 3) st.click = c == 'c', v.push_back(st);
        }
        return v;
    }();
    if (steps.empty()) return;
    const double t = TestClock();
    for (const Step& st : steps)
        if (t >= st.t) {
            mi->cursor = true;
            mi->x = st.x / 1920.0f, mi->y = st.y / 1080.0f;
            mi->button = st.click && t < st.t + 0.1;
        }
}

void EnsureControls() {
    static const bool loaded = (LoadControls(), true);
    (void)loaded;
}

constexpr int kPlayers = 4;

// test hook: CTR_TEST_PAD<n>="<s>[-<s>]:<control>[+<control>...];..." holds controls of player n
// (names as in Controls.sav) from that time on, for 150 ms or up to the second time. Players 2-4
// get a controller of their own, connected from the start; for player 1 they add to the keys.
struct TestStep {
    double t0, t1;
    u32 actions;  // 1 << Action
};
struct TestPad {
    bool present = false;
    std::vector<TestStep> steps;
};
const TestPad& TestPadOf(int player) {
    static const std::vector<TestPad> pads = [] {
        std::vector<TestPad> v(kPlayers);
        for (int p = 0; p < kPlayers; p++) {
            char var[16], buf[4096] = {};
            snprintf(var, sizeof(var), "CTR_TEST_PAD%d", p + 1);
            if (!GetEnvironmentVariableA(var, buf, sizeof(buf))) continue;
            v[p].present = true;
            std::istringstream steps(buf);
            std::string tok;
            while (std::getline(steps, tok, ';')) {
                const size_t colon = tok.find(':');
                if (colon == std::string::npos) continue;
                TestStep st{};
                const std::string when = tok.substr(0, colon);
                const size_t dash = when.find('-', 1);
                st.t0 = atof(when.c_str());
                st.t1 = dash != std::string::npos ? atof(when.c_str() + dash + 1) : st.t0 + 0.15;
                std::istringstream names(tok.substr(colon + 1));
                std::string name;
                while (std::getline(names, name, '+'))
                    for (int a = 0; a < A_COUNT; a++)
                        if (_stricmp(kActionNames[a], name.c_str()) == 0) st.actions |= 1u << a;
                v[p].steps.push_back(st);
            }
        }
        return v;
    }();
    return pads[player];
}

u32 TestActions(int player) {
    const TestPad& tp = TestPadOf(player);
    if (tp.steps.empty()) return 0;
    const double t = TestClock();
    u32 mask = 0;
    for (const TestStep& st : tp.steps)
        if (t >= st.t0 && t < st.t1) mask |= st.actions;
    return mask;
}

// ------------------------------------------------------------------ players
struct Player {
    bool logged_in = false;
    bool test = false;  // scripted controller (CTR_TEST_PAD<n>)
    int slot = -1;      // XInput controller
};
std::mutex g_players_mutex;
Player g_players[kPlayers];
std::vector<int> g_logins;  // users whose login event the game has not seen yet
std::atomic<u64> g_slot_retry[XUSER_MAX_COUNT];
std::atomic<bool> g_slot_connected[XUSER_MAX_COUNT];

// XInputGetState on an empty slot is slow, so an empty slot is polled again only after a second.
bool PollSlot(int slot, XINPUT_STATE* xs) {
    if (slot < 0 || slot >= XUSER_MAX_COUNT) return false;
    const u64 now = GetTickCount64();
    if (now < g_slot_retry[slot]) return false;
    const bool ok = XInputGetState(slot, xs) == ERROR_SUCCESS;
    if (!ok) g_slot_retry[slot] = now + 1000;
    g_slot_connected[slot] = ok;
    return ok;
}

void LogIn(int p) {  // with g_players_mutex held
    g_players[p].logged_in = true;
    g_logins.push_back(p + 1);
}

void InitPlayers() {  // with g_players_mutex held
    static bool done = false;
    if (done) return;
    done = true;
    EnsureControls();
    LogIn(0);
    for (int p = 1; p < kPlayers; p++)
        if (TestPadOf(p).present) {
            g_players[p].test = true;
            LogIn(p);
            LOG("pad: player %d has a scripted controller", p + 1);
        }
}

// Gives the controllers that turned up to players, logging in the players new to the game.
void AssignControllers() {
    std::lock_guard lk(g_players_mutex);
    InitPlayers();
    static u64 next = 0;
    const u64 now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    for (int s = 0; s < XUSER_MAX_COUNT; s++) {
        bool taken = false;
        for (const Player& pl : g_players) taken |= pl.slot == s;
        XINPUT_STATE xs;
        if (taken || !PollSlot(s, &xs)) continue;
        auto gone = [](const Player& pl) { return pl.slot < 0 || !g_slot_connected[pl.slot]; };
        // player 1 first (unless the keyboard plays on its own), then a player without a
        // controller, then one whose controller is gone
        int p = !g_separate_keyboard && gone(g_players[0]) ? 0 : -1;
        for (int i = 1; i < kPlayers && p < 0; i++)
            if (!g_players[i].test && g_players[i].slot < 0) p = i;
        for (int i = 1; i < kPlayers && p < 0; i++)
            if (!g_players[i].test && gone(g_players[i])) p = i;
        if (p < 0) {
            static bool logged = false;
            if (!logged) LOG("pad: controller %d: every player has a controller", s + 1), logged = true;
            continue;
        }
        g_players[p].slot = s;
        LOG("pad: controller %d is player %d", s + 1, p + 1);
        if (!g_players[p].logged_in) LogIn(p);
    }
}

int SlotOf(int p) {
    std::lock_guard lk(g_players_mutex);
    return g_players[p].slot;
}

bool Connected(int p) {
    std::lock_guard lk(g_players_mutex);
    const Player& pl = g_players[p];
    return p == 0 || pl.test || (pl.slot >= 0 && g_slot_connected[pl.slot]);
}

// ------------------------------------------------------------------ reading
u8 Axis(SHORT v, bool invert) {
    int x = (int)v / 256 + 128;
    if (invert) x = 255 - x;
    return (u8)(x < 0 ? 0 : x > 255 ? 255 : x);
}

void Blank(PadData* d, bool connected) {
    memset(d, 0, sizeof(*d));
    d->lx = d->ly = d->rx = d->ry = 128;
    d->orientation[3] = 1.0f;
    d->connected = connected;
    d->connected_count = 1;
    d->timestamp = GetTickCount64() * 1000;
}

// The PS4 controls of an XInput controller; true while it is in use.
bool FromXInput(const XINPUT_GAMEPAD& g, PadData* d) {
    const WORD b = g.wButtons;
    if (b & XINPUT_GAMEPAD_A) d->buttons |= CROSS;
    if (b & XINPUT_GAMEPAD_B) d->buttons |= CIRCLE;
    if (b & XINPUT_GAMEPAD_X) d->buttons |= SQUARE;
    if (b & XINPUT_GAMEPAD_Y) d->buttons |= TRIANGLE;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) d->buttons |= L1;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) d->buttons |= R1;
    if (b & XINPUT_GAMEPAD_START) d->buttons |= OPTIONS;
    if (b & XINPUT_GAMEPAD_BACK) d->buttons |= TOUCH_PAD;
    if (b & XINPUT_GAMEPAD_LEFT_THUMB) d->buttons |= L3;
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB) d->buttons |= R3;
    if (b & XINPUT_GAMEPAD_DPAD_UP) d->buttons |= UP;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) d->buttons |= DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) d->buttons |= LEFT;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) d->buttons |= RIGHT;
    d->l2 = g.bLeftTrigger;
    d->r2 = g.bRightTrigger;
    if (d->l2 > 30) d->buttons |= L2;
    if (d->r2 > 30) d->buttons |= R2;
    d->lx = Axis(g.sThumbLX, false);
    d->ly = Axis(g.sThumbLY, true);
    d->rx = Axis(g.sThumbRX, false);
    d->ry = Axis(g.sThumbRY, true);
    auto tilted = [](SHORT v) { return v > 12000 || v < -12000; };
    return b || g.bLeftTrigger > 40 || g.bRightTrigger > 40 || tilted(g.sThumbLX) || tilted(g.sThumbLY) ||
           tilted(g.sThumbRX) || tilted(g.sThumbRY);
}

// The PS4 buttons and stick deflections of keyboard actions (mask bits 1 << Action).
void ApplyActions(PadData* d, u32 mask) {
    static const u32 kButton[A_DRIGHT + 1] = {CROSS, CIRCLE, SQUARE, TRIANGLE, L1,   R1,    L2,   R2,
                                              L3,    R3,     OPTIONS, TOUCH_PAD, UP, DOWN, LEFT, RIGHT};
    auto on = [&](Action a) { return (mask >> a & 1) != 0; };
    for (int a = A_CROSS; a <= A_DRIGHT; a++)
        if (on((Action)a)) d->buttons |= kButton[a];
    if (on(A_L2)) d->l2 = 255;
    if (on(A_R2)) d->r2 = 255;
    if (on(A_LUP)) d->ly = 0;
    if (on(A_LDOWN)) d->ly = 255;
    if (on(A_LLEFT)) d->lx = 0;
    if (on(A_LRIGHT)) d->lx = 255;
    if (on(A_RUP)) d->ry = 0;
    if (on(A_RDOWN)) d->ry = 255;
    if (on(A_RLEFT)) d->rx = 0;
    if (on(A_RRIGHT)) d->rx = 255;
}

// Players 2-4: their controller only.
void ReadOther(int p, PadData* d) {
    bool test;
    int slot;
    {
        std::lock_guard lk(g_players_mutex);
        test = g_players[p].test, slot = g_players[p].slot;
    }
    XINPUT_STATE xs;
    const bool pad = PollSlot(slot, &xs);
    Blank(d, pad || test);
    if (pad) FromXInput(xs.Gamepad, d);
    if (test) ApplyActions(d, TestActions(p));
}

// Player 1: keyboard and mouse, plus a controller unless SeparateKeyboardPlayer is set.
void ReadPlayer1(PadData* d) {
    EnsureControls();
    Blank(d, true);
    XINPUT_STATE xs;
    const bool pad_connected = PollSlot(SlotOf(0), &xs);
    if (g_device < 0) g_device = pad_connected ? kGamepad : kKeyboardMouse;
    if (pad_connected && FromXInput(xs.Gamepad, d)) g_device = kGamepad;

    // test hook: CTR_AUTOPRESS=1 taps Cross for 150 ms every 3 s; =2 also taps R1 twice a second
    // (pages through the license agreement); =3 taps the Space key instead of Cross
    static const int autopress = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("CTR_AUTOPRESS", v, sizeof(v)) ? atoi(v) : 0;
    }();
    u64 now = GetTickCount64();
    if (autopress && autopress != 3 && now % 3000 < 150) d->buttons |= CROSS;
    const bool test_space = autopress == 3 && now % 3000 < 150;
    if (autopress == 2 && now % 500 >= 250 && now % 500 < 350 && now % 3000 >= 500) d->buttons |= R1;

    // wheel notches become short d-pad / button taps (held for ~2 reads)
    static int wheel_hold = 0, wheel_dir = 0;
    if (int w = VideoOutTakeWheel()) {
        wheel_dir = w > 0 ? 1 : -1;
        wheel_hold = 3;
    }
    bool wheel_up = wheel_hold > 0 && wheel_dir > 0, wheel_down = wheel_hold > 0 && wheel_dir < 0;
    if (wheel_hold > 0) wheel_hold--;

    // PC prompt contexts: a video on screen (skip) and the title screen (begin)
    const bool video = GfxVideoPlaying();
    const bool title = !video && !SaveDataMountCalled();
    static int last_context = 0;
    if (int context = video ? 1 : title ? 2 : 3; context != last_context) {
        static const char* const kContext[] = {"", "video (Space / left click skip)", "title screen (Space begins)",
                                               "game"};
        LOG("pad: context %s", kContext[context]);
        last_context = context;
    }
    PromptTextUpdate(PadPromptStyle() == PromptStyle::Keyboard);

    MouseUiInput test{};  // scripted mouse (CTR_TEST_MOUSE); its click counts as the left button
    TestMouse(&test);
    // scripted test runs (CTR_TEST_PAD1 / CTR_TEST_MOUSE) ignore the real keyboard and mouse
    static const bool scripted = GetEnvironmentVariableA("CTR_TEST_PAD1", nullptr, 0) > 0 ||
                                 GetEnvironmentVariableA("CTR_TEST_MOUSE", nullptr, 0) > 0;
    const bool focused = !scripted && VideoOutHasFocus(), mouse_ok = focused && VideoOutCursorInside();
    auto down = [&](int vk) {
        if ((vk == VK_SPACE && test_space) || (vk == VK_LBUTTON && test.button)) return true;
        if (!focused) return false;
        if (vk == kWheelUp) return wheel_up;
        if (vk == kWheelDown) return wheel_down;
        if (IsMouseButton(vk) && !mouse_ok) return false;
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    // menus and dialogs: pointing at an item focuses it, a click chooses it (mouse_ui.cpp)
    MouseUiOutput mouse{};
    if (!video && !title) {
        MouseUiInput mi{};
        mi.cursor = mouse_ok && VideoOutCursorInPicture(&mi.x, &mi.y);
        if (test.cursor) mi.cursor = true, mi.x = test.x, mi.y = test.y;
        mi.button = mi.cursor && down(VK_LBUTTON);
        MouseUiUpdate(mi, &mouse);
    }
    // keys and mouse buttons a context takes over do not act as bound meanwhile; the mouse buttons
    // act as bound (item, drift) only while driving
    const bool mouse_ui = video || title || mouse.ui;
    g_menu_context = mouse_ui;
    auto taken = [&](int vk) {
        return (vk == VK_SPACE && (video || title)) || (mouse_ui && (vk == VK_LBUTTON || vk == VK_RBUTTON));
    };
    bool any_key = false;
    auto active = [&](Action a) {
        for (int vk : g_bind[a])
            if (!taken(vk) && down(vk)) return any_key = true;
        return false;
    };
    if (video && (down(VK_SPACE) || down(VK_LBUTTON))) d->buttons |= TRIANGLE, any_key = true;
    if (title && (down(VK_SPACE) || down(VK_LBUTTON))) d->buttons |= CROSS, any_key = true;
    if (mouse.ui && down(VK_RBUTTON)) d->buttons |= CIRCLE, any_key = true;  // back
    if (mouse.buttons) {
        static const u32 kMouseButtons[] = {UP, DOWN, LEFT, RIGHT, CROSS};
        for (int i = 0; i < 5; i++)
            if (mouse.buttons & (1u << i)) d->buttons |= kMouseButtons[i];
        any_key = true;
    }
    u32 keys = TestActions(0);
    for (int a = 0; a < A_COUNT; a++)
        if (active((Action)a)) keys |= 1u << a;
    ApplyActions(d, keys);
    if (any_key) g_device = kKeyboardMouse;
}

// pad handle: the user id, 1-4
int PlayerOf(int handle) { return handle >= 1 && handle <= kPlayers ? handle - 1 : -1; }

void Read(int p, PadData* d) {
    if (p == 0)
        ReadPlayer1(d);
    else
        ReadOther(p, d);
}

constexpr int kInvalidArg = (int)0x80920001, kInvalidHandle = (int)0x80920003;
SYSV int scePadInit() { return 0; }
SYSV int scePadOpen(int user, int type, int index, const void*) {
    LOG("scePadOpen(user %d, type %d, index %d)", user, type, index);
    return PlayerOf(user) >= 0 ? user : kInvalidArg;
}
SYSV int scePadClose(int) { return 0; }
SYSV int scePadReadState(int h, PadData* d) {
    const int p = PlayerOf(h);
    if (p < 0) return kInvalidHandle;
    Read(p, d);
    return 0;
}
SYSV int scePadRead(int h, PadData* d, int num) {
    const int p = PlayerOf(h);
    if (p < 0) return kInvalidHandle;
    if (num < 1) return 0;
    Read(p, d);
    return 1;
}
SYSV int scePadGetControllerInformation(int h, ControllerInfo* info) {
    const int p = PlayerOf(h);
    if (p < 0) return kInvalidHandle;
    memset(info, 0, sizeof(*info));
    info->touch_pixel_density = 44.86f;
    info->touch_res_x = 1920;
    info->touch_res_y = 943;
    info->dead_zone_left = info->dead_zone_right = 13;
    info->connected = Connected(p);
    info->connected_count = 1;
    return 0;
}
SYSV int scePadSetVibration(int h, const u8* v) {
    const int p = PlayerOf(h);
    if (p < 0) return kInvalidHandle;
    XINPUT_VIBRATION vib{(WORD)(v[0] * 257), (WORD)(v[1] * 257)};
    if (int slot = SlotOf(p); slot >= 0) XInputSetState(slot, &vib);
    return 0;
}
SYSV int scePadSetLightBar(int, const void*) { return 0; }
SYSV int scePadResetLightBar(int) { return 0; }
HLE("libScePad", scePadInit);
HLE("libScePad", scePadOpen);
HLE("libScePad", scePadClose);
HLE("libScePad", scePadReadState);
HLE("libScePad", scePadRead);
HLE("libScePad", scePadGetControllerInformation);
HLE("libScePad", scePadSetVibration);
HLE("libScePad", scePadSetLightBar);
HLE("libScePad", scePadResetLightBar);

}  // namespace

int PadTakeLogin() {
    AssignControllers();
    std::lock_guard lk(g_players_mutex);
    if (g_logins.empty()) return 0;
    const int user = g_logins.front();
    g_logins.erase(g_logins.begin());
    return user;
}

void PadLoginUsers(int ids[4]) {
    std::lock_guard lk(g_players_mutex);
    InitPlayers();
    for (int p = 0; p < kPlayers; p++) ids[p] = g_players[p].logged_in ? p + 1 : -1;
}

PromptStyle PadPromptStyle() {
    // 0 = follow the device used last, 1 = Xbox, 2 = PlayStation, 3 = keyboard/mouse
    static const int fixed = ConfigInt("ButtonPrompts", 0);
    if (fixed == 1) return PromptStyle::Xbox;
    if (fixed == 2) return PromptStyle::PlayStation;
    if (fixed == 3) return PromptStyle::Keyboard;
    return g_device == kKeyboardMouse ? PromptStyle::Keyboard : PromptStyle::Xbox;
}

bool PadMenuContext() { return g_menu_context; }

int PadPromptKey(PadControl c, int dir, bool menu) {
    EnsureControls();
    if (menu && c == PadControl::Cross) return VK_LBUTTON;
    if (menu && c == PadControl::Circle) return VK_RBUTTON;
    static const Action kButtons[] = {A_CROSS, A_CIRCLE, A_SQUARE, A_TRIANGLE, A_L1, A_R1, A_L2, A_R2};
    static const Action kSticks[2][4] = {{A_LUP, A_LLEFT, A_LDOWN, A_LRIGHT}, {A_RUP, A_RLEFT, A_RDOWN, A_RRIGHT}};
    if (c == PadControl::LeftStick || c == PadControl::RightStick) {
        for (int vk : g_bind[kSticks[c == PadControl::RightStick][dir & 3]])
            if (!IsMouseButton(vk) && vk < 0x10000) return vk;
        return 0;
    }
    const std::vector<int>& keys = g_bind[kButtons[(int)c]];
    for (int vk : keys)  // in menus the left / right buttons are taken (select / back)
        if (IsMouseButton(vk) && !(menu && (vk == VK_LBUTTON || vk == VK_RBUTTON))) return vk;
    for (int vk : keys)
        if (vk < 0x10000) return vk;
    return 0;
}

const char* PadKeyLabel(int vk, char* buf, size_t size) {
    static const KeyName kLabels[] = {
        {"SHIFT", VK_SHIFT},   {"SHIFT", VK_LSHIFT},  {"SHIFT", VK_RSHIFT}, {"CTRL", VK_CONTROL},
        {"CTRL", VK_LCONTROL}, {"CTRL", VK_RCONTROL}, {"ALT", VK_MENU},     {"ALT", VK_LMENU},
        {"ALT", VK_RMENU},     {"ESC", VK_ESCAPE},    {"BKSP", VK_BACK},    {"ENTER", VK_RETURN},
        {"SPACE", VK_SPACE},   {"TAB", VK_TAB},       {"INS", VK_INSERT},   {"DEL", VK_DELETE},
        {"PGUP", VK_PRIOR},    {"PGDN", VK_NEXT},     {"CAPS", VK_CAPITAL}, {"LMB", VK_LBUTTON},
        {"RMB", VK_RBUTTON},   {"MMB", VK_MBUTTON},   {"M4", VK_XBUTTON1},  {"M5", VK_XBUTTON2},
        {"\xE2\x86\x91", VK_UP}, {"\xE2\x86\x93", VK_DOWN}, {"\xE2\x86\x90", VK_LEFT}, {"\xE2\x86\x92", VK_RIGHT},
    };
    for (const KeyName& k : kLabels)
        if (k.vk == vk) return k.name;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(buf, size, "%c", vk);
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, size, "F%d", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, size, "NUM%d", vk - VK_NUMPAD0);
        return buf;
    }
    for (const KeyName& k : kKeys)
        if (k.vk == vk) {
            size_t i = 0;
            for (; k.name[i] && i + 1 < size; i++) buf[i] = (char)toupper((u8)k.name[i]);
            buf[i] = 0;
            return buf;
        }
    return "?";
}
