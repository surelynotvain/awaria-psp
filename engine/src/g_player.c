/* PlayerScript, RotationScript (level loop), UpscaleAnimatorScript (full screen sequences). */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "g_game.h"
#include "w_render.h"

static int pid(const char *n) { return anim_param_id(n); }

/* ================================================================== PlayerScript */
struct PlayerS {
    int sprite_t, rot_go, dust_anim, dust_obj, drone_go, drone_t, drone_holo;
    int shock_n; int shock[2];
    int hand_sr[2];
    int body_sr[12]; int nbody;
    int hitbox;              /* own CircleCollider2D */
    int inputs_blocked, dashing, superdash, move_drag, in_menu, in_cutscene, movement_mode;
    float move_speed, charge_ease;
    float dash_x, dash_y, sec_x, sec_y, mdx, mdy, mnx, mny;
    int hand[2], arr[2], old_slot;
    int game_began, babeczki_state, babeczki_count;
    int eat_src;
    float target_t;
    int target_go, old_target_go, target_type;
    Co dash; int dash_type;
    Co flash; int flash_white;
    Co eat_cd; Co boost;
    float boost_t;
};

static int hl_layer_ok(int mask, int layer) { return mask == -1 || ((mask >> layer) & 1); }

static int p_runnin = -2, p_isDashin, p_isEating, p_dust, p_appear;

static void player_cache_params(void)
{
    if (p_runnin != -2) return;
    p_runnin = pid("runnin"); p_isDashin = pid("isDashin"); p_isEating = pid("isEating");
    p_dust = pid("dust"); p_appear = pid("appear");
}

static int arr_sprite(Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n && e[i].kind == F_ASSET) ? e[i].v.i : SPR_NONE;
}

static void player_init(Script *s)
{
    PlayerS *p = s->self;
    player_cache_params();
    p->sprite_t = f_ref(s, CRC("spriteTransform"));
    p->rot_go = f_ref(s, CRC("rotationScript"));
    p->dust_anim = f_ref(s, CRC("dustAnimator"));
    p->dust_obj = f_ref(s, CRC("dustObject"));
    p->drone_go = f_ref(s, CRC("droneRb"));
    p->drone_t = f_ref(s, CRC("droneTransform"));
    p->drone_holo = f_ref(s, CRC("droneHolo"));
    int n;
    const WField *e = f_elems(s, CRC("shock"), &n);
    p->shock_n = n > 2 ? 2 : n;
    for (int i = 0; i < p->shock_n; i++) p->shock[i] = f_ref_at(s, &e[i]);
    e = f_elems(s, CRC("handSprite"), &n);
    for (int i = 0; i < 2; i++) p->hand_sr[i] = (e && i < n) ? sr_of(f_ref_at(s, &e[i])) : -1;
    e = f_elems(s, CRC("bodyparts"), &n);
    p->nbody = n > 12 ? 12 : n;
    for (int i = 0; i < p->nbody; i++) p->body_sr[i] = sr_of(f_ref_at(s, &e[i]));
    p->hitbox = col_of(s->go, COL_CIRCLE);
    p->inputs_blocked = f_int(s, CRC("inputsBlocked"), 1);
    p->superdash = f_int(s, CRC("superDash"), 0);
    p->move_drag = f_int(s, CRC("moveDrag"), 0);
    p->in_menu = f_int(s, CRC("inMenu"), 0);
    p->in_cutscene = f_int(s, CRC("inCutscene"), 0);
    p->movement_mode = f_int(s, CRC("movementMode"), 0);
    p->move_speed = f_float(s, CRC("moveSpeed"), 8);
    p->charge_ease = f_float(s, CRC("chargeEase"), 0.5f);
    float y; p->dash_x = f_vec(s, CRC("dashDir"), &y); p->dash_y = y;
    if (p->dash_x == 0 && p->dash_y == 0) p->dash_x = -1;
    p->sec_x = -1; p->sec_y = 0;
    p->eat_src = -1;
    p->target_go = p->old_target_go = -1;
}

static void player_start(Script *s)
{
    PlayerS *p = s->self;
    if (p->move_speed == 8.0f)
        p->babeczki_state = 1;
    p->hand[0] = p->hand[1] = 0;
    p->arr[0] = 0; p->arr[1] = 1;
    p->target_t = 0.05f;
}

int player_go(Script *pl) { return pl->go; }
int player_hand(Script *pl, int slot) { return ((PlayerS *)pl->self)->hand[slot]; }
int player_input_blocked(Script *pl) { return ((PlayerS *)pl->self)->inputs_blocked; }
void player_block_input(Script *pl, int on) { ((PlayerS *)pl->self)->inputs_blocked = on; }
int player_in_menu(Script *pl) { return ((PlayerS *)pl->self)->in_menu; }
int player_in_cutscene(Script *pl) { return ((PlayerS *)pl->self)->in_cutscene; }
void player_set_in_menu(Script *pl, int on) { ((PlayerS *)pl->self)->in_menu = on; }
void player_set_in_cutscene(Script *pl, int on) { ((PlayerS *)pl->self)->in_cutscene = on; }
int player_superdash(Script *pl) { return ((PlayerS *)pl->self)->superdash; }
int player_dashing(Script *pl) { return ((PlayerS *)pl->self)->dashing; }
void player_dash_dir(Script *pl, float *x, float *y) { *x = ((PlayerS *)pl->self)->dash_x; *y = ((PlayerS *)pl->self)->dash_y; }

