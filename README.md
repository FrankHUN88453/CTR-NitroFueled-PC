# CTR Nitro-Fueled (PS4, CUSA13795) – reverse engineering

> **Nem tartalmaz játékfájlt.** A repóban csak saját kód, eszközök és leírások vannak; a futtatáshoz
> és a shaderek / gombikonok generálásához a saját, legálisan beszerzett PS4-es dumpod kell (a
> generált adatok az `out/` mappába kerülnek, ami nincs gitben). Nem kapcsolódik az Activisionhöz,
> a Beenoxhoz, a Vicarious Visionshöz vagy a Sonyhoz.
>
> *No game files are included: only original code, tools and notes. Running it, and generating the
> shaders and button glyphs, needs your own legally obtained PS4 dump. Not affiliated with
> Activision, Beenox, Vicarious Visions or Sony.*

Cél: a PS4-es build teljes feltérképezése egy későbbi PC-port alapjaként.

## Forrás

| | |
|---|---|
| Title ID | CUSA13795 (EU, UP0002) |
| Build | `//Octane/ms-rc-ps4` CL 1233015, 2019-04-16 |
| Modul | `game-ps4-fin` (ET_SCE_DYNEXEC, 0xFE10) |
| Motor | Vicarious Visions Alchemy (`ig*` reflection), VSC visual script, C# (saját DotNet runtime) |
| Middleware | Havok 2014.1.0-r1 (Physics/Behavior/Animation), FMOD, Bink 2 (BinkGPU), Demonware (online) |
| Dump | fake-signed SELF, titkosítatlan szegmensek |

## Könyvtárstruktúra

- `tools/self2elf.py` – SELF/SPRX → ELF
- `tools/ps4dyn.py` – SCE dinamikus info: modulok, libek, importok (NID), exportok, relokációk
- `tools/prep_ghidra.py` – Ghidra-barát ELF (relokációk alkalmazva, import-stubok, eh_frame függvénykezdetek)
- `tools/alchemy_meta.py` – Alchemy reflection kinyerése → `out/alchemy_classes.json`
- `tools/gen_alchemy_types.py` – C++ header + Ghidra apply fájl
- `tools/vsc_methods.py` – VSC-nek kiajánlott metódusok név → cím
- `tools/iga.py` – `.pak` (IGA v13) listázó/kicsomagoló
- `tools/igz.py` – `.igz` (v10) olvasó, objektumok dekódolása a reflection alapján
- `ghidra_scripts/` – `ApplyPs4Symbols.java` (pre-script), `ApplyAlchemyMeta.java`
- `elf/` – kinyert ELF-ek (eboot + sce_module), `data/` – NID-névlista (nincs gitben)
- `out/` – generált adatok (dyn.json, symbols.tsv, alchemy_*, vsc_methods.json, pak_index.txt)

A Python-eszközök a `.venv`-ből futnak (capstone, pefile):

```bash
.venv/Scripts/python -I tools/alchemy_meta.py elf/eboot.ghidra.elf out/eboot.symbols.tsv out/alchemy_classes.json
```

Ghidra: `F:\PS4\CRTNF\_tools\ghidra_12.1.4_PUBLIC`, projekt: `ghidra_proj/CTR`.

## Memória-elrendezés (eboot, base 0)

| Phdr | Típus | VA | Méret | |
|---|---|---|---|---|
| 0 | LOAD R-X | 0x0000000 | 0x21fb5d0 | .text |
| 1 | SCE_RELRO | 0x2200000 | 0x3e5670 | relro / vtables |
| 2 | LOAD RW- | 0x2600000 | 0x15f90 (mem 0x1ea26d) | .data/.bss |
| 7 | GNU_EH_FRAME | 0x20ce45c | | |

455 372 R_X86_64_RELATIVE relokáció → a vtable-ök és pointertáblák jól felderíthetők.

## Platformfüggőség (amit PC-n ki kell váltani)

~700 importált függvény, 41 könyvtár. A legnagyobbak:

| Lib | db | PC-megfelelő |
|---|---|---|
| libc | 140 | MSVC CRT |
| libSceGnmDriver | 86 | Vulkan/D3D12 (fő feladat) |
| libkernel / libScePosix | 113 | Win32 szálak, fájl, memória |
| libSceNpToolkit2 + Np* | ~110 | online: stub / Steam |
| libSceFont | 41 | FreeType |
| libSceNet / Http / Ssl | 52 | WinSock / WinHTTP |
| libSceAjm, AudioOut | 18 | FMOD PC backend / XAudio2 |
| libScePad | 8 | XInput / SDL |

