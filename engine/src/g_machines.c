/* EngineScript (generators), AparatusScript (part machines), HitboxScript. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "g_game.h"
#include "w_render.h"

static int pid(const char *n) { return anim_param_id(n); }

static int ref_i(Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n) ? f_ref_at(s, &e[i]) : -1;
}
static int sprite_i(Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n && e[i].kind == F_ASSET) ? e[i].v.i : SPR_NONE;
}

/* ================================================================== EngineScript */
typedef struct {
    int highlight, junk[2], junk_anim[2], barfill, timer_text, second_bubble, box, ui, spark_anim;
    int sparks[3], nsparks;
    Script *rot, *player;
    float timer_tic, empty_bar, filled_scale;
    int next_spark, spark_index, xflip, red_bats;
    int want[2];
    float required, passed;
    int interactible, wanting, spark_timer, is_late, sound_side, late_src;
    Co counter, broken; int broken_state;
} EngineS;

static void engine_init(Script *s)
{
    EngineS *e = s->self;
    e->highlight = f_ref(s, CRC("highlight"));
    for (int i = 0; i < 2; i++) {
        e->junk[i] = ref_i(s, CRC("junk"), i);
        e->junk_anim[i] = ref_i(s, CRC("junkAnimator"), i);
    }
    e->barfill = f_ref(s, CRC("barfillTransform"));
    e->timer_text = f_ref(s, CRC("timerText"));
    /* PSP: lift the countdown clear of the part bubble so it stays readable on the small screen */
    if (e->timer_text >= 0 && W.obj[e->timer_text].rect >= 0)
        W.rect[W.obj[e->timer_text].rect].ap_y += 0.15f;
    e->second_bubble = f_ref(s, CRC("secondBubble"));
    e->box = f_ref(s, CRC("boxCollider"));
    e->ui = f_ref(s, CRC("uiObject"));
    e->spark_anim = f_ref(s, CRC("sparkAnimator"));
    int n;
    f_elems(s, CRC("sparks"), &n);
    e->nsparks = n > 3 ? 3 : n;
    for (int i = 0; i < e->nsparks; i++) {
        e->sparks[i] = ref_i(s, CRC("sparks"), i);
        particles_stop(e->sparks[i]);
    }
    e->rot = script_ref(s, CRC("rotationScript"), ST_RotationScript);
    e->player = script_ref(s, CRC("playerScript"), ST_PlayerScript);
    e->timer_tic = 0.2f;
    e->empty_bar = -0.9f;
    e->filled_scale = 1.8f;
    e->next_spark = f_int(s, CRC("nextSparkTime"), 5);
    e->spark_index = f_int(s, CRC("sparkIndex"), 0);
    e->xflip = f_int(s, CRC("xFlip"), 1);
    e->red_bats = f_int(s, CRC("redBats"), 0);
    e->want[0] = e->want[1] = 10;
    e->late_src = -1;
}

static void engine_start(Script *s)
{
    EngineS *e = s->self;
    e->sound_side = e->xflip < 0 ? 0 : e->xflip;
}

static int box_col(EngineS *e) { return col_of(e->box, COL_BOX); }

void engine_highlight(Script *s, int active)
{
    EngineS *e = s->self;
    if (active && e->wanting && e->player) {
        int any = 0;
        for (int i = 0; i < 2; i++)
            for (int k = 0; k < 2; k++)
                if (e->want[i] == player_hand(e->player, k)) any = 1;
        if (!any) active = 0;
    }
    if (e->interactible != active) {
        sr_enable(e->highlight, active);
        e->interactible = active;
    }
}

void engine_defect_start(Script *s, const int want[2], float time)
{
    EngineS *e = s->self;
    e->passed = 0;
    e->required = time;
    co_start(&e->counter);
    co_wait(&e->counter, e->timer_tic, 0);
    e->want[0] = want[0];
    e->want[1] = want[1];
    if (e->player) sr_set_sprite(sr_of(e->junk[0]), sprite_i(e->player, CRC("junkSpriteArray"), 0));
    obj_set_active(e->second_bubble, 0);
    anim_trigger(s->go, pid("break"));
    mgr_short(snd_at(s, CRC("dmgClip"), e->sound_side), 0);
    for (int i = 0; i < e->nsparks; i++) particles_play(e->sparks[i]);
    e->broken_state = 1;
    co_start(&e->broken);
    co_wait(&e->broken, 0.5f, 1);
}

