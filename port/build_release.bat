@echo off
rem Release build: CrashTeamRacingNitroFueled.exe WITHOUT the translated shaders and button glyphs
rem (nothing of the games inside; tools/setup_shaders.py makes them from the player's dump).
rem Output: build-release\CrashTeamRacingNitroFueled.exe
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build-release (
    cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCTR_EMBED_DATA=OFF ^
        -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" || exit /b 1
)
cmake --build build-release || exit /b 1
