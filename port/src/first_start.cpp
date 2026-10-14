#include "first_start.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <thread>

#include "common.h"

namespace fs = std::filesystem;

fs::path ShaderCacheDir() {
    wchar_t buf[MAX_PATH];
    fs::path dir;
    if (DWORD n = GetEnvironmentVariableW(L"CTR_USER_DIR", buf, MAX_PATH); n && n < MAX_PATH) {
        dir = buf;
    } else if (DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH); m && m < MAX_PATH) {
        dir = fs::path(buf) / "CTRNF";
    } else {
        return {};
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

namespace {

constexpr UINT kMsgLine = WM_APP + 1;  // lParam: std::string* (one output line of the setup)
constexpr UINT kMsgDone = WM_APP + 2;  // wParam: the setup's exit code

struct SetupState {
    HANDLE job = nullptr;
    bool running = true, cancelled = false;
    DWORD exit_code = 1;
    double progress = 0;  // 0..1
    std::wstring status = L"Starting...";
    std::deque<std::string> tail;  // the last output lines (shown when it fails)
    std::string log;
};
SetupState* g_state = nullptr;

std::wstring Widen(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size()));
    return w;
}

// setup_shaders.py prints "1/4 ..." per step and "translated <n>/<m>" while translating
void OnLine(SetupState& st, const std::string& line) {
    st.log += line + "\n";
    st.tail.push_back(line);
    if (st.tail.size() > 12) st.tail.pop_front();
    int a = 0, b = 0;
    if (sscanf(line.c_str(), "translated %d/%d", &a, &b) == 2 && b > 0) {
        st.progress = 0.10 + 0.85 * a / b;
        st.status = L"Translating the shaders: " + std::to_wstring(a) + L" / " + std::to_wstring(b);
    } else if (line.size() > 4 && line[1] == '/' && line[3] == ' ') {
        static const double kStep[] = {0.0, 0.02, 0.06, 0.10, 0.95};
        const int step = std::clamp(line[0] - '0', 1, 4);
        st.progress = std::max(st.progress, kStep[step]);
        if (step != 3) st.status = Widen(line.substr(4));
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SetupState& st = *g_state;
    switch (msg) {
    case kMsgLine: {
        std::string* line = (std::string*)lp;
        OnLine(st, *line);
        delete line;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case kMsgDone:
        st.running = false;
        st.exit_code = (DWORD)wp;
        DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        if (st.running && MessageBoxW(hwnd, L"Stop building the shaders? The game cannot start without them.",
                                      L"Crash Team Racing Nitro-Fueled", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            st.cancelled = true;
            TerminateJobObject(st.job, 1);  // the reader thread then reports the end
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old_bmp = SelectObject(mem, bmp);
        FillRect(mem, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                 L"Segoe UI");
        HFONT bold = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                 L"Segoe UI");
        SetBkMode(mem, TRANSPARENT);
        HGDIOBJ old_font = SelectObject(mem, bold);
        RECT t{20, 16, rc.right - 20, 44};
        DrawTextW(mem, L"First start: building the shaders from your game files", -1, &t, DT_LEFT | DT_SINGLELINE);
        SelectObject(mem, font);
        t = {20, 44, rc.right - 20, 66};
        SetTextColor(mem, RGB(90, 90, 90));
        DrawTextW(mem, L"This happens only once and takes a few minutes.", -1, &t, DT_LEFT | DT_SINGLELINE);
        RECT bar{20, 78, rc.right - 20, 100};
        HBRUSH frame = CreateSolidBrush(RGB(200, 200, 200)), fill = CreateSolidBrush(RGB(230, 120, 30));
        FrameRect(mem, &bar, frame);
        RECT done = bar;
        InflateRect(&done, -2, -2);
        done.right = done.left + (LONG)((done.right - done.left) * std::clamp(st.progress, 0.0, 1.0));
        FillRect(mem, &done, fill);
        t = {20, 108, rc.right - 20, 130};
        SetTextColor(mem, RGB(40, 40, 40));
        DrawTextW(mem, st.status.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old_font);
        SelectObject(mem, old_bmp);
        DeleteObject(font), DeleteObject(bold), DeleteObject(frame), DeleteObject(fill), DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// The embedded setup archive ("CTRP", see tools/pack_setup.py) unpacked into `dir`.
bool Unpack(const fs::path& dir) {
    HRSRC r = FindResourceA(nullptr, "SETUP", MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const u8* p = g ? (const u8*)LockResource(g) : nullptr;
    const u32 total = r ? SizeofResource(nullptr, r) : 0;
    if (!p || total < 12 || memcmp(p, "CTRP", 4) != 0) return false;
    u32 count;
    memcpy(&count, p + 8, 4);
    const u8 *q = p + 12, *end = p + total;
    for (u32 i = 0; i < count; i++) {
        u16 nl;
        u32 size;
        if (q + 2 > end) return false;
        memcpy(&nl, q, 2);
        if (q + 2 + nl + 4 > end) return false;
        std::string name((const char*)q + 2, nl);
        memcpy(&size, q + 2 + nl, 4);
        const u8* data = q + 6 + nl;
        if (data + size > end || name.find("..") != std::string::npos) return false;
        fs::path out = dir / fs::path(name);
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f.write((const char*)data, size);
        if (!f) return false;
        q = data + size;
    }
    return true;
}

}  // namespace

bool RunFirstStartSetup(const fs::path& game_dir) {
    const fs::path cache = ShaderCacheDir();
    wchar_t tmp_buf[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_buf);
    const fs::path work = fs::path(tmp_buf) / ("ctrnf_setup_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(work, ec);
    if (cache.empty() || !Unpack(work)) {
        fs::remove_all(work, ec);
        LOG("first start: no setup in this build (or it could not be unpacked)");
        MessageBoxW(nullptr,
                    L"The translated shaders are missing and this build cannot make them.\n\n"
                    L"Download the release from GitHub, or build them with tools/setup_shaders.py.",
                    L"Crash Team Racing Nitro-Fueled", MB_OK | MB_ICONERROR);
        return false;
    }
    LOG("first start: building the shaders into %s", cache.string().c_str());

    SetupState st;
    g_state = &st;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"CTRNFSetup";
    RegisterClassW(&wc);
    RECT r{0, 0, 560, 146};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Crash Team Racing Nitro-Fueled", style,
                              (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w,
                              h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // python.exe (hidden console, which its worker processes share) with stdout/stderr into a pipe;
    // a job object takes every process of the setup down with the exe
    st.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(st.job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = nullptr;
    std::wstring cmd = L"\"" + (work / "python" / "python.exe").wstring() + L"\" -I -u \"" +
                       (work / "setup" / "setup_shaders.py").wstring() + L"\" \"" + game_dir.wstring() +
                       L"\" --out \"" + cache.wstring() + L"\"";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
                        nullptr, work.wstring().c_str(), &si, &pi)) {
        LOG("first start: cannot start the setup (error %lu)", GetLastError());
        DestroyWindow(hwnd);
        CloseHandle(rd), CloseHandle(wr), CloseHandle(st.job);
        fs::remove_all(work, ec);
        MessageBoxW(nullptr, L"The shader setup could not be started.", L"Crash Team Racing Nitro-Fueled",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    AssignProcessToJobObject(st.job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    std::thread reader([rd, proc = pi.hProcess, hwnd] {
        std::string pending;
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
            pending.append(buf, n);
            for (size_t e; (e = pending.find('\n')) != std::string::npos; pending.erase(0, e + 1)) {
                std::string line = pending.substr(0, e);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                PostMessageW(hwnd, kMsgLine, 0, (LPARAM) new std::string(line));
            }
        }
        if (!pending.empty()) PostMessageW(hwnd, kMsgLine, 0, (LPARAM) new std::string(pending));
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        PostMessageW(hwnd, kMsgDone, code, 0);
    });
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    reader.join();
    // lines still queued for the destroyed window
    while (PeekMessageW(&m, nullptr, kMsgLine, kMsgLine, PM_REMOVE)) {
        std::string* line = (std::string*)m.lParam;
        OnLine(st, *line);
        delete line;
    }
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(st.job);
    g_state = nullptr;
    {
        std::ofstream f(cache / "setup.log", std::ios::binary);
        f << st.log;
    }
    for (int i = 0; i < 20 && fs::exists(work, ec); i++) {  // the worker processes may still hold files
        fs::remove_all(work, ec);
        if (ec) Sleep(250);
    }
    LOG("first start: setup %s (exit code %lu)", st.cancelled ? "cancelled" : st.exit_code ? "failed" : "done",
        st.exit_code);
    if (st.cancelled) return false;
    if (st.exit_code != 0 || !fs::exists(cache / "shaders.bin")) {
        std::string tail;
        for (const std::string& l : st.tail) tail += l + "\n";
        std::wstring text = L"Building the shaders failed:\n\n" + Widen(tail) + L"\nThe full output is in " +
                            (cache / "setup.log").wstring();
        MessageBoxW(nullptr, text.c_str(), L"Crash Team Racing Nitro-Fueled", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}
