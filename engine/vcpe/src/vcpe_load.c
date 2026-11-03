/* VCPE loading screen (see vcpe_load.h). */

/* HOW THIS FILE WORKS
 *
 * Loading normally blocks the main thread: while a 10 MB level is read, nothing draws and the screen freezes. This
 * module draws frames *from inside* the load instead. Between vcpe_loading_begin and vcpe_loading_end it installs two
 * hooks:
 * - vcpe_pak_progress: the PAK reader calls it after every 256 KB chunk;
 * - vcpe_step_hook: engine modules (and the game) report what they are doing, one line per step. Each hook calls
 *   vcpe_loading_tick(), which draws a complete frame (begin, screen, end, present) if enough time passed since the
 *   last one. So the screen animates as long as the load makes progress, with no extra thread.
 *
 * The screen appears only after show_after_us, so quick loads never flash a loading screen. */
#include <pspgu.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vcpe_plat.h"
#include "vcpe_sys.h"
#include "vcpe_test.h"
#include "vcpe_gfx.h"
#include "vcpe_pak.h"
#include "vcpe_hud.h"
#include "vcpe_load.h"
#include "vcpe_log.h"
/* Weak: the contract monitor logs only when the game links vcpe_log (older module lists do not). */
int vcpe_log_is_open(void) __attribute__((weak));
void vcpe_log(const char *fmt, ...) __attribute__((weak, format(printf, 1, 2)));
#include "vcpe_version.h"
#include "vcpe_loadimg.h"   /* tools/mkloadimg.py: engineassets/hetalia-czechia.gif */
#ifdef VCPE_LOAD_BOOT_ANIM
#include "vcpe_loadimg_boot.h"   /* tools/mkloadimg.py ... boot: engineassets/wow.gif */
#endif

static int show_after_us = 150000;
/* Each redraw waits for a vblank (up to 16.7 ms), so redrawing less often lets the load finish sooner. */
static int redraw_us = 50000;   /* 4.0: was 33 ms; each redraw waits for a vblank, so fewer redraws load faster */

static int active, shown;
void vcpe_adapt_loading(int active) __attribute__((weak));
void vcpe_diag_loading(int active) __attribute__((weak));
void vcpe_adapt_loading_tick(void) __attribute__((weak));
static uint32_t total, done;
static int64_t t_begin, t_last;
static int64_t t_adapt;
static void (*hook)(float progress, int ms);
static const char *word = "LOADING";

/* the boot log: the last steps reported (vcpe_step_*), newest at the bottom */
/* Retain history independently of the visible height. */
#define LOG_LINES 24
#define LOG_CHARS 58
typedef struct { int id, status; char text[LOG_CHARS + 1]; } LogLine;
static LogLine log_lines[LOG_LINES];
static int log_n;   /* lines used */
static int show_log = 1;
static int visible_lines = 8;
static int assert_loads;
static uint32_t assert_violations,assert_bytes;
void vcpe_load_assert_enable(int on) { assert_loads=!!on; }
void vcpe_load_assert_reset(void) { assert_violations=assert_bytes=0; }
uint32_t vcpe_load_assert_violations(void) { return assert_violations; }
uint32_t vcpe_load_assert_bytes(void) { return assert_bytes; }
void vcpe_load_assert_observe(uint32_t bytes) {
    if(!assert_loads) return;
    assert_bytes+=bytes;
    if(!active) {
        ++assert_violations;
        if(vcpe_log_is_open && vcpe_log_is_open()) vcpe_log("LOAD CONTRACT: %u byte PAK read outside vcpe_loading_begin/end",(unsigned)bytes);
    }
}

void vcpe_loading_show_log(int on) { show_log = on; }
void vcpe_loading_log_lines(int lines) { visible_lines = lines < 1 ? 1 : lines > LOG_LINES ? LOG_LINES : lines; }
int vcpe_loading_log_height(void) { return visible_lines; }

/* In autotest runs, finished steps also go to stdout with their result, for test logs. */
static void log_print(const LogLine *l)
{
    static const char *tag[] = {"[ .... ]", "[  OK  ]", "[FAILED]", "[ INFO ]"};
    if (vcpe_autotest)
        printf("%s %s\n", tag[l->status], l->text);
}

