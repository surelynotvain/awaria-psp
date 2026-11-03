/* VCPE HUD built-ins (see vcpe_hud.h). */

/* HOW THIS FILE WORKS
 *
 * The HUD needs no game data at all: its font is the 8x8 debug font that ships inside the PSP SDK (libpspdebug's
 * "msx" table, 1 bit per pixel). At first use it is expanded into a 128x128 4-bit texture (16x16 cells of 8x8 pixels,
 * one per character code) with a 2-colour palette: transparent and white. Text is then one quad per character, tinted
 * by the vertex colour.
 *
 * Button icons are drawn from shapes (discs, rings, lines, triangles) with vcpe_strip, so they look the same at any
 * size and need no texture. */
#include <pspgu.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_plat.h"
#include "vcpe_sys.h"
#include "vcpe_gfx.h"
#include "vcpe_math.h"
#include "vcpe_hud.h"
#include "vcpe_gfx.h"

/* Each character is 8 bytes, one per row; bit 7 is the leftmost pixel. */
extern unsigned char msx[];   /* libpspdebug's 8x8 font, 8 bytes per character, bit 7 = left pixel */

/* The expanded font texture (4-bit, two pixels per byte) and its palette: entry 0 transparent, entry 1 white. */
static uint8_t __attribute__((aligned(16))) font_tex[128 * 128 / 2];   /* 16x16 cells of 8x8, T4 */
static uint32_t __attribute__((aligned(16))) font_clut[16] = {0x00FFFFFF, 0xFFFFFFFF};
static int font_ready;

/* Expand the 1-bit font into the 4-bit texture: character c goes to cell (c % 16, c / 16). In a 4-bit texture the
 * even pixel of a pair is the low nibble, the odd pixel the high nibble. */
static void font_init(void)
{
    for (int c = 0; c < 256; c++) {
        int cx = (c & 15) * 8, cy = (c >> 4) * 8;
        for (int y = 0; y < 8; y++) {
            uint8_t row = msx[c * 8 + y];
            for (int x = 0; x < 8; x++) {
                int px = cx + x, py = cy + y, on = (row >> (7 - x)) & 1;
                uint8_t *b = &font_tex[(py * 128 + px) >> 1];
                if (px & 1) *b = (uint8_t)((*b & 0x0F) | (on << 4));
                else *b = (uint8_t)((*b & 0xF0) | on);
            }
        }
    }
    /* The GE reads RAM, not the CPU cache: write the new texels back to RAM. */
    vcpe_cache_writeback(font_tex, sizeof font_tex);
    vcpe_cache_writeback(font_clut, sizeof font_clut);
    font_ready = 1;
}

/* Bind the font texture, unless it is still bound from the previous string (vcpe_tex_bound): drawing 300 strings
 * costs one texture bind, not 300. */
static void font_bind(void)
{
    if (!font_ready)
        font_init();
    vcpe_blend(0);
    vcpe_tex_white(0);
    vcpe_filter(1);
    if (vcpe_tex_bound(font_tex))
        return;   /* still bound from the last string: consecutive lines cost no texture commands */
    vcpe_tex_claim(font_tex);
    sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
    sceGuClutLoad(2, font_clut);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, 128, 128, 128, font_tex);
    vcpe_tex_flush();
}

/* One quad per visible character. Character code c sits at texel (c % 16 * 8, c / 16 * 8). Spaces only move the pen;
 * '\n' goes back to x and down one line. */
void vcpe_hud_text(float x, float y, const char *s, uint32_t color, float scale)
{
    if (!s || !*s || (color >> 24) == 0)
        return;
    font_bind();
    float cx = x, d = 8 * scale;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n') { cx = x; y += d; continue; }
        if (*p != ' ') {
            float u = (*p & 15) * 8, v = (*p >> 4) * 8;
            vcpe_quad(cx, y, cx + d, y + d, u, v, u + 8, v + 8, color);
        }
        cx += d;
    }
    vcpe_gfx_flush();   /* drawn now: a game may bind raw sceGu textures next (the binding itself stays cached) */
}

/* Readable on any background: the same text in black one pixel down-right, then the text itself. */
void vcpe_hud_text_shadow(float x, float y, const char *s, uint32_t color, float scale)
{
    vcpe_hud_text(x + scale, y + scale, s, color & 0xFF000000u, scale);
    vcpe_hud_text(x, y, s, color, scale);
}

void vcpe_hud_printf(float x, float y, uint32_t color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vcpe_hud_text(x, y, buf, color, 1);
}

