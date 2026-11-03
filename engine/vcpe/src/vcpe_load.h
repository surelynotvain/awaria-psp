/* VCPE loading screen: shown while a game swaps its content (the old level's textures, world and sounds out, the
 * next ones in), so a long load shows "LOADING" and a progress bar instead of a frozen picture.
 *
 * Together with vcpe_mem (vcpe_bundle_slab, the volatile partition) this is how a game made for 64 MB fits a 32 MB
 * PSP-1000: keep only the current content in memory and load the next behind this screen.
 *
 *   vcpe_loading_begin(bytes);      // bytes about to be read from the PAK (0 if unknown: no bar)
 *   ...free the old content, vcpe_bundle_load / vcpe_pak_read the new...   (PAK reads report their progress)
 *   vcpe_loading_end();
 *
 * Call it between frames (after vcpe_gfx_end), never inside one. Nothing is drawn when the load takes less than
 * 150 ms, so quick switches don't flash. A game can draw its own screen with vcpe_loading_draw_hook, using only
 * memory that stays loaded (not the bundles being swapped). Autotest recordings are not affected: loading frames do
 * not count as game frames.
 *
 * The standard screen is the same in every VCPE game, like the error screen: black; a boot log in the top-left
 * corner, plain text, one line per step in progress or done ("Loading textures 3: 28 pages, 7196 KB"), written by
 * the engine itself from the steps its modules report (vcpe_step_*, vcpe_sys.h) plus any the game adds;
 * The log defaults to eight visible lines with Linux-style result tags. It retains the last 24 steps.
 * The engine's loading animation (engineassets/hetalia-czechia.gif, built into src/vcpe_loadimg.h by tools/mkloadimg.py) is in
 * the bottom-right corner with the word and the progress bar just above it. In autotest runs the finished log
 * lines also go to stdout. A game's own screen can still use the pieces: vcpe_loading_anim() draws
 * the animation anywhere, vcpe_loading_draw_standard() the whole standard screen (e.g. over its own background). */
#ifndef VCPE_LOAD_H
#define VCPE_LOAD_H
#include <stdint.h>

void vcpe_loading_begin(uint32_t total_bytes);
void vcpe_loading_end(void);
void vcpe_loading_add(uint32_t bytes);   /* progress from other work (PAK reads add theirs themselves) */
void vcpe_loading_tick(void);            /* redraw if one is due: call it in long CPU-only work */
int vcpe_loading_active(void);
void vcpe_loading_show_after(int ms);    /* the delay before the screen appears (default 150; 0: always, for tests) */
void vcpe_loading_redraw_ms(int ms);     /* 4.0: time between redraws while loading (default 50; was 33) */
int vcpe_loading_shown(void);            /* frames drawn by the current / last loading screen */
/* the game's own loading screen: called inside a frame (black already cleared), progress 0..1 or -1 if unknown */
void vcpe_loading_draw_hook(void (*draw)(float progress, int ms));
void vcpe_loading_text(const char *text);  /* the default screen's word ("LOADING"), e.g. a translation */
/* Startup animation (build the engine with -DVCPE_LOAD_BOOT_ANIM, about 220 KB): 1 around the game's boot load shows
 * engineassets/wow.gif, 0 goes back to the in-game animation for every later load. Without the define it does nothing */
void vcpe_loading_boot_anim(int on);
/* 5.6: the animation's decode buffer (about 48 KB) is freed by vcpe_loading_end. A game that draws vcpe_loading_anim
 * outside a loading window (its own wait screen) can free it with this afterwards. */
void vcpe_loading_anim_release(void);
void vcpe_loading_anim(float x, float y, float size, int ms);    /* size = width; original aspect ratio */
void vcpe_loading_draw_standard(float progress, int ms);        /* the standard screen's contents (in a frame) */
void vcpe_loading_draw_log(int ms);                             /* only the boot log (top-left) */
void vcpe_loading_show_log(int on);                             /* 0: the standard screen without the log */
void vcpe_loading_log_lines(int lines);                         /* visible rows, clamped 1..24; default 8 */
int vcpe_loading_log_height(void);
/* Opt-in PAK contract monitor: count actual data reads issued outside a loading window. */
void vcpe_load_assert_enable(int on);
void vcpe_load_assert_reset(void);
uint32_t vcpe_load_assert_violations(void);
uint32_t vcpe_load_assert_bytes(void);
void vcpe_load_assert_observe(uint32_t bytes);

#endif