/* A new step (text given) adds a line; a finished step (text 0) only updates the status of its line. */
static void step_hook(int id, int status, const char *text)
{
    if (text) {
        if (log_n == LOG_LINES) {
            memmove(log_lines, log_lines + 1, sizeof(LogLine) * (LOG_LINES - 1));
            log_n--;
        }
        LogLine *l = &log_lines[log_n++];
        l->id = id;
        l->status = status;
        snprintf(l->text, sizeof l->text, "%s", text);
        if (status != VCPE_STEP_RUN)
            log_print(l);
    } else {
        for (int i = log_n - 1; i >= 0; i--)
            if (log_lines[i].id == id) {
                log_lines[i].status = status;
                log_print(&log_lines[i]);
                break;
            }
    }
    vcpe_loading_tick();
}

/* what the engine is doing, one line per step ("Loading textures 3: 28 pages, 7196 KB"), top-left like a PC
 * booting. Plain text: the results only go to stdout in autotest runs */
/* The log is drawn in the 8x8 HUD font, top-left, one line every 9 pixels. */
void vcpe_loading_draw_log(int ms)
{
    (void)ms;
    static const char *tag[] = {"[ .... ]", "[  OK  ]", "[FAILED]", "[ INFO ]"};
    static const uint32_t color[] = {0xFFC0C0C0, 0xFF80D080, 0xFF8080FF, 0xFFC0C0C0};
    int first = log_n > visible_lines ? log_n - visible_lines : 0;
    for (int i = first; i < log_n; i++) {
        char line[59];
        int status = log_lines[i].status;
        if (status < 0 || status > 3) status = VCPE_STEP_INFO;
        snprintf(line, sizeof line, "%s %.49s", tag[status], log_lines[i].text);
        vcpe_hud_text(8, 8 + (i - first) * 9, line, color[status], 1);
    }
}

void vcpe_loading_draw_hook(void (*draw)(float progress, int ms)) { hook = draw; }
void vcpe_loading_text(const char *text) { word = text ? text : "LOADING"; }
int vcpe_loading_active(void) { return active; }
void vcpe_loading_show_after(int ms) { show_after_us = ms * 1000; }
void vcpe_loading_redraw_ms(int ms) { redraw_us = (ms < 16 ? 16 : ms) * 1000; }
int vcpe_loading_shown(void) { return shown; }

/* The animation: frames of 8-bit indexed pixels sharing one palette (src/vcpe_loadimg.h, built from
 * engineassets/hetalia-czechia.gif by tools/mkloadimg.py). Size is the drawn width; height keeps its aspect ratio.
 * 5.6 memory: the frames are stored LZ4-compressed (each one on its own or as the XOR with the previous frame; 880 KB
 * of raw pixels for both animations became about 300 KB). The picture shown is decoded into a buffer that exists only
 * while a loading screen is up: two frames (the GE may still read the one drawn last) plus one scratch frame, about
 * 48 KB, freed by vcpe_loading_end. Pixels are byte-identical to the raw form. */
typedef struct {
    const unsigned short *durs;
    const unsigned int *clut;
    const unsigned int *off;      /* frames + 1 byte offsets into lz; bit 31: XOR with the previous frame */
    const unsigned char *lz;
    int frames, frame_bytes, tex_w, tex_h, w, h, loop_ms;
} LoadAnim;
static LoadAnim anim_game = { vcpe_loadimg_ms, vcpe_loadimg_clut, vcpe_loadimg_off, vcpe_loadimg_lz, VCPE_LOADIMG_FRAMES,
                              VCPE_LOADIMG_FRAME_BYTES, VCPE_LOADIMG_TEX_W, VCPE_LOADIMG_TEX_H, VCPE_LOADIMG_WIDTH,
                              VCPE_LOADIMG_HEIGHT, 0 };
static LoadAnim *anim_now = &anim_game;
#ifdef VCPE_LOAD_BOOT_ANIM
static LoadAnim anim_boot = { vcpe_bootimg_ms, vcpe_bootimg_clut, vcpe_bootimg_off, vcpe_bootimg_lz, VCPE_BOOTIMG_FRAMES,
                              VCPE_BOOTIMG_FRAME_BYTES, VCPE_BOOTIMG_TEX_W, VCPE_BOOTIMG_TEX_H, VCPE_BOOTIMG_WIDTH,
                              VCPE_BOOTIMG_HEIGHT, 0 };
void vcpe_loading_boot_anim(int on) { anim_now = on ? &anim_boot : &anim_game; }
#else
void vcpe_loading_boot_anim(int on) { (void)on; }
#endif

