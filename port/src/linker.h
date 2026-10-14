#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "module.h"

struct Linker {
    std::vector<std::unique_ptr<Module>> modules;
    Module* eboot = nullptr;
    std::string game_dir;
    u64 tls_static_size = 0;  // bytes below the thread pointer
    u64 tls_max_align = 16;

    Module* Load(const std::string& path);
    void LinkAll();
    Module* FindByAddr(u64 addr);
    // "module+0xoffset" description of an address
    std::string Describe(u64 addr);

private:
    u64 Resolve(Module& m, const Symbol& s);
    u64 MakeStub(const Symbol& s, const std::string& library);
    std::unordered_map<std::string, u64> stub_cache_;
};

extern Linker g_linker;
