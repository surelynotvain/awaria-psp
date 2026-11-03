/* VCPE graphics core: GU setup, frames, texture bundles and pages, cached render state, primitives. */

/* HOW THIS FILE WORKS (for people new to the PSP)
 *
 * The PSP draws with the GE ("Graphics Engine"), a fixed-function chip that reads a *display list*: a block of 32-bit
 * commands in RAM ("set texture", "set blend mode", "draw these vertices"). The CPU writes commands into the list
 * with the sceGu* functions, and the GE executes them. With GU_DIRECT (used here) the GE starts executing while the
 * CPU is still writing, so the list is a queue the two share.
 *
 * A frame therefore goes: vcpe_gfx_begin() starts a new list and clears the screen, the game calls vcpe_quad /
 * vcpe_fill / ... which append commands, vcpe_gfx_end() closes the list, waits for the GE to finish and swaps the two
 * frame buffers (one is shown while the other is drawn: double buffering).
 *
 * Textures live in "bundles": groups of pages read from the game's data PAK in one go. A page is one GE texture,
 * usually 4- or 8-bit indexed (paletted, with a CLUT = colour look-up table), "swizzled" (re-ordered in 16x8 blocks
 * so the GE's texture cache reads it faster).
 *
 * Most functions here keep a small cache of what the GE currently has set (bound page, blend mode, filter...) and
 * skip the sceGu* call when nothing changes: every command costs list space and GE time. */
#include <pspkernel.h>
#include "vcpe_observe.h"
#include <pspdisplay.h>
#include <pspgu.h>
#include <malloc.h>
#include <string.h>
#include "vcpe_sys.h"
#include "vcpe_pak.h"
#include "vcpe_gfx.h"
int vcpe_volatile_suspended(void) __attribute__((weak));

/* The frame buffer is 480x272 visible pixels, but the GE wants a power-of-two stride, so each row is 512 pixels wide
 * in memory. FB_SIZE is one 32-bit (8888) buffer in VRAM; the second buffer sits right after it. */
#define BUF_W 512
#define FB_SIZE (BUF_W * VCPE_SCR_H * 4)
/* Fixed-size tables per bundle: no allocation, and the limit is a compile flag. */
#ifndef MAX_BUNDLES
#define MAX_BUNDLES 64   /* games with more texture bundles pass -DMAX_BUNDLES=n (page tables hold up to 255) */
#endif
#ifndef VCPE_DLIST_KB
#define VCPE_DLIST_KB 512   /* display list per frame; -DVCPE_DLIST_KB=n trims it (PSP-1000) or grows it */
#endif
#define DLIST_BYTES (VCPE_DLIST_KB * 1024)
/* vcpe_gfx_mem refuses vertex memory once less than this is left, so the state commands that follow (blend,
 * texture...) still fit. */
#define DLIST_RESERVE (16 * 1024)   /* kept free for state commands (4 bytes each) after the last vertex block */

/* The display list. Static (BSS) so it never moves; 64-byte aligned because the GE reads memory in cache lines. It is
 * reused every frame: sceGuStart rewinds it. */
static unsigned int __attribute__((aligned(64))) dlist[DLIST_BYTES / 4];
/* Page and bundle tables are written by the game's converter and handed over with vcpe_gfx_tables(). The engine never
 * copies them, so they must stay alive. */
static const VcpePage *pages;
static const VcpeBundle *bundles;
static int nbundles, npages;
/* bundle_mem[b]: where bundle b's pages are in RAM, 0 when not loaded. */
static uint8_t *bundle_mem[MAX_BUNDLES];
static uint8_t bundle_attached[MAX_BUNDLES];   /* game memory: not ours to free */
static uint8_t bundle_reload[MAX_BUNDLES];     /* placed in memory the system may take back while asleep */
/* reload_gen: the vcpe_resume_gen value the bundles were last (re)loaded at. When the console wakes from sleep,
 * vcpe_sys bumps vcpe_resume_gen; a mismatch here means "volatile bundles must be read again". */
static int reload_gen, reload_tries;
/* The slab: one block shared by a range of bundles (vcpe_bundle_slab). Only one of them is loaded at a time, so a
 * level switch frees and reuses the same memory and the heap cannot fragment around it. */
static uint8_t *slab;                          /* vcpe_bundle_slab: one block shared by a range of bundles */
static uint32_t slab_size;
static int slab_first = -1, slab_last = -2;
static int vsync = 1;
/* Pipelining (opt-in): vcpe_gfx_end returns without waiting for the GE, so the CPU can run the game's next update
 * while the GE draws. pending = 1: a list is on the GE; 2: the GE finished, but the frame is not shown yet. */
