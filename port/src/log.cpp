#include <windows.h>

#include <cstdarg>
#include <cstdlib>
#include <mutex>

#include "common.h"

static FILE* g_log;
static std::mutex g_log_mutex;

void LogInit(const char* path) {
    g_log = fopen(path, "w");
}

static void LogV(const char* fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    std::lock_guard lk(g_log_mutex);
    fprintf(stderr, "[%5lu] %s\n", GetCurrentThreadId(), buf);
    if (g_log) {
        fprintf(g_log, "[%5lu] %s\n", GetCurrentThreadId(), buf);
    }
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void LogFlush() {
    std::lock_guard lk(g_log_mutex);
    fflush(stderr);
    if (g_log) {
        fflush(g_log);
    }
}

void Fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
    LogFlush();
    ExitProcess(1);
}