void player_eat_end(Script *pl)
{
    PlayerS *p = pl->self;
    if (p->eat_src >= 0) { snd_stop(p->eat_src); p->eat_src = -1; }
}

void player_material_flash(Script *pl)
{
    PlayerS *p = pl->self;
    /* MaterialSwapper: white silhouette for 0.1 s (no-op when already white) */
    if (p->flash.on && p->flash_white) return;
    co_start(&p->flash);
}

void player_drone_holo_appear(Script *pl) { anim_trigger(((PlayerS *)pl->self)->drone_holo, p_appear); }

static void set_white(PlayerS *p, int on)
{
    for (int i = 0; i < p->nbody; i++) {
        if (p->body_sr[i] < 0) continue;
        if (on) W.sr[p->body_sr[i]].d.flags |= WSR_WHITE;
        else W.sr[p->body_sr[i]].d.flags &= ~WSR_WHITE;
    }
    p->flash_white = on;
}

static void hand_sprites(Script *s)
{
    PlayerS *p = s->self;
    for (int k = 0; k < 2; k++) {
        int slot_sr = p->hand_sr[p->arr[k]];
        uint32_t field = p->arr[k] == 0 ? CRC("fronthandSpriteArray") : CRC("backhandSpriteArray");
        if (slot_sr >= 0)
            W.sr[slot_sr].d.spr = arr_sprite(s, field, p->hand[k]);
    }
}

static void turnaround(Script *s, float scale)
{
    PlayerS *p = s->self;
    obj_scale(p->sprite_t, scale, 1);
    int t = p->arr[0]; p->arr[0] = p->arr[1]; p->arr[1] = t;
    hand_sprites(s);
}

void player_junk_pickup(Script *pl, int food)
{
    PlayerS *p = pl->self;
    p->hand[p->old_slot] = food;
    hand_sprites(pl);
    p->old_slot = (p->old_slot + 1) % 2;
    player_material_flash(pl);
}

void player_junk_deposit(Script *pl, int slot)
{
    PlayerS *p = pl->self;
    p->hand[slot] = 0;
    hand_sprites(pl);
    p->old_slot = slot;
}

void player_gameover(Script *pl)
{
    PlayerS *p = pl->self;
    Script *rot = script_on(p->rot_go, ST_RotationScript);
    if (rot) rotation_gameover(rot, pl->go, 1, 1);
}

void player_post_eat(Script *pl)
{
    PlayerS *p = pl->self;
    p->move_speed += 1;
    co_start(&p->boost);
    p->boost.wait = 2.0f;
}

/* IHighlightable dispatch */
static void target_highlight(int go, int on)
{
    Script *e = script_on(go, ST_EngineScript);
    if (e) { engine_highlight(e, on); return; }
    Script *a = script_on(go, ST_AparatusScript);
    if (a) aparatus_highlight(a, on);
}

static void target_update(Script *s)
{
    PlayerS *p = s->self;
    float x, y;
    obj_pos(s->go, &x, &y);
    int mask = f_int(s, CRC("mask"), -1);
    int col = -1;
    int go = phys_overlap_circle(x, y, 1.0f, mask, &col);
    (void)hl_layer_ok;
    if (go != p->old_target_go) {
        if (p->old_target_go >= 0)
            target_highlight(p->old_target_go, 0);
        if (go >= 0)
            target_highlight(go, 1);
        p->old_target_go = go;
    }
    p->target_go = go;
}

void player_input_confirm(Script *pl)
{
    PlayerS *p = pl->self;
    if (g_autotest) printf("[aw] confirm target %d blocked %d\n", p->target_go, p->inputs_blocked);
    if (p->target_go < 0 || p->inputs_blocked)
        return;
    Script *e = script_on(p->target_go, ST_EngineScript);
    if (e) { engine_interaction(e); return; }
    Script *a = script_on(p->target_go, ST_AparatusScript);
    if (a) aparatus_interaction(a);
}

static void dash_start(Script *s, int type)
{
    PlayerS *p = s->self;
    p->dash_type = type;
    co_start(&p->dash);
}

