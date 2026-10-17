// SPDX-License-Identifier: GPL-2.0-or-later
// The HUD button pictures (gpu/shim/gow3_glyphs.h): BC1 and BC4 blocks, the 1D-thin block order,
// a pak's VRAM_DESC records, and a face button's replacements.

#include "gow3_bc.h"
#include "gow3_glyphs.h"
#include "gow3_pak.h"

#include <SDL3/SDL.h>
#include <xxhash.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

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

extern "C" int runtime_file_host_path(const char*, char*, size_t) {
    return 2;
}

namespace {

int MaxError(const uint8_t* a, const uint8_t* b, int texels, int channels, int stride) {
    int worst = 0;
    for (int i = 0; i < texels; ++i) {
        for (int k = 0; k < channels; ++k) worst = std::max(worst, std::abs(int(a[i * stride + k]) - int(b[i * stride + k])));
    }
    return worst;
}

void Bc1() {
    uint8_t in[64], block[8], out[64];
    // One colour: exact up to 5:6:5.
    for (int i = 0; i < 16; ++i) std::memcpy(in + i * 4, "\x37\xd8\x68\xff", 4);
    Gow3Bc::EncodeBc1(in, block);
    Gow3Bc::DecodeBc1(block, out);
    assert(MaxError(in, out, 16, 3, 4) <= 4);
    // Two colours (text on a key): exact up to 5:6:5.
    for (int i = 0; i < 16; ++i) std::memcpy(in + i * 4, i % 3 ? "\xf7\xf1\xe4\xff" : "\x16\x14\x12\xff", 4);
    Gow3Bc::EncodeBc1(in, block);
    Gow3Bc::DecodeBc1(block, out);
    assert(MaxError(in, out, 16, 3, 4) <= 4);
    // A ramp: four steps of the palette.
    for (int i = 0; i < 16; ++i) {
        in[i * 4] = uint8_t(i * 16), in[i * 4 + 1] = uint8_t(255 - i * 8), in[i * 4 + 2] = 40, in[i * 4 + 3] = 255;
    }
    Gow3Bc::EncodeBc1(in, block);
    Gow3Bc::DecodeBc1(block, out);
    assert(MaxError(in, out, 16, 3, 4) <= 44);
    // Opaque 4-colour mode: the first end above the second.
    assert((block[0] | block[1] << 8) > (block[2] | block[3] << 8));
}

void Bc4() {
    uint8_t in[16], block[8], out[16];
    for (int i = 0; i < 16; ++i) in[i] = uint8_t(i * 17);
    Gow3Bc::EncodeBc4(in, block);
    Gow3Bc::DecodeBc4(block, out);
    assert(MaxError(in, out, 16, 1, 1) <= 19);
    std::memset(in, 200, sizeof(in));
    Gow3Bc::EncodeBc4(in, block);
    Gow3Bc::DecodeBc4(block, out);
    assert(MaxError(in, out, 16, 1, 1) == 0);
}

void Thin1d() {
    std::set<uint32_t> seen;
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) seen.insert(Gow3Bc::Thin1dBlock(x, y, 8));
    }
    assert(seen.size() == 64 && *seen.rbegin() == 63);
    assert(Gow3Bc::Thin1dBlock(1, 0, 16) == 1 && Gow3Bc::Thin1dBlock(0, 1, 16) == 2);
    assert(Gow3Bc::Thin1dBlock(8, 0, 16) == 64 && Gow3Bc::Thin1dBlock(0, 8, 16) == 128);
    assert(Gow3Bc::Thin1dLevelBytes(256, 256) == 32768 && Gow3Bc::Thin1dLevelBytes(16, 16) == 512);
    assert(Gow3Pak::Level0Bytes(512, 512) == 131072);
}

void Put32(std::vector<char>& data, size_t at, uint32_t value) {
    std::memcpy(&data[at], &value, 4);
}