/* Monospaced: the widest line's character count times 8 pixels. */
float vcpe_hud_text_width(const char *s, float scale)
{
    int best = 0, n = 0;
    for (; s && *s; s++) {
        if (*s == '\n') n = 0;
        else if (++n > best) best = n;
    }
    return best * 8 * scale;
}

/* ------------------------------------------------------------------ perf overlay */
/* Perf overlay. The game wraps its update in vcpe_perf_update_begin/end (several times a frame if it runs several
 * fixed steps); the times add up per shown frame. */
int vcpe_stat_update_us;
static int64_t upd_t0;
static int upd_acc, heap_kb, heap_age, last_quads, quads_n;
static int last_quads_n(void) { return quads_n; }

void vcpe_perf_update_begin(void) { upd_t0 = (int64_t)vcpe_time_us(); }
void vcpe_perf_update_end(void) { upd_acc += (int)((int64_t)vcpe_time_us() - upd_t0); }

/* Counters like vcpe_stat_quads are reset by games on their own schedule; "share" turns a running total into this
 * frame's part of it. */
static int detail, last_binds, last_misses, frame_binds, frame_misses;
static float last_area, frame_area;
void vcpe_hud_perf_detail(int on) { detail = on; }

/* this frame's share of counters games reset on their own schedules */
static int share(int now, int *last)
{
    int d = now - *last;
    *last = now;
    return d < 0 ? now : d;
}

/* The standard one-line perf report for log files, the same format in every game. */
int vcpe_perf_line(char *out, int n)
{
    return snprintf(out, n, "perf %d fps upd %d.%d build %d.%d ge %d.%d ms, %d draws %d binds %d quads %d.%d screens, "
                    "%d vblanks late, dlist %d KB", vcpe_stat_fps, vcpe_stat_update_us / 1000,
                    vcpe_stat_update_us / 100 % 10, vcpe_stat_build_us / 1000, vcpe_stat_build_us / 100 % 10,
                    vcpe_stat_ge_us / 1000, vcpe_stat_ge_us / 100 % 10, vcpe_stat_draws, frame_binds, last_quads_n(),
                    (int)(frame_area / (VCPE_SCR_W * VCPE_SCR_H)), (int)(frame_area * 10 / (VCPE_SCR_W * VCPE_SCR_H)) % 10,
                    vcpe_stat_vblank_misses, vcpe_stat_dlist_kb);
}

/* 3D counters: only games that link vcpe_3d / vcpe_lod have them, so 2D games still link and show no 3D lines. */
extern int vcpe_stat_tris __attribute__((weak));
extern int vcpe_stat_culled __attribute__((weak));
extern int vcpe_stat_cull_nodes __attribute__((weak));
extern int vcpe_stat_cull_tris __attribute__((weak));
extern int vcpe_lod_stat_full __attribute__((weak));
extern int vcpe_lod_stat_drawn __attribute__((weak));
void vcpe_3d_autocull(int on) __attribute__((weak));
int vcpe_3d_autocull_enabled(void) __attribute__((weak));

void vcpe_hud_perf_chord(int down)
{
    static int was;
    if (down && !was && vcpe_3d_autocull && vcpe_3d_autocull_enabled) vcpe_3d_autocull(!vcpe_3d_autocull_enabled());
    was = down;
}

/* the top panel: frame rate, frame time, triangles, culling, with the culling toggle's hint */
static void perf_top(void)
{
    char t[96];
    int has3d = &vcpe_stat_tris != 0, lod = (&vcpe_lod_stat_full && &vcpe_lod_stat_drawn) ? vcpe_lod_stat_full - vcpe_lod_stat_drawn : 0;
    if (has3d) snprintf(t, sizeof t, "%2d FPS  %d.%d MS  %d TRIS (LOD -%d)", vcpe_stat_fps, vcpe_stat_frame_us / 1000, vcpe_stat_frame_us / 100 % 10, vcpe_stat_tris, lod);
    else snprintf(t, sizeof t, "%2d FPS  %d.%d MS", vcpe_stat_fps, vcpe_stat_frame_us / 1000, vcpe_stat_frame_us / 100 % 10);
    vcpe_fill(2, 2, 340, 12, 0xA0000000);
    vcpe_hud_text(5, 4, t, 0xFF00FF00, 1);
    if (has3d && &vcpe_stat_culled) {
        snprintf(t, sizeof t, "CULL %s  DRAW %d  NODE %d  TRI -%d", (vcpe_3d_autocull_enabled && vcpe_3d_autocull_enabled()) ? "ON" : "OFF",
                 vcpe_stat_culled, &vcpe_stat_cull_nodes ? vcpe_stat_cull_nodes : 0, &vcpe_stat_cull_tris ? vcpe_stat_cull_tris : 0);
        vcpe_fill(2, 15, 340, 23, 0xD0000000);
        vcpe_hud_text(5, 17, t, 0xFF00FF00, 1);
        vcpe_hud_text(5, 28, "SELECT + TRIANGLE: TOGGLE CULLING", 0xFFFFFFFF, 1);
    }
}

