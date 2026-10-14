#pragma once
// Native (Win32) implementations of PS4 system library functions, looked up by NID.
#include <string>

#include "common.h"

void HleRegister(const char* lib, const char* name, void* addr);
void* HleLookup(const std::string& nid);

#define HLE_CAT2(a, b) a##b
#define HLE_CAT(a, b) HLE_CAT2(a, b)
// HLE("libkernel", sceKernelUsleep) registers a SYSV function under its own name.
#define HLE(lib, fn) \
    static const int HLE_CAT(hle_reg_, __COUNTER__) = (HleRegister(lib, #fn, (void*)&fn), 0)
// HLE_AS("libkernel", "pthread_mutex_lock", posix_mutex_lock) registers under another name.
#define HLE_AS(lib, name, fn) \
    static const int HLE_CAT(hle_reg_, __COUNTER__) = (HleRegister(lib, name, (void*)&fn), 0)
