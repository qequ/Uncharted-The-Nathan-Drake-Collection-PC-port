// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// gow3: what the runtime draws over the game in the games' own look (the keyboard page on the
// Controller Layout screen, the save list, the text dialog). Text uses the running game's menu
// font, Adelon Serial: a BMFont in its data.psarc, read from the player's copy of the game at
// start (never shipped with the port). Panels, highlight bars, keycaps and button badges follow
// the games' menus. Dear ImGui draw lists, present thread, with the overlay's ImGui lock held.

#include <string>
#include <string_view>

#include "imgui.h"
#include "../gow3gpu.h"

namespace Gow3Ui {

/// Starts reading the font from the game's archive on a thread of its own (once).
void Start();
/// Present thread, before ImGui::NewFrame: makes the font's texture once it is read.
void Update();
/// The game's font is in use (else Dear ImGui's own stands in).
bool GameFont();

/// Width of `text` at `size`, the font's line height in pixels.
float TextWidth(std::string_view text, float size);
/// Draws `text` with the top of its line at `pos`; returns its width.
float Text(ImDrawList* dl, ImVec2 pos, std::string_view text, float size, ImU32 color);
/// The same, made smaller to fit `max_width`; returns the width drawn.
float TextFit(ImDrawList* dl, ImVec2 pos, std::string_view text, float size, float max_width,
              ImU32 color);

/// The Controller Layout screen's panel: dark and rounded, over [min, max].
void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float scale);
/// The bar under a selected row: light at the left, fading out to the right.
void Highlight(ImDrawList* dl, ImVec2 min, ImVec2 max);
/// A thin rule like the Controller Layout screen's lines.
void Rule(ImDrawList* dl, ImVec2 a, ImVec2 b, float scale);

/// A key's cap with its left edge at x, centred on y; returns its width. `lit`: drawn brighter
/// (waiting for a key).
float Keycap(ImDrawList* dl, float x, float y, float height, int scancode, bool lit);
float KeycapLabel(ImDrawList* dl, float x, float y, float height, std::string_view label, bool lit);
float KeycapWidth(float height, int scancode);
/// A mouse with one source lit (1 left, 2 middle, 3 right, 4 back, 5 forward, 6 wheel up,
/// 7 wheel down); returns its width.
float Mouse(ImDrawList* dl, float x, float y, float height, int source);
constexpr float MouseWidth(float height) {
    return height * 0.8f;
}
/// The PS4 button of a GOW3_BIND_* row as the Controller Layout screen shows it, centred at
/// (x, y); returns its width.
float PadBadge(ImDrawList* dl, float x, float y, float height, int bind);
/// A gamepad's button for a help line, as the player's pad shows it (Xbox letters, a Nintendo
/// pad's, or PlayStation shapes); returns its width.
float PadButton(ImDrawList* dl, float x, float y, float height, int bind, const Gow3GlyphState& pad);

/// The name on a key's cap: "SPACE", "CTRL", "F", "NUM 4", ...
std::string KeyLabel(int scancode);

// The games' menu colours.
constexpr ImU32 Cream = IM_COL32(236, 226, 200, 255);
constexpr ImU32 CreamDim = IM_COL32(205, 192, 164, 255);
constexpr ImU32 White = IM_COL32(255, 250, 238, 255);
constexpr ImU32 Grey = IM_COL32(150, 146, 138, 255);
constexpr ImU32 Orange = IM_COL32(240, 138, 40, 255);

} // namespace Gow3Ui