static void engine_post_repair(Script *s)
{
    EngineS *e = s->self;
    if (e->player) sr_set_sprite(sr_of(e->junk[0]), sprite_i(e->player, CRC("junkSpriteArray"), 0));
    e->wanting = 0;
    if (e->rot) rotation_repair_finished(e->rot, s);
}

static const char *spark_trig[3] = {"sparkA", "sparkB", "sparkC"};

static void engine_update(Script *s, float dt)
{
    EngineS *e = s->self;
    if (co_tick(&e->counter, dt, T.udt)) {
        /* one Counter() tick: runs every timerTic */
        e->spark_timer++;
        if (e->spark_timer == e->next_spark) {
            e->spark_timer = 0;
            anim_trigger(e->spark_anim, pid(spark_trig[e->spark_index]));
            e->spark_index = (e->spark_index + 1) % 3;
            e->next_spark += irange(2, 6);
            if (e->next_spark > 14) e->next_spark = 3;
        }
        if (e->required != 99.0f)
            e->passed += e->timer_tic;
        float num = e->required > 0 ? e->passed / e->required : 1;
        if (num > 1) num = 1;
        if (num < 0) num = 0;
        Obj *bf = &W.obj[e->barfill];
        if (e->barfill >= 0) {
            bf->ly = num * e->empty_bar;
            bf->lsy = (1 - num) * e->filled_scale;
        }
        int num2 = (int)(e->required - e->passed);
        if (num2 == 3 && !e->is_late) {
            e->late_src = mgr_short(snd_at(s, CRC("lateClip"), e->sound_side), 0);
            e->is_late = 1;
        }
        char buf[16];
        snprintf(buf, sizeof buf, "%d", num2);
        set_text(e->timer_text, buf);
        if (num != 1.0f) {
            co_wait(&e->counter, e->timer_tic, 0);
        } else {
            co_stop(&e->counter);
            mgr_long(snd_at(s, CRC("exploClip"), e->sound_side));
            if (e->rot) rotation_gameover(e->rot, s->go, 0, e->xflip);
        }
    }
    if (co_tick(&e->broken, dt, T.udt)) {
        if (e->broken_state) {
            col_set_enabled(box_col(e), 1);
        } else {
            engine_post_repair(s);
            anim_trigger(e->spark_anim, pid("end"));
        }
        obj_set_active(e->ui, e->broken_state);
        co_stop(&e->broken);
    }
}

void engine_interaction(Script *s)
{
    EngineS *e = s->self;
    if (!e->interactible)
        return;
    if (!e->wanting) {
        if (e->want[1] != 10)
            obj_set_active(e->second_bubble, 1);
        for (int i = 0; i < 2; i++) {
            if (e->want[i] == 2 && e->red_bats)
                e->want[i] = irange(2, 4);
            if (e->player) sr_set_sprite(sr_of(e->junk[i]), sprite_i(e->player, CRC("junkSpriteArray"), e->want[i]));
            anim_trigger(e->junk_anim[i], pid("appear"));
            anim_trigger(s->go, pid("repair"));
            if (e->want[1] == 10)
                i++;
        }
        e->wanting = 1;
        mgr_short(snd_at(s, CRC("viewClip"), e->sound_side), 0);
    } else {
        for (int j = 0; j < 2; j++) {
            if (e->want[j] == 10)
                continue;
            for (int k = 0; k < 2; k++) {
                if (e->player && player_hand(e->player, k) == e->want[j]) {
                    sr_set_sprite(sr_of(e->junk[j]), f_asset(s, CRC("checkerSprite")));
                    anim_trigger(e->junk_anim[j], pid("appear"));
                    mgr_short(snd_at(s, CRC("insertClip"), e->want[j] - 1 + e->sound_side * 5), 0);
                    if (e->want[j] == 3)
                        e->want[j] = 2;
                    if (e->rot) rotation_junk_pool_add(e->rot, e->want[j]);
                    e->want[j] = 10;
                    player_junk_deposit(e->player, k);
                }
            }
        }
        if (e->want[0] == 10 && e->want[1] == 10) {
            for (int l = 0; l < e->nsparks; l++) particles_stop(e->sparks[l]);
            col_set_enabled(box_col(e), 0);
            if (e->rot) rotation_score_update(e->rot, s->go, s->go, e->xflip);
            co_stop(&e->counter);
            co_stop(&e->broken);
            e->broken_state = 0;
            co_start(&e->broken);
            co_wait(&e->broken, 2.0f, 1);
            mgr_short(snd_at(s, CRC("repairClip"), e->sound_side), 0);
            if (e->is_late) {
                e->is_late = 0;
                snd_stop(e->late_src);
                e->late_src = -1;
            }
        } else {
            anim_trigger(s->go, pid("repair"));
        }
    }
    engine_highlight(s, 1);
}
const ScriptVT vt_EngineScript = {sizeof(EngineS), engine_init, engine_start, engine_update};

