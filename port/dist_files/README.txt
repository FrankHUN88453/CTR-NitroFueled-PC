Crash Team Racing Nitro-Fueled - native PC runner, version 0.1
https://github.com/FrankHUN88453/CTR-NitroFueled-PC

This runs the PS4 game's own code natively on Windows (no emulation) with a D3D11 renderer.
It contains NO game files: you need your own dump of the game.

Requirements
- Windows 10 or 11 (64-bit), a Direct3D 11 graphics card
- Your own decrypted dump of Crash Team Racing Nitro-Fueled, CUSA13795 (EU), base version 1.00
  (the folder with eboot.bin, sce_module and archives)
- Python 3.8 or newer, once, for the setup (https://www.python.org/downloads/)
- Optional: Crash Bandicoot N. Sane Trilogy for PC (Steam) - its Xbox button icons are used for
  controller prompts; without it a controller shows the PlayStation buttons

Install
1. Extract this zip into your game folder, next to eboot.bin.
2. Run setup.bat once. It reads your dump and builds shaders.bin (a few minutes). If N. Sane Trilogy
   is installed outside Steam's library folders: setup.bat --nsane "<its folder>"
3. Start CrashTeamRacingNitroFueled.exe.
   (setup.bat and the setup folder can be deleted afterwards.)

Controls
- Xbox controllers (XInput), up to 4 for split screen; keyboard and mouse
- Keyboard: WASD to drive, left click = item, right click / Space = drift; the mouse works in menus
- F11 or Alt+Enter: fullscreen; F12: record 3 seconds of frames (for bug reports)
- Graphics options: Options > Video in the game's own menu

Settings, saves and the log are in %APPDATA%\CTRNF.

Known limitations
- Online features do not work
- The Aspect Ratio option (ultrawide) is not implemented yet: the picture stays 16:9
- Some effects may still be missing or wrong

Not affiliated with Activision, Beenox, Vicarious Visions, Toys for Bob or Sony.
MIT License, see LICENSE.txt.
