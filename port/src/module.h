#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.h"

struct ElfPhdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};

struct ImportLib {
    u16 id;
    std::string name;
};

struct Symbol {
    std::string raw;  // "nid#L#M" or plain name
    std::string nid;
    std::string library;  // resolved library name (import or export lib)
    u8 type, bind;
    bool defined;
    u64 value;
};

struct Module {
    std::string name;  // module_info name (e.g. "libc", "game-ps4-fin")
    std::string path;
    std::vector<u8> elf;  // reconstructed ELF image (file layout)
    std::vector<ElfPhdr> phdrs;
    u64 entry = 0;

    u8* base = nullptr;  // loaded image
    u64 image_size = 0;
    u8* code_begin = nullptr;
    u64 code_size = 0;
    u8* tramp = nullptr;  // trampoline area right after the image (rel32 reachable)
    u64 tramp_size = 0, tramp_used = 0;

    // dynamic info (offsets inside dynlib data)
    const u8* dynlib = nullptr;
    u64 strtab = 0, strsz = 0, symtab = 0, symtabsz = 0;
    u64 rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    u64 init = 0, fini = 0;
    bool has_init = false;
    std::vector<ImportLib> import_libs, export_libs;
    std::vector<std::string> needed;
    std::vector<Symbol> symbols;

    // TLS
    u64 tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    u32 tls_module_id = 0;  // 0 = no TLS
    u64 tls_offset = 0;     // static TLS: block lives at TP - tls_offset

    u64 procparam = 0;  // vaddr of PT_SCE_PROCPARAM (eboot only)

    // optional function names (offset, name), sorted - from Ghidra's ExportFunctions.java
    std::vector<std::pair<u64, std::string>> func_names;
    bool LoadFunctionNames(const std::string& path);

    bool LoadFile(const std::string& path);
    void MapIntoMemory();
    // resolve(module, symbol) -> address; called for undefined symbols
    void Relocate(const std::function<u64(Module&, const Symbol&)>& resolve);
    int PatchTlsAccess(u32 win_tls_slot);

    u8* Tramp(size_t n);
    bool Contains(u64 addr) const { return addr >= (u64)base && addr < (u64)base + image_size; }
};