void Pak() {
    // Records first, the GPU part last; one texture record, as the games' paks have them.
    const uint32_t level0 = Gow3Pak::Level0Bytes(256, 256), size = 0xb400, offset = 0x400;
    const std::string cooked = "d:/dev/build/texture2/art/general/hud/buttons/Triangle-Button-Color.tga/A96A.ndb";
    std::vector<char> records(0x200);
    Put32(records, 0, 0xA79);
    const size_t item = 0x80;
    Put32(records, item + 20, 0x12345);
    const uint32_t fields[18] = {0, 0, 0, 0, offset, 0, size, 0x1234, 1, 71, 0x01000000, 9, 256, 256, 0, 3, 0x100, 0};
    std::memcpy(&records[item + 24], fields, sizeof(fields));
    const size_t text = item + 24 + sizeof(fields);
    std::memcpy(&records[text], cooked.c_str(), cooked.size() + 1);
    std::memcpy(&records[text + cooked.size() + 1], "VRAM_DESC", 10);
    std::vector<char> gpu(offset + size);
    for (size_t i = 0; i < gpu.size(); ++i) gpu[i] = char(i * 7 + i / 251);
    Put32(records, 0x20, uint32_t(gpu.size()));
    const auto path = std::filesystem::temp_directory_path() / "gow3-hud-test.pak";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(records.data(), records.size());
        file.write(gpu.data(), gpu.size());
    }
    std::string error;
    const auto textures = Gow3Pak::ReadButtons(path.string(), &error);
    assert(textures.size() == 1);
    const Gow3Pak::Texture& t = textures[0];
    assert(t.name == "triangle-button-color" && t.width == 256 && t.height == 256 && t.levels == 9);
    assert(t.dxgi == 71 && t.size == size && t.offset == records.size() + offset);
    assert(t.hash == XXH3_64bits(gpu.data() + offset, level0));
    std::filesystem::remove(path);
    assert(Gow3Pak::ReadButtons("no such file.pak", &error).empty() && !error.empty());
}

// A face button as the games draw it: a dark disc on green, a green triangle on it.
std::vector<uint8_t> Triangle() {
    std::vector<uint8_t> rgba(256 * 256 * 4);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            uint8_t* p = &rgba[(y * 256 + x) * 4];
            const float r = std::hypot(x + 0.5f - 128.5f, y + 0.5f - 128.5f);
            const bool line = std::fabs(y - 150.0f) < 4 && std::fabs(x - 128.0f) < 40;
            const uint8_t* c = r > 70 ? (const uint8_t*)"\x37\xd8\x68" : line ? (const uint8_t*)"\x3c\xc8\x64" : (const uint8_t*)"\x46\x46\x46";
            std::memcpy(p, c, 3);
            p[3] = 255;
        }
    }
    return rgba;
}

void Face() {
    const std::vector<uint8_t> original = Triangle();
    Gow3GlyphState s{};
    for (int& pad : s.pad) pad = -1;
    s.style = GOW3_GLYPHS_PLAYSTATION;
    assert(Gow3Glyphs::DrawHudPicture("triangle-button-color", 256, 256, original, s).empty());
    s.style = GOW3_GLYPHS_KEYBOARD;
    assert(Gow3Glyphs::DrawHudPicture("triangle-button-color", 256, 256, original, s).empty()); // nothing bound
    s.key[GOW3_GLYPH_TRIANGLE] = SDL_SCANCODE_R;
    const auto keyboard = Gow3Glyphs::DrawHudPicture("triangle-button-color", 256, 256, original, s);
    assert(keyboard.size() == original.size());
    assert(!std::memcmp(&keyboard[0], &original[0], 4));                                  // the glow stays green
    assert(std::memcmp(&keyboard[(128 * 256 + 128) * 4], &original[(128 * 256 + 128) * 4], 3)); // a key on top
    // The symbol's line, outside the key, is painted out: grey like the disc around it.
    const uint8_t* painted = &keyboard[(150 * 256 + 92) * 4];
    assert(std::abs(int(painted[1]) - int(painted[0])) < 24);
    s.style = GOW3_GLYPHS_XBOX;
    s.pad[GOW3_GLYPH_TRIANGLE] = SDL_GAMEPAD_BUTTON_NORTH;
    std::memcpy(s.face, "ABXY", 4);
    const auto xbox = Gow3Glyphs::DrawHudPicture("triangle-button-color", 256, 256, original, s);
    assert(xbox.size() == original.size() && xbox[0] > 200 && xbox[1] > 150 && xbox[2] < 100); // Y's yellow glow
    // Levels down to 1x1, BC1 rows of blocks.
    const auto upload = Gow3Glyphs::EncodeHudPicture(keyboard, 256, 256);
    assert(upload->levels.size() == 9 && upload->levels[8].width == 1 && upload->data.size() == 43704);
    assert(!Gow3Glyphs::HudPictureKnown("button-trans") && Gow3Glyphs::HudPictureKnown("l2-button"));
}

} // namespace

int main() {
    Bc1();
    Bc4();
    Thin1d();
    Pak();
    Face();
    std::puts("HUD icon tests passed");
    return 0;
}
