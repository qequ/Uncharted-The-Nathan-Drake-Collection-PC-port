// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the games' look for what the runtime draws over them (gow3_ui.h).

#include "gow3_ui.h"

#include <SDL3/SDL.h>
#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "imgui_internal.h" // RegisterUserTexture

namespace Gow3Ui {

namespace {

// --- The game's archive (PSARC: big-endian table of contents, zlib blocks) --------------------

uint64_t BigEndian(const uint8_t* p, int bytes) {
    uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        value = value << 8 | p[i];
    }
    return value;
}

int Seek(std::FILE* file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, int64_t(offset), SEEK_SET);
#else
    return fseeko(file, off_t(offset), SEEK_SET);
#endif
}

struct Archive {
    std::FILE* file = nullptr;
    uint32_t block_size = 0;
    struct Entry {
        uint32_t block;
        uint64_t size, offset;
    };
    std::vector<Entry> entries;
    std::vector<uint32_t> blocks; // compressed size of each block, 0: a whole stored block
    std::vector<std::string> names; // names[i] is entries[i + 1] (entry 0 is this list)

    ~Archive() {
        if (file) {
            std::fclose(file);
        }
    }

    bool Read(const Entry& entry, std::vector<uint8_t>& out) const {
        out.clear();
        if (entry.size > (64u << 20) || Seek(file, entry.offset) != 0) {
            return false;
        }
        std::vector<uint8_t> packed;
        for (uint32_t block = entry.block; out.size() < entry.size; ++block) {
            const uint64_t want = std::min<uint64_t>(block_size, entry.size - out.size());
            const uint32_t stored = block < blocks.size() ? blocks[block] : 0;
            const uint32_t size = stored ? stored : block_size;
            packed.resize(size);
            if (std::fread(packed.data(), 1, size, file) != size) {
                return false;
            }
            const size_t at = out.size();
            out.resize(at + size_t(want));
            mz_ulong length = mz_ulong(want);
            if (size != want && packed[0] == 0x78 &&
                mz_uncompress(out.data() + at, &length, packed.data(), size) == MZ_OK && length == want) {
                continue;
            }
            std::memcpy(out.data() + at, packed.data(), size_t(std::min<uint64_t>(want, size)));
        }
        return true;
    }

    bool Open(const char* path) {
        file = std::fopen(path, "rb");
        uint8_t header[32];
        if (!file || std::fread(header, 1, 32, file) != 32 || std::memcmp(header, "PSAR", 4) != 0) {
            return false;
        }
        const uint32_t toc_length = uint32_t(BigEndian(header + 12, 4));
        const uint32_t entry_size = uint32_t(BigEndian(header + 16, 4));
        const uint32_t count = uint32_t(BigEndian(header + 20, 4));
        block_size = uint32_t(BigEndian(header + 24, 4));
        if (toc_length < 32 || toc_length > (16u << 20) || entry_size < 30 || !count || !block_size ||
            uint64_t(count) * entry_size > toc_length - 32) {
            return false;
        }
        std::vector<uint8_t> toc(toc_length - 32);
        if (std::fread(toc.data(), 1, toc.size(), file) != toc.size()) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = toc.data() + size_t(i) * entry_size;
            entries.push_back({uint32_t(BigEndian(e + 16, 4)), BigEndian(e + 20, 5), BigEndian(e + 25, 5)});
        }
        const int width = block_size <= 0x10000 ? 2 : block_size <= 0x1000000 ? 3 : 4;
        for (size_t at = size_t(count) * entry_size; at + width <= toc.size(); at += width) {
            blocks.push_back(uint32_t(BigEndian(toc.data() + at, width)));
        }
        std::vector<uint8_t> manifest;
        if (!Read(entries[0], manifest)) {
            return false;
        }
        std::string name;
        for (const uint8_t c : manifest) {
            if (c == '\n') {
                names.push_back(name);
                name.clear();
            } else if (c != '\r') {
                name += char(c);
            }
        }
        names.push_back(name);
        return true;
    }

    bool ReadFile(std::string_view wanted, std::vector<uint8_t>& out) const {
        for (size_t i = 0; i < names.size() && i + 1 < entries.size(); ++i) {
            std::string_view name = names[i];
            while (!name.empty() && name.front() == '/') {
                name.remove_prefix(1);
            }
            if (name.size() == wanted.size() &&
                std::equal(name.begin(), name.end(), wanted.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                })) {
                return Read(entries[i + 1], out);
            }
        }
        return false;
    }
};

