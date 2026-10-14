@echo off
rem Builds CrashTeamRacingNitroFueled.exe with clang-cl + Ninja. Output: build\CrashTeamRacingNitroFueled.exe
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
rem translated shaders -> res\shaders.bin (embedded in the exe)
"%~dp0..\.venv\Scripts\python.exe" "%~dp0..\tools\pack_shaders.py" "%~dp0..\out\hlsl" "%~dp0res\shaders.bin" || exit /b 1
rem Xbox button glyphs from the Crash N. Sane Trilogy PC files -> res\glyphs_xbox.rgba (once;
rem delete the file to remake it). NSANE_DIR overrides the install folder.
if not defined NSANE_DIR set "NSANE_DIR=E:\SteamLibrary\steamapps\common\Crash Bandicoot - N Sane Trilogy"
if not exist "%~dp0res\glyphs_xbox.rgba" (
    "%~dp0..\.venv\Scripts\python.exe" "%~dp0..\tools\make_glyph_atlas.py" "%~dp0..\.." "%NSANE_DIR%" "%~dp0res\glyphs_xbox.rgba" || exit /b 1
)
if not exist build (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" || exit /b 1
)
cmake --build build || exit /b 1
