#pragma once
// Mouse in the game's menus: pointing at a menu item moves the menu focus to it and a left click
// on an item chooses it.
//
// The GUI (Alchemy igGui, game behaviours in C#) only knows controller navigation, so the mouse
// drives that: the focused item (igGuiProject::_focusPlaceable) and its visible siblings are read
// from the live GUI objects, the item under the cursor is found with the engine's own placeable
// hit test, and d-pad steps are sent until the game's focus reaches it; the game then plays its
// usual focus animations and sounds. A click on the focused item is a ✕ press.
#include "common.h"

enum : unsigned {
    kMouseUiUp = 1, kMouseUiDown = 2, kMouseUiLeft = 4, kMouseUiRight = 8, kMouseUiConfirm = 16,
};

struct MouseUiInput {
    bool cursor;    // the cursor is over the game picture
    float x, y;     // position in the picture, 0..1
    bool button;    // left button down
};
struct MouseUiOutput {
    unsigned buttons;  // kMouseUi* to press this read
    bool ui;           // the GUI has a focused item (menu, dialog): the mouse buttons belong to it
};

// Hooks the game's GUI input (call after linking).
void MouseUiInstall();
// Called on every pad read (the GUI thread).
void MouseUiUpdate(const MouseUiInput& in, MouseUiOutput* out);
