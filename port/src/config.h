#pragma once
#include "common.h"

// User settings: %APPDATA%/CTRNF/GraphicsOptions.sav (INI text, written with
// the defaults on first start). An environment variable CTR_<NAME> overrides the files.
int ConfigInt(const char* name, int def);
void ConfigInit();
// Saves a setting (the in-game Video options); ConfigGeneration() then changes, so code that
// keeps a setting can re-read it.
void ConfigSet(const char* name, int value);
unsigned ConfigGeneration();
