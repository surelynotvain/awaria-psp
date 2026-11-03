/* VCPE system: HOME / sleep callbacks, CPU clock, files next to the EBOOT. */

/* HOW THIS FILE WORKS
 *
 * Every PSP program must answer two system events: the HOME menu's "Exit" (the exit callback) and power events
 * (sleep, resume). The PSP delivers them as "callbacks", which only run while a thread sleeps in a callback-aware
 * wait. So vcpe_sys_init starts a small thread that registers the callbacks and then sleeps forever with
 * sceKernelSleepThreadCB: the callbacks run on that thread, never interrupting the game.
 *
 * The rest: the CPU clock (333 MHz, the maximum), the folder the EBOOT was started from (for files next to it), the
 * player-facing error screen, memory checks and the "steps" the loading screen shows.
 *
 * This module is the PSP system backend: callbacks, power and the debug screen have no counterpart elsewhere, so
 * they stay here; everything else uses vcpe_plat. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "vcpe_plat.h"
#include "vcpe_sys.h"
#include "vcpe_errimg.h"

/* volatile: written by the callback thread, read by the game loop; the compiler must re-read it every time. */
volatile int vcpe_running = 1;
volatile int vcpe_resume_gen;
void (*vcpe_present_hook)(void);
static char base_dir[256];

/* Weak: called only if the game links vcpe_math / vcpe_log (their address is 0 otherwise). */
void vcpe_math_boot(uint32_t seed) __attribute__((weak));
void vcpe_log_line(const char *s) __attribute__((weak));

/* ------------------------------------------------------------------ system callbacks */
/* HOME > Exit: stop the game loop and ask the system to end the program. */
static int exit_cb(int a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    vcpe_running = 0;
    sceKernelExitGame();
    return 0;
}

/* After waking from sleep, open files are no longer valid. Bumping this counter tells every file reader to reopen its
 * file before the next read. */
static int power_cb(int unknown, int flags, void *arg)
{
    (void)unknown; (void)arg;
    if (flags & PSP_POWER_CB_RESUME_COMPLETE)
        vcpe_resume_gen++;
    return 0;
}

/* The callback thread: register, then sleep in a callback-aware wait forever. */
static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_cb, 0));
    int pcb = sceKernelCreateCallback("power", power_cb, 0);
    if (scePowerRegisterCallback(-1, pcb) < 0)
        scePowerRegisterCallback(0, pcb);
    sceKernelSleepThreadCB();
    return 0;
}

/* Start-up: tables and RNG seed, the callback thread (priority 0x11, above the game), 333 MHz, the game folder. */
void vcpe_sys_init(int argc, char **argv)
{
    if (vcpe_math_boot)
        vcpe_math_boot(vcpe_time_us32());
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0)
        sceKernelStartThread(th, 0, 0);
    scePowerSetClockFrequency(333, 333, 166);
    /* argv[0] is the EBOOT's full path ("ms0:/PSP/GAME/X/EBOOT.PBP"); keep it up to the last '/'. */
    base_dir[0] = 0;
    if (argc > 0 && argv[0]) {
        snprintf(base_dir, sizeof base_dir, "%s", argv[0]);
        char *slash = strrchr(base_dir, '/');
        if (slash)
            slash[1] = 0;
        else
            base_dir[0] = 0;
    }
}

void vcpe_path(char *out, int n, const char *file) { snprintf(out, n, "%s%s", base_dir, file); }
void vcpe_quit(void) { vcpe_running = 0; }

/* ------------------------------------------------------------------ the error screen */
#define FATAL_COLS 58          /* the debug font is 8 px wide: 60 columns, one space margin each side */
#define UNCACHED 0x40000000u   /* the same memory without the data cache: writes reach VRAM at once */

/* Print text word-wrapped at spaces, FATAL_COLS characters per line; '\n' starts a new line. */
static void print_wrapped(const char *p)
{
    char line[FATAL_COLS + 1];
    while (*p) {
        int n = 0, cut = -1;
        while (p[n] && p[n] != '\n' && n < FATAL_COLS) {
            if (p[n] == ' ')
                cut = n;
            n++;
        }
        if (p[n] && p[n] != '\n' && cut > 0)
            n = cut;
        memcpy(line, p, n);
        line[n] = 0;
        pspDebugScreenPrintf(" %s\n", line);
        p += n;
        if (*p == ' ' || *p == '\n')
            p++;
    }
}

/* The engine's error picture (tools/mkerrimg.py, RGB565) in the bottom-right corner of the shown frame buffer, with a
 * one-pixel dim red frame, converted to whatever pixel format the display uses. */
