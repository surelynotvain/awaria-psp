/* ManagerScript, CameraScript, TransitionScript, InteractionScript and small helpers. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "g_game.h"
#include "w_render.h"

Mgr M;

/* ------------------------------------------------------------------ helpers */
float frandf(void) { return (rand() & 0xFFFFF) / (float)0x100000; }
float frange(float a, float b) { return a + (b - a) * frandf(); }
int irange(int a, int b) { return b <= a ? a : a + rand() % (b - a); }

int P(const char *param) { return anim_param_id(param); }

void obj_pos2(int o, float v[2]) { obj_pos(o, &v[0], &v[1]); }
void set_text(int go, const char *s) { text_set(text_of(go), s); }
void set_text_color(int go, uint32_t c) { text_set_color(text_of(go), c); }
void sr_enable(int go, int on)
{
    int sr = sr_of(go);
    if (sr < 0) return;
    if (on) W.sr[sr].d.flags |= WSR_ENABLED;
    else W.sr[sr].d.flags &= ~WSR_ENABLED;
}

Script *find_script(int type)
{
    for (int i = 0; i < W.nscript; i++)
        if (W.script[i].alive && W.script[i].type == type)
            return &W.script[i];
    return 0;
}

const char *menu_txt(int i) { return txt_line(TXT_M, i); }
const char *chap_txt(int i) { return txt_line(TXT_CH, i); }

/* ------------------------------------------------------------------ sounds */
static int music_track(int snd) { return (snd >= 0 && (snd & 0x8000)) ? (snd & 0x7FFF) : -1; }

int mgr_short(int snd, int priority)
{
    if (snd < 0 || (snd & 0x8000)) return -1;
    return snd_short(snd, priority);
}
int mgr_short_common(int snd)
{
    if (snd < 0 || (snd & 0x8000)) return -1;
    return snd_short_common(snd);
}
void mgr_long(int snd) { if (snd >= 0 && !(snd & 0x8000)) snd_long(snd); }
void mgr_paused(int snd) { if (snd >= 0 && !(snd & 0x8000)) snd_paused(snd); }

int snd_count(const Script *s, uint32_t field)
{
    int n;
    f_elems(s, field, &n);
    return n;
}
int snd_at(const Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    if (!e || i < 0 || i >= n || e[i].kind != F_ASSET)
        return -1;
    return e[i].v.i;
}
int snd_pick(const Script *s, uint32_t field)
{
    int n = snd_count(s, field);
    return n ? snd_at(s, field, irange(0, n)) : -1;
}

void mgr_music_check(int snd)
{
    if (snd < 0 || snd == M.requested_track)
        return;
    M.requested_track = snd;
    int t = music_track(snd);
    if (t >= 0) {
        music_play(t);
        snprintf(M.music_credit, sizeof M.music_credit, "%s %s", menu_txt(22), g_music[t].name);
    }
}

void mgr_audio_pause(int stop) { snd_pause_short(stop); }
void mgr_audio_remove_all(void) { snd_remove_all(); }

void mgr_volume(const char *type, float v)
{
    if (!strcmp(type, "musicVolume"))
        music_set_volume10(v * 10.0f);
    else
        sfx_set_volume10(v * 10.0f);
}

void mgr_music_muffle(int muffle) { music_muffle(muffle); }

void mgr_load_scene(int n) { g_scene_request(n); }

/* chapters unlocked (pref "lvl"); the demo build stops at chapter 2 */
int mgr_progress(void)
{
    int lvl = prefs_int("lvl", 1);
#if AW_DEMO_DATA
    if (lvl > 2) lvl = 2;
#endif
    return lvl;
}

void mgr_level_complete(void)
{
#if AW_DEMO_DATA
    if (M.next_level >= 2) {             /* end of the demo: back to chapter select */
        M.main_menu = 1;
        M.sub_menu = 4;
        if (prefs_int("lvl", 1) < 2) prefs_set_int("lvl", 2);
        return;
    }
#endif
    if (M.next_level < 13) {
        M.main_menu = 1;
        M.sub_menu = 4;
        M.next_level++;
        if (M.next_level > prefs_int("lvl", 1))
            prefs_set_int("lvl", M.next_level);
    } else if (M.next_level == 13) {
        prefs_set_int("gallery", 1);
        M.main_menu = 1;
        M.sub_menu = 6;
        M.next_level++;
    }
}

void mgr_reload(void)
{
    if (M.main_menu)
        mgr_load_scene(0);
    else
        mgr_load_scene(M.next_level);
}