static void dash_run(Script *s)
{
    PlayerS *p = s->self;
    Script *rot = script_on(p->rot_go, ST_RotationScript);
    switch (p->dash.step) {
    case 0:
        p->dashing = 1;
        player_eat_end(s);
        if (p->dash_type == 3) {
            if (!p->game_began && rot && rotation_dash_count(rot) < 5) {
                mgr_short(snd_at(s, CRC("tutorialClips"), rotation_dash_count(rot)), 0);
                rotation_dash_counter(rot);
            } else if (p->superdash == 0) {
                mgr_short(snd_pick(s, CRC("dashClips")), 0);
            }
            p->movement_mode = 1;
            co_wait(&p->dash, 0.05f, 1);
            return;
        }
        /* fallthrough */
    case 1: {
        float x, y;
        obj_pos(s->go, &x, &y);
        if (p->superdash != 0) {
            mgr_short(snd_pick(s, CRC("superdashClips")), 0);
            p->superdash = 2;
            player_material_flash(s);
            p->move_speed = 14;
            col_set_radius(p->hitbox, 1.0f);
            p->dash_x = p->sec_x; p->dash_y = p->sec_y;
            int si = p->dash_type - 3;
            if (si >= 0 && si < p->shock_n) {
                Script *sh = script_on(p->shock[si], ST_ShockwaveScript);
                if (sh) shock_emit(sh, x, y, 1, p->dash_x, p->dash_y);
            }
            obj_scale(p->dust_obj, W.obj[p->sprite_t].lsx, W.obj[p->sprite_t].lsy);
        }
        obj_set_pos(p->dust_obj, x, y);
        obj_set_active(p->dust_obj, 1);
        anim_trigger(p->dust_anim, p_dust);
        p->movement_mode = 2;
        anim_trigger(s->go, p_isDashin);
        /* playerHitbox.enabled = false; true: re-fires trigger enters */
        col_set_enabled(p->hitbox, 0);
        col_set_enabled(p->hitbox, 1);
        extern void phys_forget_pairs(int col);
        phys_forget_pairs(p->hitbox);
        co_wait(&p->dash, 0.1f, 2);
        return;
    }
    case 2:
        if (p->superdash != 0) {
            p->superdash = p->dash_type;
            player_material_flash(s);
            p->move_speed = 12;
            col_set_radius(p->hitbox, 0.1f);
        }
        p->movement_mode = -2;
        co_wait(&p->dash, 0.1f, 3);
        return;
    case 3:
        p->movement_mode = -1;
        p->dashing = 0;
        co_wait(&p->dash, 0.25f, 4);
        return;
    case 4:
        if (p->superdash == 0) {
            co_wait(&p->dash, 0.1f, 5);
            return;
        }
        p->superdash = 1;
        /* fallthrough */
    case 5:
        p->movement_mode = 0;
        co_stop(&p->dash);
        return;
    }
}

static void player_update(Script *s, float dt)
{
    PlayerS *p = s->self;
    /* coroutines run regardless of input state */
    if (co_tick(&p->dash, dt, T.udt))
        dash_run(s);
    if (co_tick(&p->flash, dt, T.udt)) {
        if (p->flash.step == 0) {
            if (!p->flash_white) {
                set_white(p, 1);
                co_wait(&p->flash, 0.1f, 1);
            } else {
                co_wait(&p->flash, 0.1f, 2);
            }
        } else {
            if (p->flash.step == 1) set_white(p, 0);
            co_stop(&p->flash);
        }
    }
    if (co_tick(&p->eat_cd, dt, T.udt)) { p->babeczki_state = 1; co_stop(&p->eat_cd); }
    if (co_tick(&p->boost, dt, T.udt)) { p->move_speed -= 1; co_stop(&p->boost); }
    p->target_t -= dt;
    if (p->target_t <= 0) {
        p->target_t += 0.05f;
        target_update(s);
    }
    if (p->in_menu || p->in_cutscene)
        return;
    if (!p->inputs_blocked) {
        float nx = IN.mx, ny = IN.my;
        if (nx < 0.08f && nx > -0.08f) nx = 0;
        if (ny < 0.08f && ny > -0.08f) ny = 0;
        float l = sqrtf(nx * nx + ny * ny);
        p->mnx = l > 1e-5f ? nx / l : 0;
        p->mny = l > 1e-5f ? ny / l : 0;
        if (p->mnx != 0 || p->mny != 0) {
            if (p->movement_mode < 2 && p->movement_mode != -2) {
                p->sec_x = p->dash_x = p->mnx;
                p->sec_y = p->dash_y = p->mny;
            } else {
                p->sec_x = p->mnx;
                p->sec_y = p->mny;
            }
        }
        if (IN.secondary) {
            if (p->movement_mode == 0) {
                dash_start(s, 3);
            } else if (p->movement_mode == -2 && p->superdash == 3) {
                co_stop(&p->dash);
                dash_start(s, 4);
                p->dash.step = 1;
            } else if (p->babeczki_state == 1 && p->mnx == 0 && p->mny == 0) {
                co_stop(&p->eat_cd);
                p->babeczki_state = 2;
                p->mdx = p->mnx; p->mdy = p->mny;
                anim_trigger(s->go, p_isEating);
                player_eat_end(s);
                p->eat_src = mgr_short(f_asset(s, CRC("eatClip")), 0);
                co_start(&p->eat_cd);
                p->eat_cd.wait = 0.7f;
                p->babeczki_count++;
            }
        }
    } else {
        p->mnx = p->mny = 0;
    }
    if (p->move_drag && (p->mdx != p->mnx || p->mdy != p->mny)) {
        float md = 10.0f / p->charge_ease * dt;
        float dx = p->mnx - p->mdx, dy = p->mny - p->mdy;
        p->mdx += fabsf(dx) <= md ? dx : (dx > 0 ? md : -md);
        p->mdy += fabsf(dy) <= md ? dy : (dy > 0 ? md : -md);
    } else {
        p->mdx = p->mnx; p->mdy = p->mny;
    }
}

