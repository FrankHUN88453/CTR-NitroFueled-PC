#include "linker.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "hle.h"
#include "thread.h"

Linker g_linker;

namespace {

struct StubInfo {
    std::string library, nid;
    const char* name;
    std::atomic<u32> calls{0};
};
std::vector<StubInfo*> g_stubs;
DWORD g_stub_slot;
u8* g_stub_code;
size_t g_stub_used;

SYSV u64 UnimplementedHandler() {
    u64 idx = (u64)TlsGetValue(g_stub_slot);
    StubInfo* s = idx < g_stubs.size() ? g_stubs[idx] : nullptr;
    if (s) {
        u32 n = ++s->calls;
        if (n <= 3 || (n & (n - 1)) == 0) {
            LOG("unimplemented %s::%s (%s) call #%u -> 0", s->library.c_str(),
                s->name ? s->name : "?", s->nid.c_str(), n);
        }
    }
    return 0;
}

}  // namespace

Module* Linker::Load(const std::string& path) {
    auto m = std::make_unique<Module>();
    if (!m->LoadFile(path)) {
        return nullptr;
    }
    LOG("loaded %s: module '%s', %zu symbols, %zu import libs, %zu export libs", path.c_str(),
        m->name.c_str(), m->symbols.size(), m->import_libs.size(), m->export_libs.size());
    for (auto& l : m->export_libs) {
        LOG("    exports library %s", l.name.c_str());
    }
    modules.push_back(std::move(m));
    return modules.back().get();
}

u64 Linker::MakeStub(const Symbol& s, const std::string& library) {
    std::string key = library + "#" + s.nid;
    if (auto it = stub_cache_.find(key); it != stub_cache_.end()) {
        return it->second;
    }
    u64 addr;
    if (s.type == 1 /*STT_OBJECT*/) {
        // unresolved data object: give it zeroed storage
        addr = (u64)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        LOG("unresolved data %s::%s (%s) -> zero page", library.c_str(),
            NidToName(s.nid) ? NidToName(s.nid) : "?", s.nid.c_str());
    } else {
        if (!g_stub_code) {
            g_stub_slot = TlsAlloc();
            if (g_stub_slot >= 64) {
                Fatal("TLS slot %lu too high", g_stub_slot);
            }
            g_stub_code = (u8*)VirtualAlloc(nullptr, 1 << 20, MEM_RESERVE | MEM_COMMIT,
                                            PAGE_EXECUTE_READWRITE);
        }
        u32 idx = (u32)g_stubs.size();
        auto* info = new StubInfo{library, s.nid, NidToName(s.nid)};
        g_stubs.push_back(info);
        u8* p = g_stub_code + g_stub_used;
        g_stub_used += 32;
        // mov qword ptr gs:[TLS slot], idx ; jmp [rip+0] ; .quad handler
        u32 teb = 0x1480 + g_stub_slot * 8;
        u8 code[] = {0x65, 0x48, 0xC7, 0x04, 0x25, 0, 0, 0, 0, 0, 0, 0, 0,
                     0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(code + 5, &teb, 4);
        memcpy(code + 9, &idx, 4);
        u64 h = (u64)&UnimplementedHandler;
        memcpy(code + 19, &h, 8);
        memcpy(p, code, sizeof(code));
        addr = (u64)p;
    }
    stub_cache_[key] = addr;
    return addr;
}

u64 Linker::Resolve(Module& m, const Symbol& s) {
    if (s.nid.empty()) {
        return MakeStub(s, "?");
    }
    // 1) real (LLE) export of a loaded module, same library name
    Module* any_lib = nullptr;
    u64 any_addr = 0;
    for (auto& o : modules) {
        if (o.get() == &m) {
            continue;
        }
        for (auto& e : o->symbols) {
            if (!e.defined || e.nid != s.nid || e.bind == 0) {
                continue;
            }
            if (e.library == s.library) {
                return (u64)o->base + e.value;
            }
            any_lib = o.get();
            any_addr = (u64)o->base + e.value;
        }
    }
    // 2) native implementation
    if (void* h = HleLookup(s.nid)) {
        return (u64)h;
    }
    // 3) LLE export under a different library name (e.g. libSceLibcInternal vs libc)
    if (any_lib) {
        return any_addr;
    }
    return MakeStub(s, s.library);
}

void Linker::LinkAll() {
    // static TLS layout (variant II): main module first
    u64 off = 0;
    u32 id = 1;
    for (auto& m : modules) {
        if (m->tls_memsz == 0 && m->tls_filesz == 0) {
            continue;
        }
        m->tls_module_id = id++;
        off = (off + m->tls_memsz + m->tls_align - 1) & ~(m->tls_align - 1);
        m->tls_offset = off;
        tls_max_align = std::max(tls_max_align, m->tls_align);
        LOG("TLS: %s id %u offset 0x%llx size 0x%llx", m->name.c_str(), m->tls_module_id,
            (unsigned long long)off, (unsigned long long)m->tls_memsz);
    }
    tls_static_size = (off + tls_max_align - 1) & ~(tls_max_align - 1);

    for (auto& m : modules) {
        m->MapIntoMemory();
    }
    for (auto& m : modules) {
        m->Relocate([this](Module& mod, const Symbol& s) { return Resolve(mod, s); });
    }
    for (auto& m : modules) {
        int n = m->PatchTlsAccess(TlsWinSlot());
        LOG("%s: patched %d TLS accesses", m->name.c_str(), n);
    }
    LOG("unresolved functions: %zu (stubbed)", g_stubs.size());
}

Module* Linker::FindByAddr(u64 addr) {
    for (auto& m : modules) {
        if (m->Contains(addr)) {
            return m.get();
        }
    }
    return nullptr;
}

std::string Linker::Describe(u64 addr) {
    char buf[512];
    HMODULE self = GetModuleHandleA(nullptr);
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &owner) && owner) {
        static bool sym_init = [] {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0;
        }();
        alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 256];
        auto* si = (SYMBOL_INFO*)sbuf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        char modname[MAX_PATH] = "?";
        GetModuleFileNameA(owner, modname, MAX_PATH);
        const char* slash = strrchr(modname, '\\');
        const char* base = slash ? slash + 1 : modname;
        if (sym_init && SymFromAddr(GetCurrentProcess(), addr, &disp, si)) {
            snprintf(buf, sizeof(buf), "%s!%s+0x%llx", base, si->Name, (unsigned long long)disp);
        } else {
            snprintf(buf, sizeof(buf), "%s+0x%llx", base, (unsigned long long)(addr - (u64)owner));
        }
        (void)self;
        return buf;
    }
    if (Module* m = FindByAddr(addr)) {
        u64 off = addr - (u64)m->base;
        auto it = std::upper_bound(m->func_names.begin(), m->func_names.end(), off,
                                   [](u64 v, const auto& e) { return v < e.first; });
        if (it != m->func_names.begin()) {
            --it;
            snprintf(buf, sizeof(buf), "%s!%s+0x%llx", m->name.c_str(), it->second.c_str(),
                     (unsigned long long)(off - it->first));
            return buf;
        }
        snprintf(buf, sizeof(buf), "%s+0x%llx", m->name.c_str(), (unsigned long long)off);
    } else {
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
    }
    return buf;
}
