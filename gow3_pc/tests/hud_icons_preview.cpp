// SPDX-License-Identifier: GPL-2.0-or-later
// The HUD button pictures as the games will show them (gpu/shim/gow3_glyphs.h): for every picture
// that gets replacements, the game's own and those drawn for the keyboard and mouse (the default
// keys) and for an Xbox pad, each through BC1 and back, with the game's mask as alpha, as PNG
// files <game>-<picture>-<style>.png. tools/hud_icons_sheet.py lays them out side by side.
//   hud-icons-preview <game folder> <output folder>

#include "gow3_bc.h"
#include "gow3_glyphs.h"
#include "gow3_pak.h"

#include <SDL3/SDL.h>
#include <miniz.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// The glyph code's fallback font (gow3_overlay.cpp embeds it in the game).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
#else
asm(".section .rodata\n"
#endif
    ".balign 16\n"
    ".global gow3_font_ttf\n"
    "gow3_font_ttf:\n"
    ".incbin \"" GOW3_FONT_PATH "\"\n"
    ".global gow3_font_ttf_end\n"
    "gow3_font_ttf_end:\n"
    ".text\n");

// The game's files are opened here by their host paths.
extern "C" int runtime_file_host_path(const char*, char*, size_t) {
    return 2;
}

namespace {

namespace fs = std::filesystem;

// The mask the HUD draws a picture with (the soft glow for the face buttons).
std::string MaskFor(const std::string& name) {
    if (name == "up-button-look-color") return "up-button-look-trans";
    if (name == "up-button") return "up-button-up-trans";
    if (name == "bp-up-button") return "bp-up-button-up-trans";
    if (name == "l1-button" || name == "r1-button") return "button-trans-shoulder1";
    if (name == "l2-button" || name == "r2-button") return "button-trans-shoulder2";
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "-up") == 0) return "button-up-trans-round";
    if (name.size() > 5 && name.compare(name.size() - 5, 5, "-down") == 0) return "button-down-trans-round";
    return "button-trans";
}

// Level 0 as shown (rows top to bottom), RGBA8.
std::vector<uint8_t> Shown(const std::string& pak, const Gow3Pak::Texture& t) {
    std::vector<uint8_t> stored;
    if (!Gow3Pak::ReadLevel0(pak, t, stored)) return {};
    const auto rgba = Gow3Bc::DecodeThin1d(stored.data(), t.width, t.height,
                                           t.dxgi == 80 ? Gow3Bc::Format::Bc4 : Gow3Bc::Format::Bc1);
    std::vector<uint8_t> out(rgba.size());
    const size_t row = size_t(t.width) * 4;
    for (uint32_t y = 0; y < t.height; ++y) std::memcpy(&out[y * row], &rgba[(t.height - 1 - y) * row], row);
    return out;
}

// Level 0 of an upload (linear BC1 blocks, bottom row first) as shown.
std::vector<uint8_t> Decoded(const Gow3Glyphs::Upload& upload) {
    const auto& level = upload.levels.at(0);
    const uint32_t w = level.width, h = level.height, blocks_w = (w + 3) / 4;
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t by = 0; by < (h + 3) / 4; ++by) {
        for (uint32_t bx = 0; bx < blocks_w; ++bx) {
            uint8_t texels[64];
            Gow3Bc::DecodeBc1(&upload.data[level.offset + (size_t(by) * blocks_w + bx) * 8], texels);
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t sx = bx * 4 + x, sy = by * 4 + y;
                    if (sx < w && sy < h) std::memcpy(&out[(size_t(h - 1 - sy) * w + sx) * 4], &texels[(y * 4 + x) * 4], 4);
                }
            }
        }
    }
    return out;
}

bool WritePng(const fs::path& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), int(w), int(h), 4, &size, 6, MZ_FALSE);
    if (!png) return false;
    FILE* file = std::fopen(path.string().c_str(), "wb");
    const bool ok = file && std::fwrite(png, 1, size, file) == size;
    if (file) std::fclose(file);
    mz_free(png);
    return ok;
}

Gow3GlyphState Keyboard() {
    Gow3GlyphState s{};
    s.style = GOW3_GLYPHS_KEYBOARD;
    s.keyboard_last = 1;
    for (int& pad : s.pad) pad = -1;
    // src/runtime_pad.c's default layout.
    s.key[GOW3_GLYPH_CROSS] = SDL_SCANCODE_SPACE;
    s.key[GOW3_GLYPH_CIRCLE] = SDL_SCANCODE_LCTRL;
    s.key[GOW3_GLYPH_SQUARE] = SDL_SCANCODE_F;
    s.key[GOW3_GLYPH_TRIANGLE] = SDL_SCANCODE_R;
    s.key[GOW3_GLYPH_L1] = SDL_SCANCODE_Q;
    s.key[GOW3_GLYPH_R1] = SDL_SCANCODE_E;
    s.mouse[GOW3_GLYPH_L1] = 4;
    s.mouse[GOW3_GLYPH_R1] = 5;
    s.mouse[GOW3_GLYPH_L2] = 3;
    s.mouse[GOW3_GLYPH_R2] = 1;
    s.key[GOW3_GLYPH_UP] = SDL_SCANCODE_3;
    s.key[GOW3_GLYPH_DOWN] = SDL_SCANCODE_4;
    s.key[GOW3_GLYPH_LEFT] = SDL_SCANCODE_1;
    s.key[GOW3_GLYPH_RIGHT] = SDL_SCANCODE_2;
    return s;
}

