#pragma once
// Linux: SDL2 initialisation shared by the window, input, audio and the first-start window.
#include <SDL2/SDL.h>

// Initialises the SDL subsystems once (thread-safe); false if they are unavailable.
bool SdlEnsure(Uint32 flags);
