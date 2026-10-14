// libkernel event queues (kqueue-style), used by GNM (EOP), VideoOut (flip/vblank)
// and user events.
#include "kernel_event.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#include "../hle.h"

using namespace err;

struct Equeue {
    std::mutex m;
    std::condition_variable cv;
    struct Reg {
        u64 ident;
        s16 filter;
        void* udata;
        bool triggered;
        u64 data;
        u32 fflags;
    };
    std::vector<Reg> regs;
    std::string name;
};

void EqueueAdd(Equeue* eq, u64 ident, s16 filter, void* udata) {
    std::lock_guard lk(eq->m);
    for (auto& r : eq->regs) {
        if (r.ident == ident && r.filter == filter) {
            r.udata = udata;
            return;
        }
    }
    eq->regs.push_back({ident, filter, udata, false, 0, 0});
}

void EqueueDelete(Equeue* eq, u64 ident, s16 filter) {
    std::lock_guard lk(eq->m);
    std::erase_if(eq->regs, [&](auto& r) { return r.ident == ident && r.filter == filter; });
}

void EqueueTrigger(Equeue* eq, u64 ident, s16 filter, u64 data) {
    {
        std::lock_guard lk(eq->m);
        for (auto& r : eq->regs) {
            if (r.ident == ident && r.filter == filter) {
                r.triggered = true;
                r.data = data;
            }
        }
    }
    eq->cv.notify_all();
}

void EqueueTriggerFilter(Equeue* eq, s16 filter, u64 data) {
    {
        std::lock_guard lk(eq->m);
        for (auto& r : eq->regs) {
            if (r.filter == filter) {
                r.triggered = true;
                r.data = data;
            }
        }
    }
    eq->cv.notify_all();
}

namespace {

struct KEvent {
    u64 ident;
    s16 filter;
    u16 flags;
    u32 fflags;
    u64 data;
    void* udata;
};
static_assert(sizeof(KEvent) == 32);

SYSV int sceKernelCreateEqueue(Equeue** eq, const char* name) {
    auto* q = new Equeue();
    q->name = name ? name : "";
    *eq = q;
    return 0;
}
SYSV int sceKernelDeleteEqueue(Equeue* eq) {
    delete eq;
    return 0;
}
SYSV int sceKernelWaitEqueue(Equeue* eq, KEvent* ev, int num, int* out, u32* timeout) {
    std::unique_lock lk(eq->m);
    auto ready = [&] {
        for (auto& r : eq->regs) {
            if (r.triggered) return true;
        }
        return false;
    };
    if (!timeout) {
        eq->cv.wait(lk, ready);
    } else if (!eq->cv.wait_for(lk, std::chrono::microseconds(*timeout), ready)) {
        if (out) *out = 0;
        return SceErr(ETIMEDOUT_);
    }
    int n = 0;
    for (auto& r : eq->regs) {
        if (n >= num) break;
        if (!r.triggered) continue;
        ev[n] = {r.ident, r.filter, 0, r.fflags, r.data, r.udata};
        r.triggered = false;  // EV_CLEAR semantics
        n++;
    }
    if (out) *out = n;
    return 0;
}
constexpr s16 kFilterUser = -11;
SYSV int sceKernelAddUserEvent(Equeue* eq, int id) {
    EqueueAdd(eq, (u64)id, kFilterUser, nullptr);
    return 0;
}
SYSV int sceKernelAddUserEventEdge(Equeue* eq, int id) { return sceKernelAddUserEvent(eq, id); }
SYSV int sceKernelTriggerUserEvent(Equeue* eq, int id, void* udata) {
    {
        std::lock_guard lk(eq->m);
        for (auto& r : eq->regs) {
            if (r.ident == (u64)id && r.filter == kFilterUser) {
                r.triggered = true;
                r.udata = udata;
            }
        }
    }
    eq->cv.notify_all();
    return 0;
}
SYSV int sceKernelDeleteUserEvent(Equeue* eq, int id) {
    EqueueDelete(eq, (u64)id, kFilterUser);
    return 0;
}
SYSV int sceKernelGetEventUserData(const KEvent* ev) { return (int)(u64)ev->udata; }
SYSV u64 sceKernelGetEventId(const KEvent* ev) { return ev->ident; }
SYSV int sceKernelGetEventFilter(const KEvent* ev) { return ev->filter; }
SYSV u64 sceKernelGetEventData(const KEvent* ev) { return ev->data; }

HLE("libkernel", sceKernelCreateEqueue);
HLE("libkernel", sceKernelDeleteEqueue);
HLE("libkernel", sceKernelWaitEqueue);
HLE("libkernel", sceKernelAddUserEvent);
HLE("libkernel", sceKernelAddUserEventEdge);
HLE("libkernel", sceKernelTriggerUserEvent);
HLE("libkernel", sceKernelDeleteUserEvent);
HLE("libkernel", sceKernelGetEventUserData);
HLE("libkernel", sceKernelGetEventId);
HLE("libkernel", sceKernelGetEventFilter);
HLE("libkernel", sceKernelGetEventData);

}  // namespace
