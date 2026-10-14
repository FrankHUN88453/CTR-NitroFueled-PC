#pragma once
// Button prompt glyphs for the PC.
//
// The game draws button prompts with the "button" image of its font (fonts/GeekSpeak.igz, 512x263
// RGBA8: ✕ ○ △ □ L1 R1 L2 R2 and the sticks, referenced by the font's alternate char metrics).
// Depending on the prompt style (hle/pad.h) the renderer samples a replacement with the same
// layout instead: the Xbox buttons of the PC Crash trilogy (resource GLYPHS_XBOX, made by
// tools/make_glyph_atlas.py) or the bound keys and mouse buttons, drawn at runtime.
#include <d3d11.h>

#include "../common.h"

constexpr u32 kButtonAtlasWidth = 512, kButtonAtlasHeight = 263;

// True if `rgba` (level 0, tightly packed) is the PS4 button atlas.
bool IsButtonAtlas(u32 width, u32 height, const void* rgba);
// Keeps the original texels (the cells without buttons stay as they are).
void NoteButtonAtlas(const void* rgba);
// View to sample instead of the button atlas for the current prompt style; nullptr = original.
ID3D11ShaderResourceView* ButtonAtlasOverride(ID3D11Device* dev, DXGI_FORMAT fmt);