/* ================================================================== AparatusScript */
typedef struct {
    int junk_sr, hourglass, hourglass_anim, barfill, hl[6], nhl, ap_hl, junk_hl, hint;
    Script *player;
    int want, junk, wanting, junk_ready, interactible;
    float tic, empty_bar, filled_scale, required, passed;
    Co counter;
} AparatusS;

static void aparatus_init(Script *s)
{
    AparatusS *a = s->self;
    a->junk_sr = sr_of(f_ref(s, CRC("junkSprite")));
    a->hourglass = f_ref(s, CRC("hourglass"));
    a->hourglass_anim = f_ref(s, CRC("hourglassAnimator"));
    a->barfill = f_ref(s, CRC("barfillTransform"));
    int n;
    f_elems(s, CRC("highlights"), &n);
    a->nhl = n > 6 ? 6 : n;
    for (int i = 0; i < a->nhl; i++) a->hl[i] = ref_i(s, CRC("highlights"), i);
    a->ap_hl = f_ref(s, CRC("aparatusHlight"));
    a->junk_hl = f_ref(s, CRC("junkHlight"));
    a->hint = f_ref(s, CRC("hintBubble"));
    a->player = script_ref(s, CRC("playerScript"), ST_PlayerScript);
    a->want = f_int(s, CRC("want"), 10);
    a->junk = f_int(s, CRC("junk"), 1);
    a->wanting = f_int(s, CRC("wanting"), 1);
    a->junk_ready = f_int(s, CRC("junkReady"), 0);
    a->tic = 0.2f; a->empty_bar = 0.32f; a->filled_scale = 0.6f; a->required = 5.0f;
}

static int ap_box(Script *s) { return col_of(s->go, COL_BOX); }

void aparatus_highlight(Script *s, int active)
{
    AparatusS *a = s->self;
    if (active && a->wanting && a->player && player_hand(a->player, 0) != a->want && player_hand(a->player, 1) != a->want)
        active = 0;
    if (a->interactible != active) {
        for (int i = 0; i < a->nhl; i++) sr_enable(a->hl[i], active);
        a->interactible = active;
    }
}

static void junk_state(Script *s, int state)
{
    AparatusS *a = s->self;
    if (a->junk_sr >= 0) {
        if (state) W.sr[a->junk_sr].d.flags |= WSR_ENABLED;
        else W.sr[a->junk_sr].d.flags &= ~WSR_ENABLED;
    }
    a->junk_ready = state;
    obj_set_active(a->ap_hl, !state);
    obj_set_active(a->junk_hl, state);
}

static void forged(Script *s)
{
    AparatusS *a = s->self;
    a->passed = 0;
    obj_set_active(a->hourglass, 0);
    if (a->barfill >= 0) W.obj[a->barfill].lsy = 0;
    junk_state(s, 1);
    anim_trigger(s->go, pid("finish"));
    mgr_short(snd_at(s, CRC("endClipArray"), a->junk - 1), 0);
    col_set_enabled(ap_box(s), 1);
}