Részletek: `out/eboot.dyn.json`.

## Port-stratégia: az Alchemy platformrétege

Az Alchemy a platformfüggő részt absztrakt osztályok mögé teszi. A CTR-ben 63 `igPS4*`
osztály van; ezek közül szinte mindegyiknek van `igDx11*` / `igWin32*` párja a
N. Sane Trilogy PC exe-ben (azonos motor):

| PS4 (CTR) | alap interfész | PC (NST) |
|---|---|---|
| igPS4GraphicsDevice / …Context | igBaseGraphicsDevice / …Context | igDx11GraphicsDevice / …Context |
| igPS4*ResourcePool (Buffer, Texture, Shader, RTV, Query) | igPool | igDx11*ResourcePool |
| igPS4ConstantHelper, igPS4ShaderConstantHelper | igShaderConstantHelper | igDx11ConstantHelper, … |
| igPS4InputContext, Controller, Keyboard, Mouse | igInputContext, … | igWin32* |
| igPS4FilePath, StorageDevice | igFilePath, igPhysicalStorageDevice | igWin32* |
| igPS4Mutex, Semaphore, Signal, SystemMemoryPool | igBaseMutex, … | igWin32* |
| igPS4Window, NetworkInit | igWindow, igNetworkInit | igWin32* |

→ A port fő feladata ~40 platformosztály újraimplementálása ugyanazon vtable-interfész
mögött, nem a 700 PS4-függvény emulálása. Nyitott kérdések: a nem reflektált
platformkód (közvetlen sce*-hívások), a GCN shaderek (újrafordítás/fordítás kell), és
az NST exe `.text`-je SteamStub-bal titkosított (`.bind` szekció, entrópia 8.0).

## Alchemy reflection (out/alchemy_classes.json, out/alchemy_classes.h)

- 8608 osztály (5469 játékspecifikus `C*`), öröklődés, méret, vtable, 31 473 típusos mező
- regisztráció: `0x143fcd0` (igMetaObject register), `0x13ecc00` (ensure registered),
  `0x143f110` (mezők példányosítása), `0x143f250` (mezők elnevezése)
- játéklogika meta-osztálya `igDotNetMetaObject` → valószínűleg C#-ból AOT-fordított kód
- `ghidra_scripts/ApplyAlchemyMeta.java` + `out/ghidra_alchemy.json`: struktúrák,
  osztály-névterek, 37 400 elnevezett virtuális függvény

## Játéklogika (C# / DotNet)

A motorban saját .NET runtime van (`DotNetRuntime`, `DotNetLibrary`, `DotNetMethodDefinition`),
a játékkód névtere `Scripts.*`. A `data:/%s_script.vvl` assembly-k NINCSENEK a release-ben,
a logika AOT-fordítva az eboot-ban van. 1232 metódus `*_VSC` néven ki van ajánlva a visual
scriptnek; ebből 505 név → cím feloldva (`out/vsc_methods.json`, pl. `CKart::getKartPosition`
= `0x10a85c0`). A maradék 165 tömb más regisztrációs mintát használ (`igVscMethodCall`).

## Asset formátumok

- **IGA v13 (.pak)**: 0x38 bájtos fejléc (= `igArchive::_archiveHeader`), fileId-tábla,
  FileInfo {offset, ordinal, length, blockInfo}, u32/u16/u8 blokktáblák (szektorszám, felső bit =
  tömörített), 0x8000-es blokkok, LZMA (c2) vagy tárolt (cF). 459 archívum, 63 484 fájl
  (36 318 egyedi): 34 392 igz, 734 snd, 652 lng, Havok hka/hkx/hkb/hkp/hkc, mapnav.
- **IGZ v10**: fejléc + szekcióleírók; 0. szekció fixupok (TSTR, TMET, MTSZ, RVTB, RSTT, ROFS,
  RPID, ROOT, ONAM, …), packed offsetlisták (nibble: 3 adatbit + folytatásbit, delta ×4),
  hivatkozások `(szekció << 27) | offset`. **Ellenőrzés: 1337 fájl, 41 090 objektum, 8979
  MTSZ-típusméret, 0 eltérés a reflection-adatbázistól.**

