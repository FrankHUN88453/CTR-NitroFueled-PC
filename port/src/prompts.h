#pragma once
// PC wording of the game's button prompts: while keyboard/mouse is used, "Press ✕ to begin" on the
// title screen reads "Click to begin" and "Press △ to skip" on videos reads "Click to skip" (left
// click and Space do that, see hle/pad.cpp).
//
// The texts are localized strings (strings/ps4PlatformStrings_<lang>.lng, ui/InfoMessage_<lang>.lng)
// that the game's UI scripts read from loaded objects, so the loaded copies are found in guest
// memory (a background scan of the pages the game wrote) and rewritten in place; the PC texts are
// shorter than the originals.

// Called on every pad read: puts the PC (keyboard) or the original texts in place.
void PromptTextUpdate(bool keyboard);