Gow3GlyphState Xbox() {
    Gow3GlyphState s{};
    s.style = GOW3_GLYPHS_XBOX;
    for (int& pad : s.pad) pad = -1;
    s.pad[GOW3_GLYPH_CROSS] = SDL_GAMEPAD_BUTTON_SOUTH;
    s.pad[GOW3_GLYPH_CIRCLE] = SDL_GAMEPAD_BUTTON_EAST;
    s.pad[GOW3_GLYPH_SQUARE] = SDL_GAMEPAD_BUTTON_WEST;
    s.pad[GOW3_GLYPH_TRIANGLE] = SDL_GAMEPAD_BUTTON_NORTH;
    s.pad[GOW3_GLYPH_L1] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    s.pad[GOW3_GLYPH_R1] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    s.pad[GOW3_GLYPH_L2] = GOW3_GLYPH_PAD_LEFT_TRIGGER;
    s.pad[GOW3_GLYPH_R2] = GOW3_GLYPH_PAD_RIGHT_TRIGGER;
    s.pad[GOW3_GLYPH_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP;
    std::memcpy(s.face, "ABXY", 4);
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <game folder> <output folder>\n", argv[0]);
        return 2;
    }
    const fs::path game = argv[1], out = argv[2];
    fs::create_directories(out);
    struct Game {
        const char* tag;
        const char* data;
        const char* pak;
    };
    const Game games[] = {{"u1", "u1data", "particle.pak"}, {"u2", "u2data", "hud.pak"}, {"u3", "u3data", "hud.pak"}};
    const Gow3GlyphState keyboard = Keyboard(), xbox = Xbox();
    int written = 0;
    for (const Game& g : games) {
        std::string pak;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(game / g.data / "build" / "main", error)) {
            const fs::path candidate = entry.path() / "ps4" / g.pak;
            if (entry.path().filename().string().rfind("pak", 0) == 0 && fs::exists(candidate, error)) {
                pak = candidate.string();
            }
        }
        std::string why;
        const std::vector<Gow3Pak::Texture> textures = pak.empty() ? std::vector<Gow3Pak::Texture>{} : Gow3Pak::ReadButtons(pak, &why);
        std::printf("%s: %zu button textures in %s %s\n", g.tag, textures.size(), pak.c_str(), why.c_str());
        for (const Gow3Pak::Texture& t : textures) {
            if (!Gow3Glyphs::HudPictureKnown(t.name) || t.dxgi == 80) continue;
            const std::vector<uint8_t> original = Shown(pak, t);
            std::vector<uint8_t> mask;
            for (const Gow3Pak::Texture& m : textures) {
                if (m.name == MaskFor(t.name) && m.width == t.width && m.height == t.height) mask = Shown(pak, m);
            }
            struct Style {
                const char* name;
                const Gow3GlyphState* state;
            };
            for (const Style style : {Style{"ps", nullptr}, Style{"keyboard", &keyboard}, Style{"xbox", &xbox}}) {
                std::vector<uint8_t> shown = original;
                if (style.state) {
                    const auto drawn = Gow3Glyphs::DrawHudPicture(t.name, t.width, t.height, original, *style.state);
                    if (!drawn.empty()) shown = Decoded(*Gow3Glyphs::EncodeHudPicture(drawn, t.width, t.height));
                }
                for (size_t i = 0; i < size_t(t.width) * t.height; ++i) shown[i * 4 + 3] = mask.empty() ? 255 : mask[i * 4];
                const fs::path file = out / (std::string(g.tag) + "-" + t.name + "-" + style.name + ".png");
                if (!WritePng(file, shown, t.width, t.height)) {
                    std::fprintf(stderr, "cannot write %s\n", file.string().c_str());
                    return 1;
                }
                ++written;
            }
            std::printf("  %s %ux%u %s mask=%s\n", t.name.c_str(), t.width, t.height, t.dxgi == 72 ? "BC1 sRGB" : "BC1",
                        mask.empty() ? "none" : MaskFor(t.name).c_str());
        }
    }
    std::printf("%d pictures written to %s\n", written, out.string().c_str());
    return written ? 0 : 1;
}
