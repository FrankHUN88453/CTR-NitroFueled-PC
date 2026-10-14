// libkernel: errno, process info, time, memory.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include "../config.h"
#include "../hle.h"
#include "../linker.h"
#include "../memory.h"
#include "../thread.h"

using namespace err;

namespace {

// ---------------------------------------------------------------- errno / misc
SYSV int* __error() { return &CurrentThread()->err; }
HLE("libkernel", __error);

SYSV void __stack_chk_fail() { Fatal("guest stack corruption detected (__stack_chk_fail)"); }
HLE("libkernel", __stack_chk_fail);

u64 g_stack_chk_guard = 0x2B992DDFA23249D6ull;
const char* g_progname = "eboot.bin";
HLE_AS("libkernel", "__stack_chk_guard", g_stack_chk_guard);
HLE_AS("libkernel", "__progname", g_progname);

struct TlsIndex {
    u64 module;
    u64 offset;
};
SYSV void* __tls_get_addr(TlsIndex* ti) {
    GuestThread* t = CurrentThread();
    return (u8*)t->dtv[ti->module + 1] + ti->offset;
}
HLE("libkernel", __tls_get_addr);

SYSV void* sceKernelGetProcParam() {
    Module* e = g_linker.eboot;
    return e && e->procparam ? e->base + e->procparam : nullptr;
}
HLE("libkernel", sceKernelGetProcParam);

// setting PS4Pro: the game then renders at 2560x1440 (instead of 1920x1080) with its Pro extras
SYSV int sceKernelIsNeoMode() { return ConfigInt("PS4Pro", 1) != 0; }
SYSV int sceKernelGetCpumode() { return 0; }
SYSV int sceKernelGetCurrentCpu() { return (int)(GetCurrentProcessorNumber() % 7); }
SYSV int sceKernelIsAddressSanitizerEnabled() { return 0; }
SYSV int _sceKernelRtldSetApplicationHeapAPI(void*) { return 0; }
SYSV int sceKernelDebugRaiseException(int code, int arg) {
    LOG("sceKernelDebugRaiseException(0x%x, 0x%x)", code, arg);
    return 0;
}
SYSV int sceKernelDebugRaiseExceptionOnReleaseMode(int code, int arg) {
    LOG("sceKernelDebugRaiseExceptionOnReleaseMode(0x%x, 0x%x)", code, arg);
    return 0;
}
SYSV int _is_signal_return(void*) { return 0; }
SYSV int _sigprocmask(int, const void*, void*) { return 0; }
HLE("libkernel", sceKernelIsNeoMode);
HLE("libkernel", sceKernelGetCpumode);
HLE("libkernel", sceKernelGetCurrentCpu);
HLE("libkernel", sceKernelIsAddressSanitizerEnabled);
HLE("libkernel", _sceKernelRtldSetApplicationHeapAPI);
HLE("libkernel", sceKernelDebugRaiseException);
HLE("libkernel", sceKernelDebugRaiseExceptionOnReleaseMode);
HLE("libkernel", _is_signal_return);
HLE("libkernel", _sigprocmask);

SYSV void _exit(int code) {
    LOG("guest _exit(%d)", code);
    LogFlush();
    ExitProcess((UINT)code);
}
HLE("libkernel", _exit);

// ---------------------------------------------------------------- time
struct Timespec {
    s64 sec, nsec;
};
struct Timeval {
    s64 sec, usec;
};

LARGE_INTEGER QpcFreq() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
    }();
    return f;
}
const u64 g_qpc_start = [] {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return (u64)v.QuadPart;
}();

u64 MonotonicNs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    u64 d = (u64)c.QuadPart - g_qpc_start;
    u64 f = (u64)QpcFreq().QuadPart;
    return d / f * 1000000000ull + d % f * 1000000000ull / f;
}

u64 RealtimeNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100;  // since 1970
}

SYSV u64 sceKernelGetProcessTime() { return MonotonicNs() / 1000; }
SYSV u64 sceKernelReadTsc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (u64)c.QuadPart;
}
SYSV u64 sceKernelGetTscFrequency() { return (u64)QpcFreq().QuadPart; }
SYSV u64 sceKernelGetProcessTimeCounter() { return sceKernelReadTsc(); }
SYSV u64 sceKernelGetProcessTimeCounterFrequency() { return sceKernelGetTscFrequency(); }
HLE("libkernel", sceKernelGetProcessTime);
HLE("libkernel", sceKernelReadTsc);
HLE("libkernel", sceKernelGetTscFrequency);
HLE("libkernel", sceKernelGetProcessTimeCounter);
HLE("libkernel", sceKernelGetProcessTimeCounterFrequency);

