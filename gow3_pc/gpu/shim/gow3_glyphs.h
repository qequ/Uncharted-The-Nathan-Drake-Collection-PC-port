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

#include <cstdint>
#include <memory>
#include <vector>

namespace Gow3Glyphs {

/// Which part of the icon font a texture holds.
enum Role : uint8_t { RoleNone = 0, RoleRgba, RoleColor, RoleAlpha };

/// A texture of the icon font's size and layout: 512x512 RGBA8, one level, 1D tiled.
bool Candidate(uint32_t width, uint32_t height, bool rgba8, uint32_t levels, bool thin_1d);
/// The role of a candidate texture's guest data (by its content), RoleNone for any other.
Role Identify(const uint8_t* guest, uint32_t size);
/// Changes whenever the icons to show change.
uint32_t Generation();
/// Linear RGBA8 pixels to upload for the role, or null for the game's own icons.
std::shared_ptr<const std::vector<uint8_t>> Pixels(Role role);

} // namespace Gow3Glyphs