// --- The menu font (BMFont text description, its glyphs packed in the channels of a TGA) ------

struct Glyph {
    float u0, v0, u1, v1;
    float width, height, x_offset, y_offset, advance;
};

struct Font {
    float line = 42.0f;
    std::unordered_map<uint32_t, Glyph> glyphs;
    std::unordered_map<uint64_t, float> kerning;
    std::vector<uint8_t> rgba; // the texture: the four channel planes one under the other
    int width = 0, height = 0;
};

Font font;                       // written by the loader, then read only
std::atomic<int> font_state{0};  // 0 not started, 1 loading, 2 read, 3 texture made, -1 none
ImTextureData* font_texture = nullptr;

int Value(std::string_view line, std::string_view key, std::string* text = nullptr) {
    for (size_t at = 0; (at = line.find(key, at)) != std::string_view::npos; at += key.size()) {
        if ((at && line[at - 1] != ' ') || at + key.size() >= line.size() || line[at + key.size()] != '=') {
            continue;
        }
        std::string_view value = line.substr(at + key.size() + 1);
        if (!value.empty() && value.front() == '"') {
            value.remove_prefix(1);
            value = value.substr(0, value.find('"'));
            if (text) {
                *text = std::string(value);
            }
            return 0;
        }
        return std::atoi(std::string(value.substr(0, value.find(' '))).c_str());
    }
    return 0;
}

bool ParseFont(const std::vector<uint8_t>& fnt, const std::vector<uint8_t>& tga, Font& out) {
    if (tga.size() < 18) {
        return false;
    }
    const int id_length = tga[0], type = tga[2], bits = tga[16], descriptor = tga[17];
    const int tga_width = tga[12] | tga[13] << 8, tga_height = tga[14] | tga[15] << 8;
    const size_t colour_map = tga[1] ? size_t(tga[5] | tga[6] << 8) * ((tga[7] + 7) / 8) : 0;
    const size_t pixels = 18 + id_length + colour_map;
    if (type != 2 || bits != 32 || pixels + size_t(tga_width) * tga_height * 4 > tga.size()) {
        return false;
    }
    int scale_w = 0, scale_h = 0;
    const std::string text(fnt.begin(), fnt.end());
    size_t begin = 0;
    std::vector<std::string> lines;
    while (begin < text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    for (std::string& line : lines) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("common ", 0) == 0) {
            out.line = float(Value(line, "lineHeight"));
            scale_w = Value(line, "scaleW");
            scale_h = Value(line, "scaleH");
        }
    }
    if (scale_w != tga_width || scale_h != tga_height || out.line <= 0.0f) {
        return false;
    }
    // The four planes (BMFont channels 1 blue, 2 green, 4 red, 8 alpha; TGA bytes are B G R A),
    // one under the other, as white with the glyph coverage in alpha.
    out.width = scale_w;
    out.height = scale_h * 4;
    out.rgba.assign(size_t(out.width) * out.height * 4, 255);
    const bool top_down = descriptor & 0x20;
    for (int plane = 0; plane < 4; ++plane) {
        for (int y = 0; y < scale_h; ++y) {
            const int row = top_down ? y : scale_h - 1 - y;
            const uint8_t* src = tga.data() + pixels + size_t(row) * scale_w * 4 + plane;
            uint8_t* dst = out.rgba.data() + (size_t(plane * scale_h + y) * scale_w) * 4 + 3;
            for (int x = 0; x < scale_w; ++x) {
                dst[x * 4] = src[x * 4];
            }
        }
    }
    for (const std::string& line : lines) {
        if (line.rfind("char ", 0) == 0) {
            const int channel = Value(line, "chnl");
            const int plane = channel == 1 ? 0 : channel == 2 ? 1 : channel == 4 ? 2 : 3;
            const float x = float(Value(line, "x")), y = float(Value(line, "y") + plane * scale_h);
            const float w = float(Value(line, "width")), h = float(Value(line, "height"));
            out.glyphs[uint32_t(Value(line, "id"))] = {
                x / out.width, y / out.height, (x + w) / out.width, (y + h) / out.height, w, h,
                float(Value(line, "xoffset")), float(Value(line, "yoffset")), float(Value(line, "xadvance"))};
        } else if (line.rfind("kerning ", 0) == 0) {
            const uint64_t pair = uint64_t(uint32_t(Value(line, "first"))) << 32 | uint32_t(Value(line, "second"));
            out.kerning[pair] = float(Value(line, "amount"));
        }
    }
    return out.glyphs.count('A') && out.glyphs.count(' ');
}

