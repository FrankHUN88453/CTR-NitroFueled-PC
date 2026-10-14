#pragma once
// Input devices for the PC: which one the player uses decides the on-screen button prompts.
#include <cstddef>

#include "../common.h"

// Button prompt style: Xbox glyphs while a controller is used, keys and mouse buttons while
// keyboard/mouse is used (setting ButtonPrompts in GraphicsOptions.sav can fix one style).
enum class PromptStyle { Xbox, PlayStation, Keyboard };
PromptStyle PadPromptStyle();

// PS4 controls that have prompt glyphs.
enum class PadControl { Cross, Circle, Square, Triangle, L1, R1, L2, R2, LeftStick, RightStick };
// True while the mouse belongs to the GUI (menus, dialogs, title screen, videos) rather than to
// driving; keyboard prompts then show the menu meaning of the mouse buttons.
bool PadMenuContext();
// Key shown for a control in keyboard prompts (Windows virtual-key code, 0 = unbound). Menus: Cross
// is the left click, Circle the right click; elsewhere a bound mouse button if any, else the first
// key. Sticks: dir 0..3 = up, left, down, right.
int PadPromptKey(PadControl c, int dir, bool menu);
// Short upper-case key label for prompts ("SPACE", "W", "ENTER").
const char* PadKeyLabel(int vk, char* buf, size_t size);

// Local multiplayer: PS4 users 1-4 are the players, one per controller (player 1 also has the
// keyboard and mouse). The next user whose login the user service should report, 0 = none.
int PadTakeLogin();
// The logged-in users by player, -1 for a free place.
void PadLoginUsers(int ids[4]);