static int pipeline, pending;     /* pipeline: a finished list is queued on the GE, not shown yet */
static int bench_frames;          /* vcpe_gfx_bench: frames still drawn synchronously */
/* Batching (opt-in): consecutive rectangles are collected in these CPU arrays and sent as one draw call
 * (vcpe_gfx_flush). Anything that changes GE state must flush first, or the batch would be drawn with the new state.
 */
static int batch_on, batch_kind, batch_n;   /* batch_kind 1: textured sprites (VcpeVtx), 2: colour sprites (VcpeCVtx) */
#define BATCH_MAX 1024            /* vertices (two per rectangle) */
/* A kind change flushes the current batch before writing the next one. These staging arrays never hold live
 * vertices at the same time, so sharing storage saves 16 KB of BSS with the default vertex count. */
static union { VcpeVtx t[BATCH_MAX]; VcpeCVtx c[BATCH_MAX]; } batch;
/* The state cache: what the GE has set right now. -1/-2 mean "unknown" and force the next call to send the command.
 * Every new frame and vcpe_gfx_state_reset() set them back to unknown. */
static int bound_page = -2, bound_mode = -1, bound_white = -1, bound_filter = -1, opaque;
/* 4.0: a texture bound by another module (HUD font, glyph page...), identified by any pointer the module chooses.
 * Lets those modules skip re-sending the same texture for every string they draw. */
static const void *bound_tex;     /* vcpe_tex_claim: the module texture bound now (0: none / unknown) */
/* The GE has a small texture cache. If the CPU writes new texels to memory the GE has cached, the GE could draw stale
 * pixels, so sceGuTexFlush() empties that cache. upload_gen counts "texels were written", flushed_gen remembers the
 * count at the last flush; lazy mode skips flushes when nothing new was written. */
static int lazy_flush, upload_gen, flushed_gen = -1;
static uint32_t draw_offset;   /* VRAM offset of the buffer being drawn this frame */
/* Vblank counter at the last shown frame (for vcpe_stat_vblanks / vcpe_dt). */
static unsigned last_vcount;

uint32_t vcpe_gfx_draw_offset(void) { return draw_offset; }
/* Call after drawing with raw sceGu* calls: the cache no longer knows what is set, so forget it all. */
void vcpe_gfx_state_reset(void)
{
    vcpe_gfx_flush();
    bound_page = -2; bound_mode = -1; bound_white = -1; bound_filter = -1;
    bound_tex = 0;
}
/* Frame counter. Caches compare against it to know whether something was used *this* frame (and so may still be read
 * by the GE). */
uint32_t vcpe_gfx_frame;   /* frames begun (caches that must not change memory the current frame uses) */
int vcpe_stat_binds, vcpe_stat_quads, vcpe_stat_draws, vcpe_stat_fps, vcpe_stat_build_us, vcpe_stat_gpu_wait_us;
int vcpe_stat_ge_us, vcpe_stat_vblanks = 1, vcpe_stat_vblank_misses, vcpe_stat_dlist_kb, vcpe_stat_dlist_peak_kb;
int vcpe_stat_dlist_full, vcpe_stat_reload_fail;
int vcpe_bind_epoch;   /* bumped whenever this module changes the bound texture / CLUT (other binders compare it) */
float vcpe_stat_area;

/* One-time GE setup. The GE's state persists between frames, so everything set here (2D defaults: no depth test,
 * alpha blending, scissor to the screen) stays until someone changes it. */
