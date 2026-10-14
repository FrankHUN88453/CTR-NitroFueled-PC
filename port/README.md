# CrashTeamRacingNitroFueled.exe – native Windows runner for the CTR Nitro-Fueled PS4 code

The game's own x86-64 code (the eboot plus the `libc.prx` and `libSceFios2.prx` shipped in the dump)
**runs directly on the CPU**, without CPU emulation. `CrashTeamRacingNitroFueled.exe` only loads and
links it, and replaces the PS4 system libraries (libkernel, VideoOut, Pad, AudioOut, Gnm, …) with
native Win32-based implementations; graphics go through a D3D11 renderer fed by the engine's own
render state. The exe holds no game code: it reads your dump at start.

## Building and running

Needs Visual Studio (C++ workload), LLVM clang-cl, CMake + Ninja, and a Python `.venv` in the
repository root for the shader tools.

```bash
./build.bat
```

builds `build/CrashTeamRacingNitroFueled.exe` with the translated shaders (`../out/hlsl`, made by
`tools/gcn2hlsl.py`) and the Xbox button glyphs embedded. `build_release.bat` builds the public release
variant without them (`build-release/`). Instead it carries the first-start setup (`res/setup.bin`:
a minimal Python runtime packed from a python.org CPython install, `SETUP_PYTHON`, plus
`tools/setup_shaders.py`; see `tools/pack_setup.py`): when `shaders.bin` is missing or of another
version, `src/first_start.cpp` unpacks it to %TEMP%, runs it with a progress window and keeps
`shaders.bin` / `glyphs_xbox.rgba` in `%LOCALAPPDATA%\CTRNF` (`CTR_USER_DIR` overrides).
`tools/make_release.sh <version>` builds and zips the release (exe, README, LICENSE).

```bash
build/CrashTeamRacingNitroFueled.exe "F:/PS4/CRTNF/UP0002-CUSA13795_00-CRASHTEAMRACING1"
```

Linux: the same sources with `src/platform/linux` — a Win32 shim (our `windows.h` on top of DXVK
native's types: POSIX time / threads / timers / INI, TLS slots in a per-thread GS block, VirtualAlloc
on mmap with write watching by page protection, XInput and keys on SDL2), the SDL2 window and audio,
POSIX file I/O and signal-based crash logs. D3D11 comes from DXVK native. The CI
(`.github/workflows/linux.yml`) builds it with clang, runs `--selftest` (the platform layer without the
game) and packs the AppImage (`tools/make_appimage.sh`). `--selftest` runs on Windows too.

Without an argument the exe uses its own folder when `eboot.bin` is there (double-click start);
`./deploy.sh` copies it into the game folder. `./run.sh <seconds>` builds and runs a test with its own
saves and settings in `run/userdata` (log: `run/CrashTeamRacingNitroFueled.log`).

Test hooks:
- optional second argument: seconds to run, then a thread state dump and exit (debugging)
- `CTR_AUTOPRESS=1`: the virtual controller presses Cross every 3 seconds
- `CTR_TEST_PAD<n>="<s>[-<s>]:<button>[+<button>];..."`: player n's buttons at those times (names as in
  `Controls.sav`, e.g. `52:Cross;90-100:Cross+LeftStickRight`); players 2–4 then get their own
  virtual controller connected at start (split screen tests). Scripted runs ignore the real input.
- `CTR_RECORD_AT=<frame>`: start a recording at that frame, like F12 (`CTR_RECORD_FRAMES=<n>`,
  `CTR_RECORD_SIZE=<w>x<h>`); with `CTR_GLITCH_TRAP=1` only frames that suddenly turn dark (and the one
  before each) are kept, logged as `glitch trap`. `tools/flicker_run.sh`: a long hub drive looking for
  flicker; `tools/flicker_debug.patch`: the full debugging version (per-draw lists, pixel counts,
  render target snapshots of the broken frame, `CTR_GLITCH_TRAP=2`).
- `CTR_SCREENSHOT=<frame>[+<count>]`, `CTR_CAPTURE=<frame>` (+ `CTR_CAPTURE_AFTER=<shader>`),
  `CTR_TRACE_DEPTH`, `CTR_D3D_DEBUG=1`, `CTR_SKIP_SHADERS=<name part>`: renderer debugging aids

User data: `%APPDATA%\CTRNF` (`CTR_USER_DIR` overrides it) – the game's saves, `GraphicsOptions.sav`
(INI; `CTR_<NAME>` environment variables override single settings), `Controls.sav`, the log, and F12
recordings.

Controls: XInput controllers and keyboard/mouse (`Controls.sav`, WASD by default, left click item,
right click / Space drift, native mouse in menus). Split screen: the keyboard plays together with
player 1's first controller, every further controller is the next player; with
`SeparateKeyboardPlayer = 1` the keyboard is its own player 1 and the controllers are players 2–4.

## Layout

| File | Job |
|---|---|
| `module.cpp` | SELF → memory, SCE dynamic info, relocations, patching `fs:` TLS accesses (`mov reg, fs:[0]` → a `gs:` TEB TLS slot trampoline) |
| `linker.cpp` | NID-based linking: real prx exports → native implementation → logging stub |
| `memory.cpp` | guest memory in the PS4 address range (< 0xFC00000000, libc checks it) |
| `thread.cpp` | guest threads, static TLS (variant II) + DTV, thread state dump |
| `hle/kernel_*.cpp` | pthread/mutex/cond/rwlock/sema/eventflag, memory, files (`/app0` → dump), time, equeue |
| `hle/gnm.cpp` | Gnm driver: draws go to the renderer, synchronisation packets (EOP/EOS/RELEASE_MEM/WRITE_DATA/DMA) are performed at submit |
| `gfx/renderer.cpp` | D3D11 renderer: engine render state → D3D11 state, translated shaders, render targets, copies, present, F12 recorder |
| `gfx/texture.cpp`, `gfx/formats.cpp` | GNM texture formats and tiling → D3D11 textures |
| `hle/videoout.cpp` | Win32 window, 60 Hz vblank, flip pacing, fullscreen |
| `hle/audio.cpp` | AudioOut ports mixed into WASAPI |
| `hle/pad.cpp` | XInput + keyboard/mouse, up to 4 players |
| `hle/savedata.cpp`, `config.cpp` | saves and settings in `%APPDATA%\CTRNF` |
| `settings_menu.cpp` | the Video page added to the game's own Options menu |
| `mouse_ui.cpp`, `prompts.cpp`, `gfx/glyphs.cpp` | mouse in menus, PC button prompts and glyph atlases |
