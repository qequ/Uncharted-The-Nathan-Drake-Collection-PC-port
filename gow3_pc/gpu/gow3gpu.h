/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef GOW3GPU_H
#define GOW3GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} Gow3GpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void gow3gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int gow3gpu_init(const Gow3GpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t gow3gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int gow3gpu_handle_fault(void *ucontext, void *address);
/* GOW3_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void gow3gpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int gow3gpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int gow3gpu_overlay_captures_input(void);
/* Mouse movement since the last call (pixels) and held sources (bit 0 left, 1 middle, 2 right,
 * 3 X1, 4 X2, 5 wheel up, 6 wheel down); returns 1 while the mouse is captured (gow3_mouse.h). */
int gow3gpu_mouse_take(float *dx, float *dy, unsigned *buttons);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned gow3gpu_symbol_count(void);
/* Flips the game has submitted so far (the freeze watchdog of src/runtime_report.c). */
uint64_t gow3gpu_flip_count(void);
/* A one-line notice at the top of the game window for a few seconds (no window: nothing). */
void gow3gpu_notice(const char *text);
/* Called on the failing thread before a failed GPU library check ends the process (exit 23). */
void gow3gpu_set_stop_hook(void (*hook)(const char *why));
/* The button icons in the games' text (gpu/shim/gow3_glyphs.cpp) show what the player plays
 * with: src/runtime_pad.c sends the device last used and what presses each PS4 button whenever
 * either changes. A PlayStation pad keeps the games' own icons. */
enum { GOW3_GLYPHS_PLAYSTATION, GOW3_GLYPHS_KEYBOARD, GOW3_GLYPHS_XBOX, GOW3_GLYPHS_OTHER_PAD };
enum {
    GOW3_GLYPH_CROSS, GOW3_GLYPH_CIRCLE, GOW3_GLYPH_SQUARE, GOW3_GLYPH_TRIANGLE,
    GOW3_GLYPH_L1, GOW3_GLYPH_R1, GOW3_GLYPH_L2, GOW3_GLYPH_R2, GOW3_GLYPH_L3, GOW3_GLYPH_R3,
    GOW3_GLYPH_OPTIONS, GOW3_GLYPH_TOUCHPAD,
    GOW3_GLYPH_UP, GOW3_GLYPH_DOWN, GOW3_GLYPH_LEFT, GOW3_GLYPH_RIGHT,
    GOW3_GLYPH_LS_UP, GOW3_GLYPH_LS_DOWN, GOW3_GLYPH_LS_LEFT, GOW3_GLYPH_LS_RIGHT,
    GOW3_GLYPH_RS_UP, GOW3_GLYPH_RS_DOWN, GOW3_GLYPH_RS_LEFT, GOW3_GLYPH_RS_RIGHT,
    GOW3_GLYPH_COUNT
};
/* Gamepad sources that are not buttons (pad[] below). */
enum { GOW3_GLYPH_PAD_LEFT_TRIGGER = 100, GOW3_GLYPH_PAD_RIGHT_TRIGGER };
typedef struct {
    int32_t style;                   /* GOW3_GLYPHS_* */
    int32_t key[GOW3_GLYPH_COUNT];   /* the first key that presses it (SDL scancode), 0: none */
    int32_t mouse[GOW3_GLYPH_COUNT]; /* the first mouse source: 1 left, 2 middle, 3 right, 4 X1,
                                        5 X2, 6 wheel up, 7 wheel down; 0: none */
    int32_t pad[GOW3_GLYPH_COUNT];   /* the gamepad button (SDL_GamepadButton or a trigger
                                        above), -1: none */
    char face[4];                    /* the pad's letters at south, east, west, north */
} Gow3GlyphState;
void gow3gpu_glyphs_set(const Gow3GlyphState *state);
#ifdef __cplusplus
}
#endif
#endif
