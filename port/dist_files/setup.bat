@echo off
rem One-time setup: builds shaders.bin (and the Xbox button glyphs, when Crash Bandicoot N. Sane
rem Trilogy for PC is installed) from YOUR game dump. Run it from the game folder (next to eboot.bin).
setlocal
cd /d "%~dp0"
if not exist "eboot.bin" (
    echo eboot.bin is not here. Extract this release into your game folder ^(next to eboot.bin^) and run setup.bat again.
    pause
    exit /b 1
)
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    python -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo Python 3.8 or newer is needed for the setup: https://www.python.org/downloads/
    pause
    exit /b 1
)
%PY% -I "%~dp0setup\setup_shaders.py" "%~dp0." %*
if errorlevel 1 (
    echo Setup failed.
    pause
    exit /b 1
)
echo.
echo All set. Start CrashTeamRacingNitroFueled.exe. The setup folder and setup.bat can be deleted.
pause
