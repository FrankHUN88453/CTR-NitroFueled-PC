// Linux: the first-start setup (the Windows version is in first_start.cpp). The AppImage carries
// tools/setup_shaders.py and its modules (usr/share/ctrnf/setup) and vkd3d-compiler (usr/bin), the
// HLSL compiler on Linux; the system's python3 runs them. Progress shows in a small SDL window with
// its own tiny bitmap font (game mode on the Steam Deck shows no window titles).
#include <windows.h>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../common.h"
#include "../../first_start.h"
#include "sdl_util.h"

extern char** environ;

namespace fs = std::filesystem;

namespace {

// 5x7 glyphs, one byte per row (low 5 bits, MSB = left): A-Z 0-9 and a few signs
struct Glyph {
    char c;
    u8 rows[7];
};
const Glyph kFont[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},   {'6', {6, 8, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'/', {1, 2, 2, 4, 8, 8, 16}},       {':', {0, 12, 12, 0, 12, 12, 0}},    {'.', {0, 0, 0, 0, 0, 12, 12}},
    {',', {0, 0, 0, 0, 12, 4, 8}},       {'-', {0, 0, 0, 31, 0, 0, 0}},       {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},        {'%', {24, 25, 2, 4, 8, 19, 3}},     {'\'', {4, 4, 8, 0, 0, 0, 0}},
};

void DrawText(SDL_Renderer* r, int x, int y, int scale, const std::string& text) {
    for (char ch : text) {
        const char c = (char)toupper((unsigned char)ch);
        for (const Glyph& g : kFont) {
            if (g.c != c) continue;
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (g.rows[row] & (16 >> col)) {
                        SDL_Rect px{x + col * scale, y + row * scale, scale, scale};
                        SDL_RenderFillRect(r, &px);
                    }
            break;
        }
        x += 6 * scale;
    }
}

struct State {
    std::mutex m;
    double progress = 0;
    std::string status = "Starting...";
    std::deque<std::string> tail;
    std::string log;
    std::atomic<bool> done{false};
    int exit_code = 1;
};

void OnLine(State& st, const std::string& line) {
    std::lock_guard lk(st.m);
    st.log += line + "\n";
    st.tail.push_back(line);
    if (st.tail.size() > 12) st.tail.pop_front();
    int a = 0, b = 0;
    if (sscanf(line.c_str(), "translated %d/%d", &a, &b) == 2 && b > 0) {
        st.progress = 0.10 + 0.85 * a / b;
        st.status = "Translating the shaders: " + std::to_string(a) + " / " + std::to_string(b);
    } else if (line.size() > 4 && line[1] == '/' && line[3] == ' ') {
        static const double kStep[] = {0.0, 0.02, 0.06, 0.10, 0.95};
        const int step = std::clamp(line[0] - '0', 1, 4);
        st.progress = std::max(st.progress, kStep[step]);
        if (step != 3) st.status = line.substr(4);
    }
}

fs::path ExeDir() {
    char p[4096];
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    return n > 0 ? fs::path(std::string(p, (size_t)n)).parent_path() : fs::path(".");
}

}  // namespace

