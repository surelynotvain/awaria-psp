/* VCPE system: HOME / sleep callbacks, CPU clock, files next to the EBOOT. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <string.h>
#include <stdio.h>
#include "vcpe_sys.h"

volatile int vcpe_running = 1;
volatile int vcpe_resume_gen;
static char base_dir[256];

static int exit_cb(int a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    vcpe_running = 0;
    sceKernelExitGame();
    return 0;
}

static int power_cb(int unknown, int flags, void *arg)
{
    (void)unknown; (void)arg;
    if (flags & PSP_POWER_CB_RESUME_COMPLETE)
        vcpe_resume_gen++;   /* file readers reopen their files */
    return 0;
}

static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("exit", exit_cb, 0);
    sceKernelRegisterExitCallback(cb);
    int pcb = sceKernelCreateCallback("power", power_cb, 0);
    if (scePowerRegisterCallback(-1, pcb) < 0)
        scePowerRegisterCallback(0, pcb);
    sceKernelSleepThreadCB();
    return 0;
}

void vcpe_sys_init(int argc, char **argv)
{
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0)
        sceKernelStartThread(th, 0, 0);
    scePowerSetClockFrequency(333, 333, 166);
    base_dir[0] = 0;
    if (argc > 0 && argv[0]) {
        strncpy(base_dir, argv[0], sizeof base_dir - 1);
        char *slash = strrchr(base_dir, '/');
        if (slash)
            slash[1] = 0;
        else
            base_dir[0] = 0;
    }
}

void vcpe_path(char *out, int n, const char *file)
{
    snprintf(out, n, "%s%s", base_dir, file);
}

void vcpe_quit(void) { vcpe_running = 0; }

/* A start-up problem the player has to fix (missing or broken data files): plain text on the debug
 * screen, which needs no game data. Call vcpe_gfx_shutdown() first if the renderer is running.
 * Stays until HOME > Exit. */
void vcpe_fatal(const char *title, const char *text)
{
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFF000028);   /* dark red (ABGR), like the old red screen */
    pspDebugScreenClear();
    pspDebugScreenSetTextColor(0xFF5050FF);
    pspDebugScreenPrintf("\n %s\n\n", title);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    /* the debug font is 8 px wide: 60 columns; wrap the text at spaces */
    char line[64];
    const char *p = text;
    while (*p) {
        int n = 0, cut = -1;
        while (p[n] && p[n] != '\n' && n < 58) {
            if (p[n] == ' ')
                cut = n;
            n++;
        }
        if (p[n] && p[n] != '\n' && cut > 0)
            n = cut;
        snprintf(line, sizeof line, "%.*s", n, p);
        pspDebugScreenPrintf(" %s\n", line);
        p += n;
        if (*p == ' ' || *p == '\n')
            p++;
    }
    pspDebugScreenSetTextColor(0xFFA0A0A0);
    pspDebugScreenPrintf("\n Press HOME to quit.\n");
    while (vcpe_running)
        sceDisplayWaitVblankStart();
    sceKernelExitGame();
}