// The archives and font files of the three games, the running game's first.
void LoadFont() {
    struct Source {
        const char* executable;
        const char* archive;
        const char* fnt;
    };
    static const Source sources[] = {
        {"big3-ps4_Shipping.elf", "/app0/u3data/data.psarc", "data/fonts/ps4/main.fnt"},
        {"big2-ps4_Shipping.elf", "/app0/u2data/data.psarc", "data/fonts/ps4/main.fnt"},
        {"eboot.bin", "/app0/u1data/data.psarc", "art/general/textures/font/ps4/game3.fnt"},
    };
    const char* running = std::getenv("GOW3_REPORT_EXE");
    std::vector<const Source*> order;
    for (const Source& s : sources) {
        if (running && !std::strcmp(running, s.executable)) {
            order.insert(order.begin(), &s);
        } else {
            order.push_back(&s);
        }
    }
    for (const Source* source : order) {
        char path[1024];
        if (runtime_file_host_path(source->archive, path, sizeof(path)) != 0) {
            continue;
        }
        Archive archive;
        std::vector<uint8_t> fnt, tga;
        if (!archive.Open(path) || !archive.ReadFile(source->fnt, fnt)) {
            continue;
        }
        std::string page;
        const std::string text(fnt.begin(), fnt.end());
        const size_t page_line = text.find("\npage ");
        if (page_line == std::string::npos) {
            continue;
        }
        Value(std::string_view(text).substr(page_line + 1, text.find('\n', page_line + 1) - page_line - 1), "file", &page);
        const std::string fnt_name = source->fnt;
        const std::string tga_name = fnt_name.substr(0, fnt_name.rfind('/') + 1) + page;
        Font loaded;
        if (!archive.ReadFile(tga_name, tga) || !ParseFont(fnt, tga, loaded)) {
            continue;
        }
        font = std::move(loaded);
        std::printf("Overlay: the game's menu font from %s (%zu glyphs)\n", source->archive, font.glyphs.size());
        std::fflush(stdout);
        font_state.store(2, std::memory_order_release);
        return;
    }
    std::printf("Overlay: the game's menu font was not found; using the overlay font\n");
    std::fflush(stdout);
    font_state.store(-1, std::memory_order_release);
}

// UTF-8 to code points (a bad byte stands for itself).
uint32_t Next(std::string_view text, size_t& at) {
    const uint8_t c = uint8_t(text[at++]);
    const int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (!extra || at + extra > text.size()) {
        return c;
    }
    uint32_t cp = c & (0x3f >> extra);
    for (int i = 0; i < extra; ++i) {
        cp = cp << 6 | (uint8_t(text[at++]) & 0x3f);
    }
    return cp;
}

const Glyph* Find(uint32_t cp) {
    auto it = font.glyphs.find(cp);
    if (it == font.glyphs.end() && cp >= 'a' && cp <= 'z') {
        it = font.glyphs.find(cp - 32);
    }
    return it == font.glyphs.end() ? nullptr : &it->second;
}

// Dear ImGui's font stands in until (or unless) the game's is there.
constexpr float FallbackScale = 0.8f;

} // namespace

void Start() {
    int expected = 0;
    if (font_state.compare_exchange_strong(expected, 1)) {
        std::thread(LoadFont).detach();
    }
}

void Update() {
    if (font_state.load(std::memory_order_acquire) != 2) {
        return;
    }
    font_texture = IM_NEW(ImTextureData)();
    font_texture->Create(ImTextureFormat_RGBA32, font.width, font.height);
    std::memcpy(font_texture->GetPixels(), font.rgba.data(), font.rgba.size());
    ImGui::RegisterUserTexture(font_texture);
    std::vector<uint8_t>().swap(font.rgba);
    font_state.store(3, std::memory_order_release);
}

bool GameFont() {
    return font_state.load(std::memory_order_acquire) == 3;
}

