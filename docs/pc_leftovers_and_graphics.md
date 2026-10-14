# PC leftovers and how far the graphics can go (2026-10-14)

## 1. Leftover files / code that help a PC version

CTR Nitro-Fueled never came out on PC, and the PS4 dump holds **no PC content** (no DX shaders, PC
menus, key icons or PC settings menu). What there is:

| What | Where | Use |
|---|---|---|
| `pcPlatformStrings_*.lng/.igz` | `permanent_ps4.pak` | Only fragments (save location, "Docked Mode Required"); they belong to the Switch branch, not usable for PC |
| Per-platform level layers (`*_Switch`, `*_NX`, `*_PS4_XBOX_SWITCH_NSP`) | level paks | The same data went to Xbox One and Switch; the Switch layers are lighter |
| Split-screen exclusions (`*_Ex_4P`, `*_Excl_2P`, `*_Foliage_Ex_4P`) | level paks | With 2-4 players the PS4 drops lights, foliage and props; on PC they can stay on |
| `permanentdeveloper.pak` | archives | Developer leftovers: SmokeTestData, DebugChannelPresets, SharedCameraSettings |
| ImGui debug UI (`igDebugUiContext`, with mouse support) | eboot | Developer menus, maybe switchable on later |
| `igWin32DebugHostContext`, `igDurangoDebugHostContext` | eboot (reflection only) | The engine's PC/Xbox dev-tool connection, without code |
| Command line switches: `-pc` (primary controller), `-join`/`AutoJoinGame`, `-mmtid`, `--telemetryhost`, `-ttrsim`, `-hm`, `-gm`, `-st`, `-fst` | eboot | Developer/test switches, no hidden PC mode |
| **Crash N. Sane Trilogy PC** (same engine) | Steam | The real "PC leftover": 6159 D3D11 shaders as reference (`tools/dxbc_disasm.py`), Xbox button icons |

## 2. How far the graphics can go

### Already on
- Internal resolution: `RenderScale=200` (3840x2160 supersampling), up to 400 (7680x4320)
- 16x anisotropic filtering, fixed shadows (all three cascades, 16-bit shadow maps)
- Frame rate cap: `FpsLimit` (60 by default, 0 = unlimited); the game logic runs on real time steps

### PS4 Pro mode (`[Graphics] PS4Pro=1`, the default)
- The game renders natively at 2560x1440 (no checkerboard even with a 4K display), with about 7%
  more draws (the Pro extras). `RenderScale` stays relative to 1920x1080, so 200 = 4K internal.

### Engine switches found but not set yet
| Area | Field | Possibility |
|---|---|---|
| Shadow distance | `igCascadeShadowRenderData._maxCascadesCount` = 3, the atlas has room for 4 | 4th cascade: shadows further away |
| Shadow sharpness | `igCascadeShadowRenderPass._shadowMapSize` = 2048 | (now: Options > Video > Shadow Quality scales the shadow maps x0.5 / x1 / x2) |
| Opponent detail | `CRacerLODSettings` | always the most detailed model |
| Shader LOD distance | `igRenderLodParameters._shaderLodDistance` | expensive shaders further out |
| Effects | `CVfxQualityBundle._quality`, `igVfxManager._highQualityTrails` | better particles / trails |
| Textures | `igBaseVisualContext._textureLodBias` | sharper mips |
| Split screen | the levels' `*_Ex_4P` / `*_Excl_2P` layers | full detail with 2-4 players too |

There are no higher resolution textures in CTR's data (N. Sane PC has `4ktextures_*.pak`, CTR does
not), so texture quality is the PS4's.

### The real limit: CPU-side rendering
In heavy scenes there are 3000-5000 draws per frame and the D3D11 layer spends most of the frame time
on them, independent of the GPU. The biggest gain (a steady 60, even 120+ FPS) is a faster renderer:
less work per draw and spreading it over several threads.
