Crash Team Racing Nitro-Fueled - native PC runner
https://github.com/FrankHUN88453/CTR-NitroFueled-PC

This runs the PS4 game's own code natively on Windows (no emulation) with a D3D11 renderer.
It contains NO game files: you need your own dump of the game.

Requirements
- Windows 10 or 11 (64-bit), a Direct3D 11 graphics card
- Your own decrypted dump of Crash Team Racing Nitro-Fueled, CUSA13795 (EU), base version 1.00
  (the folder with eboot.bin, sce_module and archives)
- Optional: Crash Bandicoot N. Sane Trilogy for PC (Steam) - its Xbox button icons are used for
  controller prompts; without it a controller shows the PlayStation buttons

Install
1. Copy CrashTeamRacingNitroFueled.exe into your game folder, next to eboot.bin.
2. Start it. The first start builds the game's shaders from your dump (a few minutes, once; a
   window shows the progress), then the game starts. Later starts go straight to the game.

The built shaders, settings, saves and the log are in %LOCALAPPDATA%\CTRNF (shaders.bin,
setup.log) and %APPDATA%\CTRNF (settings, saves, log).

Controls
- Xbox controllers (XInput), up to 4 for split screen; keyboard and mouse
- Keyboard: WASD to drive, left click = item, right click / Space = drift; the mouse works in menus
- F11 or Alt+Enter: fullscreen; F12: record 3 seconds of frames (for bug reports)
- Graphics options: Options > Video in the game's own menu

Known limitations
- Online features do not work
- The Aspect Ratio option (ultrawide) is not implemented yet: the picture stays 16:9
- Some effects may still be missing or wrong

The exe carries a minimal Python runtime (Python Software Foundation License) for the first-start
shader build.

Not affiliated with Activision, Beenox, Vicarious Visions, Toys for Bob or Sony.
MIT License, see LICENSE.txt.