/* ------------------------------------------------------------------ ManagerScript */
static void manager_init(Script *s)
{
    if (!M.inited) {
        M.inited = 1;
        M.first_transition = 1;
        M.main_menu = 1;
        M.sub_menu = 1;
        M.requested_track = -1;
        M.next_level = W.scene;
    }
    snd_background(!0);
}
static void manager_start(Script *s)
{
    mgr_volume("sfxVolume", 1.0f);
    mgr_volume("musicVolume", 1.0f);
}
const ScriptVT vt_ManagerScript = {0, manager_init, manager_start};

/* ------------------------------------------------------------------ CameraScript */
typedef struct { int cam; float dur, dur0, amount; int active; float ox, oy; } CameraS;
static Script *camera_script;

void camera_shake(float duration, float amount)
{
    if (!camera_script) camera_script = find_script(ST_CameraScript);
    if (!camera_script) return;
    CameraS *c = camera_script->self;
    c->dur = c->dur0 = duration;
    c->amount = amount;
    c->active = 1;
}
static void camera_init(Script *s)
{
    CameraS *c = s->self;
    c->cam = f_ref(s, CRC("camTransform"));
    camera_script = s;
    W.shake_x = W.shake_y = 0;
}
static void camera_start(Script *s) { mgr_music_check(f_asset(s, CRC("music"))); }
static void camera_update(Script *s, float dt)
{
    CameraS *c = s->self;
    if (!c->active) { W.shake_x = W.shake_y = 0; return; }
    if (c->dur > 0) {
        float k = c->amount * (c->dur0 > 0 ? fminf(1.0f, fmaxf(0.0f, c->dur / c->dur0)) : 0);
        /* Random.insideUnitSphere: x,y of a point in the unit ball */
        float x, y, z;
        do { x = frange(-1, 1); y = frange(-1, 1); z = frange(-1, 1); } while (x * x + y * y + z * z > 1);
        W.shake_x = x * k;
        W.shake_y = y * k;
        c->dur -= dt;
    } else {
        W.shake_x = W.shake_y = 0;
        c->active = 0;
    }
}
const ScriptVT vt_CameraScript = {sizeof(CameraS), camera_init, camera_start, camera_update};

/* ------------------------------------------------------------------ TransitionScript */
static int p_noDoor = -2, p_gameover = -2;
static void transition_starter(Script *s)
{
    snd_background(1);
    T.scale = 1;
    Script *pl = script_on(f_ref(s, CRC("playerScript")), ST_PlayerScript);
    if (pl) player_block_input(pl, 0);
}
static void transition_start(Script *s)
{
    if (p_noDoor == -2) { p_noDoor = P("noDoor"); p_gameover = P("gameover"); }
    if (M.first_transition) {
        anim_trigger(s->go, p_noDoor);
        M.first_transition = 0;
        transition_starter(s);
    }
}
void transition_go(Script *t)
{
    if (p_gameover == -2) { p_noDoor = P("noDoor"); p_gameover = P("gameover"); }
    anim_trigger(t->go, p_gameover);
    mgr_long(f_asset(t, CRC("doorCloseSound")));
}
static void transition_event(Script *s, int fn, int iarg, float farg)
{
    if (fn == EV_Loader) mgr_reload();
    else if (fn == EV_Starter) transition_starter(s);
    else if (fn == EV_PlayOpen) mgr_long(f_asset(s, CRC("doorOpenSound")));
    else if (fn == EV_Transition) transition_go(s);
}
const ScriptVT vt_TransitionScript = {0, 0, transition_start, 0, 0, transition_event};

/* ------------------------------------------------------------------ InteractionScript */
static void interaction_update(Script *s, float dt)
{
    if (!IN.primary)
        return;
    Script *player = script_on(f_ref(s, CRC("player")), ST_PlayerScript);
    Script *menu = script_on(f_ref(s, CRC("menu")), ST_MenuScript);
    Script *cut = script_on(f_ref(s, CRC("cutscene")), ST_CutsceneScript);
    Script *up = script_on(f_ref(s, CRC("upscaleAniScript")), ST_UpscaleAnimatorScript);
    if (player && player_in_menu(player)) {
        if (menu) menu_input_confirm(menu);
    } else if (player && player_in_cutscene(player)) {
        if (cut) cutscene_input_confirm(cut);
    } else if (up && upscale_in_intro(up)) {
        upscale_activator(up, "introEnd", s->go, 1);
    } else if (player) {
        player_input_confirm(player);
    }
}
const ScriptVT vt_InteractionScript = {0, 0, 0, interaction_update};

