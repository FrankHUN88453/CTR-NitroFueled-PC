# PC-maradékok és a grafika felhúzhatósága (2026-10-14)

## 1. Maradék fájlok / kód, ami PC-hez jól jöhet

A CTR Nitro-Fueled sosem jelent meg PC-n, a PS4-es dumpban **nincs PC-s tartalom**
(nincs DX-shader, PC-s menü, billentyű-ikon vagy PC-s beállításmenü). Ami van:

| Mi | Hol | Haszna |
|---|---|---|
| `pcPlatformStrings_*.lng/.igz` | `permanent_ps4.pak` | Csak töredék szöveg (mentéshely, „Docked Mode Required”), a Switch-ághoz tartozik, PC-hez nem használható |
| Platform szerinti pályarétegek (`*_Switch`, `*_NX`, `*_PS4_XBOX_SWITCH_NSP`) | pálya-pak-ok | Ugyanaz az adat ment Xbox One-ra és Switch-re is; a Switch-rétegek gyengébbek |
| Osztott képernyős kizárások (`*_Ex_4P`, `*_Excl_2P`, `*_Foliage_Ex_4P`) | pálya-pak-ok | 2-4 játékosnál a PS4 kihagy fényt, növényzetet, díszleteket; PC-n ezek bekapcsolva hagyhatók |
| `permanentdeveloper.pak` | archives | Fejlesztői maradék: SmokeTestData, DebugChannelPresets, SharedCameraSettings |
| ImGui debug felület (`igDebugUiContext`, egeret is kezel) | eboot | Fejlesztői menük, később esetleg bekapcsolható |
| `igWin32DebugHostContext`, `igDurangoDebugHostContext` | eboot (csak reflection) | A motor PC/Xbox fejlesztőeszköz-kapcsolata, kód nélkül |
| Parancssori kapcsolók: `-pc` (primary controller), `-join`/`AutoJoinGame`, `-mmtid`, `--telemetryhost`, `-ttrsim`, `-hm`, `-gm`, `-st`, `-fst` | eboot | Fejlesztői/teszt kapcsolók, PC-módot nem rejtenek |
| **Crash N. Sane Trilogy PC** (ugyanez a motor) | Steam | A valódi „PC-s maradék”: 6159 D3D11 shader referenciának (`tools/dxbc_disasm.py`), Xbox gombikonok |

## 2. Meddig lehet felhúzni a grafikát

### Már bekapcsolva
- Belső felbontás: `RenderScale=200` (3840x2160 szuperszampling), legfeljebb 400 (7680x4320)
- 16x anizotróp szűrés, javított árnyékok (mindhárom kaszkád, 16 bites árnyéktérképek)
- FPS-korlát: `FpsLimit` (alapból 60, 0 = korlátlan), a játék logikája valós időlépéssel fut

### Új: PS4 Pro mód (`[Graphics] PS4Pro=1`)
- A játék natívan 2560x1440-ben renderel (4K kijelzővel sem vált checkerboardra), kb. 7%-kal több
  rajzolással (Pro extrák). A `RenderScale` továbbra is 1920x1080-hoz viszonyít, így 200 = 4K belső.
- Kipróbálva: címképernyő, menük (egérrel is), Adventure hub. Minden pályán még nincs tesztelve,
  ezért alapból kikapcsolva.

### A motorban megtalált, még nem állított kapcsolók
| Terület | Mező | Lehetőség |
|---|---|---|
| Árnyék-távolság | `igCascadeShadowRenderData._maxCascadesCount` = 3, az atlaszban 4 hely van | 4. kaszkád: messzebb is van árnyék |
| Árnyék-élesség | `igCascadeShadowRenderPass._shadowMapSize` = 2048 | 4096: kétszer élesebb árnyékok |
| Ellenfelek részletessége | `CRacerLODSettings` | mindig a legrészletesebb modell |
| Shader-LOD távolság | `igRenderLodParameters._shaderLodDistance` | a drága shaderek messzebbre is |
| Effektek | `CVfxQualityBundle._quality`, `igVfxManager._highQualityTrails` | jobb részecskék / csíkok |
| Textúrák | `igBaseVisualContext._textureLodBias` | élesebb mipek |
| Osztott képernyő | a pályák `*_Ex_4P` / `*_Excl_2P` rétegei | 2-4 játékosnál is teljes részletesség |

Magasabb felbontású textúrák nincsenek a CTR adataiban (a N. Sane PC-nek van `4ktextures_*.pak`-ja,
a CTR-nek nincs), így a textúraminőség a PS4-esé.

### A valódi korlát: a CPU-oldali renderelés
Versenyben kb. 5000 rajzolás/képkocka, ezekre a D3D11-réteg ~16 ms-ot költ, így ~27 FPS jön ki
a GPU-tól függetlenül. A legnagyobb nyereség (stabil 60, akár 120+ FPS) a renderer gyorsítása:
a rajzolásonkénti munka csökkentése és több szálra osztása.
