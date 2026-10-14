// Linux implementations of the Win32 functions ctr_pc uses (declared in include/windows.h):
// environment, time, waitable timers / events / threads as handles, TLS slots, INI files.
#include <windows.h>

#include <asm/prctl.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------ handles
struct Object {
    enum Kind { kThread, kTimer, kEvent } kind;
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
};

struct ThreadObject : Object {
    ThreadObject() : Object(kThread) {}
    pthread_t thread{};
    LPTHREAD_START_ROUTINE fn = nullptr;
    void* arg = nullptr;
    size_t stack = 0;
    bool started = false, adopted = false;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    DWORD exit_code = 0;
    std::atomic<DWORD> tid{0};
    jmp_buf exit_jump;  // ExitThread returns here (no unwinding through guest frames)
};

struct TimerObject : Object {
    TimerObject() : Object(kTimer) {}
    timespec due{};
};

struct EventObject : Object {
    EventObject(bool manual, bool initial) : Object(kEvent), manual(manual), set(initial) {}
    bool manual, set;
    std::mutex m;
    std::condition_variable cv;
};

thread_local ThreadObject* t_self = nullptr;
thread_local DWORD t_last_error = 0;

const HANDLE kPseudoProcess = (HANDLE)(intptr_t)-1;
const HANDLE kPseudoThread = (HANDLE)(intptr_t)-2;

ThreadObject* SelfObject() {
    if (!t_self) {  // a thread we did not create (main, SDL, DXVK): adopt it
        auto* t = new ThreadObject();
        t->thread = pthread_self();
        t->started = t->adopted = true;
        t->tid = (DWORD)syscall(SYS_gettid);
        t_self = t;
    }
    return t_self;
}

timespec Now(clockid_t c) {
    timespec ts;
    clock_gettime(c, &ts);
    return ts;
}

timespec AddNs(timespec t, int64_t ns) {
    int64_t total = t.tv_nsec + ns % 1000000000;
    t.tv_sec += ns / 1000000000 + total / 1000000000;
    t.tv_nsec = total % 1000000000;
    if (t.tv_nsec < 0) t.tv_nsec += 1000000000, t.tv_sec--;
    return t;
}

void* ThreadMain(void* p) {
    auto* t = (ThreadObject*)p;
    t_self = t;
    t->tid = (DWORD)syscall(SYS_gettid);
    DWORD code = 0;
    if (setjmp(t->exit_jump) == 0) code = t->fn(t->arg);
    else code = t->exit_code;
    {
        std::lock_guard lk(t->m);
        t->exit_code = code;
        t->done = true;
    }
    t->cv.notify_all();
    return nullptr;
}

bool StartThread(ThreadObject* t) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<size_t>(t->stack, 1 << 20));
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    t->started = true;
    int r = pthread_create(&t->thread, &attr, ThreadMain, t);
    pthread_attr_destroy(&attr);
    return r == 0;
}

// ------------------------------------------------------------------ strings
std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) {
        uint32_t c = (uint32_t)*w;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000)
            s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
        else
            s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3F)), s += (char)(0x80 | (c >> 6 & 0x3F)),
                s += (char)(0x80 | (c & 0x3F));
    }
    return s;
}

std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c & 0xE0) == 0xC0) cp = c & 0x1F, n = 2;
        else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, n = 3;
        else cp = c & 0x07, n = 4;
        for (int k = 1; k < n && i + k < s.size(); k++) cp = cp << 6 | ((uint8_t)s[i + k] & 0x3F);
        w += (wchar_t)cp;
        i += n;
    }
    return w;
}

DWORD CopyOut(const std::string& v, char* buf, DWORD size) {
    if (v.size() + 1 > size) return (DWORD)v.size() + 1;
    memcpy(buf, v.c_str(), v.size() + 1);
    return (DWORD)v.size();
}

DWORD CopyOutW(const std::wstring& v, wchar_t* buf, DWORD size) {
    if (v.size() + 1 > size) return (DWORD)v.size() + 1;
    wmemcpy(buf, v.c_str(), v.size() + 1);
    return (DWORD)v.size();
}