/* the LZ4 block decoder (same format as vcpe_video; a private copy so vcpe_load needs no other module) */
static int lz4_block(const uint8_t *s, int n, uint8_t *dst, int cap)
{
    const uint8_t *e = s + n;
    uint8_t *d = dst, *de = dst + cap;
    while (s < e) {
        int tok = *s++, lit = tok >> 4;
        if (lit == 15) {
            int b;
            do { if (s >= e) return -1; b = *s++; lit += b; } while (b == 255);
        }
        if (lit > e - s || lit > de - d) return -1;
        memcpy(d, s, lit); d += lit; s += lit;
        if (s >= e) break;
        if (e - s < 2) return -1;
        int off = s[0] | s[1] << 8; s += 2;
        int len = tok & 15;
        if (len == 15) {
            int b;
            do { if (s >= e) return -1; b = *s++; len += b; } while (b == 255);
        }
        len += 4;
        if (off == 0 || off > d - dst || len > de - d) return -1;
        const uint8_t *m = d - off;
        if (off >= len) { memcpy(d, m, len); d += len; }
        else while (len--) *d++ = *m++;
    }
    return (int)(d - dst);
}

static uint8_t *anim_mem;           /* 3 frames: shown A, shown B, scratch */
static int anim_mem_bytes, anim_cur, anim_have = -1;
static const LoadAnim *anim_of;

static void anim_release(void)
{
    if (!anim_mem)
        return;
    vcpe_gfx_flush();
    vcpe_gfx_gpu_wait();
    sceGuSync(0, 0);                /* the GE may still be reading the last frame shown */
    free(anim_mem);
    anim_mem = 0;
    anim_mem_bytes = 0;
    anim_have = -1;
    anim_of = 0;
}
void vcpe_loading_anim_release(void) { anim_release(); }

/* Frame f's pixels, decoded on demand: forward from the frame shown last, or from frame 0 (always stored on its own)
 * after a wrap. *fresh = 1 when the buffer changed (the texture cache must be refreshed). 0 if out of memory. */
static const uint8_t *anim_frame(const LoadAnim *a, int f, int *fresh)
{
    int fb = a->frame_bytes;
    *fresh = 0;
    if (anim_of == a && anim_have == f)
        return anim_mem + anim_cur * fb;
    if (anim_mem_bytes < 3 * fb) {
        anim_release();
        anim_mem = memalign(64, 3 * fb);
        if (!anim_mem)
            return 0;
        anim_mem_bytes = 3 * fb;
    }
    if (anim_of != a)
        anim_have = -1;
    int next = anim_have < 0 ? 0 : 1 - anim_cur, k = anim_have + 1;
    uint8_t *dst = anim_mem + next * fb, *tmp = anim_mem + 2 * fb;
    if (anim_have < 0 || f < anim_have)
        k = 0;
    else
        memcpy(dst, anim_mem + anim_cur * fb, fb);
    for (; k <= f; k++) {
        uint32_t o0 = a->off[k], o1 = a->off[k + 1] & 0x7FFFFFFFu, at = o0 & 0x7FFFFFFFu;
        int xr = (o0 >> 31) && k > 0;
        if (lz4_block(a->lz + at, (int)(o1 - at), xr ? tmp : dst, fb) != fb) {
            anim_have = -1;
            return 0;
        }
        if (xr)
            for (int i = 0; i < fb; i += 4)
                *(uint32_t *)(dst + i) ^= *(const uint32_t *)(tmp + i);
    }
    vcpe_cache_writeback(dst, fb);
    anim_of = a;
    anim_have = f;
    anim_cur = next;
    *fresh = 1;
    return dst;
}

void vcpe_loading_anim(float x, float y, float size, int ms)
{
    LoadAnim *a = anim_now;
    if (!a->loop_ms)
        for (int i = 0; i < a->frames; i++)
            a->loop_ms += a->durs[i];
    int t = (ms < 0 ? 0 : ms) % a->loop_ms, f = 0;
    while (f < a->frames - 1 && t >= a->durs[f])
        t -= a->durs[f++];
    int fresh;
    const uint8_t *img = anim_frame(a, f, &fresh);
    if (!img)
        return;   /* no memory for a 48 KB buffer: the screen still shows the log, the word and the bar */
    vcpe_blend(0);
    vcpe_tex_white(0);
    vcpe_filter(1);
    if (fresh || !vcpe_tex_bound(img)) {
        vcpe_tex_claim(img);
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, a->clut);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuTexImage(0, a->tex_w, a->tex_h, a->tex_w, img);
        vcpe_tex_flush();
    }
    vcpe_quad(x, y, x + size, y + size * a->h / a->w, 0, 0, a->w, a->h, 0xFFFFFFFF);
    vcpe_gfx_flush();
}

