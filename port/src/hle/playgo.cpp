// libScePlayGo (play-while-installing) on the PC: the game files are all there, so every chunk is
// reported as installed on fast local storage. Without this the game believes it is still
// installing ("The game is still installing...") and keeps the main menu modes other than
// Adventure locked.
#include <cstring>

#include "../hle.h"

namespace {

constexpr int kHandle = 1;
constexpr signed char kLocusLocalFast = 3;
constexpr int kErrBadHandle = (int)0x80B20004;
constexpr int kErrBadPointer = (int)0x80B20003;

struct Progress {
    u64 progress_size;
    u64 total_size;
};

SYSV int scePlayGoInitialize(const void*) { return 0; }
SYSV int scePlayGoTerminate() { return 0; }
SYSV int scePlayGoOpen(int* handle, const void*) {
    if (!handle) return kErrBadPointer;
    *handle = kHandle;
    return 0;
}
SYSV int scePlayGoClose(int handle) { return handle == kHandle ? 0 : kErrBadHandle; }
SYSV int scePlayGoGetLocus(int handle, const u16* chunks, u32 count, signed char* loci) {
    if (handle != kHandle) return kErrBadHandle;
    if (!chunks || !loci) return kErrBadPointer;
    memset(loci, kLocusLocalFast, count);
    return 0;
}
SYSV int scePlayGoGetProgress(int handle, const u16* chunks, u32 count, Progress* out) {
    if (handle != kHandle) return kErrBadHandle;
    if (!chunks || !out) return kErrBadPointer;
    out->progress_size = out->total_size = 1;
    return 0;
}
SYSV int scePlayGoGetEta(int handle, const u16*, u32, s64* eta) {
    if (handle != kHandle) return kErrBadHandle;
    if (!eta) return kErrBadPointer;
    *eta = 0;
    return 0;
}
SYSV int scePlayGoGetInstallSpeed(int handle, int* speed) {
    if (handle != kHandle) return kErrBadHandle;
    if (!speed) return kErrBadPointer;
    *speed = 2;  // SCE_PLAYGO_INSTALL_SPEED_FULL
    return 0;
}
SYSV int scePlayGoSetInstallSpeed(int handle, int) { return handle == kHandle ? 0 : kErrBadHandle; }
SYSV int scePlayGoGetToDoList(int handle, void*, u32, u32* entries) {
    if (handle != kHandle) return kErrBadHandle;
    if (!entries) return kErrBadPointer;
    *entries = 0;  // nothing left to install
    return 0;
}
SYSV int scePlayGoSetToDoList(int handle, const void*, u32) { return handle == kHandle ? 0 : kErrBadHandle; }
SYSV int scePlayGoPrefetch(int handle, const u16*, u32, int) { return handle == kHandle ? 0 : kErrBadHandle; }

HLE("libScePlayGo", scePlayGoInitialize);
HLE("libScePlayGo", scePlayGoTerminate);
HLE("libScePlayGo", scePlayGoOpen);
HLE("libScePlayGo", scePlayGoClose);
HLE("libScePlayGo", scePlayGoGetLocus);
HLE("libScePlayGo", scePlayGoGetProgress);
HLE("libScePlayGo", scePlayGoGetEta);
HLE("libScePlayGo", scePlayGoGetInstallSpeed);
HLE("libScePlayGo", scePlayGoSetInstallSpeed);
HLE("libScePlayGo", scePlayGoGetToDoList);
HLE("libScePlayGo", scePlayGoSetToDoList);
HLE("libScePlayGo", scePlayGoPrefetch);

}  // namespace
