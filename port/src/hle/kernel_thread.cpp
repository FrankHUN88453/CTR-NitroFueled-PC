// libkernel / libScePosix: threads, mutexes, condition variables, rwlocks,
// once, thread-specific data, semaphores, event flags.
//
// Guest handle types (ScePthreadMutex, pthread_cond_t, ...) are pointers; a
// value <= 3 is a static initializer and is replaced lazily.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <shared_mutex>

#include "../hle.h"
#include "../thread.h"

using namespace err;

namespace {

int Sce(int e) { return e ? SceErr(e) : 0; }

// ---------------------------------------------------------------- mutex
struct HostMutex {
    std::recursive_timed_mutex m;
    int type = 1;
    char name[32] = {};
};
struct HostMutexAttr {
    int type = 1;  // 1 = errorcheck (PS4 default), 2 = recursive, 3 = normal, 4 = adaptive
    int protocol = 0;
};

std::mutex g_lazy_init;

template <class T>
T* Lazy(T** slot) {
    T* v = *slot;
    if ((u64)v > 3) {
        return v;
    }
    std::lock_guard lk(g_lazy_init);
    if ((u64)*slot <= 3) {
        *slot = new T();
    }
    return *slot;
}

int MutexInit(HostMutex** m, HostMutexAttr** attr, const char* name) {
    if (!m) {
        return EINVAL_;
    }
    auto* hm = new HostMutex();
    if (attr && *attr) {
        hm->type = (*attr)->type;
    }
    if (name) {
        strncpy(hm->name, name, sizeof(hm->name) - 1);
    }
    *m = hm;
    return 0;
}
int MutexDestroy(HostMutex** m) {
    if (m && (u64)*m > 3) {
        delete *m;
        *m = nullptr;
    }
    return 0;
}
int MutexLock(HostMutex** m) {
    if (!m) {
        return EINVAL_;
    }
    Lazy(m)->m.lock();
    return 0;
}
int MutexTrylock(HostMutex** m) {
    if (!m) {
        return EINVAL_;
    }
    return Lazy(m)->m.try_lock() ? 0 : EBUSY_;
}
int MutexTimedlock(HostMutex** m, u32 usec) {
    if (!m) {
        return EINVAL_;
    }
    return Lazy(m)->m.try_lock_for(std::chrono::microseconds(usec)) ? 0 : ETIMEDOUT_;
}
int MutexUnlock(HostMutex** m) {
    if (!m || (u64)*m <= 3) {
        return EINVAL_;
    }
    (*m)->m.unlock();
    return 0;
}
int MutexattrInit(HostMutexAttr** a) {
    *a = new HostMutexAttr();
    return 0;
}
int MutexattrDestroy(HostMutexAttr** a) {
    delete *a;
    *a = nullptr;
    return 0;
}
int MutexattrSettype(HostMutexAttr** a, int type) {
    (*a)->type = type;
    return 0;
}
int MutexattrSetprotocol(HostMutexAttr** a, int p) {
    (*a)->protocol = p;
    return 0;
}

SYSV int scePthreadMutexInit(HostMutex** m, HostMutexAttr** a, const char* n) { return Sce(MutexInit(m, a, n)); }
SYSV int scePthreadMutexDestroy(HostMutex** m) { return Sce(MutexDestroy(m)); }
SYSV int scePthreadMutexLock(HostMutex** m) { return Sce(MutexLock(m)); }
SYSV int scePthreadMutexTrylock(HostMutex** m) { return Sce(MutexTrylock(m)); }
SYSV int scePthreadMutexTimedlock(HostMutex** m, u32 us) { return Sce(MutexTimedlock(m, us)); }
SYSV int scePthreadMutexUnlock(HostMutex** m) { return Sce(MutexUnlock(m)); }
SYSV int scePthreadMutexattrInit(HostMutexAttr** a) { return Sce(MutexattrInit(a)); }
SYSV int scePthreadMutexattrDestroy(HostMutexAttr** a) { return Sce(MutexattrDestroy(a)); }
SYSV int scePthreadMutexattrSettype(HostMutexAttr** a, int t) { return Sce(MutexattrSettype(a, t)); }
SYSV int scePthreadMutexattrSetprotocol(HostMutexAttr** a, int p) { return Sce(MutexattrSetprotocol(a, p)); }
HLE("libkernel", scePthreadMutexInit);
HLE("libkernel", scePthreadMutexDestroy);
HLE("libkernel", scePthreadMutexLock);
HLE("libkernel", scePthreadMutexTrylock);
HLE("libkernel", scePthreadMutexTimedlock);
HLE("libkernel", scePthreadMutexUnlock);
HLE("libkernel", scePthreadMutexattrInit);
HLE("libkernel", scePthreadMutexattrDestroy);
HLE("libkernel", scePthreadMutexattrSettype);
HLE("libkernel", scePthreadMutexattrSetprotocol);

SYSV int posix_mutex_init(HostMutex** m, HostMutexAttr** a) { return MutexInit(m, a, nullptr); }
SYSV int posix_mutex_destroy(HostMutex** m) { return MutexDestroy(m); }
SYSV int posix_mutex_lock(HostMutex** m) { return MutexLock(m); }
SYSV int posix_mutex_trylock(HostMutex** m) { return MutexTrylock(m); }
SYSV int posix_mutex_unlock(HostMutex** m) { return MutexUnlock(m); }
SYSV int posix_mutexattr_init(HostMutexAttr** a) { return MutexattrInit(a); }
SYSV int posix_mutexattr_destroy(HostMutexAttr** a) { return MutexattrDestroy(a); }
SYSV int posix_mutexattr_settype(HostMutexAttr** a, int t) { return MutexattrSettype(a, t); }
HLE_AS("libkernel", "pthread_mutex_init", posix_mutex_init);
HLE_AS("libkernel", "pthread_mutex_destroy", posix_mutex_destroy);
HLE_AS("libkernel", "pthread_mutex_lock", posix_mutex_lock);
HLE_AS("libkernel", "pthread_mutex_trylock", posix_mutex_trylock);
HLE_AS("libkernel", "pthread_mutex_unlock", posix_mutex_unlock);
HLE_AS("libkernel", "pthread_mutexattr_init", posix_mutexattr_init);
HLE_AS("libkernel", "pthread_mutexattr_destroy", posix_mutexattr_destroy);
HLE_AS("libkernel", "pthread_mutexattr_settype", posix_mutexattr_settype);

// ---------------------------------------------------------------- cond
struct HostCond {
    std::condition_variable_any cv;
};
int CondWait(HostCond** c, HostMutex** m, s64 usec) {
    if (!c || !m) {
        return EINVAL_;
    }
    HostCond* hc = Lazy(c);
    HostMutex* hm = Lazy(m);
    if (usec < 0) {
        hc->cv.wait(hm->m);
        return 0;
    }
    return hc->cv.wait_for(hm->m, std::chrono::microseconds(usec)) == std::cv_status::timeout
               ? ETIMEDOUT_
               : 0;
}
SYSV int scePthreadCondInit(HostCond** c, void*, const char*) { *c = new HostCond(); return 0; }
SYSV int scePthreadCondDestroy(HostCond** c) {
    if ((u64)*c > 3) delete *c;
    *c = nullptr;
    return 0;
}
SYSV int scePthreadCondWait(HostCond** c, HostMutex** m) { return Sce(CondWait(c, m, -1)); }
SYSV int scePthreadCondTimedwait(HostCond** c, HostMutex** m, u32 us) { return Sce(CondWait(c, m, us)); }
SYSV int scePthreadCondSignal(HostCond** c) { Lazy(c)->cv.notify_one(); return 0; }
SYSV int scePthreadCondBroadcast(HostCond** c) { Lazy(c)->cv.notify_all(); return 0; }
SYSV int scePthreadCondattrInit(void** a) { *a = new int(0); return 0; }
SYSV int scePthreadCondattrDestroy(void** a) { *a = nullptr; return 0; }
HLE("libkernel", scePthreadCondInit);
HLE("libkernel", scePthreadCondDestroy);
HLE("libkernel", scePthreadCondWait);
HLE("libkernel", scePthreadCondTimedwait);
HLE("libkernel", scePthreadCondSignal);
HLE("libkernel", scePthreadCondBroadcast);
HLE("libkernel", scePthreadCondattrInit);
HLE("libkernel", scePthreadCondattrDestroy);
SYSV int posix_cond_wait(HostCond** c, HostMutex** m) { return CondWait(c, m, -1); }
SYSV int posix_cond_broadcast(HostCond** c) { Lazy(c)->cv.notify_all(); return 0; }
SYSV int posix_cond_signal(HostCond** c) { Lazy(c)->cv.notify_one(); return 0; }
HLE_AS("libkernel", "pthread_cond_wait", posix_cond_wait);
HLE_AS("libkernel", "pthread_cond_broadcast", posix_cond_broadcast);
HLE_AS("libkernel", "pthread_cond_signal", posix_cond_signal);

// ---------------------------------------------------------------- rwlock
struct HostRwlock {
    std::shared_timed_mutex m;
    std::atomic<DWORD> writer{0};
};
SYSV int scePthreadRwlockInit(HostRwlock** l, void*, const char*) { *l = new HostRwlock(); return 0; }
SYSV int scePthreadRwlockDestroy(HostRwlock** l) {
    if ((u64)*l > 3) delete *l;
    *l = nullptr;
    return 0;
}
SYSV int scePthreadRwlockRdlock(HostRwlock** l) { Lazy(l)->m.lock_shared(); return 0; }
SYSV int scePthreadRwlockTryrdlock(HostRwlock** l) { return Lazy(l)->m.try_lock_shared() ? 0 : SceErr(EBUSY_); }
SYSV int scePthreadRwlockWrlock(HostRwlock** l) {
    HostRwlock* h = Lazy(l);
    h->m.lock();
    h->writer = GetCurrentThreadId();
    return 0;
}
SYSV int scePthreadRwlockTrywrlock(HostRwlock** l) {
    HostRwlock* h = Lazy(l);
    if (!h->m.try_lock()) return SceErr(EBUSY_);
    h->writer = GetCurrentThreadId();
    return 0;
}
SYSV int scePthreadRwlockUnlock(HostRwlock** l) {
    HostRwlock* h = Lazy(l);
    if (h->writer == GetCurrentThreadId()) {
        h->writer = 0;
        h->m.unlock();
    } else {
        h->m.unlock_shared();
    }
    return 0;
}
HLE("libkernel", scePthreadRwlockInit);
HLE("libkernel", scePthreadRwlockDestroy);
HLE("libkernel", scePthreadRwlockRdlock);
HLE("libkernel", scePthreadRwlockTryrdlock);
HLE("libkernel", scePthreadRwlockWrlock);
HLE("libkernel", scePthreadRwlockTrywrlock);
HLE("libkernel", scePthreadRwlockUnlock);
SYSV int posix_rwlock_rdlock(HostRwlock** l) { scePthreadRwlockRdlock(l); return 0; }
SYSV int posix_rwlock_wrlock(HostRwlock** l) { scePthreadRwlockWrlock(l); return 0; }
SYSV int posix_rwlock_unlock(HostRwlock** l) { scePthreadRwlockUnlock(l); return 0; }
HLE_AS("libkernel", "pthread_rwlock_rdlock", posix_rwlock_rdlock);
HLE_AS("libkernel", "pthread_rwlock_wrlock", posix_rwlock_wrlock);
HLE_AS("libkernel", "pthread_rwlock_unlock", posix_rwlock_unlock);

// ---------------------------------------------------------------- once / keys
SYSV int scePthreadOnce(std::atomic<int>* once, void(SYSV* init)()) {
    int expected = 0;
    if (once->compare_exchange_strong(expected, 2)) {
        init();
        *once = 1;
        return 0;
    }
    while (once->load() != 1) {
        SwitchToThread();
    }
    return 0;
}
HLE("libkernel", scePthreadOnce);

std::atomic<int> g_next_key{1};
SYSV int scePthreadKeyCreate(int* key, void*) {
    int k = g_next_key++;
    if (k >= 256) return SceErr(EAGAIN_);
    *key = k;
    return 0;
}
SYSV int scePthreadKeyDelete(int) { return 0; }
SYSV void* scePthreadGetspecific(int key) {
    return key > 0 && key < 256 ? CurrentThread()->specific[key] : nullptr;
}
SYSV int scePthreadSetspecific(int key, void* v) {
    if (key <= 0 || key >= 256) return SceErr(EINVAL_);
    CurrentThread()->specific[key] = v;
    return 0;
}
HLE("libkernel", scePthreadKeyCreate);
HLE("libkernel", scePthreadKeyDelete);
HLE("libkernel", scePthreadGetspecific);
HLE("libkernel", scePthreadSetspecific);

// ---------------------------------------------------------------- threads
struct HostThreadAttr {
    u64 stack_size = 0x10000;
    int detach = 0;
    int prio = 700;
    u64 affinity = 0x7F;
};
SYSV int scePthreadAttrInit(HostThreadAttr** a) { *a = new HostThreadAttr(); return 0; }
SYSV int scePthreadAttrDestroy(HostThreadAttr** a) { delete *a; *a = nullptr; return 0; }
SYSV int scePthreadAttrSetstacksize(HostThreadAttr** a, u64 s) { (*a)->stack_size = s; return 0; }
SYSV int scePthreadAttrSetdetachstate(HostThreadAttr** a, int d) { (*a)->detach = d; return 0; }
SYSV int scePthreadAttrGetdetachstate(HostThreadAttr** a, int* d) { *d = (*a)->detach; return 0; }
SYSV int scePthreadAttrSetinheritsched(HostThreadAttr**, int) { return 0; }
SYSV int scePthreadAttrSetschedpolicy(HostThreadAttr**, int) { return 0; }
SYSV int scePthreadAttrSetschedparam(HostThreadAttr** a, const int* p) { (*a)->prio = *p; return 0; }
SYSV int scePthreadAttrSetaffinity(HostThreadAttr** a, u64 m) { (*a)->affinity = m; return 0; }
SYSV int scePthreadAttrGetaffinity(HostThreadAttr** a, u64* m) { *m = (*a)->affinity; return 0; }
SYSV int scePthreadAttrGet(GuestThread* t, HostThreadAttr** a) {
    if (!*a) *a = new HostThreadAttr();
    (*a)->stack_size = t->stack_size;
    (*a)->detach = t->detached;
    (*a)->prio = t->prio;
    return 0;
}
HLE("libkernel", scePthreadAttrInit);
HLE("libkernel", scePthreadAttrDestroy);
HLE("libkernel", scePthreadAttrSetstacksize);
HLE("libkernel", scePthreadAttrSetdetachstate);
HLE("libkernel", scePthreadAttrGetdetachstate);
HLE("libkernel", scePthreadAttrSetinheritsched);
HLE("libkernel", scePthreadAttrSetschedpolicy);
HLE("libkernel", scePthreadAttrSetschedparam);
HLE("libkernel", scePthreadAttrSetaffinity);
HLE("libkernel", scePthreadAttrGetaffinity);
HLE("libkernel", scePthreadAttrGet);
HLE_AS("libkernel", "pthread_attr_init", scePthreadAttrInit);
HLE_AS("libkernel", "pthread_attr_destroy", scePthreadAttrDestroy);
HLE_AS("libkernel", "pthread_attr_setstacksize", scePthreadAttrSetstacksize);
HLE_AS("libkernel", "pthread_attr_setdetachstate", scePthreadAttrSetdetachstate);

int ThreadCreate(GuestThread** out, HostThreadAttr** attr, void*(SYSV* entry)(void*), void* arg,
                 const char* name) {
    HostThreadAttr def;
    HostThreadAttr* a = attr && *attr ? *attr : &def;
    // PS4 stacks are small; give guests generous host stacks (no stack probes in guest code)
    u64 stack = std::max<u64>(a->stack_size, 2 << 20);
    GuestThread* t = CreateGuestThread(entry, arg, name, stack);
    if (!t) return EAGAIN_;
    t->detached = a->detach != 0;
    t->prio = a->prio;
    *out = t;
    LOG("thread created: '%s' tid %lu entry %p stack 0x%llx", name ? name : "", t->tid,
        (void*)entry, (unsigned long long)stack);
    return 0;
}
SYSV int scePthreadCreate(GuestThread** t, HostThreadAttr** a, void*(SYSV* e)(void*), void* arg,
                          const char* name) {
    return Sce(ThreadCreate(t, a, e, arg, name));
}
SYSV int posix_create_name_np(GuestThread** t, HostThreadAttr** a, void*(SYSV* e)(void*),
                              void* arg, const char* name) {
    return ThreadCreate(t, a, e, arg, name);
}
SYSV int scePthreadJoin(GuestThread* t, void** ret) {
    if (!t) return SceErr(ESRCH_);
    WaitForSingleObject(t->handle, INFINITE);
    if (ret) *ret = t->retval;
    return 0;
}
SYSV int scePthreadDetach(GuestThread* t) { t->detached = true; return 0; }
SYSV void scePthreadExit(void* ret) {
    CurrentThread()->retval = ret;
    ExitThread(0);
}
SYSV GuestThread* scePthreadSelf() { return CurrentThread(); }
SYSV int scePthreadEqual(GuestThread* a, GuestThread* b) { return a == b; }
SYSV int scePthreadYield() { SwitchToThread(); return 0; }
SYSV int scePthreadGetname(GuestThread* t, char* buf) {
    if (!t) return SceErr(ESRCH_);
    strncpy(buf, t->name.c_str(), 31);
    buf[31] = 0;
    return 0;
}
SYSV int scePthreadGetprio(GuestThread* t, int* p) {
    if (!t) return SceErr(ESRCH_);
    *p = t->prio;
    return 0;
}
SYSV int scePthreadSetprio(GuestThread* t, int p) {
    if (!t) return SceErr(ESRCH_);
    t->prio = p;
    return 0;
}
SYSV int scePthreadGetaffinity(GuestThread*, u64* m) { *m = 0x7F; return 0; }
SYSV int scePthreadSetaffinity(GuestThread*, u64) { return 0; }
SYSV int scePthreadGetthreadid() { return (int)GetCurrentThreadId(); }
SYSV int scePthreadCancel(GuestThread*) { return 0; }
SYSV int posix_getschedparam(GuestThread* t, int* policy, int* param) {
    if (!t) return ESRCH_;
    *policy = 2;
    *param = t->prio;
    return 0;
}
SYSV int posix_setschedparam(GuestThread*, int, const int*) { return 0; }
HLE("libkernel", scePthreadCreate);
HLE_AS("libkernel", "pthread_create_name_np", posix_create_name_np);
HLE("libkernel", scePthreadJoin);
HLE_AS("libkernel", "pthread_join", scePthreadJoin);
HLE("libkernel", scePthreadDetach);
HLE("libkernel", scePthreadExit);
HLE("libkernel", scePthreadSelf);
HLE_AS("libkernel", "pthread_self", scePthreadSelf);
HLE("libkernel", scePthreadEqual);
HLE("libkernel", scePthreadYield);
HLE("libkernel", scePthreadGetname);
HLE("libkernel", scePthreadGetprio);
HLE("libkernel", scePthreadSetprio);
HLE("libkernel", scePthreadGetaffinity);
HLE("libkernel", scePthreadSetaffinity);
HLE("libkernel", scePthreadGetthreadid);
HLE("libkernel", scePthreadCancel);
HLE_AS("libkernel", "pthread_getschedparam", posix_getschedparam);
HLE_AS("libkernel", "pthread_setschedparam", posix_setschedparam);

// ---------------------------------------------------------------- semaphores
struct HostSema {
    std::mutex m;
    std::condition_variable cv;
    int count = 0, max = 0;
};
SYSV int sceKernelCreateSema(HostSema** s, const char*, u32, int init, int max, const void*) {
    auto* h = new HostSema();
    h->count = init;
    h->max = max;
    *s = h;
    return 0;
}
SYSV int sceKernelDeleteSema(HostSema*) { return 0; }
SYSV int sceKernelWaitSema(HostSema* s, int need, u32* timeout) {
    std::unique_lock lk(s->m);
    auto ok = [&] { return s->count >= need; };
    if (!timeout) {
        s->cv.wait(lk, ok);
    } else if (!s->cv.wait_for(lk, std::chrono::microseconds(*timeout), ok)) {
        *timeout = 0;
        return SceErr(ETIMEDOUT_);
    }
    s->count -= need;
    return 0;
}
SYSV int sceKernelPollSema(HostSema* s, int need) {
    std::lock_guard lk(s->m);
    if (s->count < need) return SceErr(EBUSY_);
    s->count -= need;
    return 0;
}
SYSV int sceKernelSignalSema(HostSema* s, int n) {
    {
        std::lock_guard lk(s->m);
        s->count += n;
    }
    s->cv.notify_all();
    return 0;
}
HLE("libkernel", sceKernelCreateSema);
HLE("libkernel", sceKernelDeleteSema);
HLE("libkernel", sceKernelWaitSema);
HLE("libkernel", sceKernelPollSema);
HLE("libkernel", sceKernelSignalSema);

// ---------------------------------------------------------------- event flags
struct HostEventFlag {
    std::mutex m;
    std::condition_variable cv;
    u64 bits = 0;
};
SYSV int sceKernelCreateEventFlag(HostEventFlag** ef, const char*, u32, u64 init, const void*) {
    auto* h = new HostEventFlag();
    h->bits = init;
    *ef = h;
    return 0;
}
SYSV int sceKernelDeleteEventFlag(HostEventFlag*) { return 0; }
SYSV int sceKernelSetEventFlag(HostEventFlag* ef, u64 bits) {
    {
        std::lock_guard lk(ef->m);
        ef->bits |= bits;
    }
    ef->cv.notify_all();
    return 0;
}
SYSV int sceKernelClearEventFlag(HostEventFlag* ef, u64 mask) {
    std::lock_guard lk(ef->m);
    ef->bits &= mask;
    return 0;
}
int EventFlagWait(HostEventFlag* ef, u64 pattern, u32 mode, u64* result, u32* timeout, bool poll) {
    std::unique_lock lk(ef->m);
    auto ok = [&] {
        return (mode & 1) ? (ef->bits & pattern) == pattern : (ef->bits & pattern) != 0;
    };
    if (poll) {
        if (!ok()) return SceErr(EBUSY_);
    } else if (!timeout) {
        ef->cv.wait(lk, ok);
    } else if (!ef->cv.wait_for(lk, std::chrono::microseconds(*timeout), ok)) {
        *timeout = 0;
        if (result) *result = ef->bits;
        return SceErr(ETIMEDOUT_);
    }
    if (result) *result = ef->bits;
    if (mode & 0x10) ef->bits = 0;
    if (mode & 0x20) ef->bits &= ~pattern;
    return 0;
}
SYSV int sceKernelWaitEventFlag(HostEventFlag* ef, u64 p, u32 mode, u64* res, u32* to) {
    return EventFlagWait(ef, p, mode, res, to, false);
}
SYSV int sceKernelPollEventFlag(HostEventFlag* ef, u64 p, u32 mode, u64* res) {
    return EventFlagWait(ef, p, mode, res, nullptr, true);
}
HLE("libkernel", sceKernelCreateEventFlag);
HLE("libkernel", sceKernelDeleteEventFlag);
HLE("libkernel", sceKernelSetEventFlag);
HLE("libkernel", sceKernelClearEventFlag);
HLE("libkernel", sceKernelWaitEventFlag);
HLE("libkernel", sceKernelPollEventFlag);

}  // namespace
