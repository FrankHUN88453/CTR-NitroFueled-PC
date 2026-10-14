// libSceVideoOut: a native Win32 window, flip bookkeeping and a 60 Hz vblank.
// Presenting real frames is the job of the future native graphics device.
#include "videoout.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../config.h"
#include "../gfx/gfx.h"
#include "../hle.h"
#include "kernel_event.h"

namespace {

constexpr int kHandle = 1;
#ifdef _WIN32
HWND g_hwnd;
#endif
std::atomic<u64> g_flips{0}, g_vblanks{0};
std::atomic<s64> g_flip_arg{0};
std::atomic<int> g_current_buffer{-1};
std::mutex g_mutex;
std::vector<std::pair<Equeue*, void*>> g_vblank_queues, g_flip_queues;
u64 g_start_tick = GetTickCount64();
u64 g_buffers[16];

#ifdef _WIN32
// Borderless fullscreen on the window's monitor (F11 / Alt+Enter, setting Fullscreen=1)
void SetFullscreen(HWND h, bool on) {
    static RECT windowed{};
    static bool fullscreen = false;
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
        GetWindowRect(h, &windowed);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(h, nullptr, windowed.left, windowed.top, windowed.right - windowed.left,
                     windowed.bottom - windowed.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
}
bool g_fullscreen = false;
constexpr UINT kMsgFullscreen = WM_APP + 1;
std::atomic<int> g_wheel{0};

// The mouse cursor shows while the mouse is used and hides after a few seconds without it (or
// while a controller / the keyboard is used).
ULONGLONG g_mouse_used = 0;
LPARAM g_mouse_pos = -1;
bool CursorWanted() { return GetTickCount64() - g_mouse_used < 3000; }
void UpdateCursor(HWND h) {
    POINT pt;
    RECT rc;
    if (GetCursorPos(&pt) && ScreenToClient(h, &pt) && GetClientRect(h, &rc) && PtInRect(&rc, pt))
        SetCursor(CursorWanted() ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {  // Alt+Enter
            SetFullscreen(h, g_fullscreen = !g_fullscreen);
            ConfigSet("Fullscreen", g_fullscreen);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        g_wheel += GET_WHEEL_DELTA_WPARAM(w);
        g_mouse_used = GetTickCount64();
        return 0;
    case WM_MOUSEMOVE:
        if (l != g_mouse_pos) {  // Windows also sends it without movement
            g_mouse_pos = l;
            g_mouse_used = GetTickCount64();
            UpdateCursor(h);
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        g_mouse_used = GetTickCount64();
        UpdateCursor(h);
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(CursorWanted() ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
            return TRUE;
        }
        break;
    case WM_TIMER:
        UpdateCursor(h);
        break;
    case WM_KEYDOWN:
        if (w == VK_F11) {
            SetFullscreen(h, g_fullscreen = !g_fullscreen);
            ConfigSet("Fullscreen", g_fullscreen);
            return 0;
        }
        if (w == VK_F12) {  // record 3 seconds of frames (flicker reports)
            GfxRecordFrames(180);
            return 0;
        }
        break;
    case kMsgFullscreen:  // the Video options
        SetFullscreen(h, g_fullscreen = w != 0);
        return 0;
    case WM_CLOSE:
        LOG("window closed - exiting");
        LogFlush();
        ExitProcess(0);
    case WM_PAINT: {
        if (GfxActive()) {
            ValidateRect(h, nullptr);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        char text[256];
        snprintf(text, sizeof(text),
                 "CTR Nitro-Fueled - native PS4 code running on Windows\n\n"
                 "frames flipped: %llu\n"
                 "(no renderer yet: igPS4GraphicsDevice -> D3D11/Vulkan is the next step)",
                 (unsigned long long)g_flips.load());
        SetTextColor(dc, RGB(255, 160, 0));
        SetBkMode(dc, TRANSPARENT);
        r.left += 24;
        r.top += 24;
        DrawTextA(dc, text, -1, &r, DT_LEFT | DT_TOP);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, w, l);
}

void WindowThread() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));  // res/app.rc
    wc.lpszClassName = "CrashTeamRacingNitroFueled";
    RegisterClassA(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA(wc.lpszClassName, "Crash Team Racing Nitro-Fueled", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                           nullptr, wc.hInstance, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    if (ConfigInt("Fullscreen", 0)) SetFullscreen(g_hwnd, g_fullscreen = true);
    SetTimer(g_hwnd, 1, 500, nullptr);
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_TIMER) InvalidateRect(g_hwnd, nullptr, FALSE);
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}
#else
}  // namespace
void VideoOutSdlWindowThread();  // platform/linux/videoout_sdl.cpp
namespace {
void WindowThread() { VideoOutSdlWindowThread(); }
#endif

void VblankThread() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -166667;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    for (;;) {
        WaitForSingleObject(timer, INFINITE);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        u64 n = ++g_vblanks;
        std::lock_guard lk(g_mutex);
        for (auto& [q, ud] : g_vblank_queues) EqueueTrigger(q, 0x0, kFilterVideoOut, n);
    }
}

struct FlipStatus {
    u64 count;
    u64 process_time;
    u64 tsc;
    s64 flip_arg;
    u64 submit_tsc;
    u64 reserved0;
    s32 gc_queue_num;
    s32 flip_pending_num;
    s32 current_buffer;
    u32 reserved1;
};
struct ResolutionStatus {
    u32 full_width, full_height, pane_width, pane_height;
    u64 refresh_rate;
    float screen_size_in_inch;
    u16 flags, reserved0;
    u32 reserved1[3];
};

SYSV int sceVideoOutOpen(int user, int bus, int index, const void*) {
    LOG("sceVideoOutOpen(user %d, bus %d, index %d)", user, bus, index);
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread(WindowThread).detach();
        std::thread(VblankThread).detach();
    });
    return kHandle;
}
SYSV int sceVideoOutClose(int) { return 0; }
SYSV int sceVideoOutRegisterBuffers(int, int start, void* const* addrs, int num, const void*) {
    LOG("sceVideoOutRegisterBuffers(start %d, num %d, first %p)", start, num, num ? addrs[0] : nullptr);
    for (int i = 0; i < num && start + i < 16; i++) g_buffers[start + i] = (u64)addrs[i];
    return start;
}
SYSV int sceVideoOutSubmitFlip(int handle, int index, int mode, s64 arg) {
    VideoOutFlip(handle, index, arg);
    return 0;
}
SYSV int sceVideoOutGetFlipStatus(int, FlipStatus* s) {
    memset(s, 0, sizeof(*s));
    s->count = g_flips;
    s->flip_arg = g_flip_arg;
    s->current_buffer = g_current_buffer;
    s->process_time = (GetTickCount64() - g_start_tick) * 1000;
    return 0;
}
SYSV int sceVideoOutGetResolutionStatus(int, ResolutionStatus* s) {
    memset(s, 0, sizeof(*s));
    // CTR_RESOLUTION=WxH reports a different display (experiment: does the engine follow it?)
    static const std::pair<u32, u32> res = [] {
        char v[32] = {};
        u32 w = 1920, h = 1080;
        if (GetEnvironmentVariableA("CTR_RESOLUTION", v, sizeof(v))) sscanf(v, "%ux%u", &w, &h);
        return std::make_pair(w, h);
    }();
    s->full_width = s->pane_width = res.first;
    s->full_height = s->pane_height = res.second;
    s->refresh_rate = 3;  // 59.94 Hz
    s->screen_size_in_inch = 50;
    return 0;
}
SYSV int sceVideoOutWaitVblank(int) {
    u64 n = g_vblanks;
    while (g_vblanks == n) Sleep(1);
    return 0;
}
SYSV int sceVideoOutAddVblankEvent(Equeue* eq, int handle, void* udata) {
    EqueueAdd(eq, 0x0, kFilterVideoOut, udata);
    std::lock_guard lk(g_mutex);
    g_vblank_queues.push_back({eq, udata});
    return 0;
}
SYSV int sceVideoOutAddFlipEvent(Equeue* eq, int handle, void* udata) {
    EqueueAdd(eq, 0x6, kFilterVideoOut, udata);
    std::lock_guard lk(g_mutex);
    g_flip_queues.push_back({eq, udata});
    return 0;
}
SYSV int sceVideoOutSetFlipRate(int, int rate) {
    LOG("sceVideoOutSetFlipRate(%d)", rate);  // 0: 60 Hz, 1: 30 Hz, 2: 20 Hz
    return 0;
}
SYSV int sceVideoOutSetBufferAttribute(void* attr, u32 fmt, u32 tiling, u32 aspect, u32 w, u32 h,
                                       u32 pitch) {
    LOG("sceVideoOutSetBufferAttribute(fmt 0x%x tiling %u %ux%u pitch %u)", fmt, tiling, w, h, pitch);
    memset(attr, 0, 0x40);
    u32* a = (u32*)attr;
    a[0] = fmt;
    a[1] = tiling;
    a[2] = aspect;
    a[3] = w;
    a[4] = h;
    a[5] = pitch;
    return 0;
}
// Display capabilities: report a plain SDR display. Leaving the struct untouched made the game
// pick the HDR10 output path (BT.2020 + PQ encoded final pass), which looks washed out on SDR.
struct DeviceCapabilityInfo {
    u64 capability;  // bit 0: BT.2020 PQ (HDR10) support
};
SYSV int sceVideoOutGetDeviceCapabilityInfo_(int, DeviceCapabilityInfo* info, u64 size) {
    if (info) memset(info, 0, size && size < 64 ? size : sizeof(*info));
    return 0;
}
SYSV int sceVideoOutGetDeviceCapabilityInfo(int, DeviceCapabilityInfo* info) {
    if (info) info->capability = 0;
    return 0;
}
HLE("libSceVideoOut", sceVideoOutGetDeviceCapabilityInfo_);
HLE("libSceVideoOut", sceVideoOutGetDeviceCapabilityInfo);
HLE("libSceVideoOut", sceVideoOutOpen);
HLE("libSceVideoOut", sceVideoOutClose);
HLE("libSceVideoOut", sceVideoOutRegisterBuffers);
HLE("libSceVideoOut", sceVideoOutSubmitFlip);
HLE("libSceVideoOut", sceVideoOutGetFlipStatus);
HLE("libSceVideoOut", sceVideoOutGetResolutionStatus);
HLE("libSceVideoOut", sceVideoOutWaitVblank);
HLE("libSceVideoOut", sceVideoOutAddVblankEvent);
HLE("libSceVideoOut", sceVideoOutAddFlipEvent);
HLE("libSceVideoOut", sceVideoOutSetFlipRate);
HLE("libSceVideoOut", sceVideoOutSetBufferAttribute);

}  // namespace

