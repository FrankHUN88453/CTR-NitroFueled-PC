// libkernel file I/O on top of Win32.
// /app0/...  -> game directory (the PS4 dump)
// /savedata<N> -> the mounted save data folder (savedata.cpp), /download0, /temp0, /data ->
// %APPDATA%\CTRNF\<name>
#include <windows.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../hle.h"
#include "../linker.h"
#include "../thread.h"
#include "savedata.h"

using namespace err;
namespace fs = std::filesystem;

namespace {

constexpr int O_ACCMODE_ = 3, O_WRONLY_ = 1, O_RDWR_ = 2, O_APPEND_ = 0x8, O_CREAT_ = 0x200,
              O_TRUNC_ = 0x400, O_EXCL_ = 0x800, O_DIRECTORY_ = 0x20000;

#ifdef _WIN32
using HostFile = HANDLE;
const HostFile kNoFile = INVALID_HANDLE_VALUE;
#else
using HostFile = int;
constexpr HostFile kNoFile = -1;
#endif

struct OpenFile {
    HostFile h = kNoFile;
    fs::path host;
    std::string guest;
    bool dir = false;
    std::vector<fs::directory_entry> entries;
    size_t dir_pos = 0;
};

std::mutex g_fd_mutex;
std::unordered_map<int, OpenFile*> g_fds;
int g_next_fd = 3;

fs::path UserDataDir() { return UserRoot(); }

// The dump's file names as the game asks for them: Linux file systems are case-sensitive while the
// game was only ever tested against case-insensitive ones, so a name that does not exist is looked
// up again ignoring case (directory listings cached).
fs::path ResolveCase(const fs::path& p) {
#ifdef _WIN32
    return p;
#else
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
    static std::mutex m;
    static std::unordered_map<std::string, std::vector<std::string>> listings;
    std::lock_guard lk(m);
    fs::path cur = fs::path(g_linker.game_dir);
    fs::path rel = p.lexically_relative(cur);
    if (rel.empty() || *rel.begin() == "..") return p;
    for (const fs::path& part : rel) {
        fs::path next = cur / part;
        if (!fs::exists(next, ec)) {
            auto& names = listings[cur.string()];
            if (names.empty())
                for (auto& e : fs::directory_iterator(cur, ec)) names.push_back(e.path().filename().string());
            for (const std::string& n : names)
                if (strcasecmp(n.c_str(), part.string().c_str()) == 0) {
                    next = cur / n;
                    break;
                }
        }
        cur = next;
    }
    return cur;
#endif
}

fs::path ToHost(const char* guest) {
    std::string g = guest;
    for (auto& c : g) {
        if (c == '\\') c = '/';
    }
    auto strip = [&](const char* prefix) -> std::string {
        size_t n = strlen(prefix);
        return g.compare(0, n, prefix) == 0 ? g.substr(n) : std::string("\x01");
    };
    std::string rest;
    if ((rest = strip("/app0")) != "\x01") {
        return ResolveCase(fs::path(g_linker.game_dir) / fs::path(rest).relative_path());
    }
    fs::path saved;
    if (SaveDataMountedPath(g, &saved)) return saved;
    for (const char* mount : {"/download0", "/temp0", "/temp", "/data", "/hostapp"}) {
        if ((rest = strip(mount)) != "\x01") {
            fs::path p = UserDataDir() / (mount + 1) / fs::path(rest).relative_path();
            return p;
        }
    }
    // relative / unknown: resolve against the game directory
    return fs::path(g_linker.game_dir) / fs::path(g).relative_path();
}

#ifndef _WIN32
int PosixErr() {
    switch (errno) {
    case ENOENT: return ENOENT_;
    case EACCES:
    case EPERM: return EACCES_;
    case EEXIST: return EEXIST_;
    case ENOTEMPTY: return ENOTEMPTY_;
    case ENOTDIR: return ENOTDIR_;
    case EISDIR: return EISDIR_;
    default: return EIO_;
    }
}
#endif

#ifdef _WIN32
int WinErr() {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return ENOENT_;
    case ERROR_ACCESS_DENIED: return EACCES_;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return EEXIST_;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY_;
    default: return EIO_;
    }
}
#endif

OpenFile* Get(int fd) {
    std::lock_guard lk(g_fd_mutex);
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second;
}

