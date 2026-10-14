// Linux: last-chance diagnostics for faults inside guest code (the Windows version is crash.cpp).
// Write-watch traps and memory probes are handled before this (memory_watch.cpp).
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include "../../common.h"
#include "../../linker.h"
#include "shim_linux.h"

namespace {

void OnCrash(int sig, siginfo_t* si, void* ucv) {
    static volatile int nested = 0;
    if (nested++) _exit(0xDE);
    const auto* uc = (const ucontext_t*)ucv;
    const greg_t* g = uc->uc_mcontext.gregs;
    LOG("=== guest fault: signal %d (code %d) at %s", sig, si->si_code, g_linker.Describe((u64)g[REG_RIP]).c_str());
    LOG("    %s address 0x%llx", (g[REG_ERR] & 2) ? "write" : "read", (unsigned long long)(uintptr_t)si->si_addr);
    LOG("    rax %016llx rbx %016llx rcx %016llx rdx %016llx", (unsigned long long)g[REG_RAX],
        (unsigned long long)g[REG_RBX], (unsigned long long)g[REG_RCX], (unsigned long long)g[REG_RDX]);
    LOG("    rsi %016llx rdi %016llx rbp %016llx rsp %016llx", (unsigned long long)g[REG_RSI],
        (unsigned long long)g[REG_RDI], (unsigned long long)g[REG_RBP], (unsigned long long)g[REG_RSP]);
    LOG("    r8  %016llx r9  %016llx r10 %016llx r11 %016llx", (unsigned long long)g[REG_R8],
        (unsigned long long)g[REG_R9], (unsigned long long)g[REG_R10], (unsigned long long)g[REG_R11]);
    LOG("    r12 %016llx r13 %016llx r14 %016llx r15 %016llx", (unsigned long long)g[REG_R12],
        (unsigned long long)g[REG_R13], (unsigned long long)g[REG_R14], (unsigned long long)g[REG_R15]);
    // naive stack scan for return addresses into guest modules (probed reads)
    const u64* sp = (const u64*)g[REG_RSP];
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; i++) {
        u64 v;
        if (!ShimTryCopy(&v, sp + i, sizeof(v))) break;
        if (g_linker.FindByAddr(v)) {
            LOG("    stack[%03d] %s", i, g_linker.Describe(v).c_str());
            shown++;
        }
    }
    LogFlush();
    _exit(0xDE);
}

}  // namespace

void InstallCrashHandler() {
    ShimSetCrashHook(OnCrash);
    struct sigaction sa {};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}
