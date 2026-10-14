# CTR Nitro-Fueled (PS4, CUSA13795) – reverse engineering and native PC runner

> **No game files are included.** This repository holds only original code, tools and notes. Running
> it, and generating the shaders and button glyphs, needs your own legally obtained PS4 dump (generated
> data goes to `out/`, which is not in git). Not affiliated with Activision, Beenox, Vicarious Visions,
> Toys for Bob or Sony.

Goal: map the PS4 build completely as the base of a PC port. The result so far,
`CrashTeamRacingNitroFueled.exe`, runs the game's own PS4 code natively on Windows (no CPU emulation)
with Win32 replacements of the PS4 system libraries and a D3D11 renderer.

## Playing (release)

Download the zip from [Releases](https://github.com/FrankHUN88453/CTR-NitroFueled-PC/releases), then:

1. Copy `CrashTeamRacingNitroFueled.exe` into your game folder (your own dump of CUSA13795, EU, base
   version 1.00), next to `eboot.bin`.
2. Start it. The first start builds the shaders from your dump (a few minutes, once, with a progress
   window), plus the Xbox button glyphs when Crash Bandicoot N. Sane Trilogy for PC is installed;
   then the game starts. Nothing else needs to be installed: the exe carries the setup.

The built shaders live in `%LOCALAPPDATA%\CTRNF`; settings, saves and the log in `%APPDATA%\CTRNF`;
graphics options are in the game's own Options > Video page. See [port/README.md](port/README.md) for building from source and test hooks.

## Source

| | |
|---|---|
| Title ID | CUSA13795 (EU, UP0002) |
| Build | `//Octane/ms-rc-ps4` CL 1233015, 2019-04-16 |
| Module | `game-ps4-fin` (ET_SCE_DYNEXEC, 0xFE10) |
| Engine | Vicarious Visions Alchemy (`ig*` reflection), VSC visual script, C# (own DotNet runtime) |
| Middleware | Havok 2014.1.0-r1 (Physics/Behavior/Animation), FMOD, Bink 2 (BinkGPU), Demonware (online) |
| Dump | fake-signed SELF, unencrypted segments |

## Layout

- `tools/self2elf.py` – SELF/SPRX → ELF
- `tools/ps4dyn.py` – SCE dynamic info: modules, libraries, imports (NID), exports, relocations
- `tools/prep_ghidra.py` – Ghidra-friendly ELF (relocations applied, import stubs, eh_frame function starts)
- `tools/alchemy_meta.py` – Alchemy reflection → `out/alchemy_classes.json`
- `tools/gen_alchemy_types.py` – C++ header + Ghidra apply file
- `tools/vsc_methods.py` – methods exported to VSC, name → address
- `tools/iga.py` – `.pak` (IGA v13) lister / extractor
- `tools/igz.py` – `.igz` (v10) reader, objects decoded with the reflection data
- `tools/gcn.py`, `tools/gcn2hlsl.py` – GCN shader decoder and GCN → HLSL translator
- `tools/setup_shaders.py` – the release setup: shaders and button glyphs from the player's dump;
  `tools/pack_setup.py` packs it with a minimal Python runtime into the release exe
- `ghidra_scripts/` – `ApplyPs4Symbols.java` (pre-script), `ApplyAlchemyMeta.java`
- `port/` – the native runner (see [port/README.md](port/README.md))
- `elf/` – extracted ELFs, `data/` – NID name list, `out/` – generated data (none of them in git)

The Python tools run from a `.venv` (capstone, pefile for the analysis tools; the setup needs only the
standard library):

```bash
.venv/Scripts/python -I tools/alchemy_meta.py elf/eboot.ghidra.elf out/eboot.symbols.tsv out/alchemy_classes.json
```

## Memory layout (eboot, base 0)

| Phdr | Type | VA | Size | |
|---|---|---|---|---|
| 0 | LOAD R-X | 0x0000000 | 0x21fb5d0 | .text |
| 1 | SCE_RELRO | 0x2200000 | 0x3e5670 | relro / vtables |
| 2 | LOAD RW- | 0x2600000 | 0x15f90 (mem 0x1ea26d) | .data/.bss |
| 7 | GNU_EH_FRAME | 0x20ce45c | | |

455 372 R_X86_64_RELATIVE relocations → vtables and pointer tables are easy to find.

## Platform dependencies (what a PC version replaces)

~700 imported functions from 41 libraries. The largest:

| Lib | count | PC counterpart |
|---|---|---|
| libc | 140 | MSVC CRT |
| libSceGnmDriver | 86 | D3D11 (the main task) |
| libkernel / libScePosix | 113 | Win32 threads, files, memory |
| libSceNpToolkit2 + Np* | ~110 | online: stub |
| libSceFont | 41 | FreeType |
| libSceNet / Http / Ssl | 52 | WinSock / WinHTTP |
| libSceAjm, AudioOut | 18 | WASAPI |
| libScePad | 8 | XInput |

## Port strategy: Alchemy's platform layer

Alchemy hides the platform-specific part behind abstract classes. CTR has 63 `igPS4*` classes; almost
all of them have an `igDx11*` / `igWin32*` counterpart in the N. Sane Trilogy PC exe (same engine):

| PS4 (CTR) | base interface | PC (NST) |
|---|---|---|
| igPS4GraphicsDevice / …Context | igBaseGraphicsDevice / …Context | igDx11GraphicsDevice / …Context |
| igPS4*ResourcePool (Buffer, Texture, Shader, RTV, Query) | igPool | igDx11*ResourcePool |
| igPS4ConstantHelper, igPS4ShaderConstantHelper | igShaderConstantHelper | igDx11ConstantHelper, … |
| igPS4InputContext, Controller, Keyboard, Mouse | igInputContext, … | igWin32* |
| igPS4FilePath, StorageDevice | igFilePath, igPhysicalStorageDevice | igWin32* |
| igPS4Mutex, Semaphore, Signal, SystemMemoryPool | igBaseMutex, … | igWin32* |
| igPS4Window, NetworkInit | igWindow, igNetworkInit | igWin32* |

The N. Sane exe's `.text` is protected (SteamStub, `.bind` section); only its data, strings and archives
are used as reference.

## Alchemy reflection

- 8608 classes (5469 game-specific `C*`), inheritance, sizes, vtables, 31 473 typed fields
- registration: `0x143fcd0` (igMetaObject register), `0x13ecc00` (ensure registered),
  `0x143f110` (field instantiation), `0x143f250` (field naming)
- the game logic's meta class is `igDotNetMetaObject` → most likely C# compiled ahead of time
- `ghidra_scripts/ApplyAlchemyMeta.java`: structs, class namespaces, 37 400 named virtual functions

## Game logic (C# / DotNet)

The engine has its own .NET runtime (`DotNetRuntime`, `DotNetLibrary`, `DotNetMethodDefinition`), the
game code lives in `Scripts.*`. The `data:/%s_script.vvl` assemblies are NOT in the release; the logic
is AOT-compiled into the eboot. 1232 methods are exported to the visual script as `*_VSC`; 505 of them
are resolved name → address (e.g. `CKart::getKartPosition` = `0x10a85c0`). The remaining 165 arrays
use another registration pattern (`igVscMethodCall`).

## Asset formats

- **IGA v13 (.pak)**: 0x38-byte header (= `igArchive::_archiveHeader`), fileId table,
  FileInfo {offset, ordinal, length, blockInfo}, u32/u16/u8 block tables (sector count, top bit =
  compressed), 0x8000-byte blocks, LZMA (c2) or stored (cF). 459 archives, 63 484 files
  (36 318 unique): 34 392 igz, 734 snd, 652 lng, Havok hka/hkx/hkb/hkp/hkc, mapnav.
- **IGZ v10**: header + section descriptors; section 0 holds fixups (TSTR, TMET, MTSZ, RVTB, RSTT, ROFS,
  RPID, ROOT, ONAM, …), packed offset lists (nibbles: 3 data bits + continuation bit, delta ×4),
  references `(section << 27) | offset`. **Checked: 1337 files, 41 090 objects, 8979 MTSZ type sizes,
  0 mismatches against the reflection database.**

## Native runner (port/)

`port/` builds `CrashTeamRacingNitroFueled.exe`: the PS4 code runs natively on Windows, the system
libraries are native Win32-based implementations. Graphics are native D3D11: draws are issued from the
state of igPS4GraphicsDevice, the GCN shaders are translated to HLSL ahead of time
(`tools/gcn2hlsl.py`, 3203/3203 shaders, including the Fur ES→GS→copy-VS chain as a D3D11 geometry
shader). The title screen matches the console picture pixel for pixel
([docs/title_native.png](docs/title_native.png)); adventure mode and races are playable. PC controls:
XInput controllers and keyboard/mouse (`%APPDATA%\CTRNF\Controls.sav`; driving: WASD, left click item,
right click drift; native mouse in menus: hover selects, click confirms); button prompts follow the
device in use (Xbox buttons, or keys and mouse buttons; "Click to begin/skip", Space skips/starts too).
Local multiplayer (split screen, 2–4 players): every further XInput controller logs in as a new PS4
user and can join in Local Arcade. Graphics options live in the game's own Options > Video page
(display mode, resolution, frame rate limit, shadow quality, texture filtering, motion blur).

## Status / next steps

- [x] SELF → ELF
- [x] import/export tables (NIDs), 693/701 imports named (shadPS4 aerolib)
- [x] Ghidra project + import stubs + 135 468 eh_frame function starts
- [x] Alchemy reflection: 8608 classes, 31 473 fields, vtables, 37 400 virtual functions
- [x] platform layer identified (igPS4* ↔ NST igDx11*/igWin32*)
- [x] IGA extractor, IGZ reader
- [x] Ghidra auto-analysis + `ApplyAlchemyMeta.java` (8608 structs, 55 120 named functions)
- [x] `CrashTeamRacingNitroFueled.exe`: native loader, the game boots to a playable state
- [x] PC controls: controller + keyboard/mouse, device-following prompts (Xbox / keys), mouse in menus
  (driving the GUI focus through the game's own navigation)
- [x] split screen with 2–4 players (a PS4 user + pad handle per player), `kCopyTexture` as a D3D11 copy
- [x] saves (libSceSaveData → `%APPDATA%\CTRNF`), settings (`GraphicsOptions.sav`), Video options page
- [x] native graphics device (igPS4GraphicsDevice state → D3D11), GCN → HLSL shader translation
- [x] deferred pipeline: G-buffer, stencil, tiled lights, skybox, bloom, motion blur, LUT, FXAA, UI
- [x] geometry shader path (Fur: ES → GS + copy VS), skinning (integer / scaled vertex formats)
- [x] audio output (WASAPI: the ports mixed to 48 kHz 7.1, Windows adapts it to the device)
- [x] release builds without game data (setup from the player's dump)
- [ ] ultrawide aspect ratios (the option exists, rendering is still 16:9)
- [ ] 2D macro-tiled textures, mipmaps, sampler state, compute (auto exposure, BinkGPU)
- [ ] switching the online features off cleanly
- [ ] remaining VSC arrays (igVscMethodCall pattern), DotNet metadata
- [ ] IGZ: igMemoryRef/igVector fields, handles (RHND/EXNM), textures (GNF tiling), models
- [ ] identify the Havok / FMOD versions, match known SDK functions
- [ ] map the non-reflected platform code (group the sce* callers)
