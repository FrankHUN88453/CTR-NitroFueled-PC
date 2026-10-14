#pragma once
// Native graphics backend (D3D11) for the engine's render layer.
//
// The engine's PS4 device context (igPS4GraphicsDeviceContext) keeps the complete render
// state as engine objects; at every draw this backend reads that state and issues the
// equivalent D3D11 draw with the ahead-of-time translated shaders (tools/gcn2hlsl.py).
#include "../common.h"

// Called from the GNM draw entry points while the engine records a draw.
void GfxProbeDraw(const char* kind, u32 count, u64 index_addr);
void GfxDraw(u32 count, u64 index_addr, bool indexed);
// Presents the surface at `addr` (a registered VideoOut buffer). false = not rendered yet.
bool GfxPresent(u64 addr);
bool GfxActive();
// A video (Bink) was drawn within the last few frames.
bool GfxVideoPlaying();
// Where the game picture is in the window, as fractions of the client area: x0, y0, x1, y1
// (the 16:9 picture is letterboxed).
void GfxPictureRect(float r[4]);
// Installs hooks into engine functions (clear). Call after linking.
void GfxInstallHooks();
// Saves the next frames at 1280x720 into %APPDATA%/CTRNF/capture/ (F12).
void GfxRecordFrames(int frames);
