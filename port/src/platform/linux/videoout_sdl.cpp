// Linux: the game window on SDL2 (the Win32 one is in hle/videoout.cpp). The window thread owns the
// window and pumps SDL's events, which also keeps the keyboard / mouse / controller state current
// for the input code. DXVK native presents into it: the HWND it gets is the SDL_Window*.
#include <windows.h>

#include <atomic>

#include "../../common.h"
#include "../../config.h"
#include "../../gfx/gfx.h"
#include "../../hle/videoout.h"
#include "sdl_util.h"

namespace {

std::atomic<SDL_Window*> g_window{nullptr};
std::atomic<int> g_wheel{0};
std::atomic<Uint64> g_mouse_used{0};
Uint32 g_event_fullscreen = (Uint32)-1;
bool g_fullscreen = false;

void SetFullscreen(SDL_Window* w, bool on) {
    g_fullscreen = on;
    SDL_SetWindowFullscreen(w, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

bool CursorWanted() { return SDL_GetTicks64() - g_mouse_used.load() < 3000; }

}  // namespace

void VideoOutSdlWindowThread() {
    if (!SdlEnsure(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
        LOG("videoout: SDL video init failed: %s", SDL_GetError());
        return;
    }
    g_event_fullscreen = SDL_RegisterEvents(1);
    SDL_Window* w = SDL_CreateWindow("Crash Team Racing Nitro-Fueled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w) {
        LOG("videoout: cannot create the window: %s", SDL_GetError());
        return;
    }
    if (ConfigInt("Fullscreen", 0)) SetFullscreen(w, true);
    g_window = w;
    LOG("videoout: SDL window (%s)", SDL_GetCurrentVideoDriver());
    bool cursor_shown = true;
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 250)) {
            do {
                switch (e.type) {
                case SDL_QUIT:
                    LOG("window closed - exiting");
                    LogFlush();
                    ExitProcess(0);
                case SDL_KEYDOWN:
                    if (e.key.repeat) break;
                    if (e.key.keysym.sym == SDLK_F11 ||
                        (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                        SetFullscreen(w, !g_fullscreen);
                        ConfigSet("Fullscreen", g_fullscreen);
                    } else if (e.key.keysym.sym == SDLK_F12) {
                        GfxRecordFrames(180);  // 3 seconds of frames for flicker reports
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    g_wheel += e.wheel.y * 120;
                    g_mouse_used = SDL_GetTicks64();
                    break;
                case SDL_MOUSEMOTION:
                case SDL_MOUSEBUTTONDOWN:
                    g_mouse_used = SDL_GetTicks64();
                    break;
                default:
                    if (e.type == g_event_fullscreen) SetFullscreen(w, e.user.code != 0);
                    break;
                }
            } while (SDL_PollEvent(&e));
        }
        // the cursor shows while the mouse is used and hides after a few seconds without it
        if (CursorWanted() != cursor_shown) {
            cursor_shown = !cursor_shown;
            SDL_ShowCursor(cursor_shown ? SDL_ENABLE : SDL_DISABLE);
        }
    }
}

int VideoOutTakeWheel() { return g_wheel.exchange(0) / 120; }

bool VideoOutCursorInside() {
    SDL_Window* w = g_window;
    return w && SDL_GetMouseFocus() == w;
}

bool VideoOutCursorInPicture(float* x, float* y) {
    SDL_Window* w = g_window;
    if (!w || SDL_GetMouseFocus() != w) return false;
    int mx, my, ww, wh;
    SDL_GetMouseState(&mx, &my);
    SDL_GetWindowSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0) return false;
    float r[4];
    GfxPictureRect(r);
    const float u = (float)mx / ww, v = (float)my / wh;
    *x = (u - r[0]) / (r[2] - r[0]);
    *y = (v - r[1]) / (r[3] - r[1]);
    return true;
}

bool VideoOutHasFocus() {
    SDL_Window* w = g_window;
    return w && SDL_GetKeyboardFocus() == w;
}

void VideoOutSetFullscreen(bool on) {
    if (!g_window || g_event_fullscreen == (Uint32)-1) return;
    SDL_Event e{};
    e.type = g_event_fullscreen;
    e.user.code = on;
    SDL_PushEvent(&e);
}

HWND__* VideoOutHwndRaw() { return (HWND__*)g_window.load(); }
