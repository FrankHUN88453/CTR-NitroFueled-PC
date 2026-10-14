// Linux: the Win32 / XInput functions ctr_pc uses for input, screen size and message boxes, on SDL2.
// Keyboard and mouse state come from SDL's event pumping on the window thread (videoout_sdl.cpp).
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <mutex>
#include <string>

#include "sdl_util.h"

bool SdlEnsure(Uint32 flags) {
    static std::mutex m;
    std::lock_guard lk(m);
    if (SDL_WasInit(flags) == flags) return true;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    return SDL_InitSubSystem(flags) == 0;
}

namespace {

SDL_Scancode ScancodeOf(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk < VK_F1 + 12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_F1 + 12 && vk < VK_F1 + 24) return (SDL_Scancode)(SDL_SCANCODE_F13 + (vk - VK_F1 - 12));
    if (vk >= VK_NUMPAD0 + 1 && vk <= VK_NUMPAD0 + 9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD0 - 1));
    switch (vk) {
    case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
    case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_LMENU: return SDL_SCANCODE_LALT;
    case VK_RMENU: return SDL_SCANCODE_RALT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_INSERT: return SDL_SCANCODE_INSERT;
    case VK_DELETE: return SDL_SCANCODE_DELETE;
    case VK_HOME: return SDL_SCANCODE_HOME;
    case VK_END: return SDL_SCANCODE_END;
    case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
    case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
    case VK_CAPITAL: return SDL_SCANCODE_CAPSLOCK;
    case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
    case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
    case VK_OEM_2: return SDL_SCANCODE_SLASH;
    case VK_OEM_1: return SDL_SCANCODE_SEMICOLON;
    case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE;
    case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
    case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
    case VK_OEM_5: return SDL_SCANCODE_BACKSLASH;
    case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
    case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
    case VK_OEM_3: return SDL_SCANCODE_GRAVE;
    case VK_ADD: return SDL_SCANCODE_KP_PLUS;
    case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
    case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
    case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
    case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

// XInput slots: the controllers in the order they were connected
std::mutex g_pads_mutex;
SDL_GameController* g_pads[XUSER_MAX_COUNT];
Uint64 g_pads_checked = 0;

void RefreshPads() {
    const Uint64 now = SDL_GetTicks64();
    if (now - g_pads_checked < 1000 && g_pads_checked) return;
    g_pads_checked = now;
    for (auto& p : g_pads)
        if (p && !SDL_GameControllerGetAttached(p)) SDL_GameControllerClose(p), p = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool open = false;
        for (auto* p : g_pads)
            if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) open = true;
        if (open) continue;
        for (auto& p : g_pads)
            if (!p) {
                p = SDL_GameControllerOpen(i);
                break;
            }
    }
}

SHORT Axis(SDL_GameController* c, SDL_GameControllerAxis a, bool invert) {
    int v = SDL_GameControllerGetAxis(c, a);
    if (invert) v = -v - 1;
    return (SHORT)std::max(-32768, std::min(32767, v));
}

}  // namespace

SHORT GetAsyncKeyState(int vk) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) {
        const Uint32 b = SDL_GetMouseState(nullptr, nullptr);
        const int sdl = vk == VK_LBUTTON ? SDL_BUTTON_LEFT : vk == VK_RBUTTON ? SDL_BUTTON_RIGHT
                        : vk == VK_MBUTTON ? SDL_BUTTON_MIDDLE : vk == VK_XBUTTON1 ? SDL_BUTTON_X1 : SDL_BUTTON_X2;
        return (b & SDL_BUTTON(sdl)) ? (SHORT)0x8000 : 0;
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    auto down = [&](SDL_Scancode s) { return s != SDL_SCANCODE_UNKNOWN && keys[s]; };
    bool d;
    switch (vk) {
    case VK_SHIFT: d = down(SDL_SCANCODE_LSHIFT) || down(SDL_SCANCODE_RSHIFT); break;
    case VK_CONTROL: d = down(SDL_SCANCODE_LCTRL) || down(SDL_SCANCODE_RCTRL); break;
    case VK_MENU: d = down(SDL_SCANCODE_LALT) || down(SDL_SCANCODE_RALT); break;
    case VK_RETURN: d = down(SDL_SCANCODE_RETURN) || down(SDL_SCANCODE_KP_ENTER); break;
    default: d = down(ScancodeOf(vk)); break;
    }
    return d ? (SHORT)0x8000 : 0;
}

