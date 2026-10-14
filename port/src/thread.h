#pragma once
#include <windows.h>

#include <atomic>
#include <string>

#include "common.h"

// One per guest-visible thread (ScePthread / pthread_t point at this).
struct GuestThread {
    u64 magic = 0x4448524854535547;  // "GUESTHRD"
    HANDLE handle = nullptr;
    DWORD tid = 0;
    std::string name;
    void*(SYSV* entry)(void*) = nullptr;
    void* arg = nullptr;
    void* retval = nullptr;
    u8* tls_area = nullptr;
    u64* tp = nullptr;
    u64* dtv = nullptr;
    int err = 0;  // errno for __error()
    void* specific[256] = {};
    u64 stack_size = 0;
    bool detached = false;
    int prio = 700;
};

GuestThread* CurrentThread();
// Allocates the static TLS block + TCB for t and installs it for the calling host thread.
void AttachGuestThread(GuestThread* t);
GuestThread* CreateGuestThread(void*(SYSV* entry)(void*), void* arg, const char* name,
                               u64 stack_size);

// Suspends every guest thread and logs where it is (hang diagnostics).
void DumpGuestThreads();

// static TLS layout (set up by the linker)
void TlsConfigure(u32 win_slot);
u32 TlsWinSlot();