int vcpe_gfx_init(void)
{
    void *draw_fb = (void *)0, *disp_fb = (void *)FB_SIZE;
    sceGuInit();
    sceGuStart(GU_DIRECT, dlist);
    sceGuDrawBuffer(GU_PSM_8888, draw_fb, BUF_W);
    sceGuDispBuffer(VCPE_SCR_W, VCPE_SCR_H, disp_fb, BUF_W);
    /* The GE works in a 4096x4096 virtual space; the screen is placed in its middle. These two lines map virtual
     * (2048,2048) to the screen centre, which is the standard PSP setup. */
    sceGuOffset(2048 - (VCPE_SCR_W / 2), 2048 - (VCPE_SCR_H / 2));
    sceGuViewport(2048, 2048, VCPE_SCR_W, VCPE_SCR_H);
    sceGuScissor(0, 0, VCPE_SCR_W, VCPE_SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    /* Normal alpha blending: result = src * a + dst * (1 - a). */
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    /* Texel colour times vertex colour: lets one white sprite be tinted and faded by its vertex colour. */
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuShadeModel(GU_SMOOTH);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    /* Start showing the frame buffer only after the first vblank, so the screen doesn't flash garbage. */
    sceGuDisplay(GU_TRUE);
    last_vcount = sceDisplayGetVcount();
    return 0;
}

void vcpe_gfx_shutdown(void)
{
    vcpe_gfx_pipeline(0);
    sceGuTerm();
    vcpe_pak_close();
}

/* The game hands over the converter's tables. The highest page index any bundle covers is also the length of the page
 * table, which bounds-checks later page lookups. */
void vcpe_gfx_tables(const VcpePage *p, const VcpeBundle *b, int nb)
{
    pages = p;
    bundles = b;
    nbundles = nb > MAX_BUNDLES ? MAX_BUNDLES : nb;
    /* the bundles list every page: the table's length bounds vcpe_bind_page / vcpe_page_size */
    npages = 0;
    for (int i = 0; i < nbundles; i++)
        if (b[i].first_page + b[i].npages > npages)
            npages = b[i].first_page + b[i].npages;
}

/* display-list memory for this frame; 0 when the list is full (the draw is dropped and counted, instead of writing
 * past the end of the list into whatever the linker put after it) */
/* Vertex memory for this frame. sceGuGetMemory reserves space inside the display list itself (the GE skips over it
 * with a jump command) and never checks the end of the list, so without this guard a very busy frame would write past
 * the list into other variables. */
int vcpe_gfx_room(int size)
{
    if (size < 0 || sceGuCheckList() + size + 8 > DLIST_BYTES - DLIST_RESERVE) {
        vcpe_stat_dlist_full++;
        return 0;
    }
    return 1;
}
void *vcpe_gfx_mem(int size)
{
    if (!vcpe_gfx_room(size)) return 0;
    return sceGuGetMemory(size);
}

static SceInt64 frame_t0, fps_t0;
static int fps_frames;
int vcpe_stat_frame_us;   /* last frame: vcpe_gfx_begin until the GPU finished (vblank wait not included) */

/* ------------------------------------------------------------------ batches */
/* Send the pending batch: copy the collected vertices into the display list and issue one draw call. GU_SPRITES takes
 * two vertices per rectangle (top-left and bottom-right corners), so N rectangles = 2N vertices. */
void vcpe_gfx_flush(void)
{
    if (!batch_n)
        return;
    if (batch_kind == 1) {
        VcpeVtx *v = vcpe_gfx_mem(batch_n * sizeof(VcpeVtx));
        if (v) {
            memcpy(v, batch.t, batch_n * sizeof(VcpeVtx));
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, batch_n, 0, v);
        }
    } else {
        VcpeCVtx *v = vcpe_gfx_mem(batch_n * sizeof(VcpeCVtx));
        if (v) {
            memcpy(v, batch.c, batch_n * sizeof(VcpeCVtx));
            /* Fills are untextured: switch texturing off for this one draw, then back on. */
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, batch_n, 0, v);
            sceGuEnable(GU_TEXTURE_2D);
        }
    }
    vcpe_stat_draws++;
    batch_n = 0;
}

void vcpe_gfx_batch(int on)
{
    vcpe_gfx_flush();
    batch_on = on;
}

/* ------------------------------------------------------------------ frames */
/* Frames per second, re-measured every 30 frames or every second, whichever comes first. */
static void fps_tick(void)
{
    SceInt64 t = sceKernelGetSystemTimeWide();
    if (++fps_frames >= 30 || t - fps_t0 > 1000000) {
        vcpe_stat_fps = (int)((fps_frames * 1000000LL + (t - fps_t0) / 2) / (t - fps_t0 + 1));
        fps_frames = 0;
        fps_t0 = t;
    }
}

/* a frame was shown: count the vblanks since the last one (more than one = a missed vblank) */
/* Runs after every swap. The display counts vblanks (60 per second); if more than one passed since the last swap, the
 * game missed a vblank and that frame stayed on screen twice (the "30 fps cliff"). */
void vcpe_adapt_frame(uint32_t elapsed_us) __attribute__((weak));
void vcpe_diag_frame(uint32_t elapsed_us) __attribute__((weak));
extern int vcpe_stream_underruns __attribute__((weak));
static void shown(void)
{
    static SceInt64 adapt_last;
    if (vcpe_adapt_frame || vcpe_diag_frame) {
        SceInt64 now = sceKernelGetSystemTimeWide();
        static uint32_t audio_previous;
        if (&vcpe_stream_underruns) {
            uint32_t current=(uint32_t)vcpe_stream_underruns;
            vcpe_observe_cost(VCPE_COST_AUDIO,current-audio_previous); audio_previous=current;
        }
        vcpe_observe_cost(VCPE_COST_RENDER,(uint32_t)vcpe_stat_build_us);
        vcpe_observe_cost(VCPE_COST_GPU,(uint32_t)vcpe_stat_gpu_wait_us);
        if (adapt_last) {
            if (vcpe_diag_frame) vcpe_diag_frame((uint32_t)(now-adapt_last));
            if (vcpe_adapt_frame) vcpe_adapt_frame((uint32_t)(now-adapt_last));
        }
        adapt_last = now;
    }
    unsigned vc = sceDisplayGetVcount();
    int d = (int)(vc - last_vcount);
    last_vcount = vc;
    vcpe_stat_vblanks = d > 0 ? d : 1;
    if (vsync && d > 1)
        vcpe_stat_vblank_misses += d - 1;
    fps_tick();
}