const ScriptVT vt_InputScript = {0};

/* ------------------------------------------------------------------ AnimSoundScript / TextScript / DestroyScript */
static void animsound_event(Script *s, int fn, int iarg, float farg)
{
    if (fn == EV_AnimAudio)
        mgr_long(snd_at(s, CRC("clipArray"), iarg));
}
const ScriptVT vt_AnimSoundScript = {0, 0, 0, 0, 0, animsound_event};

static void textscript_start(Script *s) { set_text(s->go, menu_txt(f_int(s, CRC("txtInt"), 0))); }
const ScriptVT vt_TextScript = {0, 0, textscript_start};

static void destroy_start(Script *s) { w_destroy(s->go, 1.0f); }
const ScriptVT vt_DestroyScript = {0, 0, destroy_start};

/* MinionDeathScript: random explosion variant, gone after 1 s */
static void miniondeath_start(Script *s)
{
    int a = f_ref(s, CRC("a"));
    static int p_explo = -2;
    if (p_explo == -2) p_explo = P("explo");
    anim_set_int(a, p_explo, irange(1, 4));
    w_destroy(s->go, 1.0f);
}
const ScriptVT vt_MinionDeathScript = {0, 0, miniondeath_start};

/* PartsScatterScript */
typedef struct { int dummy; } PartsS;
static void parts_start(Script *s)
{
    phys_add_impulse(s->go, frange(-40, 40) * 0.02f, frange(40, 120) * 0.02f);   /* AddForce for one 0.02 s step */
    phys_add_torque(s->go, 300);
    w_destroy(s->go, frange(0.7f, 0.8f));
}
static void parts_update(Script *s, float dt)
{
    if (W.obj[s->go].ly <= 0)
        w_destroy(s->go, 0);
}
const ScriptVT vt_PartsScatterScript = {sizeof(PartsS), 0, parts_start, parts_update};

/* ElectroVfxScript: Discharge(place, trigger) -> active, animator trigger, off after 0.4 s */
typedef struct { float off_t; } ElectroS;
void electro_discharge(Script *e, float x, float y, const char *trigger)
{
    ElectroS *st = e->self;
    obj_set_pos(e->go, x + frange(-0.1f, 0.1f), y + frange(-0.1f, 0.1f));
    obj_set_active(e->go, 1);
    anim_trigger(f_ref(e, CRC("animator")), P(trigger));
    st->off_t = 0.4f;
}
static void electro_update(Script *s, float dt)
{
    ElectroS *st = s->self;
    if (st->off_t > 0) {
        st->off_t -= dt;
        if (st->off_t <= 0)
            obj_set_active(s->go, 0);
    }
}
const ScriptVT vt_ElectroVfxScript = {sizeof(ElectroS), 0, 0, electro_update};

/* CutterHoloScript */
void cutterholo_cast(Script *h, float ox, float oy, float dx, float dy)
{
    obj_set_pos(h->go, ox, oy);
    float a = atan2f(dy, dx) * 57.29578f;
    if (a < 0) a += 360;
    obj_set_rot(h->go, a);
    anim_trigger(f_ref(h, CRC("animator")), P("cast"));
}
const ScriptVT vt_CutterHoloScript = {0};

/* CutterLineScript: a raycast line shown for 0.4 s */
typedef struct { float off_t; } CutLineS;
static void cutline_init(Script *s) { obj_set_active(s->go, 0); }
static void cutline_update(Script *s, float dt)
{
    CutLineS *st = s->self;
    if (st->off_t > 0) {
        st->off_t -= dt;
        if (st->off_t <= 0)
            obj_set_active(s->go, 0);
    }
}
void cutline_display(Script *s, float ox, float oy, float dx, float dy)
{
    CutLineS *st = s->self;
    float hx = ox + dx * 50, hy = oy + dy * 50;
    phys_raycast(ox, oy, dx, dy, 1e6f, f_int(s, CRC("layerMask"), -1), &hx, &hy, 0);
    int l = W.obj[s->go].line;
    if (l >= 0) {
        W.line[l].pos[0][0] = ox; W.line[l].pos[0][1] = oy;
        W.line[l].pos[1][0] = hx; W.line[l].pos[1][1] = hy;
        W.line[l].npos = 2;
        W.line[l].world = 1;
    }
    obj_set_active(s->go, 1);
    anim_trigger(s->go, P("display"));
    st->off_t = 0.4f;
}
const ScriptVT vt_CutterLineScript = {sizeof(CutLineS), cutline_init, 0, cutline_update};