float TextWidth(std::string_view text, float size) {
    if (!GameFont()) {
        return ImGui::GetFont()->CalcTextSizeA(size * FallbackScale, FLT_MAX, 0.0f, text.data(),
                                               text.data() + text.size()).x;
    }
    const float scale = size / font.line;
    float x = 0.0f;
    uint32_t previous = 0;
    for (size_t at = 0; at < text.size();) {
        const uint32_t cp = Next(text, at);
        const Glyph* g = Find(cp);
        if (!g) {
            continue;
        }
        if (previous) {
            const auto k = font.kerning.find(uint64_t(previous) << 32 | cp);
            x += k == font.kerning.end() ? 0.0f : k->second * scale;
        }
        x += g->advance * scale;
        previous = cp;
    }
    return x;
}

float Text(ImDrawList* dl, ImVec2 pos, std::string_view text, float size, ImU32 color) {
    if (!GameFont()) {
        dl->AddText(ImGui::GetFont(), size * FallbackScale, ImVec2(pos.x, pos.y + size * 0.1f), color,
                    text.data(), text.data() + text.size());
        return TextWidth(text, size);
    }
    const float scale = size / font.line;
    const ImTextureRef texture = font_texture->GetTexRef();
    float x = pos.x;
    uint32_t previous = 0;
    for (size_t at = 0; at < text.size();) {
        const uint32_t cp = Next(text, at);
        const Glyph* g = Find(cp);
        if (!g) {
            continue;
        }
        if (previous) {
            const auto k = font.kerning.find(uint64_t(previous) << 32 | cp);
            x += k == font.kerning.end() ? 0.0f : k->second * scale;
        }
        if (g->width > 0.0f && g->height > 0.0f) {
            const ImVec2 a(x + g->x_offset * scale, pos.y + g->y_offset * scale);
            dl->AddImage(texture, a, ImVec2(a.x + g->width * scale, a.y + g->height * scale),
                         ImVec2(g->u0, g->v0), ImVec2(g->u1, g->v1), color);
        }
        x += g->advance * scale;
        previous = cp;
    }
    return x - pos.x;
}

float TextFit(ImDrawList* dl, ImVec2 pos, std::string_view text, float size, float max_width, ImU32 color) {
    const float width = TextWidth(text, size);
    if (width > max_width && width > 0.0f) {
        const float smaller = size * std::max(max_width / width, 0.6f);
        return Text(dl, ImVec2(pos.x, pos.y + (size - smaller) * 0.5f), text, smaller, color);
    }
    return Text(dl, pos, text, size, color);
}

void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float scale) {
    // Opaque: the swapchain blends in linear light, where even a few percent of the game's own
    // labels under the panel stay readable.
    const float rounding = 16.0f * scale;
    dl->AddRectFilled(min, max, IM_COL32(8, 7, 6, 255), rounding);
    dl->AddRect(min, max, IM_COL32(255, 255, 255, 26), rounding, 0, 1.5f * scale);
}

void Highlight(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    const ImU32 left = IM_COL32(205, 192, 165, 120), right = IM_COL32(205, 192, 165, 0);
    dl->AddRectFilledMultiColor(min, max, left, right, right, left);
    dl->AddRectFilledMultiColor(min, ImVec2(max.x, min.y + 1.0f), IM_COL32(240, 230, 205, 150),
                                IM_COL32(240, 230, 205, 0), IM_COL32(240, 230, 205, 0),
                                IM_COL32(240, 230, 205, 150));
}

void Rule(ImDrawList* dl, ImVec2 a, ImVec2 b, float scale) {
    dl->AddLine(a, b, IM_COL32(170, 166, 158, 130), std::max(1.0f, 1.3f * scale));
}

