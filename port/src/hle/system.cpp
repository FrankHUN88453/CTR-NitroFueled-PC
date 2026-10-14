// Small system libraries: sysmodule, system service, user service.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../hle.h"
#include "pad.h"

namespace {

SYSV int sceSysmoduleLoadModule(u16 id) {
    LOG("sceSysmoduleLoadModule(0x%x)", id);
    return 0;
}
SYSV int sceSysmoduleLoadModuleInternalWithArg(u32 id, u64, const void*, u32, int* res) {
    LOG("sceSysmoduleLoadModuleInternalWithArg(0x%x)", id);
    if (res) *res = 0;
    return 0;
}
SYSV int sceSysmoduleUnloadModule(u16) { return 0; }
SYSV int sceSysmoduleIsLoaded(u16) { return 0; }
HLE("libSceSysmodule", sceSysmoduleLoadModule);
HLE("libSceSysmodule", sceSysmoduleLoadModuleInternalWithArg);
HLE("libSceSysmodule", sceSysmoduleUnloadModule);
HLE("libSceSysmodule", sceSysmoduleIsLoaded);

// ---- user service: users 1-4 are the local players (pad.cpp logs in one per controller)
SYSV int sceUserServiceInitialize(const void*) { return 0; }
SYSV int sceUserServiceGetInitialUser(int* id) {
    *id = 1;
    return 0;
}
SYSV int sceUserServiceGetUserName(int user, char* name, u64 size) {
    if (user == 1)
        strncpy(name, "Player", size);
    else
        snprintf(name, size, "Player %d", user);
    return 0;
}
struct LoginUserIdList {
    int ids[4];
};
SYSV int sceUserServiceGetLoginUserIdList(LoginUserIdList* l) {
    PadLoginUsers(l->ids);
    return 0;
}
struct UserServiceEvent {
    int type;  // 0 = login, 1 = logout
    int user;
};
SYSV int sceUserServiceGetEvent(UserServiceEvent* ev) {
    if (int user = PadTakeLogin()) {
        LOG("user service: user %d logged in", user);
        ev->type = 0;
        ev->user = user;
        return 0;
    }
    return (int)0x80960007;  // SCE_USER_SERVICE_ERROR_NO_EVENT
}
HLE("libSceUserService", sceUserServiceGetEvent);
HLE("libSceUserService", sceUserServiceInitialize);
HLE("libSceUserService", sceUserServiceGetInitialUser);
HLE("libSceUserService", sceUserServiceGetUserName);
HLE("libSceUserService", sceUserServiceGetLoginUserIdList);

// ---- system service
SYSV int sceSystemServiceHideSplashScreen() { return 0; }
SYSV int sceSystemServiceParamGetInt(int id, int* v) {
    // 1 = lang (1 = English US), 2 = date format, 3 = time format, 4 = time zone, ...
    // 1000 = enter button assign (0 = circle, 1 = cross: western layout)
    *v = id == 1 || id == 1000 ? 1 : 0;
    return 0;
}
struct SystemServiceStatus {
    s32 event_num;
    bool system_ui;
    bool overlay;
    bool cpu_mode7;
    bool bg_music;
    bool app_suspended;
    u8 reserved[123];
};
SYSV int sceSystemServiceGetStatus(SystemServiceStatus* s) {
    memset(s, 0, sizeof(*s));
    return 0;
}
HLE("libSceSystemService", sceSystemServiceHideSplashScreen);
HLE("libSceSystemService", sceSystemServiceParamGetInt);
HLE("libSceSystemService", sceSystemServiceGetStatus);

}  // namespace