void aparatus_interaction(Script *s)
{
    AparatusS *a = s->self;
    if (!a->interactible)
        return;
    if (!a->junk_ready) {
        anim_trigger(s->go, pid("activate"));
        mgr_short(snd_at(s, CRC("startClipArray"), a->junk - 1), 0);
        col_set_enabled(ap_box(s), 0);
        aparatus_highlight(s, 0);
        obj_set_active(a->hourglass, 1);
        anim_trigger(a->hourglass_anim, pid("appear"));
        co_start(&a->counter);
        co_wait(&a->counter, a->tic, 0);
        if (a->want != 10) {
            int num = 0;
            if (a->player && player_hand(a->player, 0) != a->want) num++;
            if (a->player) player_junk_deposit(a->player, num);
            obj_set_active(a->hint, 0);
            a->wanting = 0;
        }
        return;
    }
    if (a->junk == 4) {
        anim_trigger(s->go, pid("activate"));
        mgr_short(snd_at(s, CRC("startClipArray"), a->junk - 1), 0);
        col_set_enabled(ap_box(s), 0);
    } else {
        junk_state(s, 0);
        if (a->want != 10) {
            obj_set_active(a->hint, 1);
            a->wanting = 1;
        }
    }
    if (a->player) player_junk_pickup(a->player, a->junk);
    mgr_short(snd_at(s, CRC("pickupClipArray"), a->junk - 1), 0);
    aparatus_highlight(s, 1);
}

static void aparatus_update(Script *s, float dt)
{
    AparatusS *a = s->self;
    if (co_tick(&a->counter, dt, T.udt)) {
        a->passed += a->tic;
        float num = a->passed / a->required;
        if (num > 1) num = 1;
        if (a->barfill >= 0) {
            W.obj[a->barfill].ly = (1 - num) * a->empty_bar;
            W.obj[a->barfill].lsy = num * a->filled_scale;
        }
        if (num != 1.0f) {
            co_wait(&a->counter, a->tic, 0);
        } else {
            co_stop(&a->counter);
            forged(s);
        }
    }
}

static void aparatus_event(Script *s, int fn, int iarg, float farg)
{
    if (fn == EV_Ready)
        col_set_enabled(ap_box(s), 1);
}
const ScriptVT vt_AparatusScript = {sizeof(AparatusS), aparatus_init, 0, aparatus_update, 0, aparatus_event};

/* ================================================================== HitboxScript */
typedef struct { Script *target; } HitboxS;
void hitbox_set_target(Script *h, Script *target) { if (h) ((HitboxS *)h->self)->target = target; }

static void hitbox_trigger(Script *s, int other, int other_col)
{
    HitboxS *h = s->self;
    if (!h->target || other < 0)
        return;
    if (W.obj[other].tag == TAG_Player) {
        Script *pl = script_on(other, ST_PlayerScript);
        if (pl && player_superdash(pl) == 2 && !player_input_blocked(pl)) {
            float dx, dy;
            player_dash_dir(pl, &dx, &dy);
            ai_attacked(h->target, dx, dy);
        }
    } else if (W.obj[other].tag == TAG_veryDeadly) {
        ai_attacked(h->target, 0, 0);
    }
}
const ScriptVT vt_HitboxScript = {sizeof(HitboxS), 0, 0, 0, 0, 0, hitbox_trigger};

/* ================================================================== autotest bot
 * Plays the repair loop: inspect broken generators, forge the wanted parts (following machine chains),
 * deliver them. Steering only (stick + confirm), like a player. */
extern int player_target(Script *pl);
#include "w_path.h"

static int bot_has(Script *pl, int j) { return player_hand(pl, 0) == j || player_hand(pl, 1) == j; }
static void col_center(int go, float *x, float *y)
{
    obj_pos(go, x, y);
    int c = col_of(go, COL_BOX);
    if (c >= 0) { *x += W.col[c].d.ox; *y += W.col[c].d.oy; }
}

static int bot_goal_go = -1;
static float bot_stuck_t, bot_last_x, bot_last_y, bot_wiggle_t, bot_wx, bot_wy;