DWORD XInputGetState(DWORD user, XINPUT_STATE* state) {
    if (user >= XUSER_MAX_COUNT || !SdlEnsure(SDL_INIT_GAMECONTROLLER)) return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lk(g_pads_mutex);
    RefreshPads();
    SDL_GameController* c = g_pads[user];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;
    memset(state, 0, sizeof(*state));
    static const std::pair<SDL_GameControllerButton, WORD> kButtons[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
        {SDL_CONTROLLER_BUTTON_START, XINPUT_GAMEPAD_START},
        {SDL_CONTROLLER_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {SDL_CONTROLLER_BUTTON_A, XINPUT_GAMEPAD_A},
        {SDL_CONTROLLER_BUTTON_B, XINPUT_GAMEPAD_B},
        {SDL_CONTROLLER_BUTTON_X, XINPUT_GAMEPAD_X},
        {SDL_CONTROLLER_BUTTON_Y, XINPUT_GAMEPAD_Y},
    };
    XINPUT_GAMEPAD& g = state->Gamepad;
    for (auto [b, x] : kButtons)
        if (SDL_GameControllerGetButton(c, b)) g.wButtons |= x;
    g.bLeftTrigger = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    g.bRightTrigger = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    g.sThumbLX = Axis(c, SDL_CONTROLLER_AXIS_LEFTX, false);
    g.sThumbLY = Axis(c, SDL_CONTROLLER_AXIS_LEFTY, true);  // XInput: up is positive
    g.sThumbRX = Axis(c, SDL_CONTROLLER_AXIS_RIGHTX, false);
    g.sThumbRY = Axis(c, SDL_CONTROLLER_AXIS_RIGHTY, true);
    static DWORD packet = 0;
    state->dwPacketNumber = ++packet;
    return ERROR_SUCCESS;
}

DWORD XInputSetState(DWORD user, XINPUT_VIBRATION* v) {
    std::lock_guard lk(g_pads_mutex);
    if (user >= XUSER_MAX_COUNT || !g_pads[user]) return ERROR_DEVICE_NOT_CONNECTED;
    SDL_GameControllerRumble(g_pads[user], v->wLeftMotorSpeed, v->wRightMotorSpeed, 200);
    return ERROR_SUCCESS;
}

int GetSystemMetrics(int index) {
    SDL_DisplayMode mode{};
    if (!SdlEnsure(SDL_INIT_VIDEO) || SDL_GetDesktopDisplayMode(0, &mode) != 0) return index == SM_CXSCREEN ? 1920 : 1080;
    return index == SM_CXSCREEN ? mode.w : mode.h;
}

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    SdlEnsure(SDL_INIT_VIDEO);
    if (type & MB_YESNO) {
        const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, IDYES, "Yes"},
                                                    {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDNO, "No"}};
        const SDL_MessageBoxData data{SDL_MESSAGEBOX_WARNING, nullptr, caption, text, 2, buttons, nullptr};
        int pressed = IDNO;
        SDL_ShowMessageBox(&data, &pressed);
        return pressed;
    }
    const Uint32 flags = (type & MB_ICONERROR) ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION;
    SDL_ShowSimpleMessageBox(flags, caption, text, nullptr);
    return IDYES;
}

int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type) {
    auto narrow = [](LPCWSTR w) {
        std::string s;
        for (; w && *w; w++) {
            uint32_t c = (uint32_t)*w;
            if (c < 0x80) s += (char)c;
            else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
            else s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
        }
        return s;
    };
    return MessageBoxA(owner, narrow(text).c_str(), narrow(caption).c_str(), type);
}
