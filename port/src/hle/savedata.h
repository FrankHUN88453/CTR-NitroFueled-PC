#pragma once
// User data on the PC: %APPDATA%\CTRNF (save data, download/temp data).
#include <filesystem>
#include <string>

std::filesystem::path UserRoot();
// "/savedata<N>/..." -> the host folder of the save data directory mounted there.
bool SaveDataMountedPath(const std::string& guest, std::filesystem::path* host);
// True once the game mounted (or tried to mount) a save data directory: CTR does that only after
// the title screen ("Press X to begin") was confirmed.
bool SaveDataMountCalled();
