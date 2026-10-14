// libSceSaveData / libSceSaveDataDialog on the PC file system. Every save data directory is a
// folder %APPDATA%\CTRNF\savedata\<dirName> (parameters in <dirName>\sce_sys\param.bin, icon in
// sce_sys\icon0.png) and is mounted at /savedata<N> for the libkernel file functions.
#include "savedata.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <vector>

#include "../hle.h"
#include "../linker.h"

namespace fs = std::filesystem;

namespace {

constexpr int kErrParameter = (int)0x809F0000, kErrNotMounted = (int)0x809F0004, kErrExists = (int)0x809F0007,
              kErrNotFound = (int)0x809F0008;
constexpr u32 kModeCreate = 4, kModeCreate2 = 32;

struct DirName {
    char data[32];
};
struct MountPoint {
    char data[16];
};
struct Mount2 {
    s32 user_id;
    s32 pad;
    const DirName* dir_name;
    u64 blocks;
    u32 mount_mode;
    u8 reserved[32];
};
struct MountResult {
    MountPoint mount_point;
    u64 required_blocks;
    u32 unused;
    u32 mount_status;  // 1: created
    u8 reserved[28];
};
struct Param {
    char title[128];
    char sub_title[128];
    char detail[1024];
    u32 user_param;
    u32 pad;
    s64 mtime;
    u8 reserved[32];
};
struct SearchInfo {
    u64 blocks;
    u64 free_blocks;
    u8 reserved[32];
};
struct SearchCond {
    s32 user_id;
    s32 pad;
    const void* title_id;
    const DirName* dir_name;
    u32 key;
    u32 order;
    u8 reserved[32];
};
struct SearchResult {
    u32 hit_num;
    s32 pad;
    DirName* dir_names;
    u32 dir_names_num;
    u32 set_num;
    Param* params;
    SearchInfo* infos;
    u8 reserved[12];
    s32 pad2;
};
struct Icon {
    void* buf;
    u64 buf_size;
    u64 data_size;
    u8 reserved[32];
};
struct Delete {
    s32 user_id;
    s32 pad;
    const void* title_id;
    const DirName* dir_name;
    u32 unused;
    u8 reserved[32];
    s32 pad2;
};

std::mutex g_mutex;
fs::path g_mounts[16];  // /savedata<N> -> host folder
}  // namespace
fs::path FlatSaveFile(const fs::path& dir, const std::string& file);
namespace {

fs::path SaveRoot() { return UserRoot() / "savedata"; }

std::atomic<bool> g_mount_called{false};

std::string Name(const DirName* d) { return d ? std::string(d->data, strnlen(d->data, sizeof(d->data))) : ""; }

int MountIndex(const MountPoint* mp) {
    if (!mp || strncmp(mp->data, "/savedata", 9) != 0) return -1;
    int n = atoi(mp->data + 9);
    return n >= 0 && n < 16 ? n : -1;
}

fs::path MountedDir(const MountPoint* mp) {
    std::lock_guard lk(g_mutex);
    int n = MountIndex(mp);
    return n < 0 ? fs::path() : g_mounts[n];
}

Param ReadParam(const fs::path& dir) {
    Param p{};
    std::ifstream f(dir / "sce_sys" / "param.bin", std::ios::binary);
    f.read((char*)&p, sizeof(p));
    if (!p.mtime) {
        std::error_code ec;
        auto t = fs::last_write_time(dir, ec);
        if (!ec)
            p.mtime = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
                          .count();
    }
    return p;
}

void WriteParam(const fs::path& dir, const Param& p) {
    std::error_code ec;
    fs::create_directories(dir / "sce_sys", ec);
    std::ofstream f(dir / "sce_sys" / "param.bin", std::ios::binary | std::ios::trunc);
    f.write((const char*)&p, sizeof(p));
}

SYSV int sceSaveDataInitialize3(const void*) {
    std::error_code ec;
    fs::create_directories(SaveRoot(), ec);
    LOG("savedata: %s", SaveRoot().string().c_str());
    return 0;
}
SYSV int sceSaveDataTerminate() { return 0; }

SYSV int sceSaveDataMount2(const Mount2* m, MountResult* r) {
    if (!m || !m->dir_name || !r) return kErrParameter;
    g_mount_called = true;
    std::string name = Name(m->dir_name);
    fs::path dir = SaveRoot() / name;
    std::error_code ec;
    bool exists = fs::is_directory(dir, ec);
    memset(r, 0, sizeof(*r));
    if (!exists && !(m->mount_mode & (kModeCreate | kModeCreate2))) {
        LOG("savedata: mount '%s' (mode 0x%x): not found", name.c_str(), m->mount_mode);
        return kErrNotFound;
    }
    if (exists && (m->mount_mode & kModeCreate) && !(m->mount_mode & kModeCreate2)) {
        LOG("savedata: mount '%s' (mode 0x%x): exists", name.c_str(), m->mount_mode);
        return kErrExists;
    }
    if (!exists) {
        fs::create_directories(dir, ec);
        r->mount_status = 1;
    }
    std::lock_guard lk(g_mutex);
    int n = 0;
    while (n < 16 && !g_mounts[n].empty()) n++;
    if (n == 16) return (int)0x809F0003;  // busy
    g_mounts[n] = dir;
    snprintf(r->mount_point.data, sizeof(r->mount_point.data), "/savedata%d", n);
    r->required_blocks = 0;
    LOG("savedata: mount '%s' (mode 0x%x) at %s%s", name.c_str(), m->mount_mode, r->mount_point.data,
        exists ? "" : " (created)");
    return 0;
}

SYSV int sceSaveDataUmount(const MountPoint* mp) {
    std::lock_guard lk(g_mutex);
    int n = MountIndex(mp);
    if (n < 0 || g_mounts[n].empty()) return kErrNotMounted;
    g_mounts[n].clear();
    return 0;
}

SYSV int sceSaveDataDirNameSearch(const SearchCond* c, SearchResult* r) {
    if (!c || !r) return kErrParameter;
    std::string pattern = Name(c->dir_name);
    size_t wild = pattern.find('%');
    std::vector<std::string> hits;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(SaveRoot(), ec)) {
        if (!e.is_directory()) continue;
        std::string n = e.path().filename().string();
        bool match = pattern.empty() ? true
                     : wild == std::string::npos ? n == pattern
                                                 : n.compare(0, wild, pattern, 0, wild) == 0;
        if (match) hits.push_back(n);
    }
    r->hit_num = (u32)hits.size();
    r->set_num = 0;
    for (u32 i = 0; i < hits.size() && r->dir_names && i < r->dir_names_num; i++) {
        memset(&r->dir_names[i], 0, sizeof(DirName));
        strncpy(r->dir_names[i].data, hits[i].c_str(), sizeof(DirName) - 1);
        if (r->params) r->params[i] = ReadParam(SaveRoot() / hits[i]);
        if (r->infos) r->infos[i] = SearchInfo{1 << 16, 1 << 20, {}};
        r->set_num = i + 1;
    }
    return 0;
}