int Open(const char* path, int flags, int /*mode*/) {
    fs::path host = ToHost(path);
    auto* f = new OpenFile();
    f->host = host;
    f->guest = path;
    std::error_code ec;
    if (fs::is_directory(host, ec)) {
        f->dir = true;
        for (auto& e : fs::directory_iterator(host, ec)) {
            f->entries.push_back(e);
        }
    } else {
        if (flags & O_DIRECTORY_) {
            delete f;
            return -ENOTDIR_;
        }
#ifdef _WIN32
        DWORD access = GENERIC_READ;
        if ((flags & O_ACCMODE_) == O_WRONLY_) access = GENERIC_WRITE;
        if ((flags & O_ACCMODE_) == O_RDWR_) access = GENERIC_READ | GENERIC_WRITE;
        DWORD disp = OPEN_EXISTING;
        if (flags & O_CREAT_) {
            disp = (flags & O_EXCL_) ? CREATE_NEW : (flags & O_TRUNC_) ? CREATE_ALWAYS : OPEN_ALWAYS;
            fs::create_directories(host.parent_path(), ec);
        } else if (flags & O_TRUNC_) {
            disp = TRUNCATE_EXISTING;
        }
        f->h = CreateFileW(host.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f->h == INVALID_HANDLE_VALUE) {
            int e = WinErr();
            delete f;
            return -e;
        }
        if (flags & O_APPEND_) {
            SetFilePointer(f->h, 0, nullptr, FILE_END);
        }
#else
        int pf = (flags & O_ACCMODE_) == O_WRONLY_ ? O_WRONLY : (flags & O_ACCMODE_) == O_RDWR_ ? O_RDWR : O_RDONLY;
        if (flags & O_CREAT_) {
            pf |= O_CREAT;
            fs::create_directories(host.parent_path(), ec);
        }
        if (flags & O_TRUNC_) pf |= O_TRUNC;
        if (flags & O_EXCL_) pf |= O_EXCL;
        if (flags & O_APPEND_) pf |= O_APPEND;
        f->h = open(host.c_str(), pf | O_CLOEXEC, 0644);
        if (f->h < 0) {
            int e = PosixErr();
            delete f;
            return -e;
        }
#endif
    }
    std::lock_guard lk(g_fd_mutex);
    int fd = g_next_fd++;
    g_fds[fd] = f;
    return fd;
}

s64 ReadFd(int fd, void* buf, u64 n) {
    if (fd == 0) return 0;
    OpenFile* f = Get(fd);
    if (!f || f->dir) return -EBADF_;
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(f->h, buf, (DWORD)n, &got, nullptr)) return -EIO_;
    return got;
#else
    // through a bounce buffer: the kernel cannot write into write-watched (protected) guest pages
    thread_local std::vector<char> bounce;
    bounce.resize(std::max<size_t>(bounce.size(), n));
    ssize_t got = read(f->h, bounce.data(), n);
    if (got < 0) return -EIO_;
    memcpy(buf, bounce.data(), (size_t)got);
    return got;
#endif
}

s64 WriteFd(int fd, const void* buf, u64 n) {
    if (fd == 1 || fd == 2) {
        std::string s((const char*)buf, n);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        LOG("[guest%s] %s", fd == 2 ? " err" : "", s.c_str());
        return (s64)n;
    }
    OpenFile* f = Get(fd);
    if (!f || f->dir) return -EBADF_;
#ifdef _WIN32
    DWORD put = 0;
    if (!WriteFile(f->h, buf, (DWORD)n, &put, nullptr)) return -EIO_;
    return put;
#else
    ssize_t put = write(f->h, buf, n);
    return put < 0 ? -EIO_ : put;
#endif
}