std::string KeyLabel(int code) {
    switch (code) {
    case SDL_SCANCODE_SPACE: return "SPACE";
    case SDL_SCANCODE_LCTRL: return "CTRL";
    case SDL_SCANCODE_RCTRL: return "R CTRL";
    case SDL_SCANCODE_LSHIFT: return "SHIFT";
    case SDL_SCANCODE_RSHIFT: return "R SHIFT";
    case SDL_SCANCODE_LALT: return "ALT";
    case SDL_SCANCODE_RALT: return "ALT GR";
    case SDL_SCANCODE_ESCAPE: return "ESC";
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "ENTER";
    case SDL_SCANCODE_BACKSPACE: return "BKSP";
    case SDL_SCANCODE_TAB: return "TAB";
    case SDL_SCANCODE_CAPSLOCK: return "CAPS";
    case SDL_SCANCODE_DELETE: return "DEL";
    case SDL_SCANCODE_INSERT: return "INS";
    case SDL_SCANCODE_PAGEUP: return "PG UP";
    case SDL_SCANCODE_PAGEDOWN: return "PG DN";
    case SDL_SCANCODE_UP: return "UP";
    case SDL_SCANCODE_DOWN: return "DOWN";
    case SDL_SCANCODE_LEFT: return "LEFT";
    case SDL_SCANCODE_RIGHT: return "RIGHT";
    default: break;
    }
    const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(SDL_Scancode(code), SDL_KMOD_NONE, false));
    if (!name || !*name) {
        name = SDL_GetScancodeName(SDL_Scancode(code));
    }
    std::string label = name && *name ? name : "#" + std::to_string(code);
    if (label.rfind("Keypad ", 0) == 0) {
        label = "NUM " + label.substr(7);
    }
    for (char& ch : label) {
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    }
    return label;
}

namespace {

bool Arrow(int code) {
    return code == SDL_SCANCODE_UP || code == SDL_SCANCODE_DOWN || code == SDL_SCANCODE_LEFT ||
           code == SDL_SCANCODE_RIGHT;
}

// The cap: a dark key with a lighter face and a rim (orange while it waits for a key).
void Cap(ImDrawList* dl, ImVec2 min, ImVec2 max, float height, bool lit) {
    const float r = height * 0.16f;
    dl->AddRectFilled(ImVec2(min.x, min.y + height * 0.04f), ImVec2(max.x, max.y + height * 0.06f),
                      IM_COL32(0, 0, 0, 150), r);
    dl->AddRectFilled(min, max, IM_COL32(30, 28, 26, 255), r);
    const float inset = height * 0.09f;
    dl->AddRectFilledMultiColor(ImVec2(min.x + inset, min.y + inset * 0.7f),
                                ImVec2(max.x - inset, max.y - inset * 1.4f), IM_COL32(86, 81, 74, 255),
                                IM_COL32(86, 81, 74, 255), IM_COL32(52, 49, 45, 255), IM_COL32(52, 49, 45, 255));
    dl->AddRect(min, max, lit ? Orange : IM_COL32(176, 166, 148, 255), r, 0, std::max(1.0f, height * 0.05f));
}

} // namespace

float KeycapWidth(float height, int code) {
    if (Arrow(code)) {
        return height;
    }
    const std::string label = KeyLabel(code);
    const float text = TextWidth(label, height * 0.66f);
    return std::max(height, text + height * 0.5f);
}

float KeycapLabel(ImDrawList* dl, float x, float y, float height, std::string_view label, bool lit) {
    const float size = height * 0.66f;
    const float width = std::max(height, TextWidth(label, size) + height * 0.5f);
    const ImVec2 min(x, y - height * 0.5f), max(x + width, y + height * 0.5f);
    Cap(dl, min, max, height, lit);
    const float text = TextWidth(label, size);
    Text(dl, ImVec2(x + (width - text) * 0.5f, y - size * 0.62f), label, size, lit ? White : Cream);
    return width;
}

float Keycap(ImDrawList* dl, float x, float y, float height, int code, bool lit) {
    if (!Arrow(code)) {
        return KeycapLabel(dl, x, y, height, KeyLabel(code), lit);
    }
    const ImVec2 min(x, y - height * 0.5f), max(x + height, y + height * 0.5f);
    Cap(dl, min, max, height, lit);
    const ImVec2 c(x + height * 0.5f, y - height * 0.04f);
    const float a = height * 0.2f;
    ImVec2 p[3];
    switch (code) {
    case SDL_SCANCODE_UP: p[0] = {c.x, c.y - a}; p[1] = {c.x + a, c.y + a * 0.7f}; p[2] = {c.x - a, c.y + a * 0.7f}; break;
    case SDL_SCANCODE_DOWN: p[0] = {c.x, c.y + a}; p[1] = {c.x - a, c.y - a * 0.7f}; p[2] = {c.x + a, c.y - a * 0.7f}; break;
    case SDL_SCANCODE_LEFT: p[0] = {c.x - a, c.y}; p[1] = {c.x + a * 0.7f, c.y - a}; p[2] = {c.x + a * 0.7f, c.y + a}; break;
    default: p[0] = {c.x + a, c.y}; p[1] = {c.x - a * 0.7f, c.y + a}; p[2] = {c.x - a * 0.7f, c.y - a}; break;
    }
    dl->AddTriangleFilled(p[0], p[1], p[2], lit ? White : Cream);
    return height;
}

