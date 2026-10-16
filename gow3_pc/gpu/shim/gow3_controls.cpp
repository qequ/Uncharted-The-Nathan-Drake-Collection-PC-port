// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the keyboard and mouse page on the Controller Layout screen (gow3_controls.h).

#include "gow3_controls.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "gow3_glyphs.h"
#include "gow3_ui.h"
#include "imgui.h"

namespace Gow3Controls {

namespace {

using namespace Gow3Ui;

enum Kind { KindBind, KindSensitivity, KindInvert, KindIcons, KindReset };

struct Row {
    const char* section; // starts a group with this heading ("": a gap), or null
    const char* label;
    Kind kind;
    int bind;
};

// The actions as the Controller Layout screen names them (Drake's Fortune's R3 differs).
constexpr Row left_rows[] = {
    {"Walk / Run", "Forward", KindBind, GOW3_BIND_LS_UP},
    {nullptr, "Back", KindBind, GOW3_BIND_LS_DOWN},
    {nullptr, "Left", KindBind, GOW3_BIND_LS_LEFT},
    {nullptr, "Right", KindBind, GOW3_BIND_LS_RIGHT},
    {"Camera", "Look Up", KindBind, GOW3_BIND_RS_UP},
    {nullptr, "Look Down", KindBind, GOW3_BIND_RS_DOWN},
    {nullptr, "Look Left", KindBind, GOW3_BIND_RS_LEFT},
    {nullptr, "Look Right", KindBind, GOW3_BIND_RS_RIGHT},
    {nullptr, "Mouse Sensitivity", KindSensitivity, -1},
    {nullptr, "Mouse Vertical", KindInvert, -1},
    {"Directional Buttons", "Look", KindBind, GOW3_BIND_UP},
    {nullptr, "Primary Weapon", KindBind, GOW3_BIND_RIGHT},
    {nullptr, "Sidearm", KindBind, GOW3_BIND_LEFT},
    {nullptr, "Photo Mode", KindBind, GOW3_BIND_DOWN},
};
constexpr Row right_rows[] = {
    {"Actions", "Jump / Climb", KindBind, GOW3_BIND_CROSS},
    {nullptr, "Take Cover / Roll / Drop Down", KindBind, GOW3_BIND_CIRCLE},
    {nullptr, "Melee Combat", KindBind, GOW3_BIND_SQUARE},
    {nullptr, "Interact / Pick Up", KindBind, GOW3_BIND_TRIANGLE},
    {nullptr, "Aim Weapon", KindBind, GOW3_BIND_L2},
    {nullptr, "Fire Weapon", KindBind, GOW3_BIND_R2},
    {nullptr, "Grenade / Hold to Aim", KindBind, GOW3_BIND_L1},
    {nullptr, "Reload", KindBind, GOW3_BIND_R1},
    {nullptr, "Switch Shoulders While Aiming", KindBind, GOW3_BIND_L3},
    {nullptr, "Zoom", KindBind, GOW3_BIND_R3},
    {nullptr, "Journal", KindBind, GOW3_BIND_TOUCHPAD_LEFT},
    {nullptr, "Pause", KindBind, GOW3_BIND_OPTIONS},
    {"", "Button Icons", KindIcons, -1},
    {nullptr, "Reset Defaults", KindReset, -1},
};
struct Column {
    const Row* rows;
    int count;
};
constexpr Column columns[2] = {{left_rows, int(std::size(left_rows))}, {right_rows, int(std::size(right_rows))}};
constexpr int MaxRows = 16;

constexpr float sensitivities[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f};
constexpr const char* icon_names[] = {"Automatic", "Keyboard", "Xbox", "PlayStation"};

std::atomic<bool> active{false};
bool suppressed = false; // closed by the player: stays closed until the screen is gone
Gow3Bindings bindings{}, defaults{};
int column = 1, selected[2] = {0, 0};
bool capturing = false, confirm_reset = false;
std::string message;
long long message_until = 0;
struct Rect {
    float x0, y0, x1, y1;
};
Rect rects[2][MaxRows]{}; // the rows as last drawn (mouse)

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int Game() {
    static const int game = [] {
        const char* exe = std::getenv("GOW3_REPORT_EXE");
        return exe && !std::strcmp(exe, "eboot.bin") ? 1 : exe && std::strstr(exe, "big2") ? 2 : 3;
    }();
    return game;
}

const char* Label(const Row& row) {
    return row.bind == GOW3_BIND_R3 && Game() == 1 ? "Reset Camera" : row.label;
}

const Row& Selected() {
    return columns[column].rows[selected[column]];
}

// The row of a binding as the page names it, or null for one it does not show (the right side
// of the touchpad).
const Row* RowOf(int bind) {
    for (const Column& c : columns) {
        for (int i = 0; i < c.count; ++i) {
            if (c.rows[i].kind == KindBind && c.rows[i].bind == bind) {
                return &c.rows[i];
            }
        }
    }
    return nullptr;
}

void Say(const std::string& text, int seconds = 4) {
    message = text;
    message_until = NowMs() + seconds * 1000LL;
}

void Apply() {
    runtime_pad_set_bindings(&bindings);
}

bool Bound(int code, int bind) {
    return code > 0 && (bindings.key[bind][0] == code || bindings.key[bind][1] == code);
}

// Keys the runtime keeps: Insert opens the settings menu, F8 marks a moment for a play-test
// report (GOW3_MARK_KEY), F9 records the pad.
const char* Reserved(int code) {
    static const int mark = [] {
        const char* name = std::getenv("GOW3_MARK_KEY");
        const int code = name && *name ? int(SDL_GetScancodeFromName(name)) : int(SDL_SCANCODE_F8);
        return code > 0 ? code : int(SDL_SCANCODE_F8);
    }();
    if (code == SDL_SCANCODE_INSERT) {
        return "opens the settings menu";
    }
    if (code == mark) {
        return "marks a moment for a play-test report";
    }
    if (code == SDL_SCANCODE_F9) {
        return "records the controls for tests";
    }
    return nullptr;
}

// A key for an action: taken from any other action, which gets this one's old key instead.
void AssignKey(int bind, int code) {
    const int previous = bindings.key[bind][0];
    std::string note;
    for (int other = 0; other < GOW3_BIND_COUNT; ++other) {
        if (other == bind || !Bound(code, other)) {
            continue;
        }
        for (int& key : bindings.key[other]) {
            if (key == code) {
                key = 0;
            }
        }
        if (!bindings.key[other][0]) {
            std::swap(bindings.key[other][0], bindings.key[other][1]);
        }
        const Row* row = RowOf(other);
        if (!row) {
            continue;
        }
        if (!bindings.key[other][0] && previous && previous != code) {
            bindings.key[other][0] = previous;
            note = std::string(Label(*row)) + " now uses " + KeyLabel(previous);
        } else if (!bindings.key[other][0] && !bindings.mouse[other]) {
            note = std::string(Label(*row)) + " has no key now";
        }
    }
    bindings.key[bind][0] = code;
    bindings.key[bind][1] = 0;
    Apply();
    if (!note.empty()) {
        Say(note);
    }
}

void AssignMouse(int bind, int source) {
    if (bind >= GOW3_BIND_LS_UP) {
        Say("Walking and the camera take keys (the mouse turns the camera)");
        return;
    }
    const int previous = bindings.mouse[bind];
    std::string note;
    for (int other = 0; other < GOW3_BIND_LS_UP; ++other) {
        if (other == bind || bindings.mouse[other] != source) {
            continue;
        }
        static const char* const names[] = {"", "the left mouse button", "the middle mouse button",
                                            "the right mouse button", "mouse button 4", "mouse button 5",
                                            "the wheel up", "the wheel down"};
        const Row* row = RowOf(other);
        bindings.mouse[other] = row && previous ? previous : 0;
        if (row) {
            note = std::string(Label(*row)) +
                   (previous ? std::string(" now uses ") + names[std::clamp(previous, 0, 7)] : " has no mouse button now");
        }
    }
    bindings.mouse[bind] = source;
    Apply();
    if (!note.empty()) {
        Say(note);
    }
}

void Close(int press) {
    active.store(false, std::memory_order_release);
    capturing = confirm_reset = false;
    suppressed = true;
    if (press >= 0) {
        runtime_pad_press(press); // the game leaves the Controller Layout screen (or resumes)
    }
}

void Adjust(int step) {
    switch (Selected().kind) {
    case KindSensitivity: {
        int index = 0;
        for (int i = 0; i < int(std::size(sensitivities)); ++i) {
            if (std::fabs(sensitivities[i] - bindings.mouse_sensitivity) <
                std::fabs(sensitivities[index] - bindings.mouse_sensitivity)) {
                index = i;
            }
        }
        index = std::clamp(index + step, 0, int(std::size(sensitivities)) - 1);
        bindings.mouse_sensitivity = sensitivities[index];
        Apply();
        break;
    }
    case KindInvert:
        bindings.mouse_invert = !bindings.mouse_invert;
        Apply();
        break;
    case KindIcons:
        bindings.glyphs = (bindings.glyphs + step + 4) % 4;
        Apply();
        break;
    default:
        break;
    }
}

void Activate() {
    const Row& row = Selected();
    switch (row.kind) {
    case KindBind:
        capturing = true;
        break;
    case KindSensitivity:
        if (bindings.mouse_sensitivity >= sensitivities[std::size(sensitivities) - 1]) {
            bindings.mouse_sensitivity = sensitivities[0];
            Apply();
        } else {
            Adjust(1);
        }
        break;
    case KindReset:
        if (!confirm_reset) {
            confirm_reset = true;
            Say("Press again to restore the default keys and mouse", 6);
        } else {
            bindings = defaults;
            Apply();
            confirm_reset = false;
            Say("The default keys and mouse are back");
        }
        break;
    default:
        Adjust(1);
        break;
    }
}

void Move(int step) {
    const int count = columns[column].count;
    selected[column] = (selected[column] + step + count) % count;
    confirm_reset = false;
}

// Left / right: a value row changes its value; any other row goes to the other column, to the
// row beside it.
void Side(int step) {
    const Kind kind = Selected().kind;
    if (kind == KindSensitivity || kind == KindInvert || kind == KindIcons) {
        Adjust(step);
        return;
    }
    const Rect& from = rects[column][selected[column]];
    const float y = (from.y0 + from.y1) * 0.5f;
    column = 1 - column;
    int best = 0;
    for (int i = 0; i < columns[column].count; ++i) {
        const Rect& r = rects[column][i];
        const Rect& b = rects[column][best];
        if (std::fabs((r.y0 + r.y1) * 0.5f - y) < std::fabs((b.y0 + b.y1) * 0.5f - y)) {
            best = i;
        }
    }
    selected[column] = best;
    confirm_reset = false;
}

bool Hit(float x, float y, int& c, int& i) {
    for (c = 0; c < 2; ++c) {
        for (i = 0; i < columns[c].count; ++i) {
            const Rect& r = rects[c][i];
            if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
                return true;
            }
        }
    }
    return false;
}

void OnKey(int code, bool repeat) {
    if (capturing) {
        if (repeat) {
            return;
        }
        capturing = false;
        if (code == SDL_SCANCODE_ESCAPE) {
            return;
        }
        if (const char* why = Reserved(code)) {
            Say(KeyLabel(code) + " " + why + ": choose another key");
            return;
        }
        AssignKey(Selected().bind, code);
        return;
    }
    const bool up = code == SDL_SCANCODE_UP || code == SDL_SCANCODE_W || Bound(code, GOW3_BIND_LS_UP) ||
                    Bound(code, GOW3_BIND_UP);
    const bool down = code == SDL_SCANCODE_DOWN || code == SDL_SCANCODE_S || Bound(code, GOW3_BIND_LS_DOWN) ||
                      Bound(code, GOW3_BIND_DOWN);
    const bool left = code == SDL_SCANCODE_LEFT || code == SDL_SCANCODE_A || Bound(code, GOW3_BIND_LS_LEFT) ||
                      Bound(code, GOW3_BIND_LEFT);
    const bool right = code == SDL_SCANCODE_RIGHT || code == SDL_SCANCODE_D ||
                       Bound(code, GOW3_BIND_LS_RIGHT) || Bound(code, GOW3_BIND_RIGHT);
    if (code == SDL_SCANCODE_ESCAPE || (!repeat && Bound(code, GOW3_BIND_CIRCLE))) {
        if (!repeat) {
            Close(GOW3_BIND_CIRCLE);
        }
    } else if (code == SDL_SCANCODE_RETURN || code == SDL_SCANCODE_KP_ENTER || code == SDL_SCANCODE_SPACE ||
               Bound(code, GOW3_BIND_CROSS)) {
        if (!repeat) {
            Activate();
        }
    } else if (code == SDL_SCANCODE_DELETE || code == SDL_SCANCODE_BACKSPACE) {
        if (!repeat && Selected().kind == KindBind) {
            const int bind = Selected().bind;
            bindings.key[bind][0] = bindings.key[bind][1] = 0;
            bindings.mouse[bind] = 0;
            Apply();
            Say(std::string(Label(Selected())) + " has no key now");
        }
    } else if (up || down) {
        Move(up ? -1 : 1);
    } else if (left || right) {
        Side(left ? -1 : 1);
    }
}

void OnMouseButton(int button, float x, float y) {
    if (capturing) {
        capturing = false;
        if (button >= SDL_BUTTON_LEFT && button <= SDL_BUTTON_X2) {
            AssignMouse(Selected().bind, button); // SDL's numbers are the pad's mouse sources
        }
        return;
    }
    int c = 0, i = 0;
    if (button == SDL_BUTTON_LEFT && Hit(x, y, c, i)) {
        const bool again = c == column && i == selected[c];
        if (!again) {
            confirm_reset = false;
        }
        column = c;
        selected[c] = i;
        Activate();
    }
}

void OnPad(int button) {
    if (capturing) {
        if (button == SDL_GAMEPAD_BUTTON_EAST || button == SDL_GAMEPAD_BUTTON_START) {
            capturing = false;
        }
        return;
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: Move(-1); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: Move(1); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: Side(-1); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: Side(1); break;
    case SDL_GAMEPAD_BUTTON_SOUTH: Activate(); break;
    case SDL_GAMEPAD_BUTTON_EAST: Close(GOW3_BIND_CIRCLE); break;
    case SDL_GAMEPAD_BUTTON_START: Close(GOW3_BIND_OPTIONS); break;
    default: break;
    }
}

// --- Drawing ---------------------------------------------------------------------------------

// The keys and mouse button of a row, right-aligned at `right`; returns their left edge.
float Bindings(ImDrawList* dl, const Row& row, float right, float y, float s, bool waiting) {
    const float h = 34.0f * s, gap = 8.0f * s;
    if (waiting) {
        const float w = std::max(h, TextWidth("...", h * 0.66f) + h * 0.5f);
        KeycapLabel(dl, right - w, y, h, "...", NowMs() % 900 < 600);
        return right - w;
    }
    float width = 0.0f;
    int items = 0;
    for (const int key : bindings.key[row.bind]) {
        if (key > 0) {
            width += KeycapWidth(h, key);
            ++items;
        }
    }
    if (bindings.mouse[row.bind] > 0 && row.bind < GOW3_BIND_LS_UP) {
        width += MouseWidth(h);
        ++items;
    }
    if (!items) {
        const float w = TextWidth("Not Set", 30.0f * s);
        Text(dl, ImVec2(right - w, y - 15.0f * s), "Not Set", 30.0f * s, Grey);
        return right - w;
    }
    width += gap * float(items - 1);
    float x = right - width;
    for (const int key : bindings.key[row.bind]) {
        if (key > 0) {
            x += Keycap(dl, x, y, h, key, false) + gap;
        }
    }
    if (bindings.mouse[row.bind] > 0 && row.bind < GOW3_BIND_LS_UP) {
        Mouse(dl, x, y, h, bindings.mouse[row.bind]);
    }
    return right - width;
}

// A value row's value, right-aligned at `right`; returns its left edge.
float Value(ImDrawList* dl, const Row& row, float right, float y, float s, bool selected_row) {
    const float size = 32.0f * s;
    const ImU32 colour = selected_row ? White : CreamDim;
    std::string text;
    switch (row.kind) {
    case KindSensitivity: {
        // The games' slider: a dark track with a light fill, the value beside it.
        const float track = 220.0f * s, th = 9.0f * s;
        const float lo = sensitivities[0], hi = sensitivities[std::size(sensitivities) - 1];
        const float fill = std::clamp((bindings.mouse_sensitivity - lo) / (hi - lo), 0.0f, 1.0f);
        const ImVec2 a(right - track, y - th * 0.5f), b(right, y + th * 0.5f);
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(a, ImVec2(a.x + track * fill, b.y), IM_COL32(245, 242, 235, 255));
        char value[16];
        std::snprintf(value, sizeof(value), "%.2fx", bindings.mouse_sensitivity);
        const float w = TextWidth(value, size);
        const float x = a.x - 16.0f * s - w;
        Text(dl, ImVec2(x, y - size * 0.5f), value, size, colour);
        if (selected_row) {
            const float aw = TextWidth("<>", size);
            Text(dl, ImVec2(x - 14.0f * s - aw, y - size * 0.5f), "<>", size, colour);
            return x - 14.0f * s - aw;
        }
        return x;
    }
    case KindInvert: text = bindings.mouse_invert ? "Flipped" : "Normal"; break;
    case KindIcons: text = icon_names[std::clamp(bindings.glyphs, 0, 3)]; break;
    case KindReset:
        if (!confirm_reset) {
            return right;
        }
        text = "Press again";
        break;
    default: return right;
    }
    const float w = TextWidth(text, size);
    Text(dl, ImVec2(right - w, y - size * 0.5f), text, size, row.kind == KindReset ? Orange : colour);
    if (selected_row && row.kind != KindReset) {
        const float aw = TextWidth("<>", size);
        Text(dl, ImVec2(right - w - 14.0f * s - aw, y - size * 0.5f), "<>", size, colour);
        return right - w - 14.0f * s - aw;
    }
    return right - w;
}

void DrawColumn(ImDrawList* dl, int c, float x, float top, float width, float s) {
    float y = top;
    const float row_height = 38.0f * s, text_size = 33.0f * s;
    for (int i = 0; i < columns[c].count; ++i) {
        const Row& row = columns[c].rows[i];
        if (row.section && *row.section) {
            const float size = 36.0f * s, w = TextWidth(row.section, size);
            const float mid = x + width * 0.5f, line_y = y + 24.0f * s;
            Rule(dl, ImVec2(x, line_y), ImVec2(mid - w * 0.5f - 18.0f * s, line_y), s);
            Rule(dl, ImVec2(mid + w * 0.5f + 18.0f * s, line_y), ImVec2(x + width, line_y), s);
            Text(dl, ImVec2(mid - w * 0.5f, y + 4.0f * s), row.section, size, White);
            y += 47.0f * s;
        } else if (row.section) {
            y += 22.0f * s;
        }
        const bool is_selected = c == column && i == selected[c];
        rects[c][i] = {x - 12.0f * s, y, x + width + 12.0f * s, y + row_height};
        if (is_selected) {
            Highlight(dl, ImVec2(x - 12.0f * s, y + 1.0f * s), ImVec2(x + width + 12.0f * s, y + row_height - 1.0f * s));
        }
        const float mid = y + row_height * 0.5f;
        if (row.kind == KindBind) {
            PadBadge(dl, x + 26.0f * s, mid, 30.0f * s, row.bind);
        }
        const float label_x = x + 62.0f * s;
        const float label_width = Text(dl, ImVec2(label_x, y + (row_height - text_size) * 0.5f + 1.0f * s),
                                       Label(row), text_size, is_selected ? White : Cream);
        const float value_left = row.kind == KindBind
                                     ? Bindings(dl, row, x + width, mid, s, is_selected && capturing)
                                     : Value(dl, row, x + width, mid, s, is_selected);
        const float rule_from = label_x + label_width + 16.0f * s, rule_to = value_left - 16.0f * s;
        if (row.kind == KindBind && rule_to - rule_from > 24.0f * s) {
            Rule(dl, ImVec2(rule_from, mid), ImVec2(rule_to, mid), s);
        }
        y += row_height;
    }
}

// The help line at the panel's bottom right, like the games' menus: keycap and what it does.
void DrawHelp(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float s) {
    const float h = 32.0f * s, size = 32.0f * s, gap = 34.0f * s, y = p1.y - 40.0f * s;
    Rule(dl, ImVec2(p0.x + 52.0f * s, y - 30.0f * s), ImVec2(p1.x - 52.0f * s, y - 30.0f * s), s);
    const int back_key = bindings.key[GOW3_BIND_CIRCLE][0] > 0 ? bindings.key[GOW3_BIND_CIRCLE][0]
                                                               : int(SDL_SCANCODE_ESCAPE);
    struct Item {
        int key;
        const char* text;
    };
    Item items[3]{};
    int count = 0;
    const Kind kind = Selected().kind;
    if (capturing) {
        items[count++] = {SDL_SCANCODE_ESCAPE, "Cancel"};
    } else if (kind == KindBind) {
        items[count++] = {SDL_SCANCODE_RETURN, "Change"};
        items[count++] = {SDL_SCANCODE_DELETE, "Clear"};
        items[count++] = {back_key, "Back"};
    } else {
        items[count++] = {SDL_SCANCODE_RETURN, kind == KindReset ? "Select" : "Change"};
        items[count++] = {back_key, "Back"};
    }
    float width = 0.0f;
    for (int i = 0; i < count; ++i) {
        width += KeycapWidth(h, items[i].key) + 12.0f * s + TextWidth(items[i].text, size) + (i ? gap : 0.0f);
    }
    float x = p1.x - 52.0f * s - width;
    for (int i = 0; i < count; ++i) {
        x += (i ? gap : 0.0f);
        x += Keycap(dl, x, y, h, items[i].key, false) + 12.0f * s;
        x += Text(dl, ImVec2(x, y - size * 0.55f), items[i].text, size, Cream);
    }
    // The message or the waiting prompt, at the left.
    std::string left;
    ImU32 colour = Cream;
    if (capturing) {
        left = std::string("Press a key or a mouse button for ") + Label(Selected());
        colour = White;
    } else if (NowMs() < message_until) {
        left = message;
    }
    if (!left.empty()) {
        TextFit(dl, ImVec2(p0.x + 52.0f * s, y - size * 0.55f), left, size, p1.x - p0.x - 160.0f * s - width,
                colour);
    }
}

} // namespace

