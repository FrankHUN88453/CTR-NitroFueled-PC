# CrashTeamRacingNitroFueled.exe – natív Windows futtató a CTR Nitro-Fueled PS4 kódjához

A játék saját x86-64 kódja (eboot + a dumpban szállított `libc.prx`, `libSceFios2.prx`)
**közvetlenül a CPU-n fut**, CPU-emuláció nélkül. A `CrashTeamRacingNitroFueled.exe` csak betölti és linkeli,
és a PS4 rendszerkönyvtárait (libkernel, VideoOut, Pad, AudioOut, …) Win32-re épülő natív
implementációkkal váltja ki. Az exe nem tartalmaz játékkódot: indításkor a saját dumpodból
olvassa.

## Fordítás és futtatás

```bash
./build.bat
```

```bash
build/CrashTeamRacingNitroFueled.exe "F:/PS4/CRTNF/UP0002-CUSA13795_00-CRASHTEAMRACING1"
```

Opcionális második argumentum: másodperc, utána szálállapot-dump és kilépés (hibakeresés).
`CTR_AUTOPRESS=1`: a virtuális kontroller 3 másodpercenként megnyomja az X-et (teszt).
`CTR_TEST_PAD<n>="<mp>[-<mp>]:<gomb>[+<gomb>];..."`: az n. játékos gombjai az adott időpontokban
(nevek mint a `Controls.sav`-ban, pl. `52:Cross;90-100:Cross+LeftStickRight`); a 2–4. játékos
ettől saját, induláskor csatlakoztatott virtuális kontrollert kap (osztott képernyő tesztje).
`CTR_RECORD_AT=<kocka>`: felvétel az adott kockától, mint az F12-vel (`CTR_RECORD_FRAMES=<n>`,
`CTR_RECORD_SIZE=<sz>x<m>`); `CTR_GLITCH_TRAP=1` mellett csak a hirtelen elsötétülő kockák (és az
előttük lévő) maradnak meg, `glitch trap` naplósorral. `tools/flicker_run.sh`: hosszú tesztút a Hubon
villogás keresésére; `tools/flicker_debug.patch`: a teljes hibakereső változat (rajzolásonkénti lista,
pixelszám-lekérdezés, render target pillanatképek a hibás kockáról, `CTR_GLITCH_TRAP=2`).
Telepítés a játék mappájába (dupla kattintással indítható, argumentum nélkül a saját
mappáját használja): `./deploy.sh` → `<játék>/CrashTeamRacingNitroFueled.exe` + `<játék>/shaders/`.

Napló: `CrashTeamRacingNitroFueled.log` az aktuális könyvtárban. Mentések: `build/userdata/`.

Irányítás: XInput kontroller és billentyűzet/egér (`%APPDATA%\CTRNF\Controls.sav`, alapból WASD,
bal klikk tárgy, jobb klikk / Space drift, menükben natív egér). Osztott képernyő: a billentyűzet az
1. játékos első kontrollerével együtt játszik, minden további kontroller a következő játékos;
`SeparateKeyboardPlayer = 1` esetén a billentyűzet önálló 1. játékos, a kontrollerek a 2–4.

## Felépítés

| Fájl | Feladat |
|---|---|
| `module.cpp` | SELF → memória, SCE dinamikus info, relokációk, `fs:` TLS-elérések patchelése (`mov reg, fs:[0]` → `gs:` TEB TLS-slot trampolin) |
| `linker.cpp` | NID-alapú linkelés: valódi prx-exportok → natív implementáció → naplózó stub |
| `memory.cpp` | vendég-memória a PS4 címtartományban (< 0xFC00000000, a libc ezt ellenőrzi) |
| `thread.cpp` | vendégszálak, statikus TLS (variant II) + DTV, szálállapot-dump |
| `hle/kernel_*.cpp` | pthread/mutex/cond/rwlock/sema/eventflag, memória, fájlok (`/app0` → dump), idő, equeue |
| `hle/gnm.cpp` | „null GPU”: nem rajzol, csak a szinkronizációs PM4-csomagokat (EOP/EOS/RELEASE_MEM/WRITE_DATA/DMA) teljesíti |
| `hle/videoout.cpp` | Win32 ablak, 60 Hz vblank, flip-ütemezés |
| `hle/audio.cpp` | AudioOut portok helyes blokkoló időzítéssel (egyelőre néma) |
| `hle/pad.cpp` | XInput + billentyűzet |

## Állapot

A játék eljut a címképernyő-pályáig (`Octane/Octane_StartScreen`, „level finished
loading”), elindulnak a motor szálai (job queue, Havok, FMOD, Bink, streaming), lejátssza a
nyitó videók időzítését, és 60 Hz-en fut a főciklus. Kép még nincs: a következő nagy lépés az
`igPS4GraphicsDevice` réteg kiváltása natív D3D11/Vulkan eszközzel (az NST `igDx11*`
osztályai mintájára), illetve a GCN shaderek PC-re fordítása.
