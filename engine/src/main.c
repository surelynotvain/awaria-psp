/* Awaria PSP - reimplementation driven by data converted from the user's own copy. */
#include <pspkernel.h>
#include "vcpe.h"
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include "aw_gfx.h"
#include "aw_audio.h"
#include "w_world.h"
#include "w_render.h"
#include "w_anim.h"
#include "g_core.h"
#include "g_game.h"
#include "w_path.h"
#include "g_input.h"
#include "prefs.h"

PSP_MODULE_INFO("Awaria PSP", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

Input IN;
void g_quit(void) { vcpe_running = 0; }



/* ------------------------------------------------------------ autotest
 * If AUTOTEST.TXT sits next to the EBOOT, inputs come from it:
 *   scene N | wait N | press B | hold B N | stick X Y N (analog, -100..100) | quit
 *   B: up down left right ok dash back start                                         */
static int dump_req;
static float goto_x, goto_y; static int goto_frames, bot_frames;
void bot_step(float *mx, float *my, int *press);
static float stick_x, stick_y;
static int stick_frames;
int g_autotest;

static unsigned btn_from(const char *s)
{
    if (!strcmp(s, "up")) return BTN_UP;
    if (!strcmp(s, "down")) return BTN_DOWN;
    if (!strcmp(s, "left")) return BTN_LEFT;
    if (!strcmp(s, "right")) return BTN_RIGHT;
    if (!strcmp(s, "ok")) return BTN_PRIMARY;
    if (!strcmp(s, "dash")) return BTN_SECONDARY;
    if (!strcmp(s, "back")) return BTN_SECONDARY;
    if (!strcmp(s, "start")) return BTN_CANCEL;
    return 0;
}

static int k_last_far(const float *wp, int n, float x, float y);

/* a "stick X Y N" keeps the analog stick there for N frames */
static void test_pre(void)
{
    if (stick_frames > 0) {
        stick_frames--;
        IN.mx = stick_x;
        IN.my = stick_y;
    }
}

/* multi-frame test actions: "bot N" plays the level, "goto X Y" walks there */
static int test_busy(unsigned *held)
{
    if (bot_frames > 0) {
        bot_frames--;
        int press;
        bot_step(&IN.mx, &IN.my, &press);
        if (press) *held = BTN_PRIMARY;
        if (bot_frames % 600 == 0) {
            Script *pl = find_script(ST_PlayerScript);
            if (pl) { float x, y; obj_pos(pl->go, &x, &y); printf("[aw] bot %d player %.2f,%.2f hand %d %d scene %d\n", bot_frames, x, y, player_hand(pl, 0), player_hand(pl, 1), W.scene); }
        }
        return 1;
    }
    if (goto_frames > 0) {
        goto_frames--;
        Script *pl = find_script(ST_PlayerScript);
        if (pl) {
            float x, y;
            obj_pos(pl->go, &x, &y);
            float tx = goto_x, ty = goto_y, wp[256];
            int n = path_find(x, y, goto_x, goto_y, wp, 128);
            for (int k = 0; k < n; k++) {
                float ex = wp[k * 2] - x, ey = wp[k * 2 + 1] - y;
                if (ex * ex + ey * ey > 0.35f * 0.35f) { tx = wp[k * 2]; ty = wp[k * 2 + 1]; break; }
            }
            if (n && k_last_far(wp, n, x, y)) { tx = goto_x; ty = goto_y; }
            float dx = tx - x, dy = ty - y, l = sqrtf(dx * dx + dy * dy);
            float gd = sqrtf((goto_x - x) * (goto_x - x) + (goto_y - y) * (goto_y - y));
            if (gd < 0.25f) goto_frames = 0;
            else { IN.mx = dx / l; IN.my = dy / l; }
            if (!goto_frames) printf("[aw] goto done at %.2f,%.2f\n", x, y);
        } else goto_frames = 0;
        return 1;
    }
    return 0;
}

/* Awaria's own test commands (the engine handles wait / press / hold / rec / quit) */
static int test_command(const char *cmd, const char *arg, int num, int num2, const char *line, unsigned *held)
{
    (void)held;
    if (!strcmp(cmd, "goto")) { float gx, gy; if (sscanf(line, "%*s %f %f", &gx, &gy) == 2) { goto_x = gx; goto_y = gy; goto_frames = 600; } return VCPE_CMD_FRAME; }
    if (!strcmp(cmd, "pos")) { Script *pl = find_script(ST_PlayerScript); if (pl) { float x, y; obj_pos(pl->go, &x, &y); printf("[aw] player %.2f,%.2f hand %d %d\n", x, y, player_hand(pl, 0), player_hand(pl, 1)); } return VCPE_CMD_NEXT; }
    if (!strcmp(cmd, "bot")) { bot_frames = atoi(arg); return VCPE_CMD_FRAME; }
    if (!strcmp(cmd, "dump")) { dump_req = 1; return VCPE_CMD_NEXT; }
    if (!strcmp(cmd, "scene")) { M.next_level = atoi(arg); M.main_menu = M.next_level == 0; g_scene_request(atoi(arg)); return VCPE_CMD_FRAME; }
    if (!strcmp(cmd, "intro")) { M.intro_ready = 1; Script *r = find_script(ST_RotationScript); if (r) rotation_ghost_intro(r); return VCPE_CMD_NEXT; }
    if (!strcmp(cmd, "cutscene")) { Script *c = find_script(ST_CutsceneScript); if (c) g_send_event(c->go, EV_CutsceneStart, 0, 0); return VCPE_CMD_NEXT; }
    if (!strcmp(cmd, "god")) { extern int g_god; g_god = 1; return VCPE_CMD_NEXT; }
    if (!strcmp(cmd, "stick")) {
        stick_x = atoi(arg) / 100.0f;
        stick_y = num / 100.0f;
        stick_frames = num2;
        return VCPE_CMD_FRAME;
    }
    if (!strcmp(cmd, "pref")) { prefs_set_int(arg, num); return VCPE_CMD_NEXT; }
    return VCPE_CMD_UNKNOWN;
}

static void script_load(void)
{
    static const VcpeTestHooks hooks = {btn_from, test_command, test_pre, test_busy, 0};
    g_autotest = vcpe_test_load(&hooks);
}

static void debug_dump(void)
{
    {
                    extern Anim *g_anim; extern int g_nanim;
                    for (int k = 0; k < g_nanim; k++) {
                        Anim *an = &g_anim[k];
                        if (!an->alive) continue;
                        printf("[aw] anim %d go %d ctrl %d en %d upd %d state %d t %.2f clip %s ah %d\n", k, an->go, an->ctrl, an->enabled, an->update, an->state, an->t, anim_state_clip(an->go), W.obj[an->go].ah);
                    }

                    for (int i = 0; i < W.nobj; i++) {
                        Obj *o = &W.obj[i];
                        if (!o->ah) continue;
                        float m[6]; obj_matrix(i, m);
                        printf("[aw]  #%d n%08x par %d scr %d sr %d img %d txt %d canvas %d pos %.1f,%.1f", i, (unsigned)o->name, o->parent, o->screen, o->sr, o->img, o->text, o->canvas, m[4], m[5]);
                        if (o->img >= 0) printf(" img %d,%d,%d,%d spr %d en %d", W.img[o->img].r, W.img[o->img].g, W.img[o->img].b, W.img[o->img].a, W.img[o->img].spr, W.img[o->img].flags & 1);
                        if (o->sr >= 0) printf(" srA %d spr %d fl %d", W.sr[o->sr].d.a, W.sr[o->sr].d.spr, W.sr[o->sr].d.flags);
                        if (o->rect >= 0) printf(" rect %.0fx%.0f", o->rw, o->rh);
                        if (o->text >= 0) printf(" str '%s' en %d", W.text[o->text].str ? W.text[o->text].str : "", W.text[o->text].d.flags & WTXT_ENABLED);
                        printf("\n");
                    }
                    }
}

static int k_last_far(const float *wp, int n, float x, float y)
{
    /* all remaining nodes are near: head straight for the exact target */
    for (int k = 0; k < n; k++) {
        float ex = wp[k * 2] - x, ey = wp[k * 2 + 1] - y;
        if (ex * ex + ey * ey > 0.35f * 0.35f) return 0;
    }
    return 1;
}

static void read_input(void)
{
    static unsigned prev;
    unsigned held = 0;
    IN.mx = IN.my = 0;
    if (vcpe_autotest) {
        vcpe_test_step(&held);
    } else {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned b = pad.Buttons;
        float ax = (pad.Lx - 128) / 127.0f, ay = (128 - pad.Ly) / 127.0f;
        if (fabsf(ax) < 0.25f) ax = 0;
        if (fabsf(ay) < 0.25f) ay = 0;
        IN.mx = ax; IN.my = ay;
        if (b & PSP_CTRL_UP) held |= BTN_UP;
        if (b & PSP_CTRL_DOWN) held |= BTN_DOWN;
        if (b & PSP_CTRL_LEFT) held |= BTN_LEFT;
        if (b & PSP_CTRL_RIGHT) held |= BTN_RIGHT;
        if (b & PSP_CTRL_CROSS) held |= BTN_PRIMARY;
        if (b & (PSP_CTRL_SQUARE | PSP_CTRL_CIRCLE | PSP_CTRL_RTRIGGER | PSP_CTRL_LTRIGGER)) held |= BTN_SECONDARY;
        if (b & (PSP_CTRL_START | PSP_CTRL_SELECT)) held |= BTN_CANCEL;
    }
    /* the D-pad adds to the stick (InputSystem composite) */
    if (held & BTN_LEFT) IN.mx = -1;
    if (held & BTN_RIGHT) IN.mx = 1;
    if (held & BTN_UP) IN.my = 1;
    if (held & BTN_DOWN) IN.my = -1;
    unsigned edges = held & ~prev;
    IN.held = held;
    IN.pressed = edges;
    IN.primary = (edges & BTN_PRIMARY) != 0;
    IN.secondary = (edges & BTN_SECONDARY) != 0;
    IN.cancel = (edges & BTN_CANCEL) != 0;
    prev = held;
}

int main(int argc, char *argv[])
{
    vcpe_sys_init(argc, argv);
    char p[300];
    vcpe_path(p, sizeof p, "SETTINGS.BIN");
    prefs_load(p);
    gfx_init();
    vcpe_path(p, sizeof p, "AW.PAK");
    int ok = gfx_pak_open(p) >= 0;
    vcpe_path(p, sizeof p, "LANG.PAK");
    if (ok)
        ok = gfx_lang_load(p, prefs_int("lang", 1)) >= 0;
    printf("[aw] language %s\n", lang_name());
    if (!ok) {
        while (vcpe_running) {
            gfx_begin(RGBA(120, 0, 0, 255));
            gfx_end();
        }
        return 0;
    }
    vcpe_path(p, sizeof p, "MUSIC.PAK");
    audio_init(p);
    music_set_volume10(10.0f);
    sfx_set_volume10(10.0f);
    prefs_set_float("musicVolume", 10.0f);
    prefs_set_float("sfxVolume", 10.0f);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    script_load();
    srand(g_autotest ? 12345u : sceKernelGetSystemTimeLow());   /* test runs are repeatable */
    g_scene_request(0);
    SceInt64 last = sceKernelGetSystemTimeWide();
    while (vcpe_running) {
        if (g_scene_pending() >= 0)
            g_scene_switch();
        read_input();
        SceInt64 now = sceKernelGetSystemTimeWide();
        float udt = (now - last) / 1000000.0f;
        last = now;
        if (g_autotest || udt <= 0 || udt > 0.1f)
            udt = 1.0f / 60.0f;
        SceInt64 p0 = sceKernelGetSystemTimeWide();
        g_update(udt);
        SceInt64 p1 = sceKernelGetSystemTimeWide();
        if (g_autotest) {
            static int fr;
            if (dump_req) { dump_req = 0; debug_dump(); }
            if (++fr % 60 == 0) {
                int act = 0;
                for (int i = 0; i < W.nobj; i++) act += W.obj[i].ah;
                printf("[aw] f%d scene %d scale %.2f objs %d active %d cam %.2f,%.2f\n", fr, W.scene, T.scale, W.nobj, act, W.cam_x, W.cam_y);
                /* memory audit (PSP-1000: 24 MB user RAM): heap high-water mark (sbrk arena) and live bytes */
                struct mallinfo mi = mallinfo();
                static int peak_used;
                if ((int)mi.uordblks > peak_used) peak_used = mi.uordblks;
                printf("[aw] mem scene %d arena %d KB used %d KB peak %d KB kfree %d KB\n", W.scene, mi.arena / 1024,
                       mi.uordblks / 1024, peak_used / 1024, (int)(sceKernelTotalFreeMemSize() / 1024));
            }
        }
        w_transforms();
        gfx_begin(RGBA(0, 0, 0, 255));
        render_world(T.dt);
        SceInt64 p2 = sceKernelGetSystemTimeWide();
        gfx_end();
        vcpe_test_frame_end();
        if (g_autotest) {
            /* frame cost (emulated guest time): update / build+draw / GPU wait+vblank */
            static int pf; static SceInt64 su, sr, sg, mu, mr;
            SceInt64 p3 = sceKernelGetSystemTimeWide();
            su += p1 - p0; sr += p2 - p1; sg += p3 - p2;
            if (p1 - p0 > mu) mu = p1 - p0;
            if (p2 - p1 > mr) mr = p2 - p1;
            if (++pf % 60 == 0) {
                printf("[aw] perf upd %.2f (max %.2f) draw %.2f (max %.2f) wait %.2f ms\n",
                       su / 60000.0, mu / 1000.0, sr / 60000.0, mr / 1000.0, sg / 60000.0);
                extern long long g_prof[8]; extern int g_prof_act, g_prof_hier;
                printf("[aw] prof fixed %.2f phys %.2f upd %.2f anim %.2f apply %.2f part %.2f destroy %.2f misc %.2f ms/f  act %d hier %d\n",
                       g_prof[0] / 60000.0, g_prof[1] / 60000.0, g_prof[2] / 60000.0, g_prof[3] / 60000.0,
                       g_prof[4] / 60000.0, g_prof[5] / 60000.0, g_prof[6] / 60000.0, g_prof[7] / 60000.0, g_prof_act, g_prof_hier);
                
                printf("[aw] gpu binds %d quads %d overdraw %.2f screens /f\n", vcpe_stat_binds / 60, vcpe_stat_quads / 60, vcpe_stat_area / 60.0f / (480.0f * 272.0f));
                vcpe_stat_binds = vcpe_stat_quads = 0; vcpe_stat_area = 0;
                memset(g_prof, 0, sizeof g_prof); g_prof_act = g_prof_hier = 0;
                su = sr = sg = mu = mr = 0;
            }
        }
        prefs_save();
    }
    audio_shutdown();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
