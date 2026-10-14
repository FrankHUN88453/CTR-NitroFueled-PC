#include "module.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#include "memory.h"

namespace {

constexpr u32 PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7;
constexpr u32 PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_PROCPARAM = 0x61000001,
              PT_SCE_RELRO = 0x61000010;

constexpr s64 DT_NULL = 0, DT_NEEDED = 1, DT_INIT = 0xC, DT_FINI = 0xD;
constexpr s64 DT_SCE_MODULE_INFO = 0x6100000D, DT_SCE_EXPORT_LIB = 0x61000013,
              DT_SCE_IMPORT_LIB = 0x61000015, DT_SCE_JMPREL = 0x61000029,
              DT_SCE_PLTRELSZ = 0x6100002D, DT_SCE_RELA = 0x6100002F,
              DT_SCE_RELASZ = 0x61000031, DT_SCE_STRTAB = 0x61000035,
              DT_SCE_STRSZ = 0x61000037, DT_SCE_SYMTAB = 0x61000039,
              DT_SCE_SYMTABSZ = 0x6100003F;

constexpr u32 R_X86_64_64 = 1, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7,
              R_X86_64_RELATIVE = 8, R_X86_64_DTPMOD64 = 16, R_X86_64_DTPOFF64 = 17,
              R_X86_64_TPOFF64 = 18;

constexpr u64 kPage = 0x4000;
constexpr u64 kTrampSize = 4 << 20;

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

u64 AlignUp(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

int DecodeB64Index(const std::string& s) {
    int v = 0;
    for (char c : s) {
        const char* p = strchr(kB64, c);
        if (!p || !c) {
            return -1;
        }
        v = v * 64 + (int)(p - kB64);
    }
    return v;
}

template <class T>
T Rd(const std::vector<u8>& b, u64 off) {
    T v;
    memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

// Fake-signed SELF -> ELF file image (same as tools/self2elf.py).
bool SelfToElf(const std::vector<u8>& self, std::vector<u8>& out) {
    if (self.size() < 0x20) {
        return false;
    }
    if (memcmp(self.data(), "\x7f" "ELF", 4) == 0) {
        out = self;
        return true;
    }
    if (memcmp(self.data(), "\x4f\x15\x3d\x1d", 4) != 0) {
        return false;
    }
    u16 nseg = Rd<u16>(self, 0x18);
    u64 elf_off = 0x20 + (u64)nseg * 0x20;
    u64 phoff = Rd<u64>(self, elf_off + 0x20);
    u16 phentsize = Rd<u16>(self, elf_off + 0x36), phnum = Rd<u16>(self, elf_off + 0x38);
    u64 hdr = phoff + (u64)phnum * phentsize;
    u64 end = hdr;
    std::vector<ElfPhdr> ph(phnum);
    for (u16 i = 0; i < phnum; i++) {
        memcpy(&ph[i], self.data() + elf_off + phoff + (u64)i * phentsize, sizeof(ElfPhdr));
        end = std::max<u64>(end, ph[i].offset + ph[i].filesz);
    }
    out.assign(end, 0);
    memcpy(out.data(), self.data() + elf_off, hdr);
    for (u16 i = 0; i < nseg; i++) {
        u64 flags = Rd<u64>(self, 0x20 + i * 0x20), off = Rd<u64>(self, 0x28 + i * 0x20);
        u64 size = Rd<u64>(self, 0x30 + i * 0x20);
        if (!(flags & 0x800)) {
            continue;
        }
        if (flags & 0x2) {
            Fatal("SELF segment %u is encrypted - a decrypted dump is required", i);
        }
        if (flags & 0x8) {
            Fatal("compressed SELF segments are not supported yet");
        }
        u32 idx = (u32)((flags >> 20) & 0xFFF);
        u64 n = std::min(size, ph[idx].filesz);
        memcpy(out.data() + ph[idx].offset, self.data() + off, n);
    }
    for (auto& p : ph) {
        if (p.type == 0x6FFFFF01 && p.filesz) {
            memcpy(out.data() + p.offset, self.data() + self.size() - p.filesz, p.filesz);
        }
    }
    return true;
}

}  // namespace

bool Module::LoadFile(const std::string& p) {
    path = p;
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return false;
    }
    std::vector<u8> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!SelfToElf(raw, elf)) {
        Fatal("%s: not a SELF/ELF file", p.c_str());
    }
    entry = Rd<u64>(elf, 0x18);
    u64 phoff = Rd<u64>(elf, 0x20);
    u16 phnum = Rd<u16>(elf, 0x38);
    phdrs.resize(phnum);
    for (u16 i = 0; i < phnum; i++) {
        memcpy(&phdrs[i], elf.data() + phoff + i * 56, sizeof(ElfPhdr));
    }

