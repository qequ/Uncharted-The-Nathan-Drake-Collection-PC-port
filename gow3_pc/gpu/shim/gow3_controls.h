// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// gow3: the keyboard and mouse page, on the games' own Controller Layout screen (Options >
// Controls > Controller Layout, in all three games). While that screen is up (its DualShock 4
// picture is drawn, gow3_glyphs.h) and the player uses the keyboard, the page covers it in the
// screen's look: every action with its keys and mouse button, changed in place (Enter, then the
// new key or mouse button), with the mouse look options and the button icons. Changes apply at
// once and are saved per game by src/runtime_pad.c. Back (Esc, or the key of Circle) closes the
// page and takes the game back to its Controls page.
//
// Frame and Draw run on the present thread, HandleEvent on the window thread; both hold the
// overlay's ImGui lock (gow3_overlay.cpp). Active is lock-free (the pad reads it).

union SDL_Event;

namespace Gow3Controls {

/// Present thread, every frame: opens the page when the screen appears, closes it after.
void Frame();
/// The page is up: the game's pad input is held neutral.
bool Active();
/// Present thread, inside the ImGui frame: draws the page over a display of width x height.
void Draw(float width, float height);
/// Window thread, while the page is up: an SDL event; true when the page used it.
bool HandleEvent(const SDL_Event& event);

} // namespace Gow3Controls
