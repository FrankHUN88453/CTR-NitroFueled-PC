// Reverse-engineering probe: dumps the engine's render state at the first draws.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "../linker.h"
#include "../memory.h"
#include "gfx.h"

namespace {

bool Readable(u64 a, u64 n) {
    return GuestReadable(a, n);
}

void Dump(const char* what, u64 a, u64 n) {
    if (!Readable(a, n)) {
        LOG("  %s @ %llx: <unreadable>", what, (unsigned long long)a);
        return;
    }
    LOG("  %s @ %llx:", what, (unsigned long long)a);
    const u8* p = (const u8*)a;
    for (u64 i = 0; i < n; i += 32) {
        char line[256];
        int o = 0;
        for (u64 j = i; j < i + 32 && j < n; j += 4) {
            u32 v;
            memcpy(&v, p + j, 4);
            o += snprintf(line + o, sizeof(line) - o, " %08x", v);
        }
        LOG("    +%03llx:%s", (unsigned long long)i, line);
    }
}

u64 Q(u64 a) {
    return Readable(a, 8) ? *(u64*)a : 0;
}

std::atomic<int> g_probes{0};

}  // namespace

void GfxProbeDraw(const char* kind, u32 count, u64 index_addr) {
    int n = g_probes++;
    if (!(n < 2 || (n >= 400 && n < 403) || (n >= 2000 && n < 2003))) return;
    u64 base = (u64)g_linker.eboot->base;
    u64 dev = Q(base + 0x26e7920);
    u64 ctx = Q(dev + 0x5b0);
    u64 gfx = Q(ctx + 0x58);
    LOG("=== probe draw #%d %s count %u index %llx dev %llx ctx %llx gfx %llx", n, kind, count,
        (unsigned long long)index_addr, (unsigned long long)dev, (unsigned long long)ctx,
        (unsigned long long)gfx);
    Dump("ctx viewport/scissor 0x0..0x40", ctx, 0x40);
    Dump("ctx states 0x100..0x200", ctx + 0x100, 0x100);
    Dump("ctx PS tex 0x220", ctx + 0x220, 0x80);
    Dump("ctx VS tex 0x320", ctx + 0x320, 0x40);
    Dump("ctx VB 0x4a0..0x660", ctx + 0x4a0, 0x1c0);
    u64 vs = Q(ctx + 0x5a8), ps = Q(ctx + 0x5b8);
    Dump("VS handle", vs, 0x60);
    Dump("VS +0x18 (header?)", Q(vs + 0x18), 0x90);
    Dump("PS handle", ps, 0x60);
    Dump("PS +0x18 (header?)", Q(ps + 0x18), 0x90);
    Dump("PS header->_shader", Q(Q(ps + 0x18) + 0x20), 0x80);
    Dump("PS header->_resourceOffsets", Q(Q(ps + 0x18) + 0x28), 0x100);
    for (int i = 0; i < 4; i++) {
        u64 t = Q(ctx + 0x220 + i * 8);
        if (t) Dump(("PS tex T# " + std::to_string(i)).c_str(), t, 0x20);
    }
    u64 rt0 = Q(ctx + 0x130);
    Dump("color target 0", rt0, 0x60);
    Dump("depth target", Q(ctx + 0x170), 0x60);
    u64 vb0 = Q(ctx + 0x4a0), vf0 = Q(ctx + 0x520);
    Dump("VB0", vb0, 0x40);
    Dump("VF0", vf0, 0x40);
    Dump("VF0 +0x18 elements", Q(vf0 + 0x18), 0x60);
    Dump("IB", Q(ctx + 0x5a0), 0x20);
    Dump("ctx 0x5a0..0x660 (ib/shaders/prim/strides)", ctx + 0x5a0, 0xc0);
    Dump("color target 0 -> +0x10", Q(rt0 + 0x10), 0x60);
    Dump("dev 0x478..0x5d0", dev + 0x478, 0x158);
    Dump("dev colorBackBufferView", Q(dev + 0x520), 0x60);
    Dump("dev colorBackBufferTexture0", Q(dev + 0x508), 0x40);
    Dump("VF0 +0x28 formats", Q(vf0 + 0x28), 0x20);
    Dump("VS header inputElements", Q(Q(vs + 0x18) + 0x40), 0x30);
    Dump("VS header cbmem[0]", Q(Q(Q(vs + 0x18) + 0x58)), 0x40);
    Dump("PS header cbmem[0]", Q(Q(Q(ps + 0x18) + 0x58)), 0x40);
    Dump("PS samplers 0x1a0", ctx + 0x1a0, 0x40);
    Dump("gfx 0x0..0x40", gfx, 0x40);
    Dump("gfx +0xab20..", gfx + 0xab20, 0x40);
    Dump("gfx VS offsets table", Q(gfx + 0xab28), 0x100);
    Dump("gfx PS offsets table", Q(gfx + 0xab30), 0x100);
    Dump("gfx +0x1a58 (PS shadow?)", gfx + 0x1a58, 0x100);
    Dump("gfx +600 (shadow base)", gfx + 600, 0x100);
    u64 ccb = Q(gfx + 0xaa58);
    Dump("gfx->0xaa58 struct", ccb, 0x40);
    LogFlush();
}