// %APPDATA% / %LOCALAPPDATA% on Linux: $XDG_DATA_HOME, else ~/.local/share
bool LookupEnv(const char* name, std::string* out) {
    if (const char* v = getenv(name)) {
        *out = v;
        return true;
    }
    if (!strcmp(name, "APPDATA") || !strcmp(name, "LOCALAPPDATA")) {
        if (const char* x = getenv("XDG_DATA_HOME"); x && *x) {
            *out = x;
            return true;
        }
        if (const char* h = getenv("HOME")) {
            *out = std::string(h) + "/.local/share";
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ INI files
struct IniLine {
    std::string text, section, key;  // key empty: not a key line
};

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::vector<IniLine> ReadIni(const char* file) {
    std::vector<IniLine> lines;
    std::ifstream f(file);
    std::string l, section;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        IniLine il{l, section, ""};
        std::string t = Trim(l);
        if (!t.empty() && t[0] == '[' && t.back() == ']') {
            section = Trim(t.substr(1, t.size() - 2));
            il.section = section;
        } else if (!t.empty() && t[0] != ';' && t[0] != '#') {
            if (size_t eq = t.find('='); eq != std::string::npos) il.key = Trim(t.substr(0, eq));
        }
        lines.push_back(il);
    }
    return lines;
}

bool IniFind(const char* file, const char* section, const char* key, std::string* value) {
    for (const IniLine& l : ReadIni(file)) {
        if (!l.key.empty() && !strcasecmp(l.section.c_str(), section) && !strcasecmp(l.key.c_str(), key)) {
            *value = Trim(l.text.substr(l.text.find('=') + 1));
            return true;
        }
    }
    return false;
}

std::mutex g_ini_mutex;

}  // namespace

// ------------------------------------------------------------------ environment / process
DWORD GetEnvironmentVariableA(LPCSTR name, char* buf, DWORD size) {
    std::string v;
    if (!LookupEnv(name, &v)) {
        t_last_error = 203;  // ERROR_ENVVAR_NOT_FOUND
        return 0;
    }
    if (!buf || !size) return (DWORD)v.size() + 1;
    return CopyOut(v, buf, size);
}

DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* buf, DWORD size) {
    std::string v;
    if (!LookupEnv(Narrow(name).c_str(), &v)) return 0;
    if (!buf || !size) return (DWORD)Widen(v).size() + 1;
    return CopyOutW(Widen(v), buf, size);
}

namespace {
std::string ExePath() {
    char p[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    return n > 0 ? std::string(p, (size_t)n) : std::string();
}
}  // namespace

DWORD GetModuleFileNameA(HMODULE, char* buf, DWORD size) {
    std::string p = ExePath();
    DWORD n = std::min<DWORD>((DWORD)p.size(), size ? size - 1 : 0);
    if (size) memcpy(buf, p.data(), n), buf[n] = 0;
    return n;
}

DWORD GetModuleFileNameW(HMODULE, wchar_t* buf, DWORD size) {
    std::wstring p = Widen(ExePath());
    DWORD n = std::min<DWORD>((DWORD)p.size(), size ? size - 1 : 0);
    if (size) wmemcpy(buf, p.data(), n), buf[n] = 0;
    return n;
}

HMODULE GetModuleHandleA(LPCSTR name) {
    if (name) return nullptr;
    Dl_info info{};
    dladdr((void*)&GetModuleHandleA, &info);
    return (HMODULE)info.dli_fbase;
}
HMODULE GetModuleHandleW(LPCWSTR name) { return GetModuleHandleA(name ? "" : nullptr); }

DWORD GetTempPathW(DWORD size, wchar_t* buf) {
    std::string t = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    if (t.empty() || t.back() != '/') t += '/';
    return CopyOutW(Widen(t), buf, size);
}

void ExitProcess(UINT code) {
    fflush(nullptr);
    _exit((int)code);
}

BOOL TerminateProcess(HANDLE process, UINT code) {
    if (process == kPseudoProcess) ExitProcess(code);
    return FALSE;
}

HANDLE GetCurrentProcess() { return kPseudoProcess; }
DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
HANDLE GetCurrentThread() { return kPseudoThread; }
DWORD GetCurrentThreadId() {
    thread_local DWORD tid = (DWORD)syscall(SYS_gettid);
    return tid;
}
DWORD GetCurrentProcessorNumber() {
    int c = sched_getcpu();
    return c < 0 ? 0 : (DWORD)c;
}
DWORD GetLastError() { return t_last_error; }
void SetLastError(DWORD e) { t_last_error = e; }

BOOL CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == kPseudoProcess || h == kPseudoThread) return TRUE;
    auto* o = (Object*)h;
    if (o->kind != Object::kThread) delete o;  // threads stay (their own code may still use them)
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE* dst, DWORD, BOOL, DWORD) {
    *dst = src == kPseudoThread ? (HANDLE)SelfObject() : src;
    return TRUE;
}

