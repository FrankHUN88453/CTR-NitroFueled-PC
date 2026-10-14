#pragma once
// ctr_pc - native Windows host for the PS4 build of Crash Team Racing Nitro-Fueled.
// Guest code (eboot + shipped prx) runs natively; this host loads/links it and
// implements the PS4 system libraries on top of Win32.

#include <cstdint>
#include <cstdio>
#include <string>

#define SYSV __attribute__((sysv_abi))

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// log.cpp
void LogInit(const char* path);
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogFlush();
[[noreturn]] void Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(...) Log(__VA_ARGS__)

// nid.cpp
std::string NameToNid(const std::string& name);
const char* NidToName(const std::string& nid);  // nullptr if unknown

// SCE error codes: 0x80020000 | errno (FreeBSD numbering)
namespace err {
constexpr int EPERM_ = 1, ENOENT_ = 2, ESRCH_ = 3, EINTR_ = 4, EIO_ = 5, EBADF_ = 9,
              EDEADLK_ = 11, ENOMEM_ = 12, EACCES_ = 13, EFAULT_ = 14, EBUSY_ = 16,
              EEXIST_ = 17, ENOTDIR_ = 20, EISDIR_ = 21, EINVAL_ = 22, EMFILE_ = 24,
              ENOSPC_ = 28, EAGAIN_ = 35, ETIMEDOUT_ = 60, ENAMETOOLONG_ = 63,
              ENOTEMPTY_ = 66, ENOSYS_ = 78;
}
constexpr int SceErr(int e) { return (int)(0x80020000u | (u32)e); }