static void player_fixed(Script *s, float dt)
{
    PlayerS *p = s->self;
    Script *rot = script_on(p->rot_go, ST_RotationScript);
    /* drone follows */
    float px, py, dx, dy;
    obj_pos(s->go, &px, &py);
    obj_pos(p->drone_go, &dx, &dy);
    float dist = sqrtf((px - dx) * (px - dx) + (py - dy) * (py - dy));
    if (dist > 0.3f && (!rot || rotation_drone_state(rot) != 1)) {
        float nx = (px - dx) / dist, ny = (py - dy) / dist;
        phys_set_velocity(p->drone_go, nx * p->move_speed * dist, ny * p->move_speed * dist);
        if (nx >= 0.01f && W.obj[p->drone_t].lsx == 1.0f) obj_scale(p->drone_t, -1, 1);
        else if (nx <= -0.01f && W.obj[p->drone_t].lsx == -1.0f) obj_scale(p->drone_t, 1, 1);
    } else {
        phys_set_velocity(p->drone_go, 0, 0);
    }
    if (p->movement_mode == 2) {
        float vx = p->dash_x + p->mdx, vy = p->dash_y + p->mdy;
        float l = sqrtf(vx * vx + vy * vy);
        if (l > 1e-5f) { vx /= l; vy /= l; }
        phys_set_velocity(s->go, vx * p->move_speed * 3, vy * p->move_speed * 3);
    } else if (p->movement_mode == -2) {
        phys_set_velocity(s->go, p->dash_x * 2, p->dash_y * 2);
    } else {
        if (p->mdx == 0 && p->mdy == 0) {
            anim_set_bool(s->go, p_runnin, 0);
            return;
        }
        phys_set_velocity(s->go, p->mdx * p->move_speed, p->mdy * p->move_speed);
        if (p->mdx >= 0.01f && p->arr[0] == 0) turnaround(s, -1);
        else if (p->mdx <= -0.01f && p->arr[0] == 1) turnaround(s, 1);
    }
    if (!p->game_began && rot && rotation_dash_count(rot) == 5) {
        p->game_began = 1;
        rotation_game_starter(rot);
    }
    anim_set_bool(s->go, p_runnin, 1);
    player_eat_end(s);
}

static void player_event(Script *s, int fn, int iarg, float farg)
{
    if (fn == EV_FootstepsAudio)
        mgr_short(snd_pick(s, CRC("footstep")), 0);
    else if (fn == EV_PostEat)
        player_post_eat(s);
}

static void player_trigger(Script *s, int other, int other_col)
{
    if (other >= 0 && W.obj[other].tag == TAG_Deadly)
        player_gameover(s);
}
const ScriptVT vt_PlayerScript = {sizeof(PlayerS), player_init, player_start, player_update, player_fixed, player_event, player_trigger};

/* ================================================================== RotationScript */
#define MAXJ 24
#define MAXE 8
struct RotationS {
    Script *good[MAXE]; int ngood, total;
    int junk[MAXJ], njunk, jover[MAXJ], njover;
    int score_go, cap_go;
    int boss_go[4]; int nboss;
    Script *player, *upscale;
    int wave_t, drone_anim, big_striga, dash_tut, dash_text, electro_wipe;
    int drone_state, dash_count, repair_goal, double_after, two_after, three_after, has_intro, calm, defect_cap, solo_haunt;
    float base_time, difficulty;
    int repair_count, defect_count, want[2], game_ended, delay_state, xflip_delay, dead_delay;
    int loc_delay, ani_delay;
    Co rot; Co regen;
};

static int p_start, p_death, p_fixed;
static Script *ghost_of(int go)
{
    static const int types[] = {ST_ZmoraAiScript, ST_StrigaAiScript, ST_CutwireAiScript, ST_NikitaAiScript, ST_DoppelAiScript, ST_DogoAiScript, ST_MinionAiScript};
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
        Script *g = script_on(go, types[i]);
        if (g) return g;
    }
    return 0;
}

static void int_list(Script *s, uint32_t field, int *out, int *n, int max)
{
    int c;
    const WField *e = f_elems(s, field, &c);
    *n = 0;
    for (int i = 0; e && i < c && *n < max; i++)
        out[(*n)++] = e[i].v.i;
}