/* Draw the overlay: the top panel, then at the bottom of the screen: FPS, update ms, draw ms (CPU building the list + waiting for the GE),
 * draw calls, quads, and the largest free heap block (re-measured twice a second, the probe is not free). */
void vcpe_hud_perf(void)
{
    /* smooth the update time over frames (this is called once per shown frame) */
    /* Exponential smoothing: move 1/8 of the way to the new value, so the number is readable instead of flickering.
     */
    vcpe_stat_update_us += (upd_acc - vcpe_stat_update_us) / 8;
    upd_acc = 0;
    if (heap_age-- <= 0) {   /* the heap probe allocates: twice a second is plenty */
        heap_kb = vcpe_heap_kb();
        heap_age = 30;
    }
    /* games reset vcpe_stat_quads on their own schedules: show this frame's share */
    int q = share(vcpe_stat_quads, &last_quads);
    quads_n = q;
    frame_binds = share(vcpe_stat_binds, &last_binds);
    frame_misses = share(vcpe_stat_vblank_misses, &last_misses);
    frame_area = vcpe_stat_area - last_area;
    if (frame_area < 0)
        frame_area = vcpe_stat_area;
    last_area = vcpe_stat_area;
    perf_top();
    char buf[96];
    snprintf(buf, sizeof buf, "%2d FPS  UPD %d.%d  DRAW %d.%d+%d.%d MS  %d CALLS %d Q  %dK", vcpe_stat_fps,
             vcpe_stat_update_us / 1000, vcpe_stat_update_us / 100 % 10, vcpe_stat_build_us / 1000,
             vcpe_stat_build_us / 100 % 10, vcpe_stat_gpu_wait_us / 1000, vcpe_stat_gpu_wait_us / 100 % 10,
             vcpe_stat_draws, q, heap_kb);
    /* The optional second line: GE time, texture binds, overdraw in screens, late vblanks, display list use. */
    if (detail) {
        static int late_age;
        if (frame_misses)
            late_age = 60;   /* keep a late frame visible for a second */
        char d[96];
        snprintf(d, sizeof d, "GE %d.%d MS  %d BINDS  %d.%dX SCREEN  LATE %d%s  DL %dK", vcpe_stat_ge_us / 1000,
                 vcpe_stat_ge_us / 100 % 10, frame_binds, (int)(frame_area / (VCPE_SCR_W * VCPE_SCR_H)),
                 (int)(frame_area * 10 / (VCPE_SCR_W * VCPE_SCR_H)) % 10, vcpe_stat_vblank_misses,
                 late_age > 0 ? " !" : "", vcpe_stat_dlist_kb);
        if (late_age > 0)
            late_age--;
        vcpe_fill(0, VCPE_SCR_H - 22, VCPE_SCR_W, 11, 0xA0000000);
        vcpe_hud_text(3, VCPE_SCR_H - 20, d, 0xFFFFFFFF, 1);
    }
    vcpe_fill(0, VCPE_SCR_H - 11, VCPE_SCR_W, 11, 0xA0000000);
    vcpe_hud_text(3, VCPE_SCR_H - 9, buf, 0xFFFFFFFF, 1);
}

/* ------------------------------------------------------------------ button icons */
/* Fill a convex polygon with one triangle strip by zig-zagging between the two ends of the point list (0, 1, n-1, 2,
 * n-2, ...): every consecutive triple is then a triangle inside the polygon. */
static void convex(const float *pts, int n, uint32_t color)
{
    /* convex polygon as a strip: 0, 1, n-1, 2, n-2, ... */
    float xy[64];
    uint32_t c[32];
    int k = 0, lo = 1, hi = n - 1;
    xy[0] = pts[0]; xy[1] = pts[1]; c[k++] = color;
    while (lo <= hi && k < 32) {
        xy[k * 2] = pts[lo * 2]; xy[k * 2 + 1] = pts[lo * 2 + 1]; c[k++] = color; lo++;
        if (lo > hi || k >= 32) break;
        xy[k * 2] = pts[hi * 2]; xy[k * 2 + 1] = pts[hi * 2 + 1]; c[k++] = color; hi--;
    }
    vcpe_strip(xy, c, k, 0);
}

