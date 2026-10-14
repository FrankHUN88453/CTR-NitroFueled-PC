// `CrashTeamRacingNitroFueled --selftest`: checks the platform layer without the game (CI runs it on
// Linux, where the Win32 shim implements it): guest memory and write watching, probed reads, the
// TLS slot that patched guest code reads through gs:, threads, timers and INI files.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "common.h"
#include "memory.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) g_failures++;
}

DWORD WINAPI ExitingThread(LPVOID p) {
    ((std::atomic<int>*)p)->store(1);
    ExitThread(7);
}

DWORD WINAPI ReturningThread(LPVOID p) {
    ((std::atomic<int>*)p)->store(2);
    return 0;
}

}  // namespace

int RunSelfTest() {
    printf("ctr_pc self test\n");
    // guest memory with write watching
    u8* m = (u8*)GuestAlloc(0x20000, 0x10000, PAGE_READWRITE);
    Check(m != nullptr && (u64)m >= kGuestMemBegin && (u64)m < kGuestMemEnd, "guest allocation in the PS4 range");
    if (m) {
        Check(!GuestWrittenSince((u64)m, 0x20000, false), "fresh memory reads as not written");
        m[0x5000] = 1;
        Check(GuestWrittenSince((u64)m + 0x5000, 0x1000, false), "a write is seen");
        Check(!GuestWrittenSince((u64)m, 0x5000, false), "pages before it stay clean");
        GuestResetWritten((u64)m, 0x20000);
        Check(!GuestWrittenSince((u64)m, 0x20000, false), "reset clears it");
        m[0x5001] = 2;
        Check(GuestWrittenSince((u64)m + 0x5000, 0x1000, true), "a write after the reset is seen again");
        Check(m[0x5000] == 1 && m[0x5001] == 2, "the data survives");
        Check(GuestReadable((u64)m + 0x100, 16) && !GuestReadable((u64)m + 0x30000, 16), "readable range");
        u64 v = 0;
        Check(!GuestTryCopy(&v, (const void*)(kGuestMemEnd - 0x1000), sizeof(v)), "probed read of unmapped memory");
        Check(GuestTryCopy(&v, m + 0x5000, 1) && (v & 0xFF) == 1, "probed read of guest memory");
    }
    // TLS slot read the way patched guest code does: mov rax, gs:[0x1480 + 8 * slot]
    DWORD slot = TlsAlloc();
    Check(slot < 64, "TLS slot below 64");
    if (slot < 64) {
        TlsSetValue(slot, (void*)0x1234567890ull);
        u8* code = (u8*)GuestAlloc(0x10000, 0x10000, PAGE_EXECUTE_READWRITE);
        const u32 off = 0x1480 + slot * 8;
        const u8 fn[] = {0x65, 0x48, 0x8B, 0x04, 0x25, (u8)off, (u8)(off >> 8), (u8)(off >> 16), (u8)(off >> 24),
                         0xC3};
        if (code) {
            memcpy(code, fn, sizeof(fn));  // written into watched, executable guest memory
            FlushInstructionCache(GetCurrentProcess(), code, sizeof(fn));
            u64 got = ((u64(SYSV*)())code)();
            Check(got == 0x1234567890ull, "gs: TLS slot read by generated code");
            Check(TlsGetValue(slot) == (void*)0x1234567890ull, "TlsGetValue");
        }
    }
    // threads
    std::atomic<int> flag{0};
    HANDLE t = CreateThread(nullptr, 1 << 20, ExitingThread, &flag, 0, nullptr);
    Check(t && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && flag == 1, "thread ending with ExitThread");
    t = CreateThread(nullptr, 1 << 20, ReturningThread, &flag, CREATE_SUSPENDED, nullptr);
    if (t) ResumeThread(t);
    Check(t && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && flag == 2, "suspended thread, resumed");
    // timer
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due, f, a, b;
    due.QuadPart = -20000;  // 2 ms
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
    QueryPerformanceCounter(&b);
    const double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    Check(ms >= 1.9 && ms < 50, "2 ms waitable timer");
    CloseHandle(timer);
    // INI files
    namespace fs = std::filesystem;
    std::string ini = (fs::temp_directory_path() / "ctr_selftest.ini").string();
    fs::remove(ini);
    WritePrivateProfileStringA("Graphics", "RenderScale", "150", ini.c_str());
    WritePrivateProfileStringA("Display", "VSync", "1", ini.c_str());
    WritePrivateProfileStringA("Graphics", "RenderScale", "175", ini.c_str());
    char buf[32] = {};
    GetPrivateProfileStringA("graphics", "renderscale", "", buf, sizeof(buf), ini.c_str());
    Check(!strcmp(buf, "175") && GetPrivateProfileIntA("Display", "VSync", 0, ini.c_str()) == 1 &&
              GetPrivateProfileIntA("Display", "Missing", 42, ini.c_str()) == 42,
          "INI write / read");
    fs::remove(ini);
    printf("%s (%d failed)\n", g_failures ? "SELF TEST FAILED" : "self test passed", g_failures);
    return g_failures ? 1 : 0;
}
