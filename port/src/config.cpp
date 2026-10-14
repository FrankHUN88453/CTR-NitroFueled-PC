// User settings: %APPDATA%/CTRNF/GraphicsOptions.sav (INI text; the PC Crash trilogy keeps its
// graphics options under the same name). Written with the defaults on first start;
// CTR_<NAME> environment variables override it.
#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string g_path;

struct Key {
    const char* name;
    const char* section;
};
const Key kKeys[] = {
    {"Fullscreen", "Display"},  {"VSync", "Display"},      {"FpsLimit", "Display"},
    {"RenderScale", "Graphics"}, {"Anisotropy", "Graphics"}, {"MotionBlur", "Graphics"},
    {"DirectMemoryMB", "System"}, {"ButtonPrompts", "Interface"}, {"Volume", "Audio"}, {"PS4Pro", "Graphics"},
    {"AspectRatio", "Graphics"}, {"ShadowQuality", "Graphics"}, {"ConfigVersion", "System"},
};

const char kDefaults[] =
    "; Crash Team Racing Nitro-Fueled - graphics options\n"
    "[Display]\n"
    "; 1 = borderless fullscreen (toggle: F11 or Alt+Enter)\n"
    "Fullscreen=0\n"
    "; 1 = wait for the monitor refresh (overrides FpsLimit)\n"
    "VSync=0\n"
    "; frame rate cap (the PS4 version runs at 30); 0 = unlimited\n"
    "FpsLimit=60\n"
    "\n"
    "[Graphics]\n"
    "; internal resolution in percent of 1920x1080: 200 = 3840x2160 supersampling (anti-aliasing),\n"
    "; 100 = PS4 resolution\n"
    "RenderScale=200\n"
    "; anisotropic texture filtering (1 = as on PS4, up to 16)\n"
    "Anisotropy=16\n"
    "; 0 = no motion blur (colour grading stays)\n"
    "MotionBlur=1\n"
    "; 1 = PS4 Pro mode (default): the game renders natively at 2560x1440 with its Pro extras\n"
    "; (RenderScale stays relative to 1920x1080); 0 = as on the base PS4\n"
    "PS4Pro=1\n"
    "; picture shape: 0 = the window's, 1 = 16:9, 2 = 21:9, 3 = 32:9\n"
    "AspectRatio=0\n"
    "; shadow map resolution: 0 = low (1024), 1 = as on PS4 (2048), 2 = high (4096)\n"
    "ShadowQuality=1\n"
    "\n"
    "[Audio]\n"
    "; master volume in percent (the game's own sound options stay as they are)\n"
    "Volume=100\n"
    "\n"
    "[Interface]\n"
    "; button prompts: 0 = follow the device in use (Xbox buttons with a controller, keys and mouse\n"
    "; buttons with keyboard/mouse), 1 = Xbox, 2 = PlayStation, 3 = keyboard/mouse\n"
    "ButtonPrompts=0\n"
    "\n"
    "[System]\n"
    "; memory the game may use as PS4 direct memory, in MiB (PS4: 5056)\n"
    "DirectMemoryMB=8192\n"
    "ConfigVersion=2\n";

const char* SectionOf(const char* key) {
    for (const Key& k : kKeys)
        if (_stricmp(k.name, key) == 0) return k.section;
    return nullptr;
}

// An earlier build kept the window settings as text in WindowData.sav; that name now holds the
// game's system save (as in the PC Crash trilogy), so carry the values over and free it.
void MigrateWindowDataText(const fs::path& dir) {
    fs::path old = dir / "WindowData.sav";
    std::ifstream f(old, std::ios::binary);
    char head[64] = {};
    f.read(head, sizeof(head) - 1);
    f.close();
    if (strncmp(head, "; Crash Team Racing Nitro-Fueled - window settings", 50) != 0) return;
    for (const char* k : {"Fullscreen", "VSync", "FpsLimit"}) {
        char v[32];
        if (GetPrivateProfileStringA("Window", k, "", v, sizeof(v), old.string().c_str()) && v[0])
            WritePrivateProfileStringA("Display", k, v, g_path.c_str());
    }
    std::error_code ec;
    fs::remove(old, ec);
    LOG("settings: moved the window settings from %s into %s", old.string().c_str(), g_path.c_str());
}

}  // namespace

