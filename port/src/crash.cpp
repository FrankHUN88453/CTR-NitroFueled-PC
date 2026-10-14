// Last-chance diagnostics for faults inside guest code.
#include <windows.h>

#include "common.h"
#include "linker.h"

// set while the host probes guest memory that may have been unmapped (GuestTryCopy)
thread_local bool g_fault_probe = false;

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    const auto* rec = ep->ExceptionRecord;
    DWORD code = rec->ExceptionCode;
    if (g_fault_probe && code == EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    // let the debugger / C++ runtime handle benign codes
    // only real faults are fatal; informational codes (thread names, debug output, the
    // D3D11 debug layer's 0x87x messages, C++ exceptions) continue normally
    if ((code & 0xFFFFFFF0) == 0x870) {
        return EXCEPTION_CONTINUE_EXECUTION;  // D3D/DXGI debug layer break
    }
    if (!(code & 0x80000000) || code == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const CONTEXT* c = ep->ContextRecord;
    LOG("=== guest fault 0x%08lx at %s", code, g_linker.Describe(c->Rip).c_str());
    if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        LOG("    %s address 0x%llx", rec->ExceptionInformation[0] == 0 ? "read" :
                                    rec->ExceptionInformation[0] == 1 ? "write" : "execute",
            (unsigned long long)rec->ExceptionInformation[1]);
    }
    LOG("    rax %016llx rbx %016llx rcx %016llx rdx %016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOG("    rsi %016llx rdi %016llx rbp %016llx rsp %016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    LOG("    r8  %016llx r9  %016llx r10 %016llx r11 %016llx", c->R8, c->R9, c->R10, c->R11);
    LOG("    r12 %016llx r13 %016llx r14 %016llx r15 %016llx", c->R12, c->R13, c->R14, c->R15);
    // naive stack scan for return addresses into guest modules
    const u64* sp = (const u64*)c->Rsp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
            break;
        }
        u64 v = sp[i];
        if (g_linker.FindByAddr(v)) {
            LOG("    stack[%03d] %s", i, g_linker.Describe(v).c_str());
            shown++;
        }
    }
    LogFlush();
    TerminateProcess(GetCurrentProcess(), 0xDEAD);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler() {
    AddVectoredExceptionHandler(0, CrashHandler);
}