/* Frame time from vblanks instead of the system clock: the clock jitters with thread scheduling, vblanks don't.
 * Autotest runs always get exactly one vblank so recordings repeat frame for frame. */
float vcpe_dt(void)
{
    extern int vcpe_autotest __attribute__((weak));
    if (&vcpe_autotest && vcpe_autotest)
        return 1.0f / 59.94f;   /* recordings: every frame is one vblank */
    int n = vcpe_stat_vblanks < 1 ? 1 : vcpe_stat_vblanks > 6 ? 6 : vcpe_stat_vblanks;   /* clamp after a stall */
    return n * (1.0f / 59.94f);
}

static void ge_done(SceInt64 t)
{
    vcpe_stat_gpu_wait_us = (int)(sceKernelGetSystemTimeWide() - t);
    vcpe_stat_frame_us = vcpe_stat_build_us + vcpe_stat_gpu_wait_us;
}

/* Pipelining: block until the GE has drawn the frame that was handed over (but do not show it yet). */
void vcpe_gfx_gpu_wait(void)
{
    if (pending == 1) {
        SceInt64 t = sceKernelGetSystemTimeWide();
        sceGuSync(0, 0);
        ge_done(t);
        pending = 2;   /* drawn, not shown */
    }
}

/* Pipelining: show the handed-over frame (GE done, then wait for the vblank, then swap). */
void vcpe_gfx_present(void)
{
    if (!pending)
        return;
    vcpe_gfx_gpu_wait();
    if (vsync)
        sceDisplayWaitVblankStart();
    draw_offset = (uint32_t)(uintptr_t)sceGuSwapBuffers();
    pending = 0;
    shown();
}

int vcpe_gfx_pending(void) { return pending != 0; }

/* Turning pipelining off first shows any pending frame. vcpe_present_hook lets other modules (autotest, fatal error
 * screen) make sure the last frame is shown before they take over. */
void vcpe_gfx_pipeline(int on)
{
    if (!on)
        vcpe_gfx_present();
    pipeline = on;
    vcpe_present_hook = on ? vcpe_gfx_present : 0;   /* autotest reads and fatal screens show the last frame first */
}

void vcpe_gfx_bench(int frames) { bench_frames = frames > 0 ? frames : 0; }

static int in_frame;
int vcpe_gfx_in_frame(void) { return in_frame; }

/* Start a frame: show the previous one if pipelined, re-read volatile bundles after a sleep, open a new display list,
 * clear the screen, and forget the cached state (the GE keeps state between lists, but the cache must not assume
 * anything at a frame boundary). */
void vcpe_gfx_begin(uint32_t clear)
{
    vcpe_gfx_present();
    if (reload_gen != vcpe_resume_gen && (!vcpe_volatile_suspended || !vcpe_volatile_suspended()))
        vcpe_bundles_reload();
    in_frame = 1;
    frame_t0 = sceKernelGetSystemTimeWide();
    vcpe_gfx_frame++;
    vcpe_stat_draws = 0;
    sceGuStart(GU_DIRECT, dlist);
    /* vcpe_opaque(1) left on at the end of the last frame: turn blending back on, every frame starts blended. */
    if (opaque) {   /* a frame never starts with blending off */
        sceGuEnable(GU_BLEND);
        opaque = 0;
    }
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    vcpe_bind_epoch++;
    bound_page = -2;
    bound_mode = -1;
    bound_white = -1;
    bound_filter = -1;
    bound_tex = 0;
}

/* Finish the frame. sceGuFinish closes the list; then either hand it to the GE and return (pipelining) or wait for
 * the GE (sceGuSync), wait for the vblank (no tearing) and swap the buffers. */