    const ElfPhdr* dyn = nullptr;
    for (auto& ph : phdrs) {
        if (ph.type == PT_DYNAMIC) {
            dyn = &ph;
        } else if (ph.type == PT_SCE_DYNLIBDATA) {
            dynlib = elf.data() + ph.offset;
        } else if (ph.type == PT_TLS) {
            tls_vaddr = ph.vaddr;
            tls_filesz = ph.filesz;
            tls_memsz = ph.memsz;
            tls_align = ph.align ? ph.align : 1;
        } else if (ph.type == PT_SCE_PROCPARAM) {
            procparam = ph.vaddr;
        }
    }
    if (!dyn || !dynlib) {
        Fatal("%s: missing dynamic info", p.c_str());
    }

    struct Tag {
        s64 tag;
        u64 val;
    };
    std::vector<Tag> tags;
    for (u64 o = dyn->offset; o < dyn->offset + dyn->filesz; o += 16) {
        Tag t{Rd<s64>(elf, o), Rd<u64>(elf, o + 8)};
        if (t.tag == DT_NULL) {
            break;
        }
        tags.push_back(t);
        switch (t.tag) {
        case DT_SCE_STRTAB: strtab = t.val; break;
        case DT_SCE_STRSZ: strsz = t.val; break;
        case DT_SCE_SYMTAB: symtab = t.val; break;
        case DT_SCE_SYMTABSZ: symtabsz = t.val; break;
        case DT_SCE_RELA: rela = t.val; break;
        case DT_SCE_RELASZ: relasz = t.val; break;
        case DT_SCE_JMPREL: jmprel = t.val; break;
        case DT_SCE_PLTRELSZ: pltrelsz = t.val; break;
        case DT_INIT: init = t.val; has_init = true; break;
        case DT_FINI: fini = t.val; break;
        }
    }
    auto str = [&](u64 off) { return std::string((const char*)dynlib + strtab + off); };
    for (auto& t : tags) {
        u64 v = t.val;
        if (t.tag == DT_NEEDED) {
            needed.push_back(str(v));
        } else if (t.tag == DT_SCE_IMPORT_LIB) {
            import_libs.push_back({(u16)(v >> 48), str(v & 0xFFFFFFFF)});
        } else if (t.tag == DT_SCE_EXPORT_LIB) {
            export_libs.push_back({(u16)(v >> 48), str(v & 0xFFFFFFFF)});
        } else if (t.tag == DT_SCE_MODULE_INFO) {
            name = str(v & 0xFFFFFFFF);
        }
    }

    auto lib_name = [&](const std::vector<ImportLib>& libs, int id) -> std::string {
        for (auto& l : libs) {
            if (l.id == id) {
                return l.name;
            }
        }
        return "?";
    };
    for (u64 o = symtab; o < symtab + symtabsz; o += 24) {
        Symbol s{};
        u32 st_name;
        memcpy(&st_name, dynlib + o, 4);
        u8 info = dynlib[o + 4];
        u16 shndx;
        memcpy(&shndx, dynlib + o + 6, 2);
        memcpy(&s.value, dynlib + o + 8, 8);
        s.raw = str(st_name);
        s.type = info & 0xF;
        s.bind = info >> 4;
        s.defined = shndx != 0;
        size_t h1 = s.raw.find('#');
        size_t h2 = h1 == std::string::npos ? h1 : s.raw.find('#', h1 + 1);
        if (h2 != std::string::npos) {
            s.nid = s.raw.substr(0, h1);
            int lib = DecodeB64Index(s.raw.substr(h1 + 1, h2 - h1 - 1));
            s.library = lib_name(s.defined ? export_libs : import_libs, lib);
        }
        symbols.push_back(std::move(s));
    }
    return true;
}