static void rotation_init(Script *s)
{
    RotationS *r = s->self;
    p_start = pid("start"); p_death = pid("death"); p_fixed = pid("fixed");
    int c;
    const WField *e = f_elems(s, CRC("goodEngines"), &c);
    r->ngood = 0;
    for (int i = 0; e && i < c && r->ngood < MAXE; i++) {
        Script *en = script_on(f_ref_at(s, &e[i]), ST_EngineScript);
        if (en) r->good[r->ngood++] = en;
    }
    int_list(s, CRC("junkPool"), r->junk, &r->njunk, MAXJ);
    int_list(s, CRC("junkPoolOverride"), r->jover, &r->njover, MAXJ);
    r->score_go = f_ref(s, CRC("scoreText"));
    r->cap_go = f_ref(s, CRC("capText"));
    e = f_elems(s, CRC("boss"), &c);
    r->nboss = 0;
    for (int i = 0; e && i < c && r->nboss < 4; i++) r->boss_go[r->nboss++] = f_ref_at(s, &e[i]);
    r->player = script_on(f_ref(s, CRC("playerScript")), ST_PlayerScript);
    r->upscale = script_on(f_ref(s, CRC("upscaleAniScript")), ST_UpscaleAnimatorScript);
    r->wave_t = f_ref(s, CRC("waveSprite"));
    r->drone_anim = f_ref(s, CRC("droneAnimator"));
    r->big_striga = f_ref(s, CRC("bigStriga"));
    r->dash_tut = f_ref(s, CRC("dashTutorial"));
    r->dash_text = f_ref(s, CRC("dashText"));
    r->electro_wipe = f_ref(s, CRC("electroWipe"));
    r->drone_state = f_int(s, CRC("droneState"), 0);
    r->dash_count = f_int(s, CRC("dashCount"), 5);
    r->repair_goal = f_int(s, CRC("repairGoal"), 8);
    r->double_after = f_int(s, CRC("doubleAfter"), 0);
    r->two_after = f_int(s, CRC("twoGenAfter"), 0);
    r->three_after = f_int(s, CRC("threeGenAfter"), 0);
    r->base_time = f_float(s, CRC("baseTime"), 40);
    r->has_intro = f_int(s, CRC("hasIntro"), 0);
    r->calm = f_int(s, CRC("calmLevel"), 1);
    r->defect_cap = f_int(s, CRC("defectCap"), 1);
    r->solo_haunt = f_int(s, CRC("soloHaunt"), 0);
    r->want[0] = r->want[1] = 10;
    r->loc_delay = s->go;
    r->xflip_delay = 1;
}

static void dash_text_update(RotationS *r)
{
    char buf[96];
    snprintf(buf, sizeof buf, "%s %d / 4 %s", menu_txt(20), r->dash_count - 1, menu_txt(21));
    set_text(r->dash_text, buf);
}

static void rotation_start(Script *s)
{
    RotationS *r = s->self;
    r->loc_delay = s->go;
    r->xflip_delay = 1;
    r->ani_delay = r->drone_anim;
    r->difficulty = prefs_int("mode", 0);
    if (r->difficulty == 2) {
        r->drone_state = 2;
        obj_set_active(r->drone_anim, 0);
    }
    r->total = r->ngood;
    char buf[32];
    snprintf(buf, sizeof buf, "%d", r->repair_count);
    set_text(r->score_go, buf);
    snprintf(buf, sizeof buf, "   / %d", r->repair_goal);
    set_text(r->cap_go, buf);
    if (r->nboss == 3 && M.intro_ready) {
        obj_set_active(r->dash_tut, 1);
        r->dash_count = 1;
        dash_text_update(r);
    }
}

int rotation_dash_count(Script *rot) { return ((RotationS *)rot->self)->dash_count; }
int rotation_drone_state(Script *rot) { return ((RotationS *)rot->self)->drone_state; }
int rotation_solo_haunt(Script *rot) { return ((RotationS *)rot->self)->solo_haunt; }
void rotation_set_solo_haunt(Script *rot, int v) { ((RotationS *)rot->self)->solo_haunt = v; }
void rotation_junk_pool_add(Script *rot, int item)
{
    RotationS *r = rot->self;
    if (r->njunk < MAXJ) r->junk[r->njunk++] = item;
}

void rotation_dash_counter(Script *rot)
{
    RotationS *r = rot->self;
    r->dash_count++;
    if (r->dash_count == 5) {
        M.intro_ready = 0;
        obj_set_active(r->dash_tut, 0);
        return;
    }
    dash_text_update(r);
}

void rotation_game_starter(Script *rot) { co_start(&((RotationS *)rot->self)->rot); }