void vcpe_gfx_end_overlay(void (*overlay)(void))
{
    in_frame = 0;
    vcpe_gfx_flush();
    sceGuFinish();
    SceInt64 t = sceKernelGetSystemTimeWide();
    vcpe_stat_build_us = (int)(t - frame_t0);
    vcpe_stat_dlist_kb = (sceGuCheckList() + 1023) / 1024;
    if (vcpe_stat_dlist_kb > vcpe_stat_dlist_peak_kb)
        vcpe_stat_dlist_peak_kb = vcpe_stat_dlist_kb;
    if (pipeline && !bench_frames && !overlay) {
        /* the GE draws this frame while the game updates the next one; vcpe_gfx_begin shows it */
        pending = 1;
        return;
    }
    sceGuSync(0, 0);
    ge_done(t);
    if (overlay) {
        overlay();
        sceGuSync(0, 0);
    }
    /* Here nothing overlapped the GE's work, so the wait is the GE's real cost. */
    vcpe_stat_ge_us = vcpe_stat_gpu_wait_us;   /* drawn with nothing overlapping: the GE's own time */
    if (bench_frames)
        bench_frames--;
    if (vsync)
        sceDisplayWaitVblankStart();
    draw_offset = (uint32_t)(uintptr_t)sceGuSwapBuffers();   /* the next frame draws into the other one */
    shown();
}

void vcpe_gfx_end(void) { vcpe_gfx_end_overlay(0); }

void vcpe_gfx_vsync(int on) { vsync = on; }

/* ------------------------------------------------------------------ bundles */
int vcpe_bundle_loaded(int b) { return b >= 0 && b < nbundles && bundle_mem[b] != 0; }
uint8_t *vcpe_bundle_mem(int b) { return b >= 0 && b < nbundles ? bundle_mem[b] : 0; }

static int in_slab(int b) { return slab && b >= slab_first && b <= slab_last; }

/* Load bundle b from the PAK. A bundle in the slab range evicts the other slab bundles first and reuses the block;
 * any other bundle gets its own heap block. */
int vcpe_bundle_load(int b)
{
    if (b < 0 || b >= nbundles)
        return -1;
    if (bundle_mem[b])
        return 0;
    if (in_slab(b)) {
        for (int o = slab_first; o <= slab_last; o++)
            if (o != b)
                vcpe_bundle_unload(o);
        return vcpe_bundle_load_at(b, slab, slab_size, 0);
    }
    int st = vcpe_step_begin("Loading textures %d: %d pages, %u KB", b, bundles[b].npages,
                             (unsigned)(bundles[b].size / 1024));
    bundle_mem[b] = vcpe_pak_read_alloc(bundles[b].off, bundles[b].size);
    vcpe_step_end(st, bundle_mem[b] != 0);
    vcpe_tex_uploaded();
    return bundle_mem[b] ? 0 : -1;
}

/* Load bundle b into memory the caller owns (the slab, the volatile partition, a game buffer). The data cache must be
 * written back afterwards: the CPU wrote through its cache, the GE reads RAM directly. */
int vcpe_bundle_load_at(int b, void *mem, uint32_t cap, int reload)
{
    if (b < 0 || b >= nbundles || !mem || bundles[b].size > cap)
        return -1;
    vcpe_bundle_unload(b);
    int st = vcpe_step_begin("Loading textures %d to %s: %u KB", b,
                             mem == slab ? "block" : reload ? "volatile memory" : "game memory",
                             (unsigned)(bundles[b].size / 1024));
    if (vcpe_pak_read(bundles[b].off, mem, bundles[b].size) < 0) {
        vcpe_step_end(st, 0);
        return -1;
    }
    vcpe_step_end(st, 1);
    sceKernelDcacheWritebackRange(mem, bundles[b].size);
    vcpe_tex_uploaded();
    bundle_mem[b] = mem;
    bundle_attached[b] = 1;
    bundle_reload[b] = reload != 0;
    return 0;
}

/* Reserve one block big enough for the largest bundle of the range. */
int vcpe_bundle_slab(int first, int last)
{
    uint32_t need = 0;
    if (first < 0 || last >= nbundles || first > last)
        return -1;
    for (int b = first; b <= last; b++)
        if (bundles[b].size > need)
            need = bundles[b].size;
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_mem[b] == slab)
            vcpe_bundle_unload(b);
    if (slab && need <= slab_size) {
        /* the block already fits the new range: keep it (freeing and allocating again could fragment the heap) */
        slab_first = first;
        slab_last = last;
        return 0;
    }
    free(slab);
    int st = vcpe_step_begin("Reserving texture block %d-%d: %u KB", first, last, (unsigned)(need / 1024));
    slab = memalign(64, need ? need : 64);
    vcpe_step_end(st, slab != 0);
    slab_size = slab ? need : 0;
    slab_first = slab ? first : -1;
    slab_last = slab ? last : -2;
    return slab ? 0 : -1;
}

/* weak: games that link vcpe_load show its screen while the bundles are read again */
/* Weak symbols: if the game links vcpe_load these point at it, otherwise they are 0 and are not called. This avoids
 * making the graphics core depend on the loading screen. */
