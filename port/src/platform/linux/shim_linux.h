#pragma once
// Linux-only helpers of the Win32 shim (memory_watch.cpp).
#include <csignal>
#include <cstddef>

// memcpy that returns false instead of crashing when a page is not accessible
bool ShimTryCopy(void* dst, const void* src, size_t n);

// Faults that are not write-watch traps or probes go to this handler (crash_linux.cpp).
void ShimSetCrashHook(void (*hook)(int sig, siginfo_t* si, void* uc));
void ShimInstallFaultHandler();