void Module::MapIntoMemory() {
    u64 end = 0;
    for (auto& ph : phdrs) {
        if (ph.type == PT_LOAD || ph.type == PT_SCE_RELRO) {
            end = std::max(end, ph.vaddr + ph.memsz);
        }
    }
    image_size = AlignUp(end, kPage);
    tramp_size = kTrampSize;
    base = (u8*)GuestAlloc(image_size + tramp_size, kPage, PAGE_EXECUTE_READWRITE);
    if (!base) {
        Fatal("%s: cannot allocate %llu bytes", name.c_str(), (unsigned long long)image_size);
    }
    tramp = base + image_size;
    for (auto& ph : phdrs) {
        if (ph.type == PT_LOAD || ph.type == PT_SCE_RELRO) {
            memcpy(base + ph.vaddr, elf.data() + ph.offset, ph.filesz);
            if (ph.type == PT_LOAD && (ph.flags & 1) && !code_begin) {
                code_begin = base + ph.vaddr;
                code_size = ph.filesz;
            }
        }
    }
    LOG("mapped %-20s at %p size 0x%llx", name.c_str(), base, (unsigned long long)image_size);
}

bool Module::LoadFunctionNames(const std::string& p) {
    std::ifstream f(p);
    if (!f) {
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            continue;
        }
        func_names.emplace_back(std::stoull(line.substr(0, sp), nullptr, 16), line.substr(sp + 1));
    }
    std::sort(func_names.begin(), func_names.end());
    LOG("%s: %zu function names from %s", name.c_str(), func_names.size(), p.c_str());
    return true;
}

u8* Module::Tramp(size_t n) {
    if (tramp_used + n > tramp_size) {
        Fatal("%s: trampoline area exhausted", name.c_str());
    }
    u8* p = tramp + tramp_used;
    tramp_used += (n + 15) & ~15ull;
    return p;
}

void Module::Relocate(const std::function<u64(Module&, const Symbol&)>& resolve) {
    auto apply = [&](u64 off, u64 size) {
        for (u64 o = off; o < off + size; o += 24) {
            u64 r_off, r_info;
            s64 r_add;
            memcpy(&r_off, dynlib + o, 8);
            memcpy(&r_info, dynlib + o + 8, 8);
            memcpy(&r_add, dynlib + o + 16, 8);
            u32 type = (u32)r_info, sym = (u32)(r_info >> 32);
            u64* where = (u64*)(base + r_off);
            auto sym_addr = [&]() -> u64 {
                const Symbol& s = symbols[sym];
                return s.defined ? (u64)base + s.value : resolve(*this, s);
            };
            switch (type) {
            case R_X86_64_RELATIVE: *where = (u64)base + r_add; break;
            case R_X86_64_64: *where = sym_addr() + r_add; break;
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: *where = sym_addr(); break;
            case R_X86_64_DTPMOD64: *where = tls_module_id; break;
            case R_X86_64_DTPOFF64: *where = symbols[sym].value + r_add; break;
            case R_X86_64_TPOFF64: *where = symbols[sym].value + r_add - tls_offset; break;
            default: Fatal("%s: unsupported relocation type %u", name.c_str(), type);
            }
        }
    };
    apply(rela, relasz);
    apply(jmprel, pltrelsz);
}

// The PS4 thread pointer is read with `mov reg, qword ptr fs:[0]`
// (64 REX 8B /r with SIB 25 disp32 0). Windows does not let us point fs at our
// TCB, so each site jumps to a trampoline doing `mov reg, gs:[TEB TLS slot]`.
int Module::PatchTlsAccess(u32 slot) {
    const u32 teb_off = 0x1480 + slot * 8;
    int n = 0;
    u8* p = code_begin;
    u8* end = code_begin + code_size - 9;
    for (; p < end; p++) {
        if (p[0] != 0x64 || (p[1] != 0x48 && p[1] != 0x4C) || p[2] != 0x8B ||
            (p[3] & 0xC7) != 0x04 || p[4] != 0x25 || p[5] || p[6] || p[7] || p[8]) {
            continue;
        }
        u8* t = Tramp(16);
        t[0] = 0x65;  // gs:
        t[1] = p[1];
        t[2] = 0x8B;
        t[3] = p[3];
        t[4] = 0x25;
        memcpy(t + 5, &teb_off, 4);
        t[9] = 0xE9;  // jmp back
        s32 back = (s32)((p + 9) - (t + 14));
        memcpy(t + 10, &back, 4);
        p[0] = 0xE9;
        s32 to = (s32)(t - (p + 5));
        memcpy(p + 1, &to, 4);
        memset(p + 5, 0x90, 4);
        p += 8;
        n++;
    }
    FlushInstructionCache(GetCurrentProcess(), code_begin, code_size);
    return n;
}