void vcpe_loading_begin(uint32_t total_bytes) __attribute__((weak));
void vcpe_loading_end(void) __attribute__((weak));
int vcpe_loading_active(void) __attribute__((weak));

/* 1 when the console slept since the volatile bundles were read, and there is at least one to re-read. */
int vcpe_resume_pending(void)
{
    if (reload_gen == vcpe_resume_gen)
        return 0;
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_reload[b])
            return 1;
    reload_gen = vcpe_resume_gen;   /* nothing to read again */
    return 0;
}

/* the system may have used the volatile memory while the console slept: read those bundles again, behind the
 * loading screen when the game links it. A read that fails is tried again at the next frame (3 times) */
/* Re-read every bundle placed with reload=1. Done between frames so the GE is not reading the memory being
 * overwritten; shows the loading screen if the read takes long. */
int vcpe_bundles_reload(void)
{
    if (vcpe_volatile_suspended && vcpe_volatile_suspended())
        return -1; /* firmware owns the partition until its utility has shut down */
    if (!vcpe_resume_pending())
        return 0;
    if (in_frame) {   /* never inside a frame: the GE may be reading the memory */
        vcpe_gfx_flush();
        sceGuSync(0, 0);
    }
    vcpe_gfx_gpu_wait();
    uint32_t total = 0;
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_reload[b])
            total += bundles[b].size;
    int gen = vcpe_resume_gen, failed = 0;
    reload_gen = gen;   /* first, so a loading screen drawn in between does not start another reload */
    int screen = vcpe_loading_begin && !in_frame && !vcpe_loading_active();
    if (screen)
        vcpe_loading_begin(total);
    for (int b = 0; b < nbundles; b++)
        if (bundle_mem[b] && bundle_reload[b]) {
            int st = vcpe_step_begin("Reloading textures %d after sleep", b);
            int ok = vcpe_pak_read(bundles[b].off, bundle_mem[b], bundles[b].size) == 0;
            vcpe_step_end(st, ok);
            sceKernelDcacheWritebackRange(bundle_mem[b], bundles[b].size);
            if (!ok) {
                failed++;
                vcpe_stat_reload_fail++;
            }
        }
    sceKernelDcacheWritebackAll();
    vcpe_tex_uploaded();
    if (screen)
        vcpe_loading_end();
    if (failed && ++reload_tries < 3)
        reload_gen = gen - 1;   /* try again next frame */
    else
        reload_tries = 0;
    return failed ? -1 : 1;
}

/* Bundle data linked into the EBOOT itself: just point at it, never free it. */
void vcpe_bundle_attach(int b, uint8_t *mem)
{
    if (b < 0 || b >= nbundles)
        return;
    vcpe_bundle_unload(b);
    bundle_mem[b] = mem;
    bundle_attached[b] = mem != 0;
    vcpe_tex_uploaded();
}

/* Free bundle b. The GE might still be drawing from it (current or pipelined frame), so wait for the GE before the
 * memory goes back to the heap. */
void vcpe_bundle_unload(int b)
{
    if (b < 0 || b >= nbundles || !bundle_mem[b])
        return;
    int st = vcpe_step_begin("Unloading textures %d: %u KB", b, (unsigned)(bundles[b].size / 1024));
    vcpe_gfx_flush();
    vcpe_gfx_gpu_wait();
    sceGuSync(0, 0);   /* inside a frame the GE may still be drawing this list (GU_DIRECT) */
    if (!bundle_attached[b])
        free(bundle_mem[b]);
    bundle_attached[b] = 0;
    bundle_reload[b] = 0;
    bundle_mem[b] = 0;
    bound_page = -2;
    vcpe_step_end(st, 1);
}

/* Scene switch helper: unload everything not in list, then load the list. Freeing first lowers the peak. */
void vcpe_bundles_keep(const uint8_t *list, int n)
{
    /* free first so the new pages fit */
    for (int b = 0; b < nbundles; b++) {
        int need = 0;
        for (int i = 0; i < n; i++)
            if (list[i] == b)
                need = 1;
        if (!need)
            vcpe_bundle_unload(b);
    }
    for (int i = 0; i < n; i++)
        vcpe_bundle_load(list[i]);
}

/* ------------------------------------------------------------------ state */
/* Out-of-range pages report 1x1 so callers dividing by the size never divide by zero. */
void vcpe_page_size(int p, int *w, int *h)
{
    if (p < 0 || p >= npages) {
        *w = *h = 1;
        return;
    }
    *w = pages[p].w;
    *h = pages[p].h;
}

