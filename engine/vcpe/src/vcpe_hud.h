/* VCPE HUD built-ins that need no game data:
 *   text in the PSP SDK's 8x8 font (debug lines, test builds, the perf overlay, early prototypes)
 *   the standard perf overlay players read their numbers from: FPS, update ms, draw ms (CPU build + GE wait),
 *     draw calls, quads, free memory
 *   PSP button icons drawn from shapes (cross, circle, square, triangle, L, R, START, SELECT, d-pad arrows) for
 *     prompts like "(X) JUMP", in the PlayStation colours, at any size
 * Everything draws through vcpe_gfx (batched when batching is on). */
#ifndef VCPE_HUD_H
#define VCPE_HUD_H
#include <stdint.h>

void vcpe_hud_text(float x, float y, const char *s, uint32_t color, float scale);   /* '\n' starts a new line */
void vcpe_hud_printf(float x, float y, uint32_t color, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
float vcpe_hud_text_width(const char *s, float scale);                               /* widest line */
/* text with a 1-pixel dark shadow, readable on any background */
void vcpe_hud_text_shadow(float x, float y, const char *s, uint32_t color, float scale);

/* measure the game's update step: begin/end around it (several steps a frame add up) */
void vcpe_perf_update_begin(void);
void vcpe_perf_update_end(void);
extern int vcpe_stat_update_us;   /* smoothed update time per shown frame */
/* the overlay: one line at the bottom of the screen (hold SELECT to show it, by convention) */
void vcpe_hud_perf(void);
/* 5.6: pass "SELECT and TRIANGLE are both down" each frame: a press toggles camera culling (A/B its gain live) */
void vcpe_hud_perf_chord(int down);
/* 4.0: a second line above it: GE time of the last synchronous frame (vcpe_gfx_bench), texture binds, overdraw in
 * screens, vblanks late since start ("!" for a second after one), display list used */
void vcpe_hud_perf_detail(int on);
/* 4.0: the standard perf line for logs, the same in every game, from the numbers of the last vcpe_hud_perf call:
 * "perf 60 fps upd 3.2 build 4.1 ge 2.0 ms, 120 draws 40 binds 400 quads 2.3 screens, 0 vblanks late, dlist 96 KB".
 * Pair it with vcpe_mem_report (vcpe_mem.h) */
int vcpe_perf_line(char *out, int n);

enum { VCPE_BTN_CROSS, VCPE_BTN_CIRCLE, VCPE_BTN_SQUARE, VCPE_BTN_TRIANGLE, VCPE_BTN_L, VCPE_BTN_R, VCPE_BTN_START,
       VCPE_BTN_SELECT, VCPE_BTN_UP, VCPE_BTN_DOWN, VCPE_BTN_LEFT, VCPE_BTN_RIGHT };
/* a button icon centred on (cx, cy), r = radius of the round icons (L/R/START/SELECT are badges as tall as 2r);
 * alpha 0..255; returns the icon's width */
float vcpe_hud_button(int btn, float cx, float cy, float r, int alpha);
float vcpe_hud_button_width(int btn, float r);   /* for laying icons out before drawing them */
/* icon then label, left-aligned at (x, y top), label in the 8x8 font; returns the total width */
float vcpe_hud_prompt(float x, float y, int btn, const char *label, uint32_t color);
/* Reusable shapes: ring uses triangle strips; rectangles are made from scanlines. */
void vcpe_hud_ring(float cx,float cy,float radius,float thickness,float start_degrees,float sweep_degrees,uint32_t color);
void vcpe_hud_round_rect(float x,float y,float w,float h,float radius,uint32_t color);
void vcpe_hud_bar(float x,float y,float w,float h,float value,uint32_t back,uint32_t fill);
void vcpe_hud_toast_set(const char *text,float seconds);
void vcpe_hud_toast_tick(float dt);
void vcpe_hud_toast_draw(float x,float y,float max_width);

#endif