SYSV int sceSaveDataSetParam(const MountPoint* mp, u32 type, const void* buf, u64 size) {
    fs::path dir = MountedDir(mp);
    if (dir.empty()) return kErrNotMounted;
    Param p = ReadParam(dir);
    switch (type) {
    case 0: memcpy(&p, buf, std::min<u64>(size, sizeof(p))); break;
    case 1: strncpy(p.title, (const char*)buf, sizeof(p.title) - 1); break;
    case 2: strncpy(p.sub_title, (const char*)buf, sizeof(p.sub_title) - 1); break;
    case 3: strncpy(p.detail, (const char*)buf, sizeof(p.detail) - 1); break;
    case 4: memcpy(&p.user_param, buf, 4); break;
    default: break;
    }
    p.mtime = time(nullptr);
    WriteParam(dir, p);
    return 0;
}

SYSV int sceSaveDataGetParam(const MountPoint* mp, u32 type, void* buf, u64 size, u64* got) {
    fs::path dir = MountedDir(mp);
    if (dir.empty()) return kErrNotMounted;
    Param p = ReadParam(dir);
    const void* src = &p;
    u64 n = sizeof(p);
    switch (type) {
    case 1: src = p.title, n = sizeof(p.title); break;
    case 2: src = p.sub_title, n = sizeof(p.sub_title); break;
    case 3: src = p.detail, n = sizeof(p.detail); break;
    case 4: src = &p.user_param, n = 4; break;
    case 5: src = &p.mtime, n = 8; break;
    default: break;
    }
    n = std::min(n, size);
    memcpy(buf, src, n);
    if (got) *got = n;
    return 0;
}

SYSV int sceSaveDataSaveIcon(const MountPoint* mp, const Icon* icon) {
    fs::path dir = MountedDir(mp);
    if (dir.empty()) return kErrNotMounted;
    if (!icon || !icon->buf) return kErrParameter;
    std::error_code ec;
    fs::create_directories(dir / "sce_sys", ec);
    std::ofstream f(dir / "sce_sys" / "icon0.png", std::ios::binary | std::ios::trunc);
    f.write((const char*)icon->buf, (std::streamsize)icon->data_size);
    return 0;
}

SYSV int sceSaveDataLoadIcon(const MountPoint* mp, Icon* icon) {
    fs::path dir = MountedDir(mp);
    if (dir.empty()) return kErrNotMounted;
    std::ifstream f(dir / "sce_sys" / "icon0.png", std::ios::binary);
    if (!f || !icon || !icon->buf) return kErrNotFound;
    f.read((char*)icon->buf, (std::streamsize)icon->buf_size);
    icon->data_size = (u64)f.gcount();
    return 0;
}

