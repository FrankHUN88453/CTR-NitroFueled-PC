// Byte patches applied to the eboot after loading (CUSA13795, eboot VAs). Every patch checks
// the original bytes first, so a different game build is left untouched.
#include "patches.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "linker.h"

namespace {

struct Patch {
    u64 va;
    const char* orig;  // hex
    const char* repl;  // hex, same length
    const char* what;
};

// License agreement / privacy policy: the profile flags (persistent data +0x29 / +0x2a) read as
// accepted, so "Press X to begin" goes straight on (set CTR_SHOW_EULA=1 to keep the screens).
const Patch kSkipEula[] = {
    {0x10596b7, "0f95c0", "b00190", "CGameUtilityNodes::hasAcceptedEULA -> true"},
    {0x1059727, "0f95c0", "b00190", "CGameUtilityNodes::hasAcceptedPrivacyPolicy -> true"},
    {0x10cfbc0, "488b0599bf5f01", "b801000000c390", "CNetworkTaskWaitForEULAAndPrivatePolicy::vf23 -> true"},
    {0xa48251, "742b", "9090", "CInviteManager: EULA check"},
    {0xa48257, "7425", "9090", "CInviteManager: privacy policy check"},
};

int Hex(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

size_t Decode(const char* hex, u8* out) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) out[i] = (u8)(Hex(hex[2 * i]) << 4 | Hex(hex[2 * i + 1]));
    return n;
}

void Apply(const Patch& p) {
    u8 orig[32], repl[32];
    size_t n = Decode(p.orig, orig);
    Decode(p.repl, repl);
    u8* dst = g_linker.eboot->base + p.va;
    if (memcmp(dst, orig, n) != 0) {
        LOG("patch %llx (%s): original bytes differ, skipped", (unsigned long long)p.va, p.what);
        return;
    }
    DWORD old;
    VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(dst, repl, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    LOG("patch %llx: %s", (unsigned long long)p.va, p.what);
}

bool Env(const char* name) {
    char v[8] = {};
    return GetEnvironmentVariableA(name, v, sizeof(v)) && v[0] == '1';
}

}  // namespace

void ApplyGamePatches() {
    if (!Env("CTR_SHOW_EULA"))
        for (const Patch& p : kSkipEula) Apply(p);
}