/* the standard screen: the boot log top-left; the animation in the bottom-right corner with the word and the
 * progress bar just above it */
/* The standard screen: log top-left, animation bottom-right, the word with 0-3 animated dots and the bar above it. */
void vcpe_loading_draw_standard(float p, int ms)
{
    const float m = 12, sz = anim_now->w;
    const char *credit = "made with VCPE " VCPE_VERSION;
    float ax = VCPE_SCR_W - m - sz;
    float ay = VCPE_SCR_H - m - 12 - anim_now->h;
    if (show_log)
        vcpe_loading_draw_log(ms);
    vcpe_loading_anim(ax, ay, sz, ms);
    vcpe_hud_text(VCPE_SCR_W - m - vcpe_hud_text_width(credit, 1), VCPE_SCR_H - m - 8,
                   credit, 0xFFC0C0C0, 1);
    char s[40];
    int dots = (ms / 300) % 4;
    snprintf(s, sizeof s, "%s%.*s", word, dots, "...");
    float y = ay - (p >= 0 ? 16 : 12);
    vcpe_hud_text(ax, y, s, 0xFFC0C0C0, 1);
    if (p >= 0) {
        vcpe_fill(ax, ay - 6, sz, 2, 0xFF303030);
        vcpe_fill(ax, ay - 6, sz * (p > 1 ? 1 : p), 2, 0xFFC0C0C0);
    }
}

/* One complete frame of the loading screen, shown immediately. */
static void draw(int64_t now)
{
    float p = total ? (float)done / (float)total : -1.0f;
    int ms = (int)((now - t_begin) / 1000);
    vcpe_gfx_begin(0xFF000000);
    if (hook)
        hook(p, ms);
    else
        vcpe_loading_draw_standard(p, ms);
    vcpe_gfx_end();
    vcpe_gfx_present();
    shown++;
    t_last = now;
}

/* Draw if one is due. Never inside a game frame: the display list is shared, and a load started mid-frame would
 * otherwise begin a second frame inside the first. */
void vcpe_loading_tick(void)
{
    if (!active || vcpe_gfx_in_frame())
        return;
    int64_t now = (int64_t)vcpe_time_us();
    if (vcpe_adapt_loading_tick && now - t_adapt >= 50000) {
        t_adapt = now;
        vcpe_adapt_loading_tick();
    }
    if (now - t_begin >= show_after_us && (!shown || now - t_last >= redraw_us))
        draw(now);
}

static void pak_progress(uint32_t bytes)
{
    done += bytes;
    vcpe_loading_tick();
}

void vcpe_loading_add(uint32_t bytes)
{
    done += bytes;
    vcpe_loading_tick();
}

/* Start: remember the expected bytes for the bar, clear the log and install the hooks. */
void vcpe_loading_begin(uint32_t total_bytes)
{
    active = 1;
    if (vcpe_adapt_loading) vcpe_adapt_loading(1);
    if (vcpe_diag_loading) vcpe_diag_loading(1);
    shown = 0;
    total = total_bytes;
    done = 0;
    t_begin = (int64_t)vcpe_time_us();
    t_adapt = t_begin;
    log_n = 0;
    vcpe_pak_progress = pak_progress;
    vcpe_step_hook = step_hook;
}

/* Finish: if the screen was shown, draw it once more in its final state (full bar), then remove the hooks. */
void vcpe_loading_end(void)
{
    if (!active)
        return;
    /* a screen that was shown ends on its last state: a full bar, every step's result */
    if (shown) {
        if (total)
            done = total;
        draw((int64_t)vcpe_time_us());
    }
    active = 0;
    anim_release();   /* 5.6: the decoded animation frames exist only during a load */
    vcpe_pak_progress = 0;
    vcpe_step_hook = 0;
    if (vcpe_adapt_loading) vcpe_adapt_loading(0);
    if (vcpe_diag_loading) vcpe_diag_loading(0);
}