static int randomized_want(RotationS *r)
{
    if (r->njover == 0) {
        if (r->njunk == 0) return 1;
        int i = irange(0, r->njunk);
        int v = r->junk[i];
        r->junk[i] = r->junk[--r->njunk];
        return v;
    }
    int v = r->jover[0];
    memmove(r->jover, r->jover + 1, sizeof(int) * (r->njover - 1));
    r->njover--;
    return v;
}

static void duplicate_remover(RotationS *r)
{
    int item = r->want[1];
    r->want[1] = randomized_want(r);
    if (r->want[0] == r->want[1])
        duplicate_remover(r);
    if (r->njunk < MAXJ) r->junk[r->njunk++] = item;
}

static void defect_creator(Script *s)
{
    RotationS *r = s->self;
    if (!r->ngood) return;
    int idx = irange(0, r->ngood);
    Script *e = r->good[idx];
    for (int i = idx; i + 1 < r->ngood; i++) r->good[i] = r->good[i + 1];
    r->ngood--;
    r->want[0] = randomized_want(r);
    r->want[1] = r->defect_count >= r->double_after ? randomized_want(r) : 10;
    if (r->want[0] == r->want[1])
        duplicate_remover(r);
    r->defect_count++;
    if (r->difficulty == 0)
        engine_defect_start(e, r->want, 99);
    else
        engine_defect_start(e, r->want, r->base_time - r->repair_count - r->difficulty * 10 + 10);
    camera_shake(0.2f, 0.2f);
}

void rotation_ghost_intro(Script *s)
{
    RotationS *r = s->self;
    if (r->game_ended) return;
    if (r->upscale && upscale_drone_hit(r->upscale)) {
        r->delay_state = 1;
        return;
    }
    if (g_autotest) printf("[aw] ghost intro ready %d has %d\n", M.intro_ready, r->has_intro);
    if (M.intro_ready && r->has_intro) {
        M.intro_ready = 0;
        if (r->player) player_block_input(r->player, 1);
        if (r->upscale) upscale_activator(r->upscale, "introStart", s->go, 1);
        T.scale = 0;
    }
    r->solo_haunt = 1;
    if (r->nboss) {
        Script *g = ghost_of(r->boss_go[0]);
        if (g) ai_spawn(g);
    }
}

void rotation_repair_finished(Script *s, Script *engine)
{
    RotationS *r = s->self;
    if (r->ngood < MAXE) r->good[r->ngood++] = engine;
    if (r->defect_cap == 1 && r->repair_count >= r->two_after) r->defect_cap++;
    else if (r->defect_cap == 2 && r->repair_count >= r->three_after) r->defect_cap++;
    if (r->repair_count == 1 && r->calm) {
        rotation_ghost_intro(s);
    } else if (r->solo_haunt && r->nboss > 1) {
        r->solo_haunt = 0;
        for (int i = 1; i < r->nboss; i++) {
            Script *g = ghost_of(r->boss_go[i]);
            if (g) ai_spawn(g);
        }
    }
}

void rotation_score_update(Script *s, int gen, int gen_anim, int xflip)
{
    RotationS *r = s->self;
    if (r->game_ended) return;
    if (r->upscale && upscale_drone_hit(r->upscale) && r->repair_count + 1 == r->repair_goal) {
        r->delay_state = 3;
        r->loc_delay = gen;
        r->ani_delay = gen_anim;
        r->xflip_delay = xflip;
        return;
    }
    r->repair_count++;
    char buf[16];
    snprintf(buf, sizeof buf, "%d", r->repair_count);
    set_text(r->score_go, buf);
    if (r->repair_count == r->repair_goal) {
        if (r->difficulty == 2) {
            char key[16];
            snprintf(key, sizeof key, "%dH", M.next_level);
            if (prefs_int(key, 0) == 0) {
                prefs_set_int(key, 1);
                prefs_set_int("hardCount", prefs_int("hardCount", 0) + 1);
            }
        }
        obj_scale(r->wave_t, (float)xflip, 1);
        if (r->player) player_block_input(r->player, 1);
        r->game_ended = 1;
        Script *rs = s;
        mgr_long(f_asset(rs, CRC("victoryClip")));
        if (r->upscale) {
            upscale_activator(r->upscale, "genFix", gen, 1);
            upscale_set_gen_anim(r->upscale, gen_anim);
        }
        mgr_audio_pause(1);
        mgr_audio_remove_all();
        T.scale = 0;
    } else {
        anim_trigger(gen_anim, p_fixed);
    }
}

