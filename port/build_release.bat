@echo off
rem Release build: CrashTeamRacingNitroFueled.exe WITHOUT the translated shaders and button glyphs
rem (nothing of the games inside): on the first start the embedded setup makes them from the
rem player's dump. SETUP_PYTHON: the python.org CPython whose runtime gets packed (tools/pack_setup.py).
rem Output: build-release\CrashTeamRacingNitroFueled.exe
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not defined SETUP_PYTHON set "SETUP_PYTHON=C:\Python314\python.exe"
"%SETUP_PYTHON%" "%~dp0..\tools\pack_setup.py" "%~dp0res\setup.bin" || exit /b 1
if not exist build-release (
    cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCTR_EMBED_DATA=OFF ^
        -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" || exit /b 1
)
cmake --build build-release || exit /b 1