#ifdef _WIN32
int VideoOutTakeWheel() {
    int v = g_wheel.exchange(0);
    return v / WHEEL_DELTA;
}
bool VideoOutCursorInside() {
    POINT pt;
    RECT rc;
    if (!g_hwnd || !GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt) || !GetClientRect(g_hwnd, &rc)) return false;
    return PtInRect(&rc, pt) != 0;
}
bool VideoOutCursorInPicture(float* x, float* y) {
    POINT pt;
    RECT rc;
    if (!g_hwnd || !GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt) || !GetClientRect(g_hwnd, &rc) ||
        !PtInRect(&rc, pt) || rc.right <= 0 || rc.bottom <= 0)
        return false;
    float r[4];
    GfxPictureRect(r);
    float u = (float)pt.x / rc.right, v = (float)pt.y / rc.bottom;
    *x = (u - r[0]) / (r[2] - r[0]);
    *y = (v - r[1]) / (r[3] - r[1]);
    return true;
}
bool VideoOutHasFocus() {
    return g_hwnd && GetForegroundWindow() == g_hwnd;
}

void VideoOutSetFullscreen(bool on) {
    if (g_hwnd) PostMessageA(g_hwnd, kMsgFullscreen, on, 0);
}

HWND__* VideoOutHwndRaw() {
    return g_hwnd;
}
#endif