/* A filled circle as a polygon; more sides for bigger circles. */
static void disc(float cx, float cy, float r, uint32_t color)
{
    float p[48];
    int n = r < 6 ? 12 : r < 14 ? 18 : 24;
    for (int i = 0; i < n; i++) {
        p[i * 2] = cx + r * vcpe_cos_deg(i * 360.0f / n);
        p[i * 2 + 1] = cy + r * vcpe_sin_deg(i * 360.0f / n);
    }
    convex(p, n, color);
}

/* A circle outline: a strip alternating between the outer and the inner radius. */
static void ring(float cx, float cy, float r, float w, uint32_t color)
{
    float xy[2 * 50];
    uint32_t c[50];
    int n = 24, k = 0;
    for (int i = 0; i <= n; i++) {
        float co = vcpe_cos_deg(i * 360.0f / n), si = vcpe_sin_deg(i * 360.0f / n);
        xy[k * 2] = cx + r * co; xy[k * 2 + 1] = cy + r * si; c[k++] = color;
        xy[k * 2] = cx + (r - w) * co; xy[k * 2 + 1] = cy + (r - w) * si; c[k++] = color;
    }
    vcpe_strip(xy, c, k, 0);
}

/* A thick line as a 4-vertex strip: the segment pushed out by half the width on both sides (along its normal). */
static void line(float x0, float y0, float x1, float y1, float w, uint32_t color)
{
    float dx = x1 - x0, dy = y1 - y0, l = vcpe_sqrt(dx * dx + dy * dy);
    if (l <= 0) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    float xy[8] = {x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny};
    uint32_t c[4] = {color, color, color, color};
    vcpe_strip(xy, c, 4, 0);
}

/* Pack a colour the GE way: 0xAABBGGRR. */
static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

static const char *badge_label(int btn)
{
    return btn == VCPE_BTN_L ? "L" : btn == VCPE_BTN_R ? "R" : btn == VCPE_BTN_START ? "START" : "SELECT";
}

static float badge_scale(float r) { return r >= 14 ? 2 : 1; }

float vcpe_hud_button_width(int btn, float r)
{
    if (btn >= VCPE_BTN_L && btn <= VCPE_BTN_SELECT)
        return vcpe_hud_text_width(badge_label(btn), badge_scale(r)) + 2 * r * 0.8f;
    return 2 * r;
}

/* L, R, START and SELECT are drawn as rounded badges with their name in the 8x8 font. */
static float badge(const char *label, float cx, float cy, float r, int alpha)
{
    float s = badge_scale(r), tw = vcpe_hud_text_width(label, s), w = tw + 2 * r * 0.8f, h = 2 * r;
    float x = cx - w / 2, y = cy - r;
    vcpe_fill(x + 1, y, w - 2, h, rgba(40, 40, 46, alpha));
    vcpe_fill(x, y + 1, w, h - 2, rgba(40, 40, 46, alpha));
    vcpe_fill(x + 1, y + 1, w - 2, 1, rgba(120, 120, 130, alpha));
    vcpe_hud_text(cx - tw / 2, cy - 4 * s, label, rgba(235, 235, 240, alpha), s);
    return w;
}

