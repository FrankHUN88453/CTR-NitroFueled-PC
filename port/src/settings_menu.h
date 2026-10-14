#pragma once
// PC settings inside the game's own Options menu: a "Video" button between Gameplay and Audio opens
// the options page with selectors for the PC settings (GraphicsOptions.sav).
//
// Built from the game's GUI at run time: the button is a clone of the Gameplay button and the
// rows are clones of the page's option selector (the engine's own placeable cloning), so they
// look, animate and navigate like the game's options. The selectors are detached from the game
// option they were made for and fed with our values; a changed value is applied and saved.
#include "common.h"

// Once per GUI update, on the GUI thread; `gui_context` is the game's igGuiContext.
void SettingsMenuUpdate(u64 gui_context);