// Frame limiter (settings FpsLimit / VSync). The game asks for a 30 Hz flip rate, but its clock
// (CClock) runs on measured frame times, so any rate works.
void PaceFrame() {
    static unsigned gen = ~0u;
    static int vsync = 0, limit = 60;
    if (gen != ConfigGeneration()) gen = ConfigGeneration(), vsync = ConfigInt("VSync", 0), limit = ConfigInt("FpsLimit", 60);
    if (vsync || limit <= 0) return;
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
    static const s64 freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    static s64 next = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const s64 period = freq / limit;
    if (next == 0 || now.QuadPart - next > period) next = now.QuadPart;  // fell behind: resync
    next += period;
    s64 wait = next - now.QuadPart;
    if (wait <= 0) return;
    s64 hns = wait * 10000000 / freq;  // 100 ns units
    if (hns > 15000) {                   // sleep, then spin the last ~1 ms
        LARGE_INTEGER due;
        due.QuadPart = -(hns - 10000);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < next);
}

void VideoOutFlip(int, int index, s64 arg) {
    u64 addr = index >= 0 && index < 16 ? g_buffers[index] : 0;
    if (addr) GfxPresent(addr);
    PaceFrame();
    u64 n = ++g_flips;
    g_flip_arg = arg;
    g_current_buffer = index;
    if (n == 1 || (n & (n - 1)) == 0) LOG("videoout: flip #%llu (buffer %d)", (unsigned long long)n, index);
    std::lock_guard lk(g_mutex);
    for (auto& [q, ud] : g_flip_queues) EqueueTrigger(q, 0x6, kFilterVideoOut, (u64)arg);
}