s64 Seek(int fd, s64 off, int whence) {
    OpenFile* f = Get(fd);
    if (!f || f->dir) return -EBADF_;
#ifdef _WIN32
    LARGE_INTEGER d, out;
    d.QuadPart = off;
    DWORD m = whence == 1 ? FILE_CURRENT : whence == 2 ? FILE_END : FILE_BEGIN;
    if (!SetFilePointerEx(f->h, d, &out, m)) return -EINVAL_;
    return out.QuadPart;
#else
    off_t r = lseek(f->h, off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    return r < 0 ? -EINVAL_ : r;
#endif
}

s64 PRW(int fd, void* buf, u64 n, s64 off, bool write) {
    OpenFile* f = Get(fd);
    if (!f || f->dir) return -EBADF_;
#ifdef _WIN32
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((u64)off >> 32);
    DWORD done = 0;
    BOOL ok = write ? WriteFile(f->h, buf, (DWORD)n, &done, &ov) : ReadFile(f->h, buf, (DWORD)n, &done, &ov);
    if (!ok && GetLastError() != ERROR_HANDLE_EOF) return -EIO_;
    return done;
#else
    if (write) {
        ssize_t r = pwrite(f->h, buf, n, off);
        return r < 0 ? -EIO_ : r;
    }
    thread_local std::vector<char> bounce;
    bounce.resize(std::max<size_t>(bounce.size(), n));
    ssize_t r = pread(f->h, bounce.data(), n, off);
    if (r < 0) return -EIO_;
    memcpy(buf, bounce.data(), (size_t)r);
    return r;
#endif
}

int Close(int fd) {
    if (fd <= 2) return 0;
    OpenFile* f;
    {
        std::lock_guard lk(g_fd_mutex);
        auto it = g_fds.find(fd);
        if (it == g_fds.end()) return -EBADF_;
        f = it->second;
        g_fds.erase(it);
    }
#ifdef _WIN32
    if (f->h != kNoFile) CloseHandle(f->h);
#else
    if (f->h != kNoFile) close(f->h);
#endif
    delete f;
    return 0;
}

struct OrbisStat {
    u32 dev, ino;
    u16 mode, nlink;
    u32 uid, gid, rdev;
    s64 atim[2], mtim[2], ctim[2];
    s64 size, blocks;
    u32 blksize, flags, gen;
    s32 lspare;
    s64 birthtim[2];
};
static_assert(sizeof(OrbisStat) == 0x78);

int StatPath(const fs::path& p, OrbisStat* st) {
    memset(st, 0, sizeof(*st));
    std::error_code ec;
    auto status = fs::status(p, ec);
    if (ec || !fs::exists(status)) return -ENOENT_;
    bool dir = fs::is_directory(status);
    st->mode = (dir ? 0040000 : 0100000) | 0777;
    st->nlink = 1;
    st->blksize = 0x10000;
    if (!dir) {
        st->size = (s64)fs::file_size(p, ec);
        st->blocks = (st->size + 511) / 512;
    }
    return 0;
}

// FreeBSD dirent
int Getdents(int fd, char* buf, int nbytes) {
    OpenFile* f = Get(fd);
    if (!f || !f->dir) return -EBADF_;
    int used = 0;
    while (f->dir_pos < f->entries.size()) {
        auto& e = f->entries[f->dir_pos];
        std::string name = e.path().filename().string();
        int reclen = (8 + (int)name.size() + 1 + 7) & ~7;
        if (used + reclen > nbytes) break;
        char* d = buf + used;
        memset(d, 0, reclen);
        u32 ino = (u32)f->dir_pos + 1;
        memcpy(d, &ino, 4);
        u16 rl = (u16)reclen;
        memcpy(d + 4, &rl, 2);
        d[6] = e.is_directory() ? 4 : 8;
        d[7] = (char)name.size();
        memcpy(d + 8, name.data(), name.size());
        used += reclen;
        f->dir_pos++;
    }
    return used;
}

// sce* variants return 0x8002xxxx errors, posix variants -1 + errno
s64 Sce(s64 r) { return r < 0 ? SceErr((int)-r) : r; }
s64 Posix(s64 r) {
    if (r < 0) {
        CurrentThread()->err = (int)-r;
        return -1;
    }
    return r;
}

SYSV int sceKernelOpen(const char* p, int flags, int mode) {
    int r = Open(p, flags, mode);
    LOG("open %s flags 0x%x -> %d", p, flags, r);
    return (int)Sce(r);
}
SYSV int posix_open(const char* p, int flags, int mode) { return (int)Posix(Open(p, flags, mode)); }
SYSV int sceKernelClose(int fd) { return (int)Sce(Close(fd)); }
SYSV int posix_close(int fd) { return (int)Posix(Close(fd)); }
SYSV s64 sceKernelRead(int fd, void* b, u64 n) { return Sce(ReadFd(fd, b, n)); }
SYSV s64 posix_read(int fd, void* b, u64 n) { return Posix(ReadFd(fd, b, n)); }
SYSV s64 sceKernelWrite(int fd, const void* b, u64 n) { return Sce(WriteFd(fd, b, n)); }
SYSV s64 posix_write(int fd, const void* b, u64 n) { return Posix(WriteFd(fd, b, n)); }
SYSV s64 sceKernelLseek(int fd, s64 off, int wh) { return Sce(Seek(fd, off, wh)); }
SYSV s64 sceKernelPread(int fd, void* b, u64 n, s64 off) { return Sce(PRW(fd, b, n, off, false)); }
SYSV s64 sceKernelPwrite(int fd, const void* b, u64 n, s64 off) {
    return Sce(PRW(fd, (void*)b, n, off, true));
}
SYSV int sceKernelStat(const char* p, OrbisStat* st) { return (int)Sce(StatPath(ToHost(p), st)); }
SYSV int sceKernelFstat(int fd, OrbisStat* st) {
    OpenFile* f = Get(fd);
    if (!f) return SceErr(EBADF_);
    return (int)Sce(StatPath(f->host, st));
}
SYSV int sceKernelMkdir(const char* p, int) {
    std::error_code ec;
    fs::path h = ToHost(p);
    if (fs::exists(h, ec)) return SceErr(EEXIST_);
    fs::create_directories(h, ec);
    return ec ? SceErr(EIO_) : 0;
}
SYSV int sceKernelRmdir(const char* p) {
    std::error_code ec;
    return fs::remove(ToHost(p), ec) ? 0 : SceErr(ENOENT_);
}
SYSV int sceKernelUnlink(const char* p) {
    std::error_code ec;
    return fs::remove(ToHost(p), ec) ? 0 : SceErr(ENOENT_);
}
SYSV int sceKernelRename(const char* a, const char* b) {
    std::error_code ec;
    fs::rename(ToHost(a), ToHost(b), ec);
    return ec ? SceErr(ENOENT_) : 0;
}
SYSV int sceKernelFsync(int fd) {
#ifdef _WIN32
    if (OpenFile* f = Get(fd); f && f->h != kNoFile) FlushFileBuffers(f->h);
#else
    if (OpenFile* f = Get(fd); f && f->h != kNoFile) fsync(f->h);
#endif
    return 0;
}
SYSV int sceKernelFtruncate(int fd, s64 len) {
    OpenFile* f = Get(fd);
    if (!f || f->dir) return SceErr(EBADF_);
#ifdef _WIN32
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = len;
    return SetFileInformationByHandle(f->h, FileEndOfFileInfo, &info, sizeof(info)) ? 0 : SceErr(EIO_);
#else
    return ftruncate(f->h, len) == 0 ? 0 : SceErr(EIO_);
#endif
}
SYSV int sceKernelGetdents(int fd, char* buf, int n) { return (int)Sce(Getdents(fd, buf, n)); }
SYSV int sceKernelGetdirentries(int fd, char* buf, int n, s64* base) {
    if (base) *base = 0;
    return (int)Sce(Getdents(fd, buf, n));
}

HLE("libkernel", sceKernelOpen);
HLE_AS("libkernel", "open", posix_open);
HLE("libkernel", sceKernelClose);
HLE_AS("libkernel", "close", posix_close);
HLE_AS("libkernel", "_close", posix_close);
HLE("libkernel", sceKernelRead);
HLE_AS("libkernel", "read", posix_read);
HLE_AS("libkernel", "_read", posix_read);
HLE("libkernel", sceKernelWrite);
HLE_AS("libkernel", "write", posix_write);
HLE_AS("libkernel", "_write", posix_write);
HLE("libkernel", sceKernelLseek);
HLE("libkernel", sceKernelPread);
HLE("libkernel", sceKernelPwrite);
HLE("libkernel", sceKernelStat);
HLE("libkernel", sceKernelFstat);
HLE("libkernel", sceKernelMkdir);
HLE("libkernel", sceKernelRmdir);
HLE("libkernel", sceKernelUnlink);
HLE("libkernel", sceKernelRename);
HLE("libkernel", sceKernelFsync);
HLE("libkernel", sceKernelFtruncate);
HLE("libkernel", sceKernelGetdents);
HLE("libkernel", sceKernelGetdirentries);

}  // namespace
