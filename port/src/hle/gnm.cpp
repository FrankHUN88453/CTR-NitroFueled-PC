// libSceGnmDriver - "null GPU".
//
// Placeholder until the engine's igPS4GraphicsDevice layer is replaced by a native
// D3D11/Vulkan device: nothing is drawn, but every submitted command buffer is
// scanned for synchronisation packets (EOP/EOS/RELEASE_MEM/WRITE_DATA/DMA fill) whose
// memory writes and interrupts are performed immediately, so CPU code waiting on GPU
// fences can continue.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../hle.h"
#include "../linker.h"
#include "../memory.h"
#include "kernel_event.h"
#include "videoout.h"
#include "../gfx/gfx.h"

namespace {

enum : u32 {
    IT_NOP = 0x10,
    IT_INDIRECT_BUFFER_CONST = 0x33,
    IT_WRITE_DATA = 0x37,
    IT_INDIRECT_BUFFER = 0x3F,
    IT_EVENT_WRITE_EOP = 0x47,
    IT_EVENT_WRITE_EOS = 0x48,
    IT_RELEASE_MEM = 0x49,
    IT_DMA_DATA = 0x50,
};

std::mutex g_eq_mutex;
std::vector<Equeue*> g_eop_queues;
std::atomic<u64> g_submits{0};

u64 Addr(u32 lo, u32 hi16) { return (u64)lo | ((u64)(hi16 & 0xFFFF) << 32); }

bool Writable(u64 addr, u64 size) {
    return GuestReadable(addr, size);
}

void WriteData(u64 addr, u32 sel, u32 lo, u32 hi) {
    switch (sel) {
    case 1:  // 32-bit data
        if (Writable(addr, 4)) *(u32*)addr = lo;
        break;
    case 2:  // 64-bit data
        if (Writable(addr, 8)) *(u64*)addr = (u64)lo | ((u64)hi << 32);
        break;
    case 3:  // 64-bit GPU clock
    case 4: {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        if (Writable(addr, 8)) *(u64*)addr = (u64)c.QuadPart;
        break;
    }
    default: break;
    }
}

void RaiseEop() {
    std::lock_guard lk(g_eq_mutex);
    for (Equeue* q : g_eop_queues) EqueueTriggerFilter(q, kFilterGraphicsCore, 0);
}

void Process(const u32* cmd, u64 dwords, int depth = 0) {
    if (!cmd || depth > 4) return;
    u64 i = 0;
    while (i < dwords) {
        u32 h = cmd[i];
        u32 type = h >> 30;
        if (type == 2) {  // filler
            i++;
            continue;
        }
        u32 count = ((h >> 16) & 0x3FFF) + 2;
        if (type == 0 || type == 1) {
            i += count;
            continue;
        }
        if (i + count > dwords) break;
        const u32* b = cmd + i + 1;
        switch ((h >> 8) & 0xFF) {
        case IT_EVENT_WRITE_EOP: {
            u32 sel = (b[2] >> 29) & 7, intsel = (b[2] >> 24) & 3;
            WriteData(Addr(b[1], b[2]), sel, b[3], b[4]);
            if (intsel) RaiseEop();
            break;
        }
        case IT_EVENT_WRITE_EOS: {
            u32 cmdsel = (b[2] >> 29) & 7;
            if (cmdsel == 2) WriteData(Addr(b[1], b[2]), 1, b[3], 0);
            break;
        }
        case IT_RELEASE_MEM: {
            u32 sel = (b[1] >> 29) & 7, intsel = (b[1] >> 24) & 3;
            WriteData(Addr(b[2], b[3]), sel, b[4], b[5]);
            if (intsel) RaiseEop();
            break;
        }
        case IT_WRITE_DATA: {
            u32 dst = (b[0] >> 8) & 0xF;
            if (dst == 5 || dst == 2) {  // memory
                u64 a = (u64)b[1] | ((u64)b[2] << 32);
                u32 n = count - 4;
                if (Writable(a, n * 4ull)) memcpy((void*)a, b + 3, n * 4ull);
            }
            break;
        }
        case IT_DMA_DATA: {
            u32 src_sel = (b[0] >> 29) & 3, dst_sel = (b[0] >> 20) & 3;
            u32 bytes = b[5] & 0x1FFFFF;
            u64 dst = (u64)b[3] | ((u64)b[4] << 32);
            if (dst_sel == 0 && Writable(dst, bytes)) {
                if (src_sel == 2) {  // fill with immediate
                    for (u32 k = 0; k + 4 <= bytes; k += 4) *(u32*)(dst + k) = b[1];
                } else if (src_sel == 0) {
                    u64 src = (u64)b[1] | ((u64)b[2] << 32);
                    if (Writable(src, bytes)) memmove((void*)dst, (void*)src, bytes);
                }
            }
            break;
        }
        case IT_INDIRECT_BUFFER:
        case IT_INDIRECT_BUFFER_CONST: {
            u64 a = (u64)(b[0] & ~3u) | ((u64)(b[1] & 0xFFFF) << 32);
            u32 n = b[2] & 0xFFFFF;
            if (Writable(a, n * 4ull)) Process((const u32*)a, n, depth + 1);
            break;
        }
        default: break;
        }
        i += count;
    }
}

void ProcessSubmit(u32 count, const u32* const* dcb, const u32* dcb_bytes, const u32* const* ccb,
                   const u32* ccb_bytes) {
    for (u32 k = 0; k < count; k++) {
        if (ccb && ccb[k] && ccb_bytes) Process(ccb[k], ccb_bytes[k] / 4);
        if (dcb && dcb[k]) Process(dcb[k], dcb_bytes[k] / 4);
    }
    u64 n = ++g_submits;
    if (n == 1 || (n & (n - 1)) == 0) LOG("gnm: %llu submits", (unsigned long long)n);
}

// ---- submission
SYSV int sceGnmSubmitCommandBuffers(u32 count, const u32* const* dcb, const u32* dcb_bytes,
                                    const u32* const* ccb, const u32* ccb_bytes) {
    ProcessSubmit(count, dcb, dcb_bytes, ccb, ccb_bytes);
    return 0;
}
SYSV int sceGnmSubmitCommandBuffersForWorkload(u64, u32 count, const u32* const* dcb,
                                               const u32* dcb_bytes, const u32* const* ccb,
                                               const u32* ccb_bytes) {
    ProcessSubmit(count, dcb, dcb_bytes, ccb, ccb_bytes);
    return 0;
}
SYSV int sceGnmSubmitAndFlipCommandBuffers(u32 count, const u32* const* dcb, const u32* dcb_bytes,
                                           const u32* const* ccb, const u32* ccb_bytes, int handle,
                                           int index, int mode, s64 arg) {
    ProcessSubmit(count, dcb, dcb_bytes, ccb, ccb_bytes);
    VideoOutFlip(handle, index, arg);
    return 0;
}
SYSV int sceGnmSubmitAndFlipCommandBuffersForWorkload(u64, u32 count, const u32* const* dcb,
                                                      const u32* dcb_bytes, const u32* const* ccb,
                                                      const u32* ccb_bytes, int handle, int index,
                                                      int mode, s64 arg) {
    return sceGnmSubmitAndFlipCommandBuffers(count, dcb, dcb_bytes, ccb, ccb_bytes, handle, index,
                                             mode, arg);
}
SYSV int sceGnmSubmitDone() { return 0; }
SYSV int sceGnmAreSubmitsAllowed() { return 1; }
SYSV int sceGnmFlushGarlic() { return 0; }
HLE("libSceGnmDriver", sceGnmSubmitCommandBuffers);
HLE("libSceGnmDriver", sceGnmSubmitCommandBuffersForWorkload);
HLE("libSceGnmDriver", sceGnmSubmitAndFlipCommandBuffers);
HLE("libSceGnmDriver", sceGnmSubmitAndFlipCommandBuffersForWorkload);
HLE("libSceGnmDriver", sceGnmSubmitDone);
HLE("libSceGnmDriver", sceGnmAreSubmitsAllowed);
HLE("libSceGnmDriver", sceGnmFlushGarlic);

// ---- EOP interrupts
SYSV int sceGnmAddEqEvent(Equeue* eq, u64 id, void* udata) {
    EqueueAdd(eq, id, kFilterGraphicsCore, udata);
    std::lock_guard lk(g_eq_mutex);
    g_eop_queues.push_back(eq);
    return 0;
}
SYSV int sceGnmDeleteEqEvent(Equeue* eq, u64 id) {
    EqueueDelete(eq, id, kFilterGraphicsCore);
    std::lock_guard lk(g_eq_mutex);
    std::erase(g_eop_queues, eq);
    return 0;
}
SYSV int sceGnmGetEqEventType(const void*) { return 0; }
HLE("libSceGnmDriver", sceGnmAddEqEvent);
HLE("libSceGnmDriver", sceGnmDeleteEqEvent);
HLE("libSceGnmDriver", sceGnmGetEqEventType);

// ---- command-buffer writers: (u32* cmd, u32 dwords, ...) -> fill with a NOP packet
void Nop(u32* cmd, u32 dwords) {
    if (!cmd || dwords == 0) return;
    if (dwords == 1) {
        cmd[0] = 0x80000000;
        return;
    }
    cmd[0] = 0xC0000000 | ((dwords - 2) << 16) | (IT_NOP << 8);
    memset(cmd + 1, 0, (dwords - 1) * 4ull);
}
#define GNM_NOP(fn)                          \
    SYSV int fn(u32* cmd, u32 dwords) {      \
        Nop(cmd, dwords);                    \
        return 0;                            \
    }                                        \
    HLE("libSceGnmDriver", fn)
// draw / dispatch variants the renderer does not handle yet: counted, the first calls logged
#define GNM_UNHANDLED(fn)                                                                              \
    SYSV int fn(u32* cmd, u32 dwords) {                                                                \
        static std::atomic<u64> calls{0};                                                              \
        u64 n = ++calls;                                                                               \
        if (n <= 3 || (n & (n - 1)) == 0) LOG("gnm: %s not rendered (call %llu)", #fn, (unsigned long long)n); \
        Nop(cmd, dwords);                                                                              \
        return 0;                                                                                      \
    }                                                                                                  \
    HLE("libSceGnmDriver", fn)
SYSV int sceGnmDrawIndex(u32* cmd, u32 dwords, u32 count, u64 addr, u32 flags, u32 type) {
    GfxDraw(count, addr, true);
    Nop(cmd, dwords);
    return 0;
}
SYSV int sceGnmDrawIndexAuto(u32* cmd, u32 dwords, u32 count, u32 flags) {
    GfxDraw(count, 0, false);
    Nop(cmd, dwords);
    return 0;
}
HLE("libSceGnmDriver", sceGnmDrawIndex);
HLE("libSceGnmDriver", sceGnmDrawIndexAuto);

// Compute is not rendered yet; record which programs the game dispatches (CsStageRegisters:
// COMPUTE_PGM_LO / HI, RSRC1 / 2, NUM_THREAD_X / Y / Z).
thread_local u64 t_cs_program = 0;
thread_local u32 t_cs_threads[3];
SYSV int sceGnmSetCsShaderWithModifier(u32* cmd, u32 dwords, const u32* regs, u32 modifier) {
    if (regs) {
        t_cs_program = ((u64)regs[1] << 40 | (u64)regs[0] << 8);
        t_cs_threads[0] = regs[4], t_cs_threads[1] = regs[5], t_cs_threads[2] = regs[6];
    }
    Nop(cmd, dwords);
    return 0;
}
SYSV int sceGnmDispatchDirect(u32* cmd, u32 dwords, u32 x, u32 y, u32 z, u32 mode) {
    static std::mutex m;
    static std::unordered_map<u64, u64> seen;
    {
        std::lock_guard lk(m);
        u64& n = seen[t_cs_program];
        if (n++ == 0)
            LOG("gnm: compute program %llx (threads %ux%ux%u) dispatched %ux%ux%u from %s, %s", (unsigned long long)t_cs_program,
                t_cs_threads[0], t_cs_threads[1], t_cs_threads[2], x, y, z,
                g_linker.Describe((u64)__builtin_return_address(0)).c_str(), g_linker.Describe(t_cs_program).c_str());
    }
    Nop(cmd, dwords);
    return 0;
}
HLE("libSceGnmDriver", sceGnmSetCsShaderWithModifier);
HLE("libSceGnmDriver", sceGnmDispatchDirect);
GNM_UNHANDLED(sceGnmDrawIndexOffset);
GNM_UNHANDLED(sceGnmDrawIndirect);
GNM_UNHANDLED(sceGnmDrawIndexIndirect);
GNM_UNHANDLED(sceGnmDrawIndexIndirectCountMulti);
GNM_UNHANDLED(sceGnmDrawIndirectCountMulti);
GNM_UNHANDLED(sceGnmDrawIndexMultiInstanced);
GNM_UNHANDLED(sceGnmDrawOpaqueAuto);
GNM_UNHANDLED(sceGnmDispatchIndirect);
GNM_UNHANDLED(sceGnmDispatchIndirectOnMec);
GNM_NOP(sceGnmSetVsShader);
GNM_NOP(sceGnmSetPsShader350);
GNM_NOP(sceGnmSetEsShader);
GNM_NOP(sceGnmSetGsShader);
GNM_NOP(sceGnmSetHsShader);
GNM_NOP(sceGnmSetLsShader);
GNM_NOP(sceGnmSetEmbeddedVsShader);
GNM_NOP(sceGnmSetEmbeddedPsShader);
GNM_NOP(sceGnmUpdateVsShader);
GNM_NOP(sceGnmUpdatePsShader350);
GNM_NOP(sceGnmUpdateGsShader);
GNM_NOP(sceGnmUpdateHsShader);
GNM_NOP(sceGnmInsertWaitFlipDone);
GNM_NOP(sceGnmInsertPushMarker);
GNM_NOP(sceGnmInsertPopMarker);
GNM_NOP(sceGnmInsertSetMarker);
GNM_NOP(sceGnmInsertPushColorMarker);
GNM_NOP(sceGnmInsertSetColorMarker);
GNM_NOP(sceGnmInsertDingDongMarker);
GNM_NOP(sceGnmSetVgtControl);
GNM_NOP(sceGnmResetVgtControl);
GNM_NOP(sceGnmComputeWaitOnAddress);
GNM_NOP(sceGnmComputeWaitSemaphore);

// init-state writers return the number of dwords they wrote
SYSV u32 sceGnmDrawInitDefaultHardwareState350(u32* cmd, u32 dwords) {
    Nop(cmd, dwords);
    return dwords;
}
SYSV u32 sceGnmDispatchInitDefaultHardwareState(u32* cmd, u32 dwords) {
    Nop(cmd, dwords);
    return dwords;
}
SYSV u32 sceGnmDrawInitToDefaultContextState400(u32* cmd, u32 dwords) {
    Nop(cmd, dwords);
    return dwords;
}
HLE("libSceGnmDriver", sceGnmDrawInitDefaultHardwareState350);
HLE("libSceGnmDriver", sceGnmDispatchInitDefaultHardwareState);
HLE("libSceGnmDriver", sceGnmDrawInitToDefaultContextState400);

// ---- misc queries
u8 g_tess_ring[0x10000];
SYSV void* sceGnmGetTheTessellationFactorRingBufferBaseAddress() { return g_tess_ring; }
SYSV u32 sceGnmGetOffChipTessellationBufferSize() { return 0x800000; }
SYSV int sceGnmIsUserPaEnabled() { return 0; }
SYSV int sceGnmDriverCaptureInProgress() { return 0; }
SYSV int sceGnmDriverTraceInProgress() { return 0; }
SYSV int sceRazorIsLoaded() { return 0; }
SYSV u32 sceGnmGetNumTcaUnits() { return 18; }
SYSV int sceGnmMapComputeQueue(u32 pipe, u32 queue, void* ring, u32 size, void* rptr) {
    static std::atomic<int> next{1};
    return next++;
}
SYSV int sceGnmMapComputeQueueWithPriority(u32 p, u32 q, void* r, u32 s, void* rp, u32) {
    return sceGnmMapComputeQueue(p, q, r, s, rp);
}
SYSV void sceGnmDingDong(u32, u32) {}
SYSV void sceGnmDingDongForWorkload(u32, u32, u64) {}
HLE("libSceGnmDriver", sceGnmGetTheTessellationFactorRingBufferBaseAddress);
HLE("libSceGnmDriver", sceGnmGetOffChipTessellationBufferSize);
HLE("libSceGnmDriver", sceGnmIsUserPaEnabled);
HLE("libSceGnmDriver", sceGnmDriverCaptureInProgress);
HLE("libSceGnmDriver", sceGnmDriverTraceInProgress);
HLE("libSceGnmDriver", sceRazorIsLoaded);
HLE("libSceGnmDriver", sceGnmGetNumTcaUnits);
HLE("libSceGnmDriver", sceGnmMapComputeQueue);
HLE("libSceGnmDriver", sceGnmMapComputeQueueWithPriority);
HLE("libSceGnmDriver", sceGnmDingDong);
HLE("libSceGnmDriver", sceGnmDingDongForWorkload);

}  // namespace