float Mouse(ImDrawList* dl, float x, float y, float height, int source) {
    const float w = height * 0.68f, h = height * 1.1f;
    const float left = x + (MouseWidth(height) - w) * 0.5f, top = y - h * 0.5f;
    const ImVec2 min(left, top), max(left + w, top + h);
    const float r = w * 0.5f, line = std::max(1.0f, height * 0.05f);
    const ImU32 edge = IM_COL32(210, 202, 186, 255);
    dl->AddRectFilled(min, max, IM_COL32(30, 28, 26, 255), r);
    const float split = top + h * 0.42f, mid = left + w * 0.5f;
    // The pressed button, filled orange: a quarter of the rounded top and the part under it.
    if (source == 1 || source == 3) {
        const bool l = source == 1;
        dl->PathClear();
        dl->PathLineTo(ImVec2(mid, split));
        dl->PathArcTo(ImVec2(mid, top + r), r - line * 0.5f, -IM_PI * 0.5f, l ? -IM_PI : 0.0f, 12);
        dl->PathLineTo(ImVec2(l ? left + line * 0.5f : left + w - line * 0.5f, split));
        dl->PathFillConvex(Orange);
    }
    dl->AddRect(min, max, edge, r, 0, line);
    dl->AddLine(ImVec2(left + line, split), ImVec2(left + w - line, split), edge, line);
    dl->AddLine(ImVec2(mid, top + line), ImVec2(mid, split), edge, line);
    // The wheel (middle button and wheel steps).
    const ImVec2 wheel_min(mid - w * 0.09f, top + h * 0.12f), wheel_max(mid + w * 0.09f, top + h * 0.32f);
    const bool wheel = source == 2 || source == 6 || source == 7;
    dl->AddRectFilled(wheel_min, wheel_max, wheel ? Orange : IM_COL32(30, 28, 26, 255), w * 0.09f);
    dl->AddRect(wheel_min, wheel_max, edge, w * 0.09f, 0, line);
    if (source == 6 || source == 7) {
        const float a = w * 0.16f, cy = source == 6 ? top - a * 0.9f : top + h + a * 0.9f;
        const float d = source == 6 ? -a : a;
        dl->AddTriangleFilled(ImVec2(mid, cy + d * 0.6f), ImVec2(mid - a, cy - d * 0.4f), ImVec2(mid + a, cy - d * 0.4f), Orange);
    }
    // The side buttons (4 back, 5 forward) on the left.
    if (source == 4 || source == 5) {
        const float by = source == 5 ? top + h * 0.46f : top + h * 0.66f;
        dl->AddRectFilled(ImVec2(left - w * 0.16f, by), ImVec2(left + w * 0.14f, by + h * 0.17f), Orange, w * 0.06f);
    }
    return MouseWidth(height);
}

namespace {

// PlayStation shapes in the Controller Layout screen's colours.
void FaceShape(ImDrawList* dl, ImVec2 c, float radius, int bind) {
    const float s = radius * 0.5f, line = std::max(1.2f, radius * 0.16f);
    switch (bind) {
    case GOW3_BIND_CROSS:
        dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(140, 180, 235, 255), line);
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), IM_COL32(140, 180, 235, 255), line);
        break;
    case GOW3_BIND_CIRCLE:
        dl->AddCircle(c, s * 1.05f, IM_COL32(235, 105, 105, 255), 20, line);
        break;
    case GOW3_BIND_SQUARE:
        dl->AddRect(ImVec2(c.x - s * 0.9f, c.y - s * 0.9f), ImVec2(c.x + s * 0.9f, c.y + s * 0.9f),
                    IM_COL32(225, 135, 205, 255), 0.0f, 0, line);
        break;
    default:
        dl->AddTriangle(ImVec2(c.x, c.y - s * 1.05f), ImVec2(c.x + s * 1.05f, c.y + s * 0.75f),
                        ImVec2(c.x - s * 1.05f, c.y + s * 0.75f), IM_COL32(100, 210, 160, 255), line);
        break;
    }
}

