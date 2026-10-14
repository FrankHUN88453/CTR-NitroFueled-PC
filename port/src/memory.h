#pragma once
#include "common.h"

// Guest-visible memory must live inside the PS4 user address range: libc rejects
// heaps above 0xFC00000000, and guest code may rely on 40-bit pointers.
constexpr u64 kGuestMemBegin = 0x0000000200000000ull;  // 8 GiB
constexpr u64 kGuestMemEnd = 0x000000FC00000000ull;

// Reserve+commit `size` bytes inside the guest range. If `fixed` is set, exactly there.
void* GuestAlloc(u64 size, u64 align, u32 win_protect, void* fixed = nullptr);

// Fast check that [addr, addr+size) lies inside guest memory we allocated (no syscalls;
// VirtualQuery is O(region size) and our heaps are gigabytes large).
bool GuestReadable(u64 addr, u64 size);

// Write tracking (all guest allocations are MEM_WRITE_WATCH): true if the guest wrote to the
// range since the last reset. Used to refresh GPU copies of CPU-written data (video frames).
bool GuestWrittenSince(u64 addr, u64 size, bool reset);
void GuestResetWritten(u64 addr, u64 size);

// memcpy that fails instead of crashing when a page is not accessible (guest memory the game
// may have unmapped meanwhile).
bool GuestTryCopy(void* dst, const void* src, size_t n);

// Calls `fn(begin, end)` for each run of guest pages the guest has written to (pages whose write
// watch was reset since are skipped). Used to find data the game loaded, e.g. localized text.
void GuestWrittenRuns(void (*fn)(u64 begin, u64 end, void* user), void* user);
template <class F>
void GuestForEachWrittenRun(F fn) {
    GuestWrittenRuns([](u64 b, u64 e, void* u) { (*(F*)u)(b, e); }, &fn);
}