int g_god;   /* autotest: ghosts cannot kill */
void rotation_gameover(Script *s, int location, int dead, int xflip)
{
    RotationS *r = s->self;
    if (r->game_ended) return;
    if (dead && g_god) return;
    if (dead) {
        if ((r->player && player_dashing(r->player)) || r->drone_state == 1)
            return;
    } else if (r->upscale && upscale_drone_hit(r->upscale)) {
        r->delay_state = 2;
        r->loc_delay = location;
        r->dead_delay = dead;
        r->xflip_delay = xflip;
        return;
    }
    if (dead)
        mgr_long(snd_pick(s, CRC("dmgAudio")));
    T.scale = 0;
    mgr_audio_pause(1);
    if (r->player) player_block_input(r->player, 1);
    if (dead && r->drone_state == 0) {
        if (r->upscale) upscale_set_drone_hit(r->upscale, 1);
        r->drone_state++;
        co_start(&r->regen);
        if (r->upscale && r->player) upscale_activator(r->upscale, "droneDeath", player_go(r->player), 1);
        return;
    }
    sr_enable(s->go, 1);   /* blackout */
    r->game_ended = 1;
    mgr_audio_remove_all();
    if (r->upscale) {
        if (dead) upscale_activator(r->upscale, "charaDeath", location, 1);
        else upscale_activator(r->upscale, "genDeath", location, xflip);
    }
}

void rotation_anti_dronebug(Script *s)
{
    RotationS *r = s->self;
    if (r->delay_state == 0) return;
    int d = r->delay_state;
    r->delay_state = 0;
    if (d == 1) rotation_ghost_intro(s);
    else if (d == 2) rotation_gameover(s, r->loc_delay, r->dead_delay, r->xflip_delay);
    else if (d == 3) rotation_score_update(s, r->loc_delay, r->ani_delay, r->xflip_delay);
}

int rotation_cam_go(Script *rot) { (void)rot; return W.camera; }

static void rotation_update(Script *s, float dt)
{
    RotationS *r = s->self;
    if (co_tick(&r->rot, dt, T.udt)) {
        switch (r->rot.step) {
        case 0:
            if (!r->calm) { co_wait(&r->rot, 0.5f, 1); break; }
            r->rot.step = 2;
            /* fallthrough */
        case 2:
            if (r->game_ended || r->defect_count >= r->repair_goal) { co_stop(&r->rot); break; }
            co_wait(&r->rot, 0.5f, 3);
            break;
        case 1:
            if (r->big_striga >= 0 && script_on(r->big_striga, ST_BigStrigaScript))
                boss_start(script_on(r->big_striga, ST_BigStrigaScript));
            else
                rotation_ghost_intro(s);
            r->rot.step = 2;
            break;
        case 3:
            if (r->ngood > r->total - r->defect_cap)
                defect_creator(s);
            co_wait(&r->rot, 2.5f, 2);
            break;
        }
    }
    if (co_tick(&r->regen, dt, T.udt)) {
        switch (r->regen.step) {
        case 0: co_wait(&r->regen, 0.01f, 1); break;
        case 1:
            anim_trigger(r->drone_anim, p_death);
            if (r->electro_wipe >= 0 && script_on(r->electro_wipe, ST_ElectroWipeScript)) {
                mgr_short(f_asset(s, CRC("electroWipeClip")), 0);
                float x, y;
                obj_pos(r->player ? player_go(r->player) : s->go, &x, &y);
                electrowipe_activate(script_on(r->electro_wipe, ST_ElectroWipeScript), x, y);
            }
            co_wait(&r->regen, 0.3f, 2);
            break;
        case 2:
            mgr_short(f_asset(s, CRC("droneDeathClip")), 0);
            co_wait(&r->regen, 0.5f, 3);
            break;
        case 3:
            r->drone_state++;
            co_wait(&r->regen, 2.7f + 6.0f * r->difficulty, 4);
            break;
        case 4:
            anim_trigger(r->drone_anim, p_start);
            if (r->player) {
                player_drone_holo_appear(r->player);
                player_material_flash(r->player);
            }
            mgr_short(f_asset(s, CRC("droneRepairAudio")), 0);
            r->drone_state = 0;
            co_stop(&r->regen);
            break;
        }
    }
}
const ScriptVT vt_RotationScript = {sizeof(RotationS), rotation_init, rotation_start, rotation_update};

/* ================================================================== UpscaleAnimatorScript */
typedef struct { int anim, tran, flip, handle, gen_anim, screen_anim, player_go, score; int drone_hit, in_intro; int ghost_tier, tier; } UpscaleS;

static void upscale_init(Script *s)
{
    UpscaleS *u = s->self;
    u->anim = s->go;
    u->tran = f_ref(s, CRC("tranScript"));
    u->flip = f_ref(s, CRC("flipSprite"));
    u->handle = f_ref(s, CRC("handle"));
    u->gen_anim = f_ref(s, CRC("genAnim"));
    u->screen_anim = f_ref(s, CRC("screenAnim"));
    u->player_go = f_ref(s, CRC("playerScript"));
    u->score = f_ref(s, CRC("scoreObject"));
    u->drone_hit = f_int(s, CRC("droneHit"), 0);
    u->in_intro = f_int(s, CRC("inIntro"), 0);
    u->ghost_tier = f_ref(s, CRC("ghostTier"));
    u->tier = f_int(s, CRC("tierNumber"), 0);
}

