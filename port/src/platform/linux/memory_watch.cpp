// Linux: VirtualAlloc & co. on mmap, and write watching (GetWriteWatch / ResetWriteWatch) with page
// protection. A watched page that is "clean" is mapped without write access; the first write
// faults, the SIGSEGV handler marks the page written and gives the access back. Reset takes it
// away again. The region list is copied on change and never freed, so the fault handler reads it
// without locks.
//
// Kernel writes into a protected page fail with EFAULT instead of faulting: host code that lets
// the kernel fill guest memory (read() into a guest buffer) goes through a bounce buffer.
#include <windows.h>

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "shim_linux.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace {

constexpr uintptr_t kPage = 4096;

struct Region {
    uintptr_t base = 0;
    size_t size = 0;
    bool watch = false;
    std::atomic<bool> released{false};
    std::vector<uint8_t> prot;                      // per page: PAGE_* (0: not committed)
    std::unique_ptr<std::atomic<uint8_t>[]> state;  // per page: 1 written, 2 write access withheld
    size_t pages() const { return size / kPage; }
};

std::mutex g_mutex;
std::atomic<std::vector<Region*>*> g_list{new std::vector<Region*>()};
void (*g_crash_hook)(int, siginfo_t*, void*) = nullptr;
thread_local sigjmp_buf* t_probe = nullptr;

Region* Find(uintptr_t a) {
    const std::vector<Region*>* v = g_list.load(std::memory_order_acquire);
    auto it = std::upper_bound(v->begin(), v->end(), a, [](uintptr_t x, const Region* r) { return x < r->base; });
    if (it == v->begin()) return nullptr;
    Region* r = *--it;
    return !r->released && a < r->base + r->size ? r : nullptr;
}

void Publish(Region* add, Region* remove) {  // under g_mutex
    auto* v = new std::vector<Region*>();
    for (Region* r : *g_list.load()) {
        if (r != remove && !r->released) v->push_back(r);
    }
    if (add) v->insert(std::upper_bound(v->begin(), v->end(), add->base,
                                        [](uintptr_t x, const Region* r) { return x < r->base; }),
                       add);
    g_list.store(v, std::memory_order_release);  // the old list leaks: a handler may still read it
}

int Posix(DWORD p) {
    switch (p) {
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC | PROT_READ;
    case PAGE_EXECUTE_READ: return PROT_EXEC | PROT_READ;
    case PAGE_EXECUTE_READWRITE: return PROT_EXEC | PROT_READ | PROT_WRITE;
    default: return PROT_NONE;
    }
}
bool Writable(DWORD p) { return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE; }

int Effective(const Region* r, size_t i) {
    int p = Posix(r->prot[i]);
    if (r->watch && (r->state[i].load(std::memory_order_relaxed) & 2)) p &= ~PROT_WRITE;
    return p;
}

void Apply(const Region* r, size_t first, size_t last) {
    for (size_t i = first; i < last;) {
        const int p = Effective(r, i);
        size_t j = i + 1;
        while (j < last && Effective(r, j) == p) j++;
        mprotect((void*)(r->base + i * kPage), (j - i) * kPage, p);
        i = j;
    }
}

void PageRange(const Region* r, const void* addr, SIZE_T size, size_t* first, size_t* last) {
    uintptr_t a = std::max<uintptr_t>((uintptr_t)addr, r->base);
    uintptr_t e = std::min<uintptr_t>((uintptr_t)addr + size, r->base + r->size);
    *first = (a - r->base) / kPage;
    *last = e > a ? (e - r->base + kPage - 1) / kPage : *first;
}

void OnFault(int sig, siginfo_t* si, void* uc) {
    const uintptr_t a = (uintptr_t)si->si_addr;
    if (Region* r = Find(a); r && r->watch && sig == SIGSEGV) {
        const size_t i = (a - r->base) / kPage;
        if (Writable(r->prot[i])) {  // a write-watch trap (or a race with another thread's trap)
            r->state[i].fetch_or(1);
            r->state[i].fetch_and((uint8_t)~2);
            mprotect((void*)(r->base + i * kPage), kPage, Posix(r->prot[i]));
            return;  // the instruction runs again
        }
    }
    if (t_probe) siglongjmp(*t_probe, 1);
    if (g_crash_hook) g_crash_hook(sig, si, uc);
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

void ShimInstallFaultHandler() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa {};
        sa.sa_sigaction = OnFault;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
    });
}

void ShimSetCrashHook(void (*hook)(int, siginfo_t*, void*)) {
    g_crash_hook = hook;
    ShimInstallFaultHandler();
}

bool ShimTryCopy(void* dst, const void* src, size_t n) {
    sigjmp_buf env;
    sigjmp_buf* saved = t_probe;
    if (sigsetjmp(env, 1)) {
        t_probe = saved;
        return false;
    }
    t_probe = &env;
    memcpy(dst, src, n);
    t_probe = saved;
    return true;
}

void* VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect) {
    ShimInstallFaultHandler();
    std::lock_guard lk(g_mutex);
    const uintptr_t a = (uintptr_t)addr & ~(kPage - 1);
    size = ((uintptr_t)addr + size - a + kPage - 1) & ~(kPage - 1);
    if (!size) return nullptr;
    if ((type & MEM_RESERVE) || !addr) {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0);
        void* p = mmap(addr ? (void*)a : nullptr, size, PROT_NONE, flags, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        if (addr && (uintptr_t)p != a) {  // a kernel without MAP_FIXED_NOREPLACE took it as a hint
            munmap(p, size);
            return nullptr;
        }
        auto* r = new Region();
        r->base = (uintptr_t)p;
        r->size = size;
        r->watch = (type & MEM_WRITE_WATCH) != 0;
        r->prot.assign(r->pages(), 0);
        r->state.reset(new std::atomic<uint8_t>[r->pages()]());
        if (type & MEM_COMMIT) {
            for (size_t i = 0; i < r->pages(); i++) {
                r->prot[i] = (uint8_t)protect;
                r->state[i] = r->watch && Writable(protect) ? 2 : 0;
            }
            Apply(r, 0, r->pages());
        }
        Publish(r, nullptr);
        return p;
    }
    // MEM_COMMIT inside a reservation of ours
    Region* r = Find(a);
    if (!r || a + size > r->base + r->size) return nullptr;
    size_t first, last;
    PageRange(r, (void*)a, size, &first, &last);
    for (size_t i = first; i < last; i++) {
        if (!r->prot[i]) r->state[i] = r->watch && Writable(protect) ? 2 : 0;
        r->prot[i] = (uint8_t)protect;
    }
    Apply(r, first, last);
    return (void*)a;
}

BOOL VirtualFree(void* addr, SIZE_T size, DWORD type) {
    std::lock_guard lk(g_mutex);
    Region* r = Find((uintptr_t)addr);
    if (!r) return FALSE;
    if (type & MEM_RELEASE) {
        munmap((void*)r->base, r->size);
        r->released = true;
        Publish(nullptr, r);
        return TRUE;
    }
    size_t first, last;
    PageRange(r, addr, size, &first, &last);
    if (last > first) {
        mmap((void*)(r->base + first * kPage), (last - first) * kPage, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        for (size_t i = first; i < last; i++) r->prot[i] = 0, r->state[i] = 0;
    }
    return TRUE;
}

BOOL VirtualProtect(void* addr, SIZE_T size, DWORD protect, DWORD* old) {
    std::lock_guard lk(g_mutex);
    Region* r = Find((uintptr_t)addr);
    if (!r) {  // not ours: change it directly
        uintptr_t a = (uintptr_t)addr & ~(kPage - 1);
        if (old) *old = PAGE_EXECUTE_READ;
        return mprotect((void*)a, ((uintptr_t)addr + size - a + kPage - 1) & ~(kPage - 1), Posix(protect)) == 0;
    }
    size_t first, last;
    PageRange(r, addr, size, &first, &last);
    if (old) *old = first < r->pages() ? r->prot[first] : PAGE_NOACCESS;
    for (size_t i = first; i < last; i++) {
        if (!r->prot[i]) continue;
        r->prot[i] = (uint8_t)protect;
        if (!Writable(protect)) r->state[i].fetch_and((uint8_t)~2);
    }
    Apply(r, first, last);
    return TRUE;
}

SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* info, SIZE_T size) {
    if (size < sizeof(*info)) return 0;
    memset(info, 0, sizeof(*info));
    const uintptr_t a = (uintptr_t)addr & ~(kPage - 1);
    info->BaseAddress = (void*)a;
    if (Region* r = Find(a)) {
        size_t i = (a - r->base) / kPage, j = i + 1;
        while (j < r->pages() && r->prot[j] == r->prot[i]) j++;
        info->AllocationBase = (void*)r->base;
        info->State = r->prot[i] ? MEM_COMMIT : MEM_RESERVE;
        info->Protect = r->prot[i];
        info->RegionSize = (j - i) * kPage;
        info->Type = 0x20000;  // MEM_PRIVATE
        return sizeof(*info);
    }
    // free as far as we know: up to our next region
    const std::vector<Region*>* v = g_list.load(std::memory_order_acquire);
    uintptr_t next = 0x00007F0000000000ull;
    for (const Region* r : *v)
        if (r->base > a) {
            next = r->base;
            break;
        }
    info->State = MEM_FREE;
    info->Protect = PAGE_NOACCESS;
    info->RegionSize = next - a;
    return sizeof(*info);
}

UINT GetWriteWatch(DWORD flags, void* base, SIZE_T size, void** addresses, ULONG_PTR* count, ULONG* granularity) {
    Region* r = Find((uintptr_t)base);
    if (!r || !r->watch) return 1;
    size_t first, last;
    PageRange(r, base, size, &first, &last);
    ULONG_PTR n = 0;
    for (size_t i = first; i < last && n < *count; i++) {
        if (r->state[i].load(std::memory_order_relaxed) & 1) addresses[n++] = (void*)(r->base + i * kPage);
    }
    *count = n;
    if (granularity) *granularity = (ULONG)kPage;
    if (flags & WRITE_WATCH_FLAG_RESET) ResetWriteWatch(base, size);
    return 0;
}

UINT ResetWriteWatch(void* base, SIZE_T size) {
    std::lock_guard lk(g_mutex);
    Region* r = Find((uintptr_t)base);
    if (!r || !r->watch) return 1;
    size_t first, last;
    PageRange(r, base, size, &first, &last);
    bool any = false;
    for (size_t i = first; i < last; i++) {
        if (!r->prot[i] || !Writable(r->prot[i])) continue;
        uint8_t s = r->state[i].load(std::memory_order_relaxed);
        if (s != 2) any = true;
        r->state[i] = 2;
    }
    if (any) Apply(r, first, last);
    return 0;
}