/* Draw one PSP button icon in the PlayStation colours (green triangle, red circle, blue cross, pink square). */
float vcpe_hud_button(int btn, float cx, float cy, float r, int alpha)
{
    if (alpha <= 0) return 0;
    if (alpha > 255) alpha = 255;
    vcpe_math_init();   /* the icons use the sin table (a no-op once it is filled) */
    switch (btn) {
    case VCPE_BTN_L: case VCPE_BTN_R: case VCPE_BTN_START: case VCPE_BTN_SELECT:
        return badge(badge_label(btn), cx, cy, r, alpha);
    }
    disc(cx, cy, r, rgba(32, 32, 38, alpha));
    float w = r * 0.22f + 0.6f, k = r * 0.48f;
    switch (btn) {
    case VCPE_BTN_CROSS: {
        uint32_t c = rgba(124, 178, 232, alpha);
        line(cx - k, cy - k, cx + k, cy + k, w, c);
        line(cx - k, cy + k, cx + k, cy - k, w, c);
        break;
    }
    case VCPE_BTN_CIRCLE: ring(cx, cy, r * 0.6f, w, rgba(255, 102, 102, alpha)); break;
    case VCPE_BTN_SQUARE: {
        uint32_t c = rgba(255, 105, 248, alpha);
        float a = r * 0.48f;
        vcpe_fill(cx - a, cy - a, 2 * a, w, c);
        vcpe_fill(cx - a, cy + a - w, 2 * a, w, c);
        vcpe_fill(cx - a, cy - a, w, 2 * a, c);
        vcpe_fill(cx + a - w, cy - a, w, 2 * a, c);
        break;
    }
    case VCPE_BTN_TRIANGLE: {
        uint32_t c = rgba(64, 226, 160, alpha);
        float t = r * 0.58f, x0 = cx, y0 = cy - t, x1 = cx - t * 0.87f, y1 = cy + t * 0.5f, x2 = cx + t * 0.87f;
        line(x0, y0, x1, y1, w, c);
        line(x1, y1, x2, y1, w, c);
        line(x2, y1, x0, y0, w, c);
        break;
    }
    default: {   /* d-pad arrows */
        static const float dir[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        int d = btn - VCPE_BTN_UP;
        if (d < 0 || d > 3) break;
        float fx = dir[d][0], fy = dir[d][1], t = r * 0.55f;
        float p[6] = {cx + fx * t, cy + fy * t, cx - fy * t - fx * t * 0.5f, cy + fx * t - fy * t * 0.5f,
                      cx + fy * t - fx * t * 0.5f, cy - fx * t - fy * t * 0.5f};
        convex(p, 3, rgba(235, 235, 240, alpha));
    }
    }
    return 2 * r;
}

/* Icon followed by a label, e.g. "(X) JUMP"; returns the width so prompts can be laid out in a row. */
float vcpe_hud_prompt(float x, float y, int btn, const char *label, uint32_t color)
{
    float r = 6, w = vcpe_hud_button_width(btn, r);
    vcpe_hud_button(btn, x + w / 2, y + r, r, (int)(color >> 24));
    vcpe_hud_text_shadow(x + w + 4, y + r - 4, label, color, 1);
    return w + 4 + vcpe_hud_text_width(label, 1);
}

/* Optional shape helpers; ordinary text and prompt rendering stays allocation-free. */
#include <math.h>
#include <string.h>
static char toast_text[128];
static float toast_left;
void vcpe_hud_ring(float cx,float cy,float radius,float thick,float start,float sweep,uint32_t color)
{
    if(!isfinite(radius)||!isfinite(thick)||radius<=0||thick<=0||thick>radius||sweep==0)return;
    int steps=(int)(fabsf(sweep)/8)+1;if(steps>64)steps=64;
    float xy[130*2];uint32_t colors[130];
    for(int i=0;i<=steps;i++) {
        float a=(start+sweep*(float)i/steps)*(3.14159265358979323846f/180.0f);
        float c=cosf(a),s=sinf(a);int j=i*2;
        xy[j*2]=cx+c*radius;xy[j*2+1]=cy+s*radius;
        xy[(j+1)*2]=cx+c*(radius-thick);xy[(j+1)*2+1]=cy+s*(radius-thick);
        colors[j]=colors[j+1]=color;
    }
    vcpe_strip(xy,colors,(steps+1)*2,0);
}
void vcpe_hud_round_rect(float x,float y,float w,float h,float r,uint32_t color)
{
    if(w<=0||h<=0)return;
    r=fmaxf(0,fminf(r,fminf(w,h)*.5f));
    if(r<.5f){vcpe_fill(x,y,w,h,color);return;}
    int rows=(int)ceilf(r);if(rows<1)rows=1;
    for(int i=0;i<rows;i++) {
        float dy=r-(i+.5f),dx=r-sqrtf(fmaxf(0,r*r-dy*dy));
        vcpe_fill(x+dx,y+i,w-2*dx,1,color);
        vcpe_fill(x+dx,y+h-i-1,w-2*dx,1,color);
    }
    vcpe_fill(x,y+r,w,h-2*r,color);
}
void vcpe_hud_bar(float x,float y,float w,float h,float v,uint32_t back,uint32_t fill)
{
    if(w<=0||h<=0)return;
    vcpe_fill(x,y,w,h,back);
    vcpe_fill(x,y,w*fmaxf(0,fminf(1,v)),h,fill);
}
void vcpe_hud_toast_set(const char *s,float seconds)
{
    snprintf(toast_text,sizeof toast_text,"%s",s?s:"");toast_left=isfinite(seconds)&&seconds>0?seconds:0;
}
void vcpe_hud_toast_tick(float dt) { if(isfinite(dt)&&dt>0)toast_left=fmaxf(0,toast_left-dt); }
void vcpe_hud_toast_draw(float x,float y,float maxw)
{
    if(!toast_left||!toast_text[0])return;
    float w=fminf(maxw,vcpe_hud_text_width(toast_text,1)+16);
    vcpe_hud_round_rect(x,y,w,20,4,0xD0101010);vcpe_hud_text_shadow(x+8,y+6,toast_text,0xFFFFFFFF,1);
}
