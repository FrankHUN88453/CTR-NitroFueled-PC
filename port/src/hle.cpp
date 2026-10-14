#include "hle.h"

#include <unordered_map>

namespace {
std::unordered_map<std::string, void*>& Table() {
    static std::unordered_map<std::string, void*> t;
    return t;
}
}  // namespace

void HleRegister(const char* /*lib*/, const char* name, void* addr) {
    Table()[NameToNid(name)] = addr;
}

void* HleLookup(const std::string& nid) {
    auto it = Table().find(nid);
    return it == Table().end() ? nullptr : it->second;
}