static void draw_error_picture(void)
{
    void *top = 0;
    int bw = 0, pf = 0;
    sceDisplayWaitVblankStart();   /* the debug screen's buffer is shown from the next frame */
    sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (!top || bw <= 0)
        return;
    volatile uint32_t *fb32 = (volatile uint32_t *)((uintptr_t)top | UNCACHED);
    volatile uint16_t *fb16 = (volatile uint16_t *)fb32;
    const int x0 = 480 - 8 - VCPE_ERRIMG_W, y0 = 272 - 8 - VCPE_ERRIMG_H;
    for (int y = -1; y <= VCPE_ERRIMG_H; y++)
        for (int x = -1; x <= VCPE_ERRIMG_W; x++) {
            int edge = x < 0 || y < 0 || x == VCPE_ERRIMG_W || y == VCPE_ERRIMG_H;
            unsigned v = edge ? 0x7A08u : vcpe_errimg[y * VCPE_ERRIMG_W + x];
            unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            int at = (y0 + y) * bw + x0 + x;
            if (pf == PSP_DISPLAY_PIXEL_FORMAT_8888)
                fb32[at] = 0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
            else if (pf == PSP_DISPLAY_PIXEL_FORMAT_565)
                fb16[at] = (uint16_t)(b << 11 | g << 5 | r);
        }
}

/* A start-up problem the player has to fix (missing or broken data files): plain text on the debug screen, which
 * needs no game data. Call vcpe_gfx_shutdown() first if the renderer is running. Stays until HOME > Exit. */
void vcpe_fatal(const char *title, const char *text)
{
    if (vcpe_present_hook)
        vcpe_present_hook();   /* the GE must be idle before the debug screen takes the display */
    if (vcpe_log_line) {
        vcpe_log_line("FATAL:");
        vcpe_log_line(title);
        vcpe_log_line(text);
    }
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFF000028);   /* dark red (ABGR) */
    pspDebugScreenClear();
    pspDebugScreenSetTextColor(0xFF5050FF);
    pspDebugScreenPrintf("\n %s\n\n", title);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    print_wrapped(text);
    pspDebugScreenSetTextColor(0xFFA0A0A0);
    pspDebugScreenPrintf("\n Press HOME to quit.\n");
    draw_error_picture();
    while (vcpe_running)
        sceDisplayWaitVblankStart();
    sceKernelExitGame();
}

/* ------------------------------------------------------------------ start-up checks */
/* 36000 KB sits between what a PSP-1000 (about 20 MB) and a 64 MB model (about 50 MB) can give. */
void vcpe_require_64mb(const char *title)
{
    if (vcpe_heap_kb() < 36000)
        vcpe_fatal(title, "This PSP model is not supported.\n\nThis game needs the 64 MB of a PSP-2000, PSP-3000 or "
                          "PSP Go (a PSP-1000 has 32 MB).");
}

void vcpe_require_file(const char *file, const char *title, const char *what)
{
    char p[300], msg[600];
    vcpe_path(p, sizeof p, file);
    int fd = vcpe_file_open(p);
    if (fd >= 0) {
        vcpe_file_close(fd);
        return;
    }
    snprintf(msg, sizeof msg, "%s is missing.\n\n%s", file, what ? what : "Copy it next to EBOOT.PBP.");
    vcpe_fatal(title, msg);
}

/* Largest block malloc can give, found by binary search to 64 KB precision. */
int vcpe_heap_kb(void)
{
    unsigned lo = 0, hi = 64u << 20;
    while (hi - lo > 65536) {
        unsigned mid = (lo + hi) / 2;
        /* through a volatile pointer: GCC may drop a malloc that is only freed and treat it as a success */
        void *volatile p = malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (int)(lo / 1024);
}

/* ------------------------------------------------------------------ steps (see vcpe_sys.h) */
/* One-line reports of what is happening ("Loading textures 3"). Only the outermost step is reported: a step inside
 * another (a PAK read inside a bundle load) is an implementation detail. */
void (*vcpe_step_hook)(int id, int status, const char *text);
static int step_ids, step_depth;

static void step_report(int id, int status, const char *fmt, va_list ap)
{
    char t[96];
    vsnprintf(t, sizeof t, fmt, ap);
    vcpe_step_hook(id, status, t);
}

int vcpe_step_begin(const char *fmt, ...)
{
    if (step_depth++ > 0 || !vcpe_step_hook)   /* depth counts every step, so begin/end always pair */
        return -1;
    int id = ++step_ids;
    va_list ap;
    va_start(ap, fmt);
    step_report(id, VCPE_STEP_RUN, fmt, ap);
    va_end(ap);
    return id;
}

void vcpe_step_end(int id, int ok)
{
    if (step_depth > 0)
        step_depth--;
    if (id > 0 && vcpe_step_hook)
        vcpe_step_hook(id, ok ? VCPE_STEP_OK : VCPE_STEP_FAIL, 0);
}

void vcpe_step_note(int status, const char *fmt, ...)
{
    if (!vcpe_step_hook || step_depth > 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    step_report(++step_ids, status, fmt, ap);
    va_end(ap);
}