int ClockGettime(int clock, Timespec* ts) {
    u64 ns;
    switch (clock) {
    case 0:  // REALTIME
    case 9:  // REALTIME_PRECISE
    case 10: // REALTIME_FAST
        ns = RealtimeNs();
        break;
    default:  // MONOTONIC, UPTIME, CPUTIME...
        ns = MonotonicNs();
        break;
    }
    ts->sec = (s64)(ns / 1000000000ull);
    ts->nsec = (s64)(ns % 1000000000ull);
    return 0;
}
SYSV int sceKernelClockGettime(int clock, Timespec* ts) { return ClockGettime(clock, ts); }
SYSV int posix_clock_gettime(int clock, Timespec* ts) { return ClockGettime(clock, ts); }
SYSV int sceKernelGettimeofday(Timeval* tv) {
    u64 ns = RealtimeNs();
    tv->sec = (s64)(ns / 1000000000ull);
    tv->usec = (s64)(ns % 1000000000ull / 1000);
    return 0;
}
SYSV int posix_gettimeofday(Timeval* tv, void*) { return sceKernelGettimeofday(tv); }
HLE("libkernel", sceKernelClockGettime);
HLE_AS("libkernel", "clock_gettime", posix_clock_gettime);
HLE("libkernel", sceKernelGettimeofday);
HLE_AS("libkernel", "gettimeofday", posix_gettimeofday);

void SleepUs(u64 us) {
    if (us == 0) {
        SwitchToThread();
        return;
    }
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(s64)(us * 10);
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}
SYSV int sceKernelUsleep(u32 us) {
    SleepUs(us);
    return 0;
}
SYSV int posix_nanosleep(const Timespec* req, Timespec* rem) {
    SleepUs((u64)req->sec * 1000000 + (u64)req->nsec / 1000);
    if (rem) *rem = {0, 0};
    return 0;
}
HLE("libkernel", sceKernelUsleep);
HLE_AS("libkernel", "nanosleep", posix_nanosleep);
HLE_AS("libkernel", "_nanosleep", posix_nanosleep);

SYSV int posix_getrusage(int, void* ru) {
    memset(ru, 0, 144);
    return 0;
}
HLE_AS("libkernel", "getrusage", posix_getrusage);

// ---------------------------------------------------------------- memory
constexpr int MAP_FIXED = 0x10;
std::atomic<u64> g_direct_next{0};
// PS4: 5056 MiB; on PC the budget comes from the settings (DirectMemoryMB)
u64 DirectMemorySize() {
    static const u64 size = (u64)std::max(1024, ConfigInt("DirectMemoryMB", 8192)) << 20;
    return size;
}

DWORD WinProt(int prot) {
    // PS4: 1 = CPU read, 2 = CPU write, 4 = CPU exec, 0x10/0x20 = GPU read/write
    if (prot & 4) return PAGE_EXECUTE_READWRITE;
    if (prot & 0x22) return PAGE_READWRITE;
    if (prot & 0x11) return PAGE_READWRITE;  // GPU-only mappings are still CPU-visible here
    return PAGE_READWRITE;
}

struct Region {
    u64 end;
    s64 phys;
    int prot;
    int type;  // direct memory type (0 = WB_ONION, 3 = WC_GARLIC, 10 = WB_GARLIC), -1 flexible
};
std::mutex g_regions_mutex;
std::map<u64, Region> g_regions;
std::map<s64, int> g_direct_types;  // phys start -> memory type

void AddRegion(void* p, u64 len, s64 phys, int prot, int type) {
    std::lock_guard lk(g_regions_mutex);
    g_regions[(u64)p] = {(u64)p + len, phys, prot, type};
}

const std::pair<const u64, Region>* FindRegion(u64 addr, bool next) {
    auto it = g_regions.upper_bound(addr);
    if (it != g_regions.begin()) {
        auto prev = std::prev(it);
        if (addr < prev->second.end) return &*prev;
    }
    return next && it != g_regions.end() ? &*it : nullptr;
}

void* MapAnywhere(void* hint, u64 len, u64 align, bool fixed, int prot) {
    return GuestAlloc(len, align, WinProt(prot), fixed && hint ? hint : nullptr);
}

SYSV int sceKernelMapNamedFlexibleMemory(void** addr, u64 len, int prot, int flags, const char* name) {
    void* p = MapAnywhere(*addr, len, 0x4000, flags & MAP_FIXED, prot);
    LOG("MapFlexibleMemory '%s' len 0x%llx -> %p", name ? name : "", (unsigned long long)len, p);
    if (!p) return SceErr(ENOMEM_);
    AddRegion(p, len, 0, prot, -1);
    *addr = p;
    return 0;
}
SYSV int sceKernelMapFlexibleMemory(void** addr, u64 len, int prot, int flags) {
    return sceKernelMapNamedFlexibleMemory(addr, len, prot, flags, nullptr);
}
SYSV int sceKernelMunmap(void* addr, u64 len) {
    VirtualFree(addr, len, MEM_DECOMMIT);
    return 0;
}
SYSV int sceKernelMlock(void*, u64) { return 0; }
SYSV int sceKernelMprotect(void* addr, u64 len, int prot) {
    DWORD old;
    VirtualProtect(addr, len, WinProt(prot), &old);
    return 0;
}
HLE("libkernel", sceKernelMapNamedFlexibleMemory);
HLE("libkernel", sceKernelMapFlexibleMemory);
HLE("libkernel", sceKernelMunmap);
HLE("libkernel", sceKernelMlock);
HLE("libkernel", sceKernelMprotect);