// ------------------------------------------------------------------ time
BOOL QueryPerformanceCounter(LARGE_INTEGER* c) {
    timespec t = Now(CLOCK_MONOTONIC);
    c->QuadPart = (LONGLONG)t.tv_sec * 1000000000 + t.tv_nsec;
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) {
    f->QuadPart = 1000000000;
    return TRUE;
}
uint64_t GetTickCount64() {
    timespec t = Now(CLOCK_MONOTONIC);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
void GetSystemTimePreciseAsFileTime(FILETIME* ft) {
    timespec t = Now(CLOCK_REALTIME);
    uint64_t v = (uint64_t)t.tv_sec * 10000000 + (uint64_t)t.tv_nsec / 100 + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}
void GetLocalTime(SYSTEMTIME* st) {
    timespec t = Now(CLOCK_REALTIME);
    struct tm tm;
    localtime_r(&t.tv_sec, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)(t.tv_nsec / 1000000);
}
void Sleep(DWORD ms) {
    timespec t{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {
    }
}
BOOL SwitchToThread() {
    sched_yield();
    return TRUE;
}

HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES*, LPCWSTR, DWORD, DWORD) { return (HANDLE) new TimerObject(); }

BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG, void*, void*, BOOL) {
    auto* t = (TimerObject*)timer;
    if (due->QuadPart < 0) {  // relative, 100 ns units
        t->due = AddNs(Now(CLOCK_MONOTONIC), -due->QuadPart * 100);
    } else {  // absolute FILETIME
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        t->due = AddNs(Now(CLOCK_MONOTONIC), (due->QuadPart - now) * 100);
    }
    return TRUE;
}

HANDLE CreateEventW(SECURITY_ATTRIBUTES*, BOOL manual, BOOL initial, LPCWSTR) {
    return (HANDLE) new EventObject(manual, initial);
}

BOOL SetEvent(HANDLE event) {
    auto* e = (EventObject*)event;
    {
        std::lock_guard lk(e->m);
        e->set = true;
    }
    e->cv.notify_all();
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!h || h == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    auto* o = (Object*)(h == kPseudoThread ? (HANDLE)SelfObject() : h);
    switch (o->kind) {
    case Object::kTimer: {
        auto* t = (TimerObject*)o;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t->due, nullptr) == EINTR) {
        }
        return WAIT_OBJECT_0;
    }
    case Object::kEvent: {
        auto* e = (EventObject*)o;
        std::unique_lock lk(e->m);
        auto ready = [&] { return e->set; };
        if (ms == INFINITE) e->cv.wait(lk, ready);
        else if (!e->cv.wait_until(lk, deadline, ready)) return WAIT_TIMEOUT;
        if (!e->manual) e->set = false;
        return WAIT_OBJECT_0;
    }
    case Object::kThread: {
        auto* t = (ThreadObject*)o;
        std::unique_lock lk(t->m);
        auto ready = [&] { return t->done; };
        if (ms == INFINITE) t->cv.wait(lk, ready);
        else if (!t->cv.wait_until(lk, deadline, ready)) return WAIT_TIMEOUT;
        return WAIT_OBJECT_0;
    }
    }
    return WAIT_FAILED;
}

// ------------------------------------------------------------------ TLS slots
// Guest code (patched `mov reg, fs:[0]`) and the import stubs read the slots as gs:[0x1480 + 8 *
// slot], the TEB layout on Windows. On Linux each thread that uses a slot gets a block of its own
// as its GS base (glibc only uses FS), laid out the same way.
namespace {
constexpr size_t kTebSlots = 0x1480;
std::atomic<DWORD> g_next_slot{0};
thread_local uint8_t* t_gs_block = nullptr;

uint8_t* GsBlock() {
    if (!t_gs_block) {
        t_gs_block = (uint8_t*)calloc(1, 0x2000);
        syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)t_gs_block);
    }
    return t_gs_block;
}
}  // namespace

DWORD TlsAlloc() {
    DWORD s = g_next_slot++;
    return s < 64 ? s : TLS_OUT_OF_INDEXES;
}
void* TlsGetValue(DWORD slot) { return t_gs_block ? ((void**)(t_gs_block + kTebSlots))[slot] : nullptr; }
BOOL TlsSetValue(DWORD slot, void* value) {
    ((void**)(GsBlock() + kTebSlots))[slot] = value;
    return TRUE;
}

// ------------------------------------------------------------------ threads
HANDLE CreateThread(SECURITY_ATTRIBUTES*, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void* arg, DWORD flags,
                    DWORD* tid) {
    auto* t = new ThreadObject();
    t->fn = fn;
    t->arg = arg;
    t->stack = stack;
    if (!(flags & CREATE_SUSPENDED) && !StartThread(t)) {
        delete t;
        return nullptr;
    }
    if (tid) *tid = 0;  // the thread reads its own id (gettid) once it runs
    return (HANDLE)t;
}