bool RunFirstStartSetup(const fs::path& game_dir) {
    const fs::path cache = ShaderCacheDir();
    const fs::path bin = ExeDir();
    fs::path setup = getenv("CTR_SETUP_DIR") ? fs::path(getenv("CTR_SETUP_DIR")) : bin / ".." / "share" / "ctrnf" / "setup";
    fs::path compiler = bin / "vkd3d-compiler";
    std::error_code ec;
    if (cache.empty() || !fs::exists(setup / "setup_shaders.py", ec)) {
        LOG("first start: no setup next to the executable (%s)", setup.string().c_str());
        MessageBoxA(nullptr,
                    "The translated shaders are missing and this build cannot make them.\n\nDownload the AppImage "
                    "release, or build them with tools/setup_shaders.py.",
                    "Crash Team Racing Nitro-Fueled", MB_OK | MB_ICONERROR);
        return false;
    }
    LOG("first start: building the shaders into %s", cache.string().c_str());

    // python3 setup_shaders.py <game> --out <cache>, its output into a pipe
    int fds[2];
    if (pipe(fds) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    std::vector<std::string> env_s;
    for (char** e = environ; *e; e++)
        if (strncmp(*e, "CTR_VKD3D_COMPILER=", 19) && strncmp(*e, "PYTHONHOME=", 11) && strncmp(*e, "PYTHONPATH=", 11))
            env_s.push_back(*e);
    if (fs::exists(compiler, ec)) env_s.push_back("CTR_VKD3D_COMPILER=" + compiler.string());
    env_s.push_back("PYTHONUNBUFFERED=1");
    std::vector<char*> envp;
    for (auto& s : env_s) envp.push_back(s.data());
    envp.push_back(nullptr);
    const char* python = getenv("CTR_PYTHON") ? getenv("CTR_PYTHON") : "python3";
    std::string script = (setup / "setup_shaders.py").string(), game = game_dir.string(), out = cache.string();
    char* argv[] = {(char*)python, (char*)"-u", script.data(), game.data(), (char*)"--out", out.data(), nullptr};
    pid_t pid = 0;
    int spawn = posix_spawnp(&pid, python, &fa, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (spawn != 0) {
        close(fds[0]);
        LOG("first start: cannot start %s (%s)", python, strerror(spawn));
        MessageBoxA(nullptr,
                    "Building the shaders needs Python 3 (python3), which was not found.\n\nInstall it with your "
                    "distribution's package manager, then start the game again.",
                    "Crash Team Racing Nitro-Fueled", MB_OK | MB_ICONERROR);
        return false;
    }

    State st;
    std::thread reader([&] {
        std::string pending;
        char buf[4096];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            pending.append(buf, (size_t)n);
            for (size_t e; (e = pending.find('\n')) != std::string::npos; pending.erase(0, e + 1)) {
                std::string line = pending.substr(0, e);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                OnLine(st, line);
            }
        }
        if (!pending.empty()) OnLine(st, pending);
        int status = 0;
        waitpid(pid, &status, 0);
        st.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        st.done = true;
    });

    bool cancelled = false;
    if (SdlEnsure(SDL_INIT_VIDEO)) {
        SDL_Window* w = SDL_CreateWindow("Crash Team Racing Nitro-Fueled", SDL_WINDOWPOS_CENTERED,
                                         SDL_WINDOWPOS_CENTERED, 640, 180, 0);
        SDL_Renderer* r = w ? SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE) : nullptr;
        while (!st.done) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)) {
                    if (MessageBoxA(nullptr, "Stop building the shaders? The game cannot start without them.",
                                    "Crash Team Racing Nitro-Fueled", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        cancelled = true;
                        kill(pid, SIGTERM);
                    }
                }
            }
            if (r) {
                std::string status;
                double progress;
                {
                    std::lock_guard lk(st.m);
                    status = st.status, progress = st.progress;
                }
                SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
                SDL_RenderClear(r);
                SDL_SetRenderDrawColor(r, 20, 20, 20, 255);
                DrawText(r, 20, 18, 3, "First start: building the shaders");
                SDL_SetRenderDrawColor(r, 90, 90, 90, 255);
                DrawText(r, 20, 50, 2, "from your game files - only once, a few minutes");
                SDL_Rect frame{20, 82, 600, 26}, fill{22, 84, (int)(596 * std::clamp(progress, 0.0, 1.0)), 22};
                SDL_SetRenderDrawColor(r, 200, 200, 200, 255);
                SDL_RenderDrawRect(r, &frame);
                SDL_SetRenderDrawColor(r, 230, 120, 30, 255);
                SDL_RenderFillRect(r, &fill);
                SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
                DrawText(r, 20, 124, 2, status.substr(0, 48));
                SDL_RenderPresent(r);
            }
            SDL_Delay(50);
        }
        if (r) SDL_DestroyRenderer(r);
        if (w) SDL_DestroyWindow(w);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);  // the game window initialises it again on its own thread
    }
    reader.join();
    close(fds[0]);
    {
        std::ofstream f(cache / "setup.log", std::ios::binary);
        f << st.log;
    }
    LOG("first start: setup %s (exit code %d)", cancelled ? "cancelled" : st.exit_code ? "failed" : "done", st.exit_code);
    if (cancelled) return false;
    if (st.exit_code != 0 || !fs::exists(cache / "shaders.bin", ec)) {
        std::string text = "Building the shaders failed:\n\n";
        for (const std::string& l : st.tail) text += l + "\n";
        text += "\nThe full output is in " + (cache / "setup.log").string();
        MessageBoxA(nullptr, text.c_str(), "Crash Team Racing Nitro-Fueled", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}