/* returns the machine object to walk to (and press at), or -1 */
static int bot_need(Script *pl, int j, int depth)
{
    if (depth > 4) return -1;
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (!s->alive || s->type != ST_AparatusScript || !obj_active(s->go)) continue;
        AparatusS *a = s->self;
        if (a->junk != j) continue;
        if (a->junk_ready) return s->go;
        if (a->counter.on) return s->go;            /* forging: wait next to it */
        if (a->wanting && a->want != 10 && !bot_has(pl, a->want)) {
            int g = bot_need(pl, a->want, depth + 1);
            if (g >= 0) return g;
            continue;
        }
        return s->go;
    }
    return -1;
}

static int bot_pick_goal(Script *pl)
{
    /* 1. deliver */
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (!s->alive || s->type != ST_EngineScript || !obj_active(s->go)) continue;
        EngineS *e = s->self;
        if (!e->counter.on) continue;
        int eg = e->box >= 0 ? e->box : s->go;
        if (!e->wanting) return eg;                   /* inspect */
        for (int k = 0; k < 2; k++)
            if (e->want[k] != 10 && bot_has(pl, e->want[k])) return eg;
    }
    /* 2. fetch a wanted part */
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (!s->alive || s->type != ST_EngineScript || !obj_active(s->go)) continue;
        EngineS *e = s->self;
        if (!e->counter.on || !e->wanting) continue;
        for (int k = 0; k < 2; k++) {
            if (e->want[k] == 10 || bot_has(pl, e->want[k])) continue;
            int g = bot_need(pl, e->want[k], 0);
            if (g >= 0) return g;
        }
    }
    return -1;
}

void bot_step(float *mx, float *my, int *press)
{
    *mx = *my = 0;
    *press = 0;
    Script *pl = find_script(ST_PlayerScript);
    static int idle;
    if (!pl || W.scene == 0 || player_input_blocked(pl) || T.scale == 0 || player_in_cutscene(pl) || player_in_menu(pl)) {
        /* intros / cutscenes / dialogue: confirm now and then */
        if (++idle >= 90) {
            idle = 0; *press = 1;
            Script *u = find_script(ST_UpscaleAnimatorScript);
            if (u) printf("[aw] bot idle: intro %d clip %s t %.2f blocked %d scale %.2f\n", upscale_in_intro(u), anim_state_clip(u->go), anim_state_time(u->go), pl ? player_input_blocked(pl) : -1, T.scale);
        }
        return;
    }
    idle = 0;
    float x, y;
    obj_pos(pl->go, &x, &y);
    int goal = bot_pick_goal(pl);
    bot_goal_go = goal;
    float tx = 0, ty = -0.5f;                       /* idle: middle of the room */
    if (goal >= 0) col_center(goal, &tx, &ty);
    /* at the machine: press when it is the player's interaction target */
    if (goal >= 0 && player_target(pl) == goal) {
        Script *a = script_on(goal, ST_AparatusScript);
        if (a && ((AparatusS *)a->self)->counter.on) return;   /* forging: wait */
        static int cool;
        if (++cool >= 8) { cool = 0; *press = 1; }
        return;
    }
    /* stuck detection */
    float moved = hypotf(x - bot_last_x, y - bot_last_y);
    bot_last_x = x; bot_last_y = y;
    if (moved < 0.005f) bot_stuck_t += 1.0f / 60; else bot_stuck_t = 0;
    if (bot_stuck_t > 0.7f) { bot_stuck_t = 0; bot_wiggle_t = 0.4f; float a = frandf() * 6.283f; bot_wx = cosf(a); bot_wy = sinf(a); }
    if (bot_wiggle_t > 0) { bot_wiggle_t -= 1.0f / 60; *mx = bot_wx; *my = bot_wy; return; }
    float d = hypotf(tx - x, ty - y);
    float sx = tx, sy = ty;
    if (d > 1.4f) {
        float wp[256];
        int n = path_find(x, y, tx, ty, wp, 128);
        for (int k = 0; k < n; k++) {
            float ex = wp[k * 2] - x, ey = wp[k * 2 + 1] - y;
            if (ex * ex + ey * ey > 0.35f * 0.35f) { sx = wp[k * 2]; sy = wp[k * 2 + 1]; break; }
        }
    } else if (goal < 0 && d < 0.3f) {
        return;
    }
    float dx = sx - x, dy = sy - y, l = hypotf(dx, dy);
    if (l > 1e-4f) { *mx = dx / l; *my = dy / l; }
}