SYSV int sceSaveDataDelete(const Delete* d) {
    if (!d || !d->dir_name) return kErrParameter;
    std::error_code ec;
    fs::path dir = SaveRoot() / Name(d->dir_name);
    if (!fs::exists(dir, ec)) return kErrNotFound;
    fs::remove_all(dir, ec);
    if (fs::path flat = FlatSaveFile(dir, "SYS-DATA.sav"); !flat.empty()) fs::remove(flat, ec);
    LOG("savedata: deleted '%s'", Name(d->dir_name).c_str());
    return 0;
}

// Save data dialog (error / confirmation popups): reported as closed right away.
SYSV int sceSaveDataDialogInitialize() { return 0; }
SYSV int sceSaveDataDialogOpen(const void*) { return 0; }
SYSV int sceSaveDataDialogUpdateStatus() { return 3; }  // SCE_COMMON_DIALOG_STATUS_FINISHED
SYSV int sceSaveDataDialogTerminate() { return 0; }

HLE("libSceSaveData", sceSaveDataInitialize3);
HLE("libSceSaveData", sceSaveDataTerminate);
HLE("libSceSaveData", sceSaveDataMount2);
HLE("libSceSaveData", sceSaveDataUmount);
HLE("libSceSaveData", sceSaveDataDirNameSearch);
HLE("libSceSaveData", sceSaveDataSetParam);
HLE("libSceSaveData", sceSaveDataGetParam);
HLE("libSceSaveData", sceSaveDataSaveIcon);
HLE("libSceSaveData", sceSaveDataLoadIcon);
HLE("libSceSaveData", sceSaveDataDelete);
HLE("libSceSaveDataDialog", sceSaveDataDialogInitialize);
HLE("libSceSaveDataDialog", sceSaveDataDialogOpen);
HLE("libSceSaveDataDialog", sceSaveDataDialogUpdateStatus);
HLE("libSceSaveDataDialog", sceSaveDataDialogTerminate);

}  // namespace

fs::path UserRoot() {
    static const fs::path root = [] {
        wchar_t buf[MAX_PATH];
        // CTR_USER_DIR: another folder (test runs keep away from the player's saves)
        DWORD n = GetEnvironmentVariableW(L"CTR_USER_DIR", buf, MAX_PATH);
        if (n && n < MAX_PATH) return fs::path(buf);
        n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
        fs::path p = n && n < MAX_PATH ? fs::path(buf) / "CTRNF" : fs::path(g_linker.game_dir) / "userdata";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }();
    return root;
}

// The game's save files have PC names directly in %APPDATA%/CTRNF:
//   savedata0/SYS-DATA.sav (global / system data) -> WindowData.sav (as the PC Crash trilogy)
//   savedata<N>/SYS-DATA.sav (save slot N)         -> SaveSlot<N>.sav
fs::path FlatSaveFile(const fs::path& dir, const std::string& file) {
    if (_stricmp(file.c_str(), "SYS-DATA.sav") != 0) return {};
    std::string d = dir.filename().string();
    if (d == "savedata0") return UserRoot() / "WindowData.sav";
    if (d.compare(0, 8, "savedata") == 0 && d.size() > 8) return UserRoot() / ("SaveSlot" + d.substr(8) + ".sav");
    return {};
}

void MoveOnce(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec) || !fs::is_regular_file(from, ec)) return;
    fs::rename(from, to, ec);
    if (ec) fs::copy_file(from, to, ec);
    LOG("savedata: moved %s -> %s", from.string().c_str(), to.string().c_str());
}

bool SaveDataMountedPath(const std::string& guest, fs::path* host) {
    if (guest.compare(0, 9, "/savedata") != 0) return false;
    size_t slash = guest.find('/', 1);
    std::string mp = guest.substr(0, slash);
    MountPoint m{};
    strncpy(m.data, mp.c_str(), sizeof(m.data) - 1);
    fs::path dir = MountedDir(&m);
    if (dir.empty()) dir = SaveRoot() / mp.substr(1);  // not mounted: keep the old layout
    *host = slash == std::string::npos ? dir : dir / fs::path(guest.substr(slash + 1)).relative_path();
    if (slash == std::string::npos || host->parent_path() != dir) return true;
    if (fs::path flat = FlatSaveFile(dir, host->filename().string()); !flat.empty()) {
        MoveOnce(*host, flat);  // layout of the previous build
        MoveOnce(fs::path(g_linker.game_dir) / host->filename(), flat);  // even older: the game folder
        *host = flat;
    }
    return true;
}

bool SaveDataMountCalled() { return g_mount_called; }
