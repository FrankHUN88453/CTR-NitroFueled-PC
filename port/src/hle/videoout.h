#pragma once
#include "../common.h"

struct HWND__;

// Called by GNM submit-and-flip and sceVideoOutSubmitFlip.
void VideoOutFlip(int handle, int index, s64 arg);
bool VideoOutHasFocus();
// Mouse wheel notches since the last call (positive = away from the user) and whether the
// cursor is inside the game window.
int VideoOutTakeWheel();
bool VideoOutCursorInside();
// Cursor position in the game picture: 0..1 across the 16:9 frame (false: outside the window).
bool VideoOutCursorInPicture(float* x, float* y);
HWND__* VideoOutHwndRaw();
// Borderless fullscreen on / off (applied on the window thread).
void VideoOutSetFullscreen(bool on);
#define VideoOutHwnd() ((HWND)VideoOutHwndRaw())
