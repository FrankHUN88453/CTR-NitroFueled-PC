#include "memory.h"

#include <windows.h>
#ifndef _WIN32
#include "platform/linux/shim_linux.h"
#endif

#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace {
std::mutex g_mutex;
u64 g_cursor = kGuestMemBegin;
std::shared_mutex g_ranges_mutex;
std::vector<std::pair<u64, u64>> g_ranges;  // sorted [begin, end)

void AddRange(void* p, u64 size) {
    if (!p) return;
    std::unique_lock lk(g_ranges_mutex);
    std::pair<u64, u64> r{(u64)p, (u64)p + size};
    g_ranges.insert(std::upper_bound(g_ranges.begin(), g_ranges.end(), r), r);
}
}  // namespace

bool GuestReadable(u64 addr, u64 size) {
    if (addr < kGuestMemBegin) return false;
    std::shared_lock lk(g_ranges_mutex);
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), std::make_pair(addr, ~0ull));
    if (it == g_ranges.begin()) return false;
    --it;
    return addr >= it->first && addr + size <= it->second;
}

void* GuestAlloc(u64 size, u64 align, u32 prot, void* fixed) {
    std::lock_guard lk(g_mutex);
    size = (size + 0xFFFF) & ~0xFFFFull;
    if (fixed) {
        if (void* p = VirtualAlloc(fixed, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, prot)) {
            AddRange(p, size);
            return p;
        }
        // part of an earlier reservation of ours: commit only
        return VirtualAlloc(fixed, size, MEM_COMMIT, prot);
    }
    align = align < 0x10000 ? 0x10000 : align;
    u64 addr = (g_cursor + align - 1) & ~(align - 1);
    while (addr + size <= kGuestMemEnd) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
            break;
        }
        u64 region_end = (u64)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_FREE && region_end >= addr + size) {
            if (void* p = VirtualAlloc((void*)addr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, prot)) {
                g_cursor = addr + size;
                AddRange(p, size);
                return p;
            }
        }
        addr = (region_end + align - 1) & ~(align - 1);
    }
    LOG("GuestAlloc: out of guest address space (size 0x%llx)", (unsigned long long)size);
    return nullptr;
}

bool GuestWrittenSince(u64 addr, u64 size, bool reset) {
    void* page;
    ULONG_PTR count = 1;
    ULONG gran;
    if (GetWriteWatch(0, (void*)addr, size, &page, &count, &gran) != 0) return false;  // not watched
    if (count && reset) ResetWriteWatch((void*)addr, size);
    return count != 0;
}

void GuestResetWritten(u64 addr, u64 size) { ResetWriteWatch((void*)addr, size); }

#ifdef _WIN32
extern thread_local bool g_fault_probe;  // crash.cpp

namespace {
// clang-cl only catches SEH exceptions raised in calls made from a __try block (no
// -fasync-exceptions): an inlined small memcpy faulting in the block itself would not be caught
// and the process would die silently, so the copy is a call of its own.
__declspec(noinline) void CopyBytes(void* dst, const void* src, size_t n) { memcpy(dst, src, n); }

bool CopyGuarded(void* dst, const void* src, size_t n) {
    __try {
        CopyBytes(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool GuestTryCopy(void* dst, const void* src, size_t n) {
    g_fault_probe = true;
    bool ok = CopyGuarded(dst, src, n);
    g_fault_probe = false;
    return ok;
}
#else
bool GuestTryCopy(void* dst, const void* src, size_t n) { return ShimTryCopy(dst, src, n); }
#endif

void GuestWrittenRuns(void (*fn)(u64 begin, u64 end, void* user), void* user) {
    std::vector<std::pair<u64, u64>> ranges;
    {
        std::shared_lock lk(g_ranges_mutex);
        ranges = g_ranges;
    }
    constexpr u64 kChunk = 256ull << 20;
    std::vector<void*> pages(kChunk / 4096);
    for (auto [begin, end] : ranges) {
        u64 run_b = 0, run_e = 0;
        for (u64 c = begin; c < end; c += kChunk) {
            ULONG_PTR count = pages.size();
            ULONG gran = 4096;
            if (GetWriteWatch(0, (void*)c, std::min(kChunk, end - c), pages.data(), &count, &gran) != 0) break;
            for (ULONG_PTR i = 0; i < count; i++) {
                u64 p = (u64)pages[i];
                if (p == run_e) {
                    run_e += gran;
                    continue;
                }
                if (run_e > run_b) fn(run_b, run_e, user);
                run_b = p, run_e = p + gran;
            }
        }
        if (run_e > run_b) fn(run_b, run_e, user);
    }
}