void ConfigInit() {
    wchar_t exe[MAX_PATH], appdata[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    fs::path dir = n && n < MAX_PATH ? fs::path(appdata) / "CTRNF" : fs::path(exe).parent_path();
    if (DWORD t = GetEnvironmentVariableW(L"CTR_USER_DIR", appdata, MAX_PATH); t && t < MAX_PATH) dir = appdata;
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path p = dir / "GraphicsOptions.sav";
    g_path = p.string();
    // replace the two-file layout of an earlier build ([Graphics] only) with the full defaults
    char probe[8] = {};
    if (fs::exists(p, ec) && !GetPrivateProfileStringA("Display", "FpsLimit", "", probe, sizeof(probe), g_path.c_str())) {
        char v[32];
        std::string keep[3];
        const char* names[3] = {"RenderScale", "Anisotropy", "MotionBlur"};
        for (int i = 0; i < 3; i++)
            if (GetPrivateProfileStringA("Graphics", names[i], "", v, sizeof(v), g_path.c_str())) keep[i] = v;
        fs::remove(p, ec);
        if (FILE* out = fopen(g_path.c_str(), "w")) {
            fputs(kDefaults, out);
            fclose(out);
        }
        for (int i = 0; i < 3; i++)
            if (!keep[i].empty()) WritePrivateProfileStringA("Graphics", names[i], keep[i].c_str(), g_path.c_str());
    }
    if (!fs::exists(p, ec)) {
        if (FILE* out = fopen(g_path.c_str(), "w")) {
            fputs(kDefaults, out);
            fclose(out);
        }
    }
    MigrateWindowDataText(dir);
    // settings added later: show them in files written by earlier builds
    if (!GetPrivateProfileStringA("Interface", "ButtonPrompts", "", probe, sizeof(probe), g_path.c_str()))
        WritePrivateProfileStringA("Interface", "ButtonPrompts", "0", g_path.c_str());
    if (!GetPrivateProfileStringA("Audio", "Volume", "", probe, sizeof(probe), g_path.c_str()))
        WritePrivateProfileStringA("Audio", "Volume", "100", g_path.c_str());
    // version 2: PS4 Pro mode became the default (files of earlier builds have PS4Pro=0 written
    // as their default)
    if (GetPrivateProfileIntA("System", "ConfigVersion", 1, g_path.c_str()) < 2) {
        WritePrivateProfileStringA("Graphics", "PS4Pro", "1", g_path.c_str());
        WritePrivateProfileStringA("System", "ConfigVersion", "2", g_path.c_str());
    }
    if (!GetPrivateProfileStringA("Graphics", "AspectRatio", "", probe, sizeof(probe), g_path.c_str()))
        WritePrivateProfileStringA("Graphics", "AspectRatio", "0", g_path.c_str());
    if (!GetPrivateProfileStringA("Graphics", "ShadowQuality", "", probe, sizeof(probe), g_path.c_str()))
        WritePrivateProfileStringA("Graphics", "ShadowQuality", "1", g_path.c_str());
    LOG("settings: %s (FpsLimit %d, VSync %d, Fullscreen %d, RenderScale %d, Anisotropy %d, MotionBlur %d, "
        "DirectMemoryMB %d)",
        g_path.c_str(), ConfigInt("FpsLimit", 60), ConfigInt("VSync", 0), ConfigInt("Fullscreen", 0),
        ConfigInt("RenderScale", 200), ConfigInt("Anisotropy", 16), ConfigInt("MotionBlur", 1),
        ConfigInt("DirectMemoryMB", 8192));
}

namespace {
std::atomic<unsigned> g_generation{0};
}

void ConfigSet(const char* name, int value) {
    const char* section = SectionOf(name);
    if (!section || g_path.empty()) return;
    WritePrivateProfileStringA(section, name, std::to_string(value).c_str(), g_path.c_str());
    g_generation++;
    LOG("settings: %s = %d", name, value);
}

unsigned ConfigGeneration() { return g_generation; }

int ConfigInt(const char* name, int def) {
    char env[64], v[32];
    snprintf(env, sizeof(env), "CTR_%s", name);
    for (char* c = env; *c; c++) *c = (char)toupper(*c);
    if (GetEnvironmentVariableA(env, v, sizeof(v))) return atoi(v);
    const char* section = SectionOf(name);
    return section && !g_path.empty() ? (int)GetPrivateProfileIntA(section, name, def, g_path.c_str()) : def;
}
