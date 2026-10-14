#pragma once
// First start of a release build: the exe carries nothing of the game, so the translated shaders
// (and the Xbox button glyphs) are made once from the player's own dump by the setup embedded in
// the exe (RCDATA "SETUP": a minimal Python runtime + tools/setup_shaders.py, see tools/pack_setup.py).
#include <filesystem>

// Where the files made from the player's dump are kept: CTR_USER_DIR, else %LOCALAPPDATA%\CTRNF.
std::filesystem::path ShaderCacheDir();

// Builds shaders.bin (+ glyphs_xbox.rgba) into ShaderCacheDir() with a progress window. false = it
// failed, was cancelled, or this build has no setup (the player has been told).
bool RunFirstStartSetup(const std::filesystem::path& game_dir);