SYSV u64 sceKernelGetDirectMemorySize() { return DirectMemorySize(); }
SYSV int sceKernelAllocateDirectMemory(s64 start, s64 end, u64 len, u64 align, int type, s64* phys) {
    if (align < 0x4000) align = 0x4000;
    u64 cur = g_direct_next.load();
    u64 a;
    do {
        a = (std::max<u64>(cur, (u64)start) + align - 1) & ~(align - 1);
        if (a + len > (u64)end || a + len > DirectMemorySize()) return SceErr(EAGAIN_);
    } while (!g_direct_next.compare_exchange_weak(cur, a + len));
    *phys = (s64)a;
    {
        std::lock_guard lk(g_regions_mutex);
        g_direct_types[(s64)a] = type;
    }
    LOG("AllocateDirectMemory len 0x%llx align 0x%llx type %d -> phys 0x%llx",
        (unsigned long long)len, (unsigned long long)align, type, (unsigned long long)a);
    return 0;
}
SYSV int sceKernelReleaseDirectMemory(s64, u64) { return 0; }
SYSV int sceKernelMapDirectMemory(void** addr, u64 len, int prot, int flags, s64 phys, u64 align) {
    void* p = MapAnywhere(*addr, len, align ? align : 0x4000, flags & MAP_FIXED, prot);
    LOG("MapDirectMemory phys 0x%llx len 0x%llx prot 0x%x -> %p", (unsigned long long)phys,
        (unsigned long long)len, prot, p);
    if (!p) return SceErr(ENOMEM_);
    int type = 0;
    {
        std::lock_guard lk(g_regions_mutex);
        auto it = g_direct_types.upper_bound(phys);
        if (it != g_direct_types.begin()) type = std::prev(it)->second;
    }
    AddRegion(p, len, phys, prot, type);
    *addr = p;
    return 0;
}
struct BatchMapEntry {
    void* start;
    s64 offset;
    u64 length;
    u8 protection;
    u8 type;
    s16 pad;
    s32 operation;
};
SYSV int sceKernelBatchMap(BatchMapEntry* e, int n, int* done) {
    int i = 0;
    for (; i < n; i++) {
        if (e[i].operation == 0 || e[i].operation == 2) {  // MAP_DIRECT / MAP_FLEXIBLE
            void* a = e[i].start;
            if (int r = sceKernelMapDirectMemory(&a, e[i].length, e[i].protection, MAP_FIXED,
                                                 e[i].offset, 0)) {
                if (done) *done = i;
                return r;
            }
        }
    }
    if (done) *done = i;
    return 0;
}
#pragma pack(push, 1)
struct VirtualQueryInfo {
    u64 start, end;
    s64 offset;
    s32 protection;
    s32 memory_type;
    u8 flags;  // is_flexible:1 is_direct:1 is_stack:1 is_pooled:1 is_committed:1
    char name[32];
    u8 pad[7];
};
#pragma pack(pop)
SYSV int sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, u64 size) {
    std::lock_guard lk(g_regions_mutex);
    auto* r = FindRegion((u64)addr, flags & 1);
    if (!r) return SceErr(EACCES_);
    memset(info, 0, std::min<u64>(size, sizeof(*info)));
    info->start = r->first;
    info->end = r->second.end;
    info->offset = r->second.phys;
    info->protection = r->second.prot;
    info->memory_type = r->second.type < 0 ? 0 : r->second.type;
    info->flags = (r->second.type < 0 ? 1 : 2) | 0x10;
    return 0;
}
SYSV int sceKernelQueryMemoryProtection(const void* addr, void** start, void** end, u32* prot) {
    std::lock_guard lk(g_regions_mutex);
    auto* r = FindRegion((u64)addr, false);
    if (!r) return SceErr(EACCES_);
    if (start) *start = (void*)r->first;
    if (end) *end = (void*)(r->second.end - 1);
    if (prot) *prot = (u32)r->second.prot;
    return 0;
}
struct DirectMemoryQueryInfo {
    s64 start, end;
    int type;
};
SYSV int sceKernelDirectMemoryQuery(s64 offset, int flags, DirectMemoryQueryInfo* info, u64 size) {
    std::lock_guard lk(g_regions_mutex);
    for (auto& [va, r] : g_regions) {
        if (r.type >= 0 && offset >= r.phys && offset < r.phys + (s64)(r.end - va)) {
            info->start = r.phys;
            info->end = r.phys + (s64)(r.end - va);
            info->type = r.type;
            return 0;
        }
    }
    return SceErr(EACCES_);
}
HLE("libkernel", sceKernelVirtualQuery);
HLE("libkernel", sceKernelQueryMemoryProtection);
HLE("libkernel", sceKernelDirectMemoryQuery);
HLE("libkernel", sceKernelGetDirectMemorySize);
HLE("libkernel", sceKernelAllocateDirectMemory);
HLE("libkernel", sceKernelReleaseDirectMemory);
HLE("libkernel", sceKernelMapDirectMemory);
HLE("libkernel", sceKernelBatchMap);

}  // namespace