int upscale_drone_hit(Script *s) { return ((UpscaleS *)s->self)->drone_hit; }
void upscale_set_drone_hit(Script *s, int v) { ((UpscaleS *)s->self)->drone_hit = v; }
int upscale_in_intro(Script *s) { return ((UpscaleS *)s->self)->in_intro; }
void upscale_set_gen_anim(Script *s, int go) { ((UpscaleS *)s->self)->gen_anim = go; }

void upscale_activator(Script *s, const char *command, int target, int xflip)
{
    UpscaleS *u = s->self;
    if (!strcmp(command, "introStart")) {
        set_text(u->ghost_tier, menu_txt(u->tier));
        mgr_long(f_asset(s, CRC("ghostIntroAudio")));
        mgr_music_muffle(20);
    }
    u->in_intro = 0;
    obj_scale(u->flip, (float)xflip, W.obj[u->flip].lsy);
    float x, y;
    obj_pos(target, &x, &y);
    obj_set_pos(u->handle, x, y);
    anim_trigger(u->anim, pid(command));
}

static void invoke_wipe(int target_go, const char *method)
{
    for (int i = 0; i < W.nscript; i++) {
        Script *t = &W.script[i];
        if (!t->alive || t->go != target_go) continue;
        switch (t->type) {
        case ST_ZmoraAiScript: case ST_StrigaAiScript: case ST_CutwireAiScript: case ST_NikitaAiScript:
        case ST_DoppelAiScript: case ST_DogoAiScript:
            if (!strcmp(method, "WipeEvent")) ghost_wipe_event(t);
            break;
        case ST_BigStrigaScript: {
            extern void boss_wipe_event(Script *b);
            if (!strcmp(method, "WipeEvent")) boss_wipe_event(t);
            break;
        }
        case ST_BoilerScript: { extern void boiler_wipe(Script *b); if (!strcmp(method, "WipeEvent")) boiler_wipe(t); break; }
        case ST_CoilboxScript: { extern void coilbox_wipe(Script *b); if (!strcmp(method, "WipeEvent")) coilbox_wipe(t); break; }
        case ST_BulletSpreadPoolScript: if (!strcmp(method, "WipeEvent")) spreadpool_wipe(t); break;
        case ST_BulletSpreadScript: if (!strcmp(method, "Wipe")) spread_wipe(t); break;
        }
    }
}

static void upscale_victory(Script *s)
{
    UpscaleS *u = s->self;
    T.scale = 1;
    anim_trigger(u->gen_anim, pid("fixed"));
    anim_trigger(u->screen_anim, pid("genFix"));
    /* wipeEvent.Invoke(): persistent calls */
    const WField *we = f_get(s, CRC("wipeEvent"));
    if (we) {
        const WField *pc = f_child(s, we);                 /* m_PersistentCalls (struct) */
        const WField *calls = pc ? f_child(s, pc) : 0;     /* m_Calls (array) */
        int n = calls ? calls->count : 0;
        const WField *c = calls ? f_child(s, calls) : 0;
        for (int i = 0; c && i < n; i++) {
            const WField *call = f_child(s, &c[i]);
            if (!call) continue;
            int tgo = -1;
            const char *method = "";
            for (int k = 0; k < c[i].count; k++) {
                if (call[k].name == CRC("m_Target")) tgo = f_ref_at(s, &call[k]);
                if (call[k].name == CRC("m_MethodName") && call[k].kind == F_STR) method = w_string(call[k].v.i);
            }
            if (tgo >= 0 && *method)
                invoke_wipe(tgo, method);
        }
    }
    obj_set_active(u->score, 0);
}

static void upscale_unlock_time(Script *s)
{
    UpscaleS *u = s->self;
    T.scale = 1;
    mgr_audio_pause(0);
    if (u->drone_hit) u->drone_hit = 0;
    else mgr_music_muffle(10);
    Script *pl = script_on(u->player_go, ST_PlayerScript);
    if (pl) {
        player_block_input(pl, 0);
        extern Script *player_rotation(Script *pl);
        Script *rot = player_rotation(pl);
        if (rot) rotation_anti_dronebug(rot);
    }
}

static void upscale_event(Script *s, int fn, int iarg, float farg)
{
    UpscaleS *u = s->self;
    switch (fn) {
    case EV_Transition: {
        Script *t = script_on(u->tran, ST_TransitionScript);
        if (t) transition_go(t);
        break;
    }
    case EV_Victory: upscale_victory(s); break;
    case EV_UnlockTime: upscale_unlock_time(s); break;
    case EV_IntroLoop: u->in_intro = 1; break;
    case EV_GameoverAudio: mgr_long(f_asset(s, CRC("gameoverClip"))); break;
    }
}
const ScriptVT vt_UpscaleAnimatorScript = {sizeof(UpscaleS), upscale_init, 0, 0, 0, upscale_event};

Script *player_rotation(Script *pl) { return script_on(((PlayerS *)pl->self)->rot_go, ST_RotationScript); }
int player_target(Script *pl) { return ((PlayerS *)pl->self)->target_go; }
