#include "prompts.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "memory.h"

namespace {

struct Text {
    const char* original;  // as loaded (English); U+0298 is the font's Cross glyph, ` its Triangle
    const char* pc;        // not longer than the original
};
const Text kTexts[] = {
    {"Press \xCA\x98 to begin", "Click to begin"},
    {"Press ` to skip", "Click to skip"},
};
constexpr int kCount = sizeof(kTexts) / sizeof(kTexts[0]);
constexpr int kMaxScans = 20;

std::mutex g_mutex;
std::vector<u64> g_sites[kCount];  // loaded copies (either wording)
std::atomic<bool> g_scanning{false};
int g_scans = 0;
ULONGLONG g_next_scan = 0;

// Both wordings including the terminator: a copy may hold either.
std::string_view Pattern(int i, bool pc) {
    const char* s = pc ? kTexts[i].pc : kTexts[i].original;
    return {s, strlen(s) + 1};
}

void Scan() {
    ULONGLONG t0 = GetTickCount64();
    constexpr size_t kChunk = 1 << 20, kOverlap = 64;
    std::vector<char> buf(kChunk + kOverlap);
    std::vector<u64> found[kCount];
    u64 scanned = 0;
    GuestForEachWrittenRun([&](u64 begin, u64 end) {
        for (u64 at = begin; at < end; at += kChunk) {
            size_t n = (size_t)std::min<u64>(kChunk + kOverlap, end - at);
            if (!GuestTryCopy(buf.data(), (const void*)at, n)) continue;
            scanned += std::min<u64>(kChunk, n);
            for (int i = 0; i < kCount; i++)
                for (bool pc : {false, true}) {
                    std::string_view p = Pattern(i, pc);
                    std::boyer_moore_horspool_searcher search(p.begin(), p.end());
                    for (auto it = buf.begin(); (it = std::search(it, buf.begin() + n, search)) != buf.begin() + n; ++it) {
                        size_t off = it - buf.begin();
                        if (off < kChunk) found[i].push_back(at + off);  // overlap matches: next chunk
                    }
                }
        }
    });
    std::lock_guard lk(g_mutex);
    g_scans++;
    for (int i = 0; i < kCount; i++) {
        for (u64 a : found[i])
            if (std::find(g_sites[i].begin(), g_sites[i].end(), a) == g_sites[i].end()) g_sites[i].push_back(a);
        LOG("prompts: scan %d: %zu copies of '%s' (%llu MiB in %llu ms)", g_scans, found[i].size(), kTexts[i].pc,
            (unsigned long long)(scanned >> 20), (unsigned long long)(GetTickCount64() - t0));
    }
    g_scanning = false;
}

// Puts the wanted wording into every known copy; drops copies that hold something else now.
void Apply(bool keyboard) {
    for (int i = 0; i < kCount; i++) {
        std::string_view want = Pattern(i, keyboard), other = Pattern(i, !keyboard);
        auto& sites = g_sites[i];
        for (size_t k = 0; k < sites.size();) {
            char cur[64] = {};
            size_t n = std::max(want.size(), other.size());
            bool ok = GuestTryCopy(cur, (const void*)sites[k], n);
            if (ok && memcmp(cur, want.data(), want.size()) == 0) {
                k++;
                continue;
            }
            if (ok && memcmp(cur, other.data(), other.size()) == 0 &&
                GuestTryCopy((void*)sites[k], want.data(), want.size())) {
                k++;
                continue;
            }
            sites.erase(sites.begin() + k);  // reused or unmapped memory
        }
    }
}

}  // namespace

void PromptTextUpdate(bool keyboard) {
    std::lock_guard lk(g_mutex);
    Apply(keyboard);
    if (!keyboard || g_scanning || g_scans >= kMaxScans) return;
    bool missing = false;
    for (const auto& s : g_sites) missing |= s.empty();
    ULONGLONG now = GetTickCount64();
    if (!missing || now < g_next_scan) return;
    // the texts load early (permanent archives): look again soon, then less often
    g_next_scan = now + std::min(1000 + 1500 * g_scans, 15000);
    g_scanning = true;
    std::thread(Scan).detach();
}