void Frame() {
    const bool screen = Gow3Glyphs::LayoutAge() < 400;
    if (!screen) {
        suppressed = false;
        if (active.load(std::memory_order_relaxed)) {
            active.store(false, std::memory_order_release);
            capturing = confirm_reset = false;
        }
        return;
    }
    if (active.load(std::memory_order_relaxed) || suppressed || !Gow3Glyphs::State().keyboard_last) {
        return;
    }
    runtime_pad_bindings(&bindings, 0);
    runtime_pad_bindings(&defaults, 1);
    capturing = confirm_reset = false;
    message.clear();
    active.store(true, std::memory_order_release);
    static int reports = 0;
    if (reports++ < 20) {
        std::printf("Overlay: keyboard page on the Controller Layout screen\n");
        std::fflush(stdout);
    }
}

bool Active() {
    return active.load(std::memory_order_acquire);
}

void Draw(float width, float height) {
    if (!Active()) {
        return;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // The game's 16:9 picture within the window, and the Controller Layout screen's panel in it.
    float w = width, h = height, ox = 0.0f, oy = 0.0f;
    if (w > h * 16.0f / 9.0f) {
        w = h * 16.0f / 9.0f;
        ox = (width - w) * 0.5f;
    } else {
        h = w * 9.0f / 16.0f;
        oy = (height - h) * 0.5f;
    }
    const float s = h / 1080.0f;
    const ImVec2 p0(ox + w * 0.0742f, oy + h * 0.0972f), p1(ox + w * 0.9258f, oy + h * 0.9028f);
    Panel(dl, p0, p1, s);
    Text(dl, ImVec2(p0.x + 52.0f * s, p0.y + 26.0f * s), "KEYBOARD AND MOUSE", 54.0f * s, Cream);
    Text(dl, ImVec2(p0.x + 54.0f * s, p0.y + 84.0f * s), "Changes apply at once and are kept for this game.",
         27.0f * s, Grey);
    const float inner0 = p0.x + 52.0f * s, inner1 = p1.x - 52.0f * s, gap = 90.0f * s;
    const float column_width = (inner1 - inner0 - gap) * 0.5f;
    for (int c = 0; c < 2; ++c) {
        DrawColumn(dl, c, inner0 + float(c) * (column_width + gap), p0.y + 122.0f * s, column_width, s);
    }
    DrawHelp(dl, p0, p1, s);
}

bool HandleEvent(const SDL_Event& event) {
    if (!Active()) {
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        OnKey(int(event.key.scancode), event.key.repeat);
        return true;
    case SDL_EVENT_MOUSE_MOTION: {
        SDL_Window* window = SDL_GetWindowFromID(event.motion.windowID);
        const float density = window ? std::max(SDL_GetWindowPixelDensity(window), 1.0f) : 1.0f;
        ImGui::GetIO().AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        int c = 0, i = 0;
        if (!capturing && Hit(event.motion.x * density, event.motion.y * density, c, i) &&
            (c != column || i != selected[c])) {
            column = c;
            selected[c] = i;
            confirm_reset = false;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        SDL_Window* window = SDL_GetWindowFromID(event.button.windowID);
        const float density = window ? std::max(SDL_GetWindowPixelDensity(window), 1.0f) : 1.0f;
        OnMouseButton(event.button.button, event.button.x * density, event.button.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (capturing) {
            capturing = false;
            AssignMouse(Selected().bind, event.wheel.y > 0 ? 6 : 7); // wheel up, wheel down
        } else if (event.wheel.y != 0) {
            Move(event.wheel.y > 0 ? -1 : 1);
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        OnPad(event.gbutton.button);
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        return false;
    }
}

} // namespace Gow3Controls