/* Texture-cache bookkeeping (see lazy_flush above). Modules call vcpe_tex_uploaded after writing texels and
 * vcpe_tex_flush after binding a texture. */
void vcpe_tex_uploaded(void) { upload_gen++; }
void vcpe_gfx_lazy_texflush(int on) { lazy_flush = on; flushed_gen = -1; }

void vcpe_tex_flush(void)
{
    if (lazy_flush && flushed_gen == upload_gen)
        return;
    sceGuTexFlush();
    flushed_gen = upload_gen;
}

/* Texture identity for modules with their own texture: "is my texture still the one on the GE?". Only true if nobody
 * bound anything else since (bound_page stays -2 while a module texture is bound). */
int vcpe_tex_bound(const void *key) { return key && key == bound_tex && bound_page == -2; }

void vcpe_tex_claim(const void *key)
{
    vcpe_gfx_flush();
    vcpe_bind_epoch++;   /* other modules' cached texture/palette are no longer bound */
    bound_page = -2;
    bound_tex = key;
}

/* Make page p the GE's current texture. Indexed pages also load their palette (CLUT) into the GE's palette memory.
 * Returns 0 if the page's bundle is not loaded, so the caller can skip the draw. */
int vcpe_bind_page(int p)
{
    if (p == bound_page)
        return 1;
    if (p < 0 || p >= npages)
        return 0;
    vcpe_gfx_flush();
    vcpe_stat_binds++;
    vcpe_bind_epoch++;
    bound_tex = 0;
    const VcpePage *pg = &pages[p];
    uint8_t *base = bundle_mem[pg->bundle];
    if (!base)
        return 0;
    /* psm = pixel storage mode. 4: 16-colour indexed (two pixels per byte, 16-entry CLUT); 8: 256-colour indexed;
     * 16/32: direct colour RGBA4444 / RGBA8888. sceGuClutLoad counts in blocks of 8 entries (2 blocks = 16 colours).
     */
    if (pg->psm == 4) {
        sceGuClutMode(GU_PSM_8888, 0, 0x0f, 0);
        sceGuClutLoad(2, base + pg->clut);
        sceGuTexMode(GU_PSM_T4, 0, 0, pg->swz);
    } else if (pg->psm == 8) {
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, base + pg->clut);
        sceGuTexMode(GU_PSM_T8, 0, 0, pg->swz);
    } else if (pg->psm == 16 || pg->psm == 32) {
        sceGuTexMode(pg->psm == 16 ? GU_PSM_4444 : GU_PSM_8888, 0, 0, pg->swz);
    } else {
        return 0;
    }
    /* Mipmaps: smaller copies of the image (half size each level), stored right after the full image. The GE picks a
     * level by on-screen size, so distant 3D surfaces don't shimmer. */
    if (pg->nmips) {
        /* mipmapped (3D): the levels follow the full-size image; the GE picks one by distance */
        int bpp2 = pg->psm == 4 ? 1 : pg->psm == 8 ? 2 : pg->psm == 16 ? 4 : 8;
        uint32_t off = pg->data;
        int mode = pg->psm == 4 ? GU_PSM_T4 : pg->psm == 8 ? GU_PSM_T8 : pg->psm == 16 ? GU_PSM_4444 : GU_PSM_8888;
        sceGuTexMode(mode, pg->nmips, 0, pg->swz);
        for (int l = 0; l <= pg->nmips; l++) {
            int w = pg->w >> l, h = pg->h >> l;
            sceGuTexImage(l, w, h, w, base + off);
            off += ((uint32_t)w * h * bpp2 / 2 + 15) & ~15u;
        }
    } else {
        sceGuTexImage(0, pg->w, pg->h, pg->w, base + pg->data);
    }
    vcpe_tex_flush();
    bound_page = p;
    return 1;
}

/* The game changed the texture behind the engine's back: forget what is bound. */
void vcpe_unbind(void) { bound_page = -2; bound_tex = 0; }   /* the game bound a texture itself (flush before) */

/* Blend modes. 0 normal alpha; 1 additive (light, glows: dst + src*a); 2 subtractive (dst - src*a). GU_FIX with
 * 0xFFFFFFFF means "factor 1 for the destination". */
void vcpe_blend(int additive)
{
    if (additive == bound_mode)
        return;
    vcpe_gfx_flush();
    if (additive == 2)
        sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
    else if (additive)
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
    else
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    bound_mode = additive;
}

/* Blending makes the GE read every destination pixel before writing it. Turning it off for opaque layers halves the
 * memory traffic of those pixels. */
void vcpe_opaque(int on)
{
    on = on != 0;
    if (on == opaque)
        return;
    vcpe_gfx_flush();
    if (on)
        sceGuDisable(GU_BLEND);
    else
        sceGuEnable(GU_BLEND);
    opaque = on;
}