DWORD ResumeThread(HANDLE h) {
    auto* t = (ThreadObject*)h;
    if (!t || t->kind != Object::kThread || t->started) return 0;
    return StartThread(t) ? 1 : (DWORD)-1;
}

DWORD SuspendThread(HANDLE) { return (DWORD)-1; }  // the thread dump is Windows-only
BOOL GetThreadContext(HANDLE, CONTEXT*) { return FALSE; }

HRESULT SetThreadDescription(HANDLE h, LPCWSTR name) {
    auto* t = (ThreadObject*)(h == kPseudoThread ? (HANDLE)SelfObject() : h);
    if (!t || t->kind != Object::kThread || !t->started) return E_FAIL;
    std::string n = Narrow(name).substr(0, 15);
    pthread_setname_np(t->thread, n.c_str());
    return S_OK;
}

void ExitThread(DWORD code) {
    ThreadObject* t = SelfObject();
    if (!t->adopted) {
        t->exit_code = code;
        longjmp(t->exit_jump, 1);
    }
    pthread_exit(nullptr);
}

// ------------------------------------------------------------------ INI
UINT GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT def, LPCSTR file) {
    std::lock_guard lk(g_ini_mutex);
    std::string v;
    if (!IniFind(file, section, key, &v) || v.empty()) return (UINT)def;
    return (UINT)strtol(v.c_str(), nullptr, 10);
}

DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, char* buf, DWORD size, LPCSTR file) {
    std::lock_guard lk(g_ini_mutex);
    std::string v;
    if (!IniFind(file, section, key, &v)) v = def ? def : "";
    if (!size) return 0;
    DWORD n = std::min<DWORD>((DWORD)v.size(), size - 1);
    memcpy(buf, v.data(), n);
    buf[n] = 0;
    return n;
}

BOOL WritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file) {
    std::lock_guard lk(g_ini_mutex);
    std::vector<IniLine> lines = ReadIni(file);
    int sec_line = -1, last_in_sec = -1, key_line = -1;
    for (int i = 0; i < (int)lines.size(); i++) {
        if (!strcasecmp(lines[i].section.c_str(), section)) {
            std::string t = Trim(lines[i].text);
            if (sec_line < 0 && !t.empty() && t[0] == '[') sec_line = i;
            if (sec_line >= 0) {
                if (!t.empty()) last_in_sec = i;
                if (key && !lines[i].key.empty() && !strcasecmp(lines[i].key.c_str(), key)) key_line = i;
            }
        }
    }
    std::string entry = key && value ? std::string(key) + "=" + value : std::string();
    if (!key) {  // delete the whole section
        if (sec_line >= 0)
            lines.erase(std::remove_if(lines.begin(), lines.end(),
                                       [&](const IniLine& l) { return !strcasecmp(l.section.c_str(), section); }),
                        lines.end());
    } else if (key_line >= 0) {
        if (value) lines[key_line].text = entry;
        else lines.erase(lines.begin() + key_line);
    } else if (value) {
        if (sec_line < 0) {
            if (!lines.empty() && !Trim(lines.back().text).empty()) lines.push_back({"", "", ""});
            lines.push_back({"[" + std::string(section) + "]", section, ""});
            lines.push_back({entry, section, key});
        } else {
            lines.insert(lines.begin() + last_in_sec + 1, {entry, section, key});
        }
    }
    std::ofstream f(file, std::ios::trunc);
    for (const IniLine& l : lines) f << l.text << "\n";
    return f.good() ? TRUE : FALSE;
}

// ------------------------------------------------------------------ strings / resources / misc
int MultiByteToWideChar(UINT, DWORD, LPCSTR s, int n, wchar_t* w, int wn) {
    std::string in = n < 0 ? std::string(s) + '\0' : std::string(s, (size_t)n);
    std::wstring out = Widen(in);
    if (!wn) return (int)out.size();
    int k = std::min<int>((int)out.size(), wn);
    wmemcpy(w, out.data(), (size_t)k);
    return k;
}

HRSRC FindResourceA(HMODULE, LPCSTR, LPCSTR) { return nullptr; }
HGLOBAL LoadResource(HMODULE, HRSRC) { return nullptr; }
void* LockResource(HGLOBAL) { return nullptr; }
DWORD SizeofResource(HMODULE, HRSRC) { return 0; }

BOOL FlushInstructionCache(HANDLE, const void*, SIZE_T) { return TRUE; }  // x86: coherent

void* _aligned_malloc(size_t size, size_t align) {
    void* p = nullptr;
    if (posix_memalign(&p, std::max<size_t>(align, sizeof(void*)), size)) return nullptr;
    return p;
}
void _aligned_free(void* p) { free(p); }
