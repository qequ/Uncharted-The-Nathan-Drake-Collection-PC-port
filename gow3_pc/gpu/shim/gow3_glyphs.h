// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// gow3: the button icons in the games' text follow what the player plays with.
//
// The three Uncharted games draw PS4 buttons inside text ("Press |S| to attack") with the
// symbol3 bitmap font, a 512x512 atlas loaded from data.psarc (Uncharted 3 keeps its colour and
// its alpha in two textures). The texture cache recognises those textures by their content when
// it uploads them (Identify) and uploads the icons drawn here instead (Pixels): the keys and
// mouse buttons as bound, or an Xbox pad's buttons. src/runtime_pad.c says what the player last
// used (gow3gpu_glyphs_set in gow3gpu.h); a worker thread redraws the atlas, and Generation()
// changes so that the texture cache uploads it again.
//
// The prompts in the world (pick up, climb, mash a button) are pictures of their own, one per
// button, in each game's HUD pak (gow3_pak.h): BC1 compressed, with mip levels. They are found by
// name in the running game's pak and recognised the same way; their replacements are the game's
// pictures with the button's symbol painted out and the key, mouse button or Xbox letter drawn in
// its place.
//
// The same way, the picture of the games' Controller Layout screen (a DualShock 4 with its
// buttons' actions, Options > Controls) is recognised (RoleLayout); the rasterizer reports each
// draw that samples it (LayoutSeen), and the keyboard page (gow3_controls.h) covers that screen
// while the player uses the keyboard.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../gow3gpu.h"

namespace Gow3Glyphs {

/// Which part of the icon font a texture holds, the Controller Layout picture, or, from RoleHud
/// on, one of the running game's HUD button pictures (RoleHud + its index).
enum Role : uint8_t { RoleNone = 0, RoleRgba, RoleColor, RoleAlpha, RoleLayout, RoleHud = 16 };

/// The texel formats of the textures above.
enum class Format : uint8_t { Other, Rgba8, Bc1 };

/// A texture of the icon font's size and layout (512x512 RGBA8, one level, 1D tiled), of the
/// Controller Layout picture's (2048x1024 RGBA8, 12 levels, 1D tiled), or of a HUD button
/// picture's (BC1, 1D tiled, 128 to 512 texels).
bool Candidate(uint32_t width, uint32_t height, Format format, uint32_t levels, bool thin_1d);
/// The role of a candidate texture's guest data (by its content), RoleNone for any other.
Role Identify(const uint8_t* guest, uint32_t size, uint32_t width, uint32_t height, Format format);
/// Changes whenever the icons to show change.
uint32_t Generation();

/// What to upload in place of a texture: its levels from level 0, in rows (BC1: rows of blocks).
struct Upload {
    struct Level {
        uint32_t offset, width, height;
    };
    std::vector<uint8_t> data;
    std::vector<Level> levels;
};
/// The data to upload for the role, or null for the game's own texture.
std::shared_ptr<const Upload> Pixels(Role role);

/// A draw sampled the Controller Layout picture (any thread).
void LayoutSeen();
/// Milliseconds since a draw last sampled it (a large value when never).
int64_t LayoutAge();
/// What src/runtime_pad.c last sent: the device used last, the icon style, the bound keys.
Gow3GlyphState State();

/// Whether HUD button pictures of this name (as in the pak) get replacements.
bool HudPictureKnown(const std::string& name);
/// The replacement of a HUD button picture for the icons in `state`: RGBA8 of level 0 with its
/// rows top to bottom as shown (`original`, the game's picture, the same way up), or empty when
/// the game's own picture stays.
std::vector<uint8_t> DrawHudPicture(const std::string& name, uint32_t width, uint32_t height,
                                    const std::vector<uint8_t>& original, const Gow3GlyphState& state);
/// A picture's levels down to 1x1 as the games keep them: bottom row first, BC1.
std::shared_ptr<const Upload> EncodeHudPicture(const std::vector<uint8_t>& shown, uint32_t width,
                                               uint32_t height);

} // namespace Gow3Glyphs