// A button's name in a capsule, at most 1.7 heights wide (the text shrinks to fit).
float Pill(ImDrawList* dl, float x, float y, float height, std::string_view label) {
    float size = height * 0.62f;
    const float max_width = height * 1.7f, text = TextWidth(label, size);
    if (text + height * 0.55f > max_width) {
        size *= (max_width - height * 0.45f) / text;
    }
    const float width = std::clamp(TextWidth(label, size) + height * 0.55f, height * 1.25f, max_width);
    const ImVec2 min(x - width * 0.5f, y - height * 0.42f), max(x + width * 0.5f, y + height * 0.42f);
    dl->AddRectFilled(min, max, IM_COL32(52, 50, 47, 255), height * 0.3f);
    dl->AddRect(min, max, IM_COL32(150, 146, 138, 255), height * 0.3f, 0, std::max(1.0f, height * 0.05f));
    Text(dl, ImVec2(x - TextWidth(label, size) * 0.5f, y - size * 0.6f), label, size, Cream);
    return width;
}

} // namespace

float PadBadge(ImDrawList* dl, float x, float y, float height, int bind) {
    const float radius = height * 0.42f;
    const ImU32 body = IM_COL32(48, 46, 43, 255), rim = IM_COL32(120, 116, 108, 255);
    switch (bind) {
    case GOW3_BIND_CROSS: case GOW3_BIND_CIRCLE: case GOW3_BIND_SQUARE: case GOW3_BIND_TRIANGLE:
        dl->AddCircleFilled(ImVec2(x, y), radius, body, 24);
        dl->AddCircle(ImVec2(x, y), radius, rim, 24, std::max(1.0f, height * 0.04f));
        FaceShape(dl, ImVec2(x, y), radius, bind);
        return radius * 2.0f;
    case GOW3_BIND_L1: return Pill(dl, x, y, height, "L1");
    case GOW3_BIND_R1: return Pill(dl, x, y, height, "R1");
    case GOW3_BIND_L2: return Pill(dl, x, y, height, "L2");
    case GOW3_BIND_R2: return Pill(dl, x, y, height, "R2");
    case GOW3_BIND_L3: return Pill(dl, x, y, height, "L3");
    case GOW3_BIND_R3: return Pill(dl, x, y, height, "R3");
    case GOW3_BIND_OPTIONS: {
        // The Options button: a small capsule with three lines.
        const float w = height * 0.5f, h = height * 0.84f, line = std::max(1.0f, height * 0.05f);
        dl->AddRectFilled(ImVec2(x - w * 0.5f, y - h * 0.5f), ImVec2(x + w * 0.5f, y + h * 0.5f), body, w * 0.5f);
        dl->AddRect(ImVec2(x - w * 0.5f, y - h * 0.5f), ImVec2(x + w * 0.5f, y + h * 0.5f), rim, w * 0.5f, 0, line);
        for (int i = -1; i <= 1; ++i) {
            const float ly = y + float(i) * h * 0.18f;
            dl->AddLine(ImVec2(x - w * 0.22f, ly), ImVec2(x + w * 0.22f, ly), Cream, line);
        }
        return w;
    }
    case GOW3_BIND_TOUCHPAD_LEFT: case GOW3_BIND_TOUCHPAD_RIGHT: {
        // The touchpad, touched on its side.
        const float w = height * 1.5f, h = height * 0.8f, line = std::max(1.0f, height * 0.05f);
        const ImVec2 min(x - w * 0.5f, y - h * 0.5f), max(x + w * 0.5f, y + h * 0.5f);
        dl->AddRectFilled(min, max, body, h * 0.22f);
        dl->AddRect(min, max, rim, h * 0.22f, 0, line);
        const float side = bind == GOW3_BIND_TOUCHPAD_LEFT ? -1.0f : 1.0f;
        dl->AddCircleFilled(ImVec2(x + side * w * 0.22f, y), h * 0.2f, Cream, 16);
        return w;
    }
    case GOW3_BIND_UP: case GOW3_BIND_DOWN: case GOW3_BIND_LEFT: case GOW3_BIND_RIGHT: {
        // A small D-pad with the arm lit.
        const float a = radius * 0.36f, l = radius;
        const ImU32 arm = IM_COL32(70, 67, 63, 255), lit = Cream;
        const ImVec2 arms[4][2] = {{{x - a, y - l}, {x + a, y - a}}, {{x - a, y + a}, {x + a, y + l}},
                                   {{x - l, y - a}, {x - a, y + a}}, {{x + a, y - a}, {x + l, y + a}}};
        dl->AddRectFilled(ImVec2(x - a, y - a), ImVec2(x + a, y + a), arm);
        for (int i = 0; i < 4; ++i) {
            dl->AddRectFilled(arms[i][0], arms[i][1], bind - GOW3_BIND_UP == i ? lit : arm, a * 0.3f);
        }
        return l * 2.0f;
    }
    default: {
        // A stick with the direction it is pushed.
        const bool right = bind >= GOW3_BIND_RS_UP;
        const int dir = (bind - GOW3_BIND_LS_UP) % 4;
        dl->AddCircleFilled(ImVec2(x, y), radius, body, 24);
        dl->AddCircle(ImVec2(x, y), radius, rim, 24, std::max(1.0f, height * 0.04f));
        const float size = height * 0.5f;
        const char* letter = right ? "R" : "L";
        Text(dl, ImVec2(x - TextWidth(letter, size) * 0.5f, y - size * 0.6f), letter, size, CreamDim);
        const float d = radius * 1.32f, a = radius * 0.3f;
        const ImVec2 dirs[4] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        const ImVec2 v = dirs[dir], tip(x + v.x * d, y + v.y * d), back(x + v.x * (d - a * 1.4f), y + v.y * (d - a * 1.4f));
        dl->AddTriangleFilled(tip, ImVec2(back.x + v.y * a, back.y - v.x * a), ImVec2(back.x - v.y * a, back.y + v.x * a), Cream);
        return radius * 2.0f;
    }
    }
}

