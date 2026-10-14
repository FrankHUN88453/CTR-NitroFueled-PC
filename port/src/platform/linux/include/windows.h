#pragma once
// Linux: the part of Win32 that ctr_pc uses, implemented on POSIX / SDL2 (win32_shim.cpp,
// memory_watch.cpp, sdl_shim.cpp). The types come from DXVK native's windows.h, which the D3D11
// headers use too; this header only adds what ctr_pc needs on top. It deliberately includes no
// POSIX headers: the HLE code defines PS4 constants (MAP_FIXED, O_CREAT, ...) of its own.
#include_next <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <strings.h>

// ------------------------------------------------------------------ types DXVK does not define
typedef void* HRSRC;
typedef void* HGLOBAL;
typedef void* HICON;
typedef void* HCURSOR;
typedef void* HBRUSH;
typedef uint64_t DWORD64;
typedef uint64_t ULONG64;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef DWORD (*LPTHREAD_START_ROUTINE)(void*);
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

typedef struct _MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

// only what the thread dump reads (SuspendThread / GetThreadContext fail on Linux)
typedef struct _CONTEXT {
    DWORD ContextFlags;
    DWORD64 Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
} CONTEXT;
#define CONTEXT_FULL 0x10000B

// ------------------------------------------------------------------ constants
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_WRITE_WATCH 0x00200000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define WRITE_WATCH_FLAG_RESET 0x01
#define TLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
#define INFINITE 0xFFFFFFFF
#define CREATE_SUSPENDED 0x00000004
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_ALL_ACCESS 0x001F0003
#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#define CP_UTF8 65001
#define CP_ACP 0
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define MB_OK 0x00000000L
#define MB_YESNO 0x00000004L
#define MB_ICONERROR 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONINFORMATION 0x00000040L
#define IDYES 6
#define IDNO 7
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(i))
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(uintptr_t)(i))
#define YieldProcessor() __builtin_ia32_pause()

// virtual keys (GetAsyncKeyState)
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F24 0x87
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE

#define _stricmp strcasecmp
#define _strnicmp strncasecmp

// ------------------------------------------------------------------ functions (win32_shim.cpp)
DWORD GetEnvironmentVariableA(LPCSTR name, char* buf, DWORD size);
DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* buf, DWORD size);
DWORD GetModuleFileNameA(HMODULE m, char* buf, DWORD size);
DWORD GetModuleFileNameW(HMODULE m, wchar_t* buf, DWORD size);
HMODULE GetModuleHandleA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
DWORD GetTempPathW(DWORD size, wchar_t* buf);
[[noreturn]] void ExitProcess(UINT code);
BOOL TerminateProcess(HANDLE process, UINT code);
HANDLE GetCurrentProcess();
DWORD GetCurrentProcessId();
HANDLE GetCurrentThread();
DWORD GetCurrentThreadId();
DWORD GetCurrentProcessorNumber();
DWORD GetLastError();
void SetLastError(DWORD e);
BOOL CloseHandle(HANDLE h);
BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, HANDLE* dst, DWORD access, BOOL inherit,
                     DWORD options);

BOOL QueryPerformanceCounter(LARGE_INTEGER* c);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* f);
uint64_t GetTickCount64();
void GetSystemTimePreciseAsFileTime(FILETIME* ft);
void GetLocalTime(SYSTEMTIME* st);
void Sleep(DWORD ms);
BOOL SwitchToThread();
HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES* sa, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period, void* routine, void* arg, BOOL resume);
HANDLE CreateEventW(SECURITY_ATTRIBUTES* sa, BOOL manual_reset, BOOL initial, LPCWSTR name);
BOOL SetEvent(HANDLE event);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);

void* VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(void* addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(void* addr, SIZE_T size, DWORD protect, DWORD* old);
SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* info, SIZE_T size);
UINT GetWriteWatch(DWORD flags, void* base, SIZE_T size, void** addresses, ULONG_PTR* count, ULONG* granularity);
UINT ResetWriteWatch(void* base, SIZE_T size);
BOOL FlushInstructionCache(HANDLE process, const void* addr, SIZE_T size);
void* _aligned_malloc(size_t size, size_t align);
void _aligned_free(void* p);

DWORD TlsAlloc();
void* TlsGetValue(DWORD slot);
BOOL TlsSetValue(DWORD slot, void* value);

HANDLE CreateThread(SECURITY_ATTRIBUTES* sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void* arg, DWORD flags,
                    DWORD* tid);
DWORD ResumeThread(HANDLE thread);
DWORD SuspendThread(HANDLE thread);
BOOL GetThreadContext(HANDLE thread, CONTEXT* ctx);
HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name);
[[noreturn]] void ExitThread(DWORD code);

UINT GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT def, LPCSTR file);
DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, char* buf, DWORD size, LPCSTR file);
BOOL WritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file);

int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, wchar_t* w, int wn);

// no resources are linked into the Linux executable: these always fail
HRSRC FindResourceA(HMODULE m, LPCSTR name, LPCSTR type);
HGLOBAL LoadResource(HMODULE m, HRSRC r);
void* LockResource(HGLOBAL g);
DWORD SizeofResource(HMODULE m, HRSRC r);

// ------------------------------------------------------------------ SDL2-backed (sdl_shim.cpp)
SHORT GetAsyncKeyState(int vk);
int GetSystemMetrics(int index);
int MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type);
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);