/* GU_TFX_ADD adds the vertex colour to the texel: with a white-ish vertex colour every visible texel becomes that
 * colour (silhouettes, hit flashes, font tinting) while the texture's alpha still cuts the shape. */
void vcpe_tex_white(int on)
{
    if (on == bound_white)
        return;
    vcpe_gfx_flush();
    sceGuTexFunc(on ? GU_TFX_ADD : GU_TFX_MODULATE, GU_TCC_RGBA);
    bound_white = on;
}

/* Nearest = crisp pixel art; linear = smooth scaling. */
void vcpe_filter(int nearest)
{
    if (nearest == bound_filter)
        return;
    vcpe_gfx_flush();
    sceGuTexFilter(nearest ? GU_NEAREST : GU_LINEAR, nearest ? GU_NEAREST : GU_LINEAR);
    bound_filter = nearest;
}

/* ------------------------------------------------------------------ primitives */
/* The workhorse: one textured rectangle (screen corners x0,y0-x1,y1, texture corners u0,v0-u1,v1 in texels). Corners
 * given in reverse (x1 < x0) mean a mirrored sprite: swap them and the texture coordinates together so the GE gets a
 * normal rectangle with flipped UVs. Fully off-screen rectangles are dropped here for free. */
void vcpe_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color)
{
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
    if (x1 < 0 || y1 < 0 || x0 > VCPE_SCR_W || y0 > VCPE_SCR_H)
        return;
    vcpe_stat_quads++;
    /* Covered pixels, for the overdraw statistic (area / 480*272 = how many screens' worth of pixels). */
    vcpe_stat_area += (x1 - x0) * (y1 - y0);
    VcpeVtx *v;
    if (batch_on) {
        if (batch_kind != 1 || batch_n + 2 > BATCH_MAX) {
            vcpe_gfx_flush();
            batch_kind = 1;
        }
        v = batch.t + batch_n;
        batch_n += 2;
    } else if (!(v = vcpe_gfx_mem(2 * sizeof(VcpeVtx)))) {
        return;
    }
    v[0].u = u0; v[0].v = v0; v[0].color = color; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].u = u1; v[1].v = v1; v[1].color = color; v[1].x = x1; v[1].y = y1; v[1].z = 0;
    if (!batch_on) {
        vcpe_stat_draws++;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    }
}

/* An untextured solid rectangle (colour 0xAABBGGRR). Fully transparent fills cost nothing. */
void vcpe_fill(float x, float y, float w, float h, uint32_t color)
{
    if ((color >> 24) == 0)
        return;
    vcpe_stat_quads++;                 /* fills count as quads too in the perf lines */
    vcpe_stat_area += w * h;
    vcpe_blend(0);
    if (batch_on) {
        if (batch_kind != 2 || batch_n + 2 > BATCH_MAX) {
            vcpe_gfx_flush();
            batch_kind = 2;
        }
        VcpeCVtx *v = batch.c + batch_n;
        v[0].color = v[1].color = color;
        v[0].x = x; v[0].y = y; v[0].z = 0;
        v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
        batch_n += 2;
        return;
    }
    VcpeCVtx *v = vcpe_gfx_mem(2 * sizeof(VcpeCVtx));
    if (!v)
        return;
    sceGuDisable(GU_TEXTURE_2D);
    v[0].color = v[1].color = color;
    v[0].x = x; v[0].y = y; v[0].z = 0;
    v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    vcpe_stat_draws++;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

/* A triangle strip of coloured vertices: each new vertex makes a triangle with the previous two. Used for shapes
 * (button icons, lines, polygons) that are not rectangles. */
void vcpe_strip(const float *xy, const uint32_t *colors, int n, int additive)
{
    if (n < 3)
        return;
    vcpe_gfx_flush();
    vcpe_blend(additive);
    VcpeCVtx *v = vcpe_gfx_mem(n * sizeof(VcpeCVtx));
    if (!v)
        return;
    sceGuDisable(GU_TEXTURE_2D);
    for (int i = 0; i < n; i++) {
        v[i].color = colors[i];
        v[i].x = xy[i * 2];
        v[i].y = xy[i * 2 + 1];
        v[i].z = 0;
    }
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, n, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

/* Scissor: the GE discards pixels outside this rectangle (scroll views, split screens). */
void vcpe_clip(int x, int y, int w, int h)
{
    vcpe_gfx_flush();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    sceGuScissor(x, y, x + w, y + h);
}

void vcpe_clip_reset(void) { vcpe_gfx_flush(); sceGuScissor(0, 0, VCPE_SCR_W, VCPE_SCR_H); }