## Natív futtató (port/)

`port/` – `CrashTeamRacingNitroFueled.exe`: a PS4 kód natívan fut Windowson (CPU-emuláció nélkül), a
rendszerkönyvtárak Win32-alapú natív implementációk. A grafika natív D3D11: az
igPS4GraphicsDevice állapotából rajzolunk, a GCN shadereket előre lefordítjuk HLSL-re
(`tools/gcn2hlsl.py`, 3196/3196 shader, köztük a Fur ES→GS→copy-VS lánc D3D11 geometry
shaderként). A címképernyő pixelre egyezik a konzolos/emulátoros képpel
([docs/title_native.png](docs/title_native.png)); a játék a kalandmódig és a versenyekig
játszható. PC-s kezelés: XInput kontroller és billentyűzet/egér (`%APPDATA%\CTRNF\Controls.sav`; vezetés: WASD,
bal klikk tárgy, jobb klikk drift; menükben natív egér: rámutatás kijelöl, kattintás választ),
a gombjelzések a használt eszközt követik (Xbox gombok, ill. billentyűk/egérgombok; „Click to
begin/skip”, a Space is kihagy/indít), mentések és beállítások a `%APPDATA%\CTRNF` mappában.
Helyi többjátékos mód (osztott képernyő, 2–4 játékos): minden további XInput-kontroller új
PS4-felhasználóként jelentkezik be, a Local Arcade-ban így csatlakozhat.
Részletek: [port/README.md](port/README.md).

## Állapot / következő lépések

- [x] SELF → ELF
- [x] import/export táblák (NID-ek), 693/701 import névvel (shadPS4 aerolib)
- [x] Ghidra projekt + import-stubok + 135 468 eh_frame függvénykezdet
- [x] Alchemy reflection: 8608 osztály, 31 473 mező, vtable-ök, 37 400 virtuális függvény
- [x] platformréteg azonosítása (igPS4* ↔ NST igDx11*/igWin32*)
- [x] IGA kicsomagoló, IGZ olvasó
- [x] Ghidra auto-analízis + `ApplyAlchemyMeta.java` (8608 struct, 55 120 elnevezett függvény)
- [x] `CrashTeamRacingNitroFueled.exe`: natív betöltő, a játék végigjátszható állapotig bootol
- [x] PC kezelés: kontroller + billentyűzet/egér, eszközkövető gombjelzések (Xbox / billentyű),
  egér a menükben (a GUI fókuszát a játék saját navigációjával vezetjük)
- [x] osztott képernyő 2–4 játékossal (játékosonként PS4-felhasználó + saját pad-handle),
  `kCopyTexture` D3D11-másolással (a PS4 compute shaderrel másol; 3–4 játékosnál nélküle szürke a kép)
- [x] mentések (libSceSaveData → `%APPDATA%\CTRNF`), beállítások (`GraphicsOptions.sav`)
- [x] natív grafikus eszköz (igPS4GraphicsDevice állapot → D3D11), GCN → HLSL shader-fordítás
- [x] deferred pipeline: G-buffer, stencil, tiled lights, skybox, bloom, motion blur, LUT, FXAA, UI
- [x] geometry shader út (Fur: ES → GS + copy VS), skinning (egész/scaled vertex formátumok)
- [ ] 2D macro-tiled textúrák, mipmapek, sampler-állapot, compute (auto exposure, BinkGPU)
- [x] hang kimenet (WASAPI: a portok 48 kHz 7.1 keverése, a Windows alakítja az eszközre), mentések
- [ ] online funkciók kikapcsolása
- [ ] maradék VSC-tömbök (igVscMethodCall minta), DotNet metaadatok
- [ ] IGZ: igMemoryRef/igVector mezők, handle-ök (RHND/EXNM), textúrák (GNF tiling), modellek
- [x] shaderek: igPS4ShaderHeader + GCN bináris → HLSL/DXBC (`tools/gcn2hlsl.py`)
- [ ] Havok / FMOD verzió azonosítása, ismert SDK-függvények illesztése
- [ ] nem reflektált platformkód feltérképezése (sce*-hívók csoportosítása)