float PadButton(ImDrawList* dl, float x, float y, float height, int bind, const Gow3GlyphState& pad) {
    if (pad.style == GOW3_GLYPHS_PLAYSTATION) {
        return PadBadge(dl, x + height * 0.5f, y, height, bind);
    }
    // Gamepad button of the PS4 button (gow3gpu.h: the GOW3_BIND_* rows up to Options are the
    // GOW3_GLYPH_* ones).
    const int button = bind <= GOW3_BIND_OPTIONS ? pad.pad[bind] : -1;
    if (button >= SDL_GAMEPAD_BUTTON_SOUTH && button <= SDL_GAMEPAD_BUTTON_NORTH) {
        const char letter[2] = {pad.face[button] ? pad.face[button] : "ABXY"[button], 0};
        const bool xbox = pad.style == GOW3_GLYPHS_XBOX;
        static const ImU32 colours[4] = {IM_COL32(118, 196, 64, 255), IM_COL32(236, 74, 60, 255),
                                         IM_COL32(64, 146, 236, 255), IM_COL32(248, 200, 52, 255)};
        const float radius = height * 0.44f;
        const ImVec2 c(x + height * 0.5f, y);
        dl->AddCircleFilled(c, radius, IM_COL32(36, 34, 32, 255), 24);
        dl->AddCircle(c, radius, xbox ? colours[button] : IM_COL32(150, 146, 138, 255), 24,
                      std::max(1.0f, height * 0.06f));
        const float size = height * 0.62f;
        Text(dl, ImVec2(c.x - TextWidth(letter, size) * 0.5f, c.y - size * 0.6f), letter, size,
             xbox ? colours[button] : Cream);
        return height;
    }
    const bool nintendo = pad.face[0] == 'B' && pad.face[1] == 'A';
    const char* label = "?";
    switch (bind) {
    case GOW3_BIND_L1: label = nintendo ? "L" : "LB"; break;
    case GOW3_BIND_R1: label = nintendo ? "R" : "RB"; break;
    case GOW3_BIND_L2: label = nintendo ? "ZL" : "LT"; break;
    case GOW3_BIND_R2: label = nintendo ? "ZR" : "RT"; break;
    case GOW3_BIND_L3: label = "LS"; break;
    case GOW3_BIND_R3: label = "RS"; break;
    case GOW3_BIND_OPTIONS: label = nintendo ? "+" : "MENU"; break;
    default: return PadBadge(dl, x + height * 0.5f, y, height, bind);
    }
    const float width = std::max(height * 1.25f, TextWidth(label, height * 0.62f) + height * 0.55f);
    Pill(dl, x + width * 0.5f, y, height, label);
    return width;
}

} // namespace Gow3Ui
