#include "thread.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "linker.h"

namespace {
u32 g_win_slot = 0xFFFFFFFF;
thread_local GuestThread* t_current;
std::mutex g_threads_mutex;
std::vector<GuestThread*> g_threads;

DWORD WINAPI ThreadProc(LPVOID p) {
    auto* t = (GuestThread*)p;
    AttachGuestThread(t);
    t->retval = t->entry(t->arg);
    return 0;
}
}  // namespace

void TlsConfigure(u32 slot) {
    g_win_slot = slot;
}

u32 TlsWinSlot() {
    return g_win_slot;
}

GuestThread* CurrentThread() {
    if (!t_current) {
        // a host thread entering guest code for the first time (e.g. main)
        auto* t = new GuestThread();
        t->tid = GetCurrentThreadId();
        t->name = "host";
        AttachGuestThread(t);
    }
    return t_current;
}

void AttachGuestThread(GuestThread* t) {
    const Linker& l = g_linker;
    u64 size = l.tls_static_size + 0x400;
    t->tls_area = (u8*)_aligned_malloc(size, l.tls_max_align);
    memset(t->tls_area, 0, size);
    t->tp = (u64*)(t->tls_area + l.tls_static_size);
    // FreeBSD-style DTV: [generation, count, block(id=1), block(id=2), ...]
    size_t nmod = 0;
    for (auto& m : l.modules) {
        nmod = std::max<size_t>(nmod, m->tls_module_id);
    }
    t->dtv = new u64[nmod + 2]();
    t->dtv[0] = 1;
    t->dtv[1] = nmod;
    for (auto& m : l.modules) {
        if (!m->tls_module_id) {
            continue;
        }
        u8* block = (u8*)t->tp - m->tls_offset;
        memcpy(block, m->base + m->tls_vaddr, m->tls_filesz);
        t->dtv[m->tls_module_id + 1] = (u64)block;
    }
    t->tp[0] = (u64)t->tp;  // tcb_self
    t->tp[1] = (u64)t->dtv;
    t->tp[2] = (u64)t;
    t->tid = GetCurrentThreadId();
    if (!t->handle) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0,
                        FALSE, DUPLICATE_SAME_ACCESS);
    }
    TlsSetValue(g_win_slot, t->tp);
    t_current = t;
    std::lock_guard lk(g_threads_mutex);
    g_threads.push_back(t);
}

void DumpGuestThreads() {
    std::lock_guard lk(g_threads_mutex);
    LOG("=== thread dump (%zu guest threads)", g_threads.size());
    for (GuestThread* t : g_threads) {
        if (SuspendThread(t->handle) == (DWORD)-1) {
            continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t->handle, &c)) {
            std::string frames;
            const u64* sp = (const u64*)c.Rsp;
            int shown = 0;
            static const u64 exe_lo = (u64)GetModuleHandleA(nullptr);
            for (int i = 0; i < 2048 && shown < 10; i++) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
                bool host = sp[i] >= exe_lo && sp[i] < exe_lo + 0x400000;
                if (g_linker.FindByAddr(sp[i]) || host) {
                    frames += " " + g_linker.Describe(sp[i]);
                    shown++;
                }
            }
            LOG("  '%s' tid %lu rip %s |%s", t->name.c_str(), t->tid, g_linker.Describe(c.Rip).c_str(),
                frames.c_str());
        }
    }
    LogFlush();
}

GuestThread* CreateGuestThread(void*(SYSV* entry)(void*), void* arg, const char* name,
                               u64 stack_size) {
    auto* t = new GuestThread();
    t->entry = entry;
    t->arg = arg;
    t->name = name ? name : "";
    // Guest code has no stack probes: commit the whole stack up front.
    t->stack_size = std::max<u64>(stack_size, 1 << 20);
    t->handle = CreateThread(nullptr, t->stack_size, ThreadProc, t, CREATE_SUSPENDED, &t->tid);
    if (!t->handle) {
        delete t;
        return nullptr;
    }
    std::wstring wname(t->name.begin(), t->name.end());
    SetThreadDescription(t->handle, wname.c_str());
    ResumeThread(t->handle);
    return t;
}
