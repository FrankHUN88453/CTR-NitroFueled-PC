// CrashTeamRacingNitroFueled.exe: loads the PS4 eboot + shipped modules from the user's own
// dump and runs them natively. Usage: CrashTeamRacingNitroFueled.exe [path-to-game-dump]
// (default: the directory of the exe, then the current directory).
#include <windows.h>

#include <filesystem>

#include "common.h"
#include "linker.h"
#include "thread.h"
#include "gfx/gfx.h"
#include "patches.h"
#include "config.h"
#include "mouse_ui.h"

void InstallCrashHandler();

namespace {

// Modules from sce_module/ that are pure software and run as-is (LLE).
const char* kLleModules[] = {"libc.prx", "libSceFios2.prx"};

struct EntryParams {
    int argc;
    u32 pad;
    const char* argv[3];
};

SYSV void ProgramExitHandler() {
    LOG("guest called the exit handler");
}

struct ProcParam {
    u64 size;
    u32 magic;
    u32 entry_count;
    u64 sdk_version;
    const char* process_name;
    const char* main_thread_name;
    const u32* main_thread_prio;
    const u32* main_thread_stack_size;
};

DWORD WINAPI MainThread(LPVOID) {
    GuestThread* t = new GuestThread();
    t->name = "main";
    AttachGuestThread(t);

    for (auto& m : g_linker.modules) {
        if (m.get() == g_linker.eboot || !m->has_init) {
            continue;
        }
        LOG("starting module %s (init at %s)", m->name.c_str(),
            g_linker.Describe((u64)m->base + m->init).c_str());
        auto init = (int(SYSV*)(u64, const void*, void*))(m->base + m->init);
        int r = init(0, nullptr, nullptr);
        LOG("module %s start returned %d", m->name.c_str(), r);
    }

    static EntryParams params{1, 0, {"eboot.bin", nullptr, nullptr}};
    auto entry = (void(SYSV*)(EntryParams*, void(SYSV*)()))(g_linker.eboot->base + g_linker.eboot->entry);
    LOG("entering eboot at %s", g_linker.Describe((u64)entry).c_str());
    LogFlush();
    entry(&params, ProgramExitHandler);
    LOG("eboot entry returned");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    wchar_t exe_buf[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    const fs::path exe_path(exe_buf);
    {
        // log: CTR_LOG, else %APPDATA%/CTRNF/<exe name>.log (keeps the game folder clean)
        char env[MAX_PATH] = {};
        wchar_t appdata[MAX_PATH];
        fs::path log = fs::path(exe_path.stem()).replace_extension(".log");
        if (GetEnvironmentVariableA("CTR_LOG", env, MAX_PATH)) {
            log = env;
        } else if (DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH); n && n < MAX_PATH) {
            std::error_code ec;
            fs::create_directories(fs::path(appdata) / "CTRNF", ec);
            log = fs::path(appdata) / "CTRNF" / log;
        }
        LogInit(log.string().c_str());
    }
    fs::path game = argc > 1                                     ? fs::path(argv[1])
                    : fs::exists(exe_path.parent_path() / "eboot.bin") ? exe_path.parent_path()
                                                                 : fs::current_path();
    if (!fs::exists(game / "eboot.bin")) {
        Fatal("eboot.bin not found in %s (usage: %s <game dump dir>)", game.string().c_str(),
              exe_path.filename().string().c_str());
    }
    g_linker.game_dir = game.string();
    if (!GfxShadersAvailable()) {
        // a release exe carries nothing of the game: the shaders are made once from the player's dump
        LOG("no shaders.bin in %s: run setup.bat first", g_linker.game_dir.c_str());
        LogFlush();
        MessageBoxW(nullptr,
                    L"shaders.bin is missing from the game folder.\n\n"
                    L"Run setup.bat once (it builds the shaders from your own game dump), then start the game "
                    L"again.",
                    L"Crash Team Racing Nitro-Fueled", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    ConfigInit();
    LOG("%s - native host for CTR Nitro-Fueled (CUSA13795)", exe_path.filename().string().c_str());
    LOG("game dir: %s", g_linker.game_dir.c_str());

    DWORD slot = TlsAlloc();
    if (slot >= 64) {
        Fatal("TLS slot %lu is not addressable via the TEB fast path", slot);
    }
    TlsConfigure(slot);
    InstallCrashHandler();

    g_linker.eboot = g_linker.Load((game / "eboot.bin").string());
    if (!g_linker.eboot) {
        Fatal("cannot load eboot.bin");
    }
    {
        // optional symbols for readable logs: CTR_SYMBOLS or <exe>/../../out/eboot.functions.txt
        char env[MAX_PATH] = {};
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fs::path sym = GetEnvironmentVariableA("CTR_SYMBOLS", env, MAX_PATH)
                           ? fs::path(env)
                           : fs::path(exe).parent_path() / ".." / ".." / "out" / "eboot.functions.txt";
        g_linker.eboot->LoadFunctionNames(sym.string());
    }
    for (const char* name : kLleModules) {
        fs::path p = game / "sce_module" / name;
        if (!g_linker.Load(p.string())) {
            LOG("warning: %s not found", p.string().c_str());
        }
    }
    g_linker.LinkAll();
    GfxInstallHooks();
    ApplyGamePatches();
    MouseUiInstall();

    u64 stack = 16 << 20;
    if (auto* pp = (const ProcParam*)(g_linker.eboot->base + g_linker.eboot->procparam);
        g_linker.eboot->procparam && pp->main_thread_stack_size) {
        stack = std::max<u64>(stack, *pp->main_thread_stack_size);
        LOG("procparam: process '%s', main thread '%s', stack 0x%x, sdk 0x%llx",
            pp->process_name ? pp->process_name : "", pp->main_thread_name ? pp->main_thread_name : "",
            *pp->main_thread_stack_size, (unsigned long long)pp->sdk_version);
    }
    HANDLE th = CreateThread(nullptr, stack, MainThread, nullptr, 0, nullptr);
    // optional watchdog: ctr_pc.exe <dir> <seconds> dumps all guest threads and exits
    DWORD limit = argc > 2 ? (DWORD)atoi(argv[2]) * 1000 : INFINITE;
    if (WaitForSingleObject(th, limit) == WAIT_TIMEOUT) {
        DumpGuestThreads();
        ExitProcess(2);
    }
    LogFlush();
    return 0;
}
