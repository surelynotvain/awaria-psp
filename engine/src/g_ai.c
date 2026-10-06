/* AiScript / GhostAiScript and the ghosts: Zmora, Striga, Minion, Dogo, Cutwire, Nikita, Doppel. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "g_game.h"
#include "w_path.h"

#define MAXWP 96
static int pid(const char *n) { return anim_param_id(n); }

typedef struct {
    /* AiScript */
    int sprite_t, player_t, death_col;
    int player_mask, dash_mask, spawn_clip;
    float move_speed, dash_power, next_wp_dist, attack_range, attack_width, spawn_time;
    float path[MAXWP * 2]; int npath, cur_wp; float path_t;
    float aim_x, aim_y;
    int busy, hitable, already_spawned;
    Co post; float post_len;
    Co chase;
    /* GhostAiScript */
    Script *rot;
    float general_cooldown, cooldown_tic;
    int throw_index;
    Co tthrow; float tt_angle, tt_mult;
    /* type specific */
    Co act, act2, act3;
    int special_ready, ival, ival2, slam_count;
    float fval;
    int gen_state;
    float dog_dx, dog_dy, org_x, org_y;
} Ai;

static int is_ghost_type(int t)
{
    return t == ST_ZmoraAiScript || t == ST_StrigaAiScript || t == ST_CutwireAiScript || t == ST_NikitaAiScript ||
           t == ST_DoppelAiScript || t == ST_DogoAiScript;
}

static void pos(Script *s, float *x, float *y) { obj_pos(s->go, x, y); }

static void flip_to(Ai *a, float sx) { if (a->sprite_t >= 0) obj_scale(a->sprite_t, sx, 1); }

static void ai_base_init(Script *s)
{
    Ai *a = s->self;
    a->sprite_t = f_ref(s, CRC("spriteTransform"));
    a->player_t = f_ref(s, CRC("playerTransform"));
    a->death_col = col_of(s->go, COL_CIRCLE);
    a->player_mask = f_int(s, CRC("playerMask"), -1);
    a->dash_mask = f_int(s, CRC("dashMask"), -1);
    a->spawn_clip = f_asset(s, CRC("spawnClip"));
    a->move_speed = f_float(s, CRC("moveSpeed"), 40);
    a->dash_power = f_float(s, CRC("dashPower"), 1500);
    a->next_wp_dist = f_float(s, CRC("nextWaypointDistance"), 0.5f);
    a->attack_range = f_float(s, CRC("attackRange"), 2.8f);
    a->attack_width = f_float(s, CRC("attackWidth"), 0.3f);
    a->spawn_time = f_float(s, CRC("spawnTime"), 0);
    a->rot = script_ref(s, CRC("rotationScript"), ST_RotationScript);
    a->general_cooldown = f_float(s, CRC("generalCooldown"), 2);
    a->cooldown_tic = f_float(s, CRC("cooldownTic"), 0.2f);
}

static void ai_start(Script *s)
{
    /* AiScript.Start: InvokeRepeating("UpdatePath", 0, 0.2) and SetActive(false)
     * (not when Start only runs because Spawn activated the object for the first time) */
    if (!((Ai *)s->self)->busy)
        obj_set_active(s->go, 0);
}

static void update_path(Script *s)
{
    Ai *a = s->self;
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    a->npath = path_find(x, y, px, py, a->path, MAXWP);
    a->cur_wp = 0;
}

/* forward decls of per-type hooks */
static void in_range_action(Script *s);
static void out_of_range_action(Script *s);

static void pre_action(Script *s)
{
    Ai *a = s->self;
    a->busy = 1;
    phys_set_velocity(s->go, 0, 0);
    obj_pos(a->player_t, &a->aim_x, &a->aim_y);
    float x, y;
    pos(s, &x, &y);
    if (x < a->aim_x) flip_to(a, -1);
    else if (x > a->aim_x) flip_to(a, 1);
}

static int player_in_range(Script *s)
{
    Ai *a = s->self;
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    float dx = px - x, dy = py - y;
    if (sqrtf(dx * dx + dy * dy) > 0.5f) {
        int hit = phys_circlecast(x, y, a->attack_width, dx, dy, a->attack_range, a->dash_mask, 0);
        if (hit < 0 || W.obj[hit].tag != TAG_Player)
            return 0;
    }
    pre_action(s);
    in_range_action(s);
    return 1;
}

static void post_action(Script *s, float len)
{
    Ai *a = s->self;
    a->post_len = len;
    co_start(&a->post);
    co_wait(&a->post, len, 1);
}

static void chase_start(Script *s)
{
    Ai *a = s->self;
    co_start(&a->chase);
}

/* PostAction body after its wait; returns 1 if the chase continues */
static void post_action_body(Script *s)
{
    Ai *a = s->self;
    a->busy = 0;
    a->hitable = 1;
    if (!player_in_range(s)) {
        if (a->npath > 0 && a->cur_wp < a->npath) {
            float x, y;
            pos(s, &x, &y);
            float wx = a->path[a->cur_wp * 2], wy = a->path[a->cur_wp * 2 + 1];
            if (hypotf(x - wx, y - wy) < a->next_wp_dist && a->cur_wp + 1 < a->npath)
                a->cur_wp++;
            wx = a->path[a->cur_wp * 2]; wy = a->path[a->cur_wp * 2 + 1];
            float dx = wx - x, dy = wy - y, l = hypotf(dx, dy);
            if (l > 1e-5f) { dx /= l; dy /= l; }
            if (dx >= 0.1f) flip_to(a, -1);
            else if (dx <= -0.1f) flip_to(a, 1);
        }
        anim_trigger(s->go, pid("isMoving"));
        chase_start(s);
    }
}

static void chase_tick(Script *s, float dt)
{
    Ai *a = s->self;
    if (!co_tick(&a->chase, dt, T.udt))
        return;
    int ghost = is_ghost_type(s->type);
    if (a->chase.step == 0) {
        if (a->busy) { co_stop(&a->chase); return; }
        player_in_range(s);
        co_wait(&a->chase, 0.2f, 1);
        return;
    }
    if (a->busy) { co_stop(&a->chase); return; }
    if (ghost) {
        a->general_cooldown -= a->cooldown_tic;
        if (a->general_cooldown <= 0) {
            a->general_cooldown = 2;
            out_of_range_action(s);
        }
    }
    a->chase.step = 0;
    if (a->busy) co_stop(&a->chase);
}

static void ai_spawn_base(Script *s)
{
    Ai *a = s->self;
    obj_set_active(s->go, 1);
    a->busy = 1;
    post_action(s, a->spawn_time);
    anim_trigger(s->go, pid("isAlive"));
}

static void ai_base_fixed(Script *s, float dt)
{
    Ai *a = s->self;
    a->path_t -= dt;
    if (a->path_t <= 0) {
        a->path_t += 0.2f;
        update_path(s);
    }
    if (!a->busy && a->npath > 0 && a->cur_wp < a->npath) {
        float x, y;
        pos(s, &x, &y);
        float wx = a->path[a->cur_wp * 2], wy = a->path[a->cur_wp * 2 + 1];
        float dx = wx - x, dy = wy - y, l = hypotf(dx, dy);
        if (l > 1e-5f) { dx /= l; dy /= l; }
        phys_add_force(s->go, dx * a->move_speed, dy * a->move_speed);
        if (hypotf(wx - x, wy - y) < a->next_wp_dist)
            a->cur_wp++;
        float vx, vy;
        phys_velocity(s->go, &vx, &vy);
        if (vx >= 0.4f) flip_to(a, -1);
        else if (vx <= -0.4f) flip_to(a, 1);
    }
}

static void set_death_col(Ai *a, int on) { col_set_enabled(a->death_col, on); }

static void kill_check(Script *s, float range)
{
    Ai *a = s->self;
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    if (hypotf(px - x, py - y) < range && a->rot)
        rotation_gameover(a->rot, a->player_t, 1, 1);
}

/* GhostAiScript.TripleThrow */
static void single_throw(Script *s, float dx, float dy);
static void triple_throw(Script *s, float dx, float dy, float mult)
{
    Ai *a = s->self;
    single_throw(s, dx, dy);
    float ang = atan2f(dy, dx) * 57.29578f;
    if (ang < 0) ang += 360;
    a->tt_angle = ang + 10 * mult;
    a->tt_mult = mult;
    co_start(&a->tthrow);
    co_wait(&a->tthrow, 0.05f, 1);
}
static void tthrow_tick(Script *s, float dt)
{
    Ai *a = s->self;
    if (!co_tick(&a->tthrow, dt, T.udt))
        return;
    float f = a->tt_angle * PI_F / 180;
    single_throw(s, cosf(f), sinf(f));
    a->tt_angle -= 20 * a->tt_mult;
    f = a->tt_angle * PI_F / 180;
    single_throw(s, cosf(f), sinf(f));
    co_stop(&a->tthrow);
}

static int ref_i(Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n) ? f_ref_at(s, &e[i]) : -1;
}
static int ref_n(Script *s, uint32_t field) { int n; f_elems(s, field, &n); return n; }

/* ================================================================== Zmora */
static void zmora_shocks(Script *s, uint32_t field, float x, float y, int adv, float tx, float ty)
{
    int n = ref_n(s, field);
    for (int i = 0; i < n; i++) {
        Script *sh = script_on(ref_i(s, field, i), ST_ShockwaveScript);
        if (sh) shock_emit(sh, x, y, adv, tx, ty);
    }
}
static void zmora_thunder(Script *s)
{
    int n = ref_n(s, CRC("thunder"));
    for (int i = 0; i < n; i++) {
        Script *t = script_on(ref_i(s, CRC("thunder"), i), ST_ThunderScript);
        if (t) thunder_bolt(t);
    }
}
static void zmora_slam(Script *s, int slams)
{
    Ai *a = s->self;
    int lvl = f_int(s, CRC("lvl"), 1);
    a->slam_count += slams;
    if (lvl == 2) a->special_ready = 0;
    anim_trigger(s->go, pid("isJumping"));
    mgr_short(snd_pick(s, CRC("slamClip")), 0);
    float px, py;
    obj_pos(a->player_t, &px, &py);
    obj_set_pos(s->go, px, py);
    a->ival = 1;   /* act = slam */
    co_start(&a->act);
    co_wait(&a->act, f_float(s, CRC("slamSpeed"), 0.6f), 1);
}
static void zmora_act(Script *s, float dt)
{
    Ai *a = s->self;
    if (!co_tick(&a->act, dt, T.udt))
        return;
    int lvl = f_int(s, CRC("lvl"), 1);
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    if (a->ival == 0) {   /* Melee */
        switch (a->act.step) {
        case 0:
            co_wait(&a->act, 0.3f, 1);
            return;
        case 1: {
            if (lvl != 1) a->special_ready = 1;
            float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
            if (l > 1e-5f) { dx /= l; dy /= l; }
            phys_add_force(s->go, dx * a->dash_power, dy * a->dash_power);
            set_death_col(a, 1);
            if (lvl < 3) post_action(s, 1.3f);
            else {
                Script *sh = script_on(ref_i(s, CRC("shock"), 0), ST_ShockwaveScript);
                if (sh) shock_emit(sh, x, y, 1, dx, dy);
            }
            co_wait(&a->act, 0.2f, 2);
            return;
        }
        case 2:
            set_death_col(a, 0);
            co_wait(&a->act, 0.2f, 3);
            return;
        case 3:
            co_stop(&a->act);
            if (lvl == 3) zmora_slam(s, 3);
            return;
        }
    } else if (a->ival == 1) {   /* Slam */
        switch (a->act.step) {
        case 1:
            if (hypotf(px - x, py - y) < 0.5f && a->rot) rotation_gameover(a->rot, a->player_t, 1, 1);
            zmora_shocks(s, CRC("shock"), x, y, 0, px, py);
            a->slam_count--;
            if (a->slam_count > 0) { co_wait(&a->act, 0.2f, 2); return; }
            co_stop(&a->act);
            post_action(s, 1.3f);
            return;
        case 2:
            co_stop(&a->act);
            zmora_slam(s, 0);
            return;
        }
    } else if (a->ival == 2) {   /* SuperSlam */
        switch (a->act.step) {
        case 0: co_wait(&a->act, 0.6f, 1); return;
        case 1:
            if (hypotf(px - x, py - y) < 0.5f && a->rot) rotation_gameover(a->rot, a->player_t, 1, 1);
            zmora_shocks(s, CRC("shock"), x, y, 0, px, py);
            co_wait(&a->act, 0.4f, 2);
            return;
        case 2:
            zmora_shocks(s, CRC("shockAlt"), x, y, 0, px, py);
            post_action(s, 1.3f);
            a->ival2 = 0;
            a->act.step = 3;
            /* fallthrough */
        case 3:
            if (a->ival2 >= 2) { co_stop(&a->act); return; }
            mgr_short(snd_pick(s, CRC("thunderClip")), 0);
            zmora_thunder(s);
            a->ival2++;
            co_wait(&a->act, 1.6f, 3);
            return;
        }
    }
}
static void zmora_in_range(Script *s)
{
    Ai *a = s->self;
    int lvl = f_int(s, CRC("lvl"), 1);
    if (a->special_ready) {
        if (lvl == 2) {
            zmora_slam(s, 1);
        } else {
            a->special_ready = 0;
            anim_trigger(s->go, pid("isRaging"));
            mgr_short(f_asset(s, CRC("rageClip")), 0);
            a->ival = 2;
            co_start(&a->act);
        }
    } else {
        anim_trigger(s->go, pid("isAttacking"));
        mgr_short(snd_pick(s, CRC("dashClip")), 0);
        a->ival = 0;
        co_start(&a->act);
    }
}
static void zmora_wipe(Script *s)
{
    int n = ref_n(s, CRC("shock"));
    for (int i = 0; i < n; i++) { Script *sh = script_on(ref_i(s, CRC("shock"), i), ST_ShockwaveScript); if (sh) shock_destruction(sh); }
    n = ref_n(s, CRC("shockAlt"));
    for (int i = 0; i < n; i++) { Script *sh = script_on(ref_i(s, CRC("shockAlt"), i), ST_ShockwaveScript); if (sh) shock_destruction(sh); }
    n = ref_n(s, CRC("thunder"));
    for (int i = 0; i < n; i++) w_destroy(ref_i(s, CRC("thunder"), i), 0);
}

/* ================================================================== Striga */
static void striga_teleport(Script *s);
static void striga_act(Script *s, float dt)
{
    Ai *a = s->self;
    if (!co_tick(&a->act, dt, T.udt))
        return;
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    if (a->ival == 0) {   /* Melee */
        switch (a->act.step) {
        case 0: co_wait(&a->act, 0.2f, 1); return;
        case 1: {
            float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
            if (l > 1e-5f) { dx /= l; dy /= l; }
            triple_throw(s, dx, dy, 0.5f);
            phys_add_force(s->go, dx * a->dash_power, dy * a->dash_power);
            set_death_col(a, 1);
            post_action(s, 0.7f);
            co_wait(&a->act, 0.15f, 2);
            return;
        }
        case 2: set_death_col(a, 0); co_stop(&a->act); return;
        }
    } else if (a->ival == 1) {   /* Shatter */
        switch (a->act.step) {
        case 0: co_wait(&a->act, 0.4f, 1); return;
        case 1: { Script *b = script_on(ref_i(s, CRC("scrapBarrage"), 0), ST_ScrapBarrageScript); if (b) barrage_aim(b, px, py); co_wait(&a->act, 0.2f, 2); return; }
        case 2: {
            Script *b = script_on(ref_i(s, CRC("scrapBarrage"), 1), ST_ScrapBarrageScript);
            if (b) barrage_aim(b, px, py);
            post_action(s, 0.7f);
            co_wait(&a->act, 0.1f, 3);
            return;
        }
        case 3: mgr_short(f_asset(s, CRC("markClip")), 0); co_wait(&a->act, 0.3f, 4); return;
        case 4: mgr_short(f_asset(s, CRC("markClip")), 0); co_stop(&a->act); return;
        }
    } else if (a->ival == 2) {   /* Teleport */
        switch (a->act.step) {
        case 1: {
            int n = ref_n(s, CRC("rotoScrap"));
            for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("rotoScrap"), i), 0);
            /* DistanceCheck */
            if (hypotf(px - x, py - y) < 0.5f && a->rot) rotation_gameover(a->rot, a->player_t, 1, 1);
            Script *nova = script_ref(s, CRC("frostNova"), ST_BulletSpreadScript);
            if (nova) spread_spread(nova, x, y);
            post_action(s, 1.3f);
            int nm = ref_n(s, CRC("minions")), sum = 0;
            for (int i = 0; i < nm; i++) { Script *m = script_on(ref_i(s, CRC("minions"), i), ST_MinionAiScript); if (m) sum += minion_available(m); }
            if (nm && sum == nm) {
                mgr_short(f_asset(s, CRC("dogSummon")), 0);
                for (int i = 0; i < nm; i++) { Script *m = script_on(ref_i(s, CRC("minions"), i), ST_MinionAiScript); if (m) ai_spawn(m); }
            }
            co_stop(&a->act);
            return;
        }
        }
    }
}
static void striga_teleport(Script *s)
{
    Ai *a = s->self;
    anim_trigger(s->go, pid("isWarping"));
    mgr_short(f_asset(s, CRC("teleportClip")), 0);
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    Script *ph = script_ref(s, CRC("phaser"), ST_PhaseScript);
    if (ph) phase_phase(ph, px, py, x, y, W.obj[a->sprite_t].lsx, W.obj[a->sprite_t].lsy);
    obj_set_pos(s->go, px, py);
    int n = ref_n(s, CRC("rotoScrap"));
    for (int i = 0; i < n; i++) { Script *r = script_on(ref_i(s, CRC("rotoScrap"), i), ST_RotoScrapScript); if (r) roto_recast(r, px, py); }
    a->ival = 2;
    co_start(&a->act);
    co_wait(&a->act, 0.6f, 1);
}
static void striga_in_range(Script *s)
{
    Ai *a = s->self;
    if (a->special_ready) {
        a->special_ready = 0;
        anim_trigger(s->go, pid("isCasting"));
        mgr_short(snd_pick(s, CRC("cannonClip")), 0);
        a->ival = 1;
    } else {
        a->special_ready = 1;
        anim_trigger(s->go, pid("isAttacking"));
        mgr_short(snd_pick(s, CRC("throwClip")), 0);
        a->ival = 0;
    }
    co_start(&a->act);
}
void striga_spawn_ice(Script *s, float x, float y)
{
    Ai *a = s->self;
    int n = ref_n(s, CRC("iceSpikeArray"));
    if (!n) return;
    Script *sp = script_on(ref_i(s, CRC("iceSpikeArray"), a->ival2 % n), ST_IceSpikeScript);
    int ns_;
    const WField *e = f_elems(s, CRC("spriteArray"), &ns_);
    int spr = (e && ns_) ? e[irange(0, ns_ < 16 ? ns_ : 16)].v.i : SPR_NONE;
    if (sp) icespike_spawn(sp, x, y, spr);
    a->ival2 = (a->ival2 + 1) % n;
}
static void striga_wipe(Script *s)
{
    int n = ref_n(s, CRC("projectileArray"));
    for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("projectileArray"), i), 0);
    n = ref_n(s, CRC("scrapBarrage"));
    for (int i = 0; i < n; i++) { Script *b = script_on(ref_i(s, CRC("scrapBarrage"), i), ST_ScrapBarrageScript); if (b) barrage_destruction(b, 1); }
    n = ref_n(s, CRC("rotoScrap"));
    for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("rotoScrap"), i), 0);
    n = ref_n(s, CRC("minions"));
    for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("minions"), i), 0);
    obj_set_active(f_ref(s, CRC("frostNova")), 0);
}

/* ================================================================== Minion */
static void minion_act(Script *s, float dt)
{
    Ai *a = s->self;
    if (!co_tick(&a->act, dt, T.udt))
        return;
    float x, y;
    pos(s, &x, &y);
    switch (a->act.step) {
    case 0: co_wait(&a->act, 0.3f, 1); return;
    case 1: {
        float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
        if (l > 1e-5f) { dx /= l; dy /= l; }
        phys_add_force(s->go, dx * a->dash_power, dy * a->dash_power);
        set_death_col(a, 1);
        post_action(s, 0.9f);
        co_wait(&a->act, 0.3f, 2);
        return;
    }
    case 2: set_death_col(a, 0); co_stop(&a->act); return;
    }
}
int minion_available(Script *m) { return ((Ai *)m->self)->ival; }   /* ival = available */
void minion_death(Script *s)
{
    Ai *a = s->self;
    if (!obj_active(s->go)) return;
    a->hitable = 0;
    mgr_short_common(snd_pick(s, CRC("deathSoundArray")));
    float x, y;
    pos(s, &x, &y);
    int pf = f_asset(s, CRC("deathVfx"));
    if (pf >= 0) w_instantiate(pf, x, y);
    a->ival = 1;
    obj_set_active(s->go, 0);
}
static void minion_spawn(Script *s)
{
    Ai *a = s->self;
    a->ival = 0;   /* available = 0 */
    extern void hitbox_set_target(Script *h, Script *target);
    hitbox_set_target(script_ref(s, CRC("hitbox"), ST_HitboxScript), s);
    Script *mover = script_ref(s, CRC("movingSummoner"), ST_StrigaAiScript);
    if (!mover) {
        a->hitable = 1;
        float ox, oy = 0;
        ox = f_vec(s, CRC("origin"), &oy);
        obj_set_pos(s->go, ox, oy);
        flip_to(a, ox < 0 ? -1 : 1);
        set_death_col(a, 0);
        ai_spawn_base(s);
        return;
    }
    a->ival2 = 1;   /* immortal */
    float mx, my;
    obj_pos(mover->go, &mx, &my);
    obj_set_pos(s->go, mx, my);
    float ang = frandf() * 2 * PI_F, dx = cosf(ang), dy = sinf(ang);
    if (phys_raycast(mx, my, dx, dy, 0.5f, 256, 0, 0, 0) >= 0) { dx = -dx; dy = -dy; }
    set_death_col(a, 0);
    ai_spawn_base(s);
    float f = frange(600, 800);
    phys_add_force(s->go, dx * f, dy * f);
}

/* ================================================================== Dogo */
static void dogo_slide(Script *s, float target);
static void dogo_post_body(Script *s)
{
    Ai *a = s->self;
    a->busy = 0;
    a->hitable = 1;
    a->special_ready = 0;   /* sliding */
    float x, y;
    pos(s, &x, &y);
    if (a->ival2) {   /* mandatorySlide */
        a->ival2 = 0;
        dogo_slide(s, x);
    } else if (!player_in_range(s)) {
        if (x < -6.5f || x > 6.5f || y > -0.4f) {
            dogo_slide(s, x);
            return;
        }
        anim_trigger(s->go, pid("isMoving"));
        chase_start(s);
    }
}
static void dogo_dash(Script *s)
{
    Ai *a = s->self;
    anim_trigger(s->go, pid("isAttacking"));
    mgr_short(snd_pick(s, CRC("soundArrayStart")), 0);
    a->ival = 10;
    co_start(&a->act2);
    co_wait(&a->act2, 0.3f, 1);
}
static void dogo_slide(Script *s, float target)
{
    Ai *a = s->self;
    float x, y;
    pos(s, &x, &y);
    float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
    if (l > 1e-5f) { dx /= l; dy /= l; }
    if (target != 0) pre_action(s);
    else if (dy > 0 && (x < -6.5f || x > 6.5f)) target = x;
    a->special_ready = 1;   /* sliding */
    anim_trigger(s->go, pid("isSliding"));
    mgr_short(snd_pick(s, CRC("soundArraySlide")), 0);
    float tx, ty;
    if (target < 0) { tx = f_vec(s, CRC("slideRight"), &ty); }
    else if (target > 0) { tx = f_vec(s, CRC("slideLeft"), &ty); }
    else { tx = x + dx; ty = y + dy; }
    if (target != 0) {
        dx = tx - x; dy = ty - y; l = hypotf(dx, dy);
        if (l > 1e-5f) { dx /= l; dy /= l; }
    }
    a->fval = 0;
    phys_add_force(s->go, dx * (a->dash_power / 3), dy * (a->dash_power / 3));
    a->dog_dx = dx; a->dog_dy = dy;
    a->ival = 20;
    co_start(&a->act);
    co_wait(&a->act, 0.3f, 1);
}
static void dogo_act(Script *s, float dt)
{
    Ai *a = s->self;
    if (co_tick(&a->act, dt, T.udt)) {
        /* Slide continuation */
        if (player_in_range(s)) {
            /* InRangeAction may have started a slide; Dash replaces it */
            dogo_dash(s);
        } else {
            post_action(s, 0.5f);
        }
        a->special_ready = 0;
        co_stop(&a->act);
    }
    if (co_tick(&a->act2, dt, T.udt)) {
        float x, y, px, py;
        pos(s, &x, &y);
        obj_pos(a->player_t, &px, &py);
        switch (a->act2.step) {
        case 1: {
            if (hypotf(px - x, py - y) < 1.2f && a->rot) rotation_gameover(a->rot, a->player_t, 1, 1);
            a->org_x = x; a->org_y = y;
            float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
            if (l > 1e-5f) { dx /= l; dy /= l; }
            a->dog_dx = dx; a->dog_dy = dy;
            phys_add_force(s->go, dx * a->dash_power, dy * a->dash_power);
            set_death_col(a, 1);
            post_action(s, 1.4f);
            co_wait(&a->act2, 0.4f, 2);
            return;
        }
        case 2:
            set_death_col(a, 0);
            if (hypotf(x - a->org_x, y - a->org_y) < 1e-5f) a->ival2 = 1;   /* mandatorySlide */
            co_stop(&a->act2);
            return;
        }
    }
    if (co_tick(&a->act3, dt, T.udt)) {   /* Death */
        float x, y;
        pos(s, &x, &y);
        if (a->act3.step == 9) {   /* HitableSet */
            a->hitable = 1;
            co_stop(&a->act3);
        } else if (a->act3.step == 1) {
            int pf = f_asset(s, CRC("deathVfx"));
            if (pf >= 0) w_instantiate(pf, x, y);
            Script *nova = script_ref(s, CRC("nova"), ST_BulletSpreadScript);
            if (nova) spread_spread(nova, x, y);
            co_wait(&a->act3, 0.4f, 2);
        } else {
            obj_set_active(s->go, 0);
            co_stop(&a->act3);
        }
    }
}
static void dogo_in_range(Script *s)
{
    Ai *a = s->self;
    if (!a->special_ready)
        dogo_slide(s, 0);
}
static void dogo_out_of_range(Script *s)
{
    Ai *a = s->self;
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    if ((px > 4 && x > 4) || (px < -4 && x < -4))
        dogo_slide(s, x);
}
static void dogo_attacked(Script *s, float dx, float dy)
{
    Ai *a = s->self;
    if (!a->hitable) return;
    a->busy = 1;
    a->hitable = 0;
    co_stop(&a->post); co_stop(&a->chase); co_stop(&a->act); co_stop(&a->act2); co_stop(&a->tthrow);
    set_death_col(a, 0);
    anim_trigger(s->go, pid("isPushed"));
    mgr_short(snd_pick(s, CRC("soundArrayDeath")), 0);
    float x, y;
    pos(s, &x, &y);
    int pf = f_asset(s, CRC("dmgVfx"));
    if (pf >= 0) w_instantiate(pf, x, y);
    float push = a->gen_state ? 100 : 1000;
    phys_add_force(s->go, dx * push, dy * push);
    co_start(&a->act3);
    co_wait(&a->act3, 0.9f, 1);
}
void dogo_waved(Script *s, float px, float py)
{
    Ai *a = s->self;
    Script *nova = script_ref(s, CRC("nova"), ST_BulletSpreadScript);
    if (nova) spread_wipe(nova);
    if (a->hitable) {
        a->gen_state = 1;   /* pushforce = 100 */
        dogo_attacked(s, -px, -py);
    }
}
static void dogo_collision(Script *s, int other, float rel)
{
    Ai *a = s->self;
    if (rel > 10 && !a->special_ready && a->hitable) {
        camera_shake(0.1f, 0.1f);
        a->special_ready = 1;
        anim_trigger(s->go, pid("isHurt"));
        mgr_short(snd_pick(s, CRC("soundArraySlam")), 0);
        set_death_col(a, 0);
        phys_add_force(s->go, -a->dog_dx * (a->dash_power / 15), -a->dog_dy * (a->dash_power / 15));
    }
}

/* ================================================================== Cutwire */
static void cutwire_act(Script *s, float dt)
{
    Ai *a = s->self;
    if (!co_tick(&a->act, dt, T.udt))
        return;
    float x, y;
    pos(s, &x, &y);
    switch (a->act.step) {
    case 0: co_wait(&a->act, 0.6f, 1); return;
    case 1:
        if (player_in_range(s) && a->ival2 < 2) {
            /* note: playerInRange re-enters InRangeAction, which does nothing while attackRange >= 3 */
            anim_trigger(s->go, pid("isAttacking"));
            mgr_short(snd_pick(s, CRC("soundArray")), 0);
            float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
            if (l > 1e-5f) { dx /= l; dy /= l; }
            a->tt_angle = dx; a->tt_mult = dy;
            int holo = f_ref(s, CRC("holoTransform"));
            float ang = atan2f(dy, dx) * 57.29578f;
            if (ang < 0) ang += 360;
            obj_set_rot(holo, ang);
            anim_trigger(f_ref(s, CRC("holoAnimator")), pid("cast"));
            co_wait(&a->act, 0.3f, 2);
            return;
        }
        /* combo over */
        anim_trigger(s->go, pid("isAlive"));
        mgr_short(snd_at(s, CRC("otherSounds"), 0), 0);
        {
            int root = w_instantiate(PREFAB_cutterTrap, x, y);
            extern void trap_setup(int root, Script *rot, int sprite);
            int n;
            const WField *e = f_elems(s, CRC("trapSprite"), &n);
            trap_setup(root, a->rot, (e && n) ? e[a->gen_state % (n ? n : 1)].v.i : SPR_NONE);
            a->gen_state = (a->gen_state + 1) % 3;
        }
        a->attack_range /= 3;
        post_action(s, 1.3f);
        {
            float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
            if (l > 1e-5f) { dx /= l; dy /= l; }
            phys_add_force(s->go, dx * a->dash_power, dy * a->dash_power);
        }
        co_stop(&a->act);
        return;
    case 2: {
        float dx = a->tt_angle, dy = a->tt_mult;
        phys_add_force(s->go, dx * 400, dy * 400);
        triple_throw(s, dx, dy, 1);
        co_wait(&a->act, 1.2f, 3);
        return;
    }
    case 3:
        a->ival2++;
        a->act.step = 1;
        co_wait(&a->act, 0, 1);
        return;
    }
}
static void cutwire_in_range(Script *s)
{
    Ai *a = s->self;
    if (a->attack_range < 3) {
        anim_trigger(s->go, pid("isOut"));
        if (!a->ival) { mgr_short(snd_at(s, CRC("otherSounds"), 2), 0); a->ival = 1; }   /* spawning */
        else mgr_short(snd_at(s, CRC("otherSounds"), 1), 0);
        a->attack_range *= 3;
        a->ival2 = 0;
        co_start(&a->act);
    }
}

/* ================================================================== Nikita */
static void nikita_act(Script *s, float dt)
{
    Ai *a = s->self;
    if (!co_tick(&a->act, dt, T.udt))
        return;
    float x, y;
    pos(s, &x, &y);
    if (a->ival == 0) {   /* FlameWave */
        if (a->act.step == 0) { co_wait(&a->act, 0.8f, 1); return; }
        kill_check(s, 0.5f);
        Script *pool = script_ref(s, CRC("spreadPoolScript"), ST_BulletSpreadPoolScript);
        if (pool) spreadpool_ignite(pool, x, y);
        post_action(s, 1.3f);
        co_stop(&a->act);
    } else {   /* Fireball */
        switch (a->act.step) {
        case 0: co_wait(&a->act, 0.4f, 1); return;
        case 1: {
            float dx = a->tt_angle, dy = a->tt_mult;
            phys_add_force(s->go, dx * a->dash_power, dy * a->dash_power);
            set_death_col(a, 1);
            Script *fb = script_ref(s, CRC("fireball"), ST_FireballScript);
            if (fb) throwable_throw(fb, x, y, dx, dy);
            post_action(s, 1.3f);
            co_wait(&a->act, 0.15f, 2);
            return;
        }
        case 2: set_death_col(a, 0); co_stop(&a->act); return;
        }
    }
}
static void nikita_in_range(Script *s)
{
    Ai *a = s->self;
    int wave = a->special_ready;
    if (wave) {
        a->special_ready = 0;
        anim_trigger(s->go, pid("isBlasting"));
        mgr_short(snd_pick(s, CRC("waveClip")), 0);
        anim_trigger(f_ref(s, CRC("holoAnimator")), pid("fire"));
        a->ival = 0;
    } else {
        a->special_ready = 1;
        anim_trigger(s->go, pid("isAttacking"));
        mgr_short(snd_pick(s, CRC("birdClip")), 0);
        float x, y;
        pos(s, &x, &y);
        float dx = a->aim_x - x, dy = a->aim_y - y, l = hypotf(dx, dy);
        if (l > 1e-5f) { dx /= l; dy /= l; }
        a->tt_angle = dx; a->tt_mult = dy;
        float ang = atan2f(dy, dx) * 57.29578f;
        if (ang < 0) ang += 360;
        obj_set_rot(f_ref(s, CRC("holoTransform")), ang);
        anim_trigger(f_ref(s, CRC("holoAnimator")), pid("bird"));
        a->ival = 1;
    }
    co_start(&a->act);
}

/* ================================================================== Doppel */
static void doppel_grind_end(Script *s)
{
    Ai *a = s->self;
    if (a->gen_state) {
        a->gen_state = 0;
        snd_stop(a->ival2);
    }
}
static void doppel_update_extra(Script *s)
{
    Ai *a = s->self;
    if (a->busy) return;
    float x, y, px, py;
    pos(s, &x, &y);
    obj_pos(a->player_t, &px, &py);
    float d = hypotf(px - x, py - y);
    if (d < 2 && !a->special_ready) {
        anim_trigger(s->go, pid("isReady"));
        mgr_short(snd_pick(s, CRC("openClip")), 0);
        a->special_ready = 1;
        doppel_grind_end(s);
    } else if (d > 4 && a->special_ready) {
        anim_trigger(s->go, pid("isUnready"));
        a->special_ready = 0;
    }
}
static void doppel_in_range(Script *s)
{
    Ai *a = s->self;
    anim_trigger(s->go, pid("isAttacking"));
    doppel_grind_end(s);
    mgr_short(snd_pick(s, CRC("closeClip")), 0);
    float px, py;
    obj_pos(a->player_t, &px, &py);
    obj_set_pos(s->go, px, py);
    if (a->rot) rotation_gameover(a->rot, a->player_t, 1, 1);
    post_action(s, 1.3f);
}

/* ================================================================== dispatch */
static void in_range_action(Script *s)
{
    switch (s->type) {
    case ST_ZmoraAiScript: zmora_in_range(s); break;
    case ST_StrigaAiScript: striga_in_range(s); break;
    case ST_MinionAiScript: {
        Ai *a = s->self;
        a->ival2 = 0;   /* immortal = false */
        anim_trigger(s->go, pid("isAttacking"));
        mgr_short(snd_pick(s, CRC("soundArray")), 0);
        co_start(&a->act);
        break;
    }
    case ST_DogoAiScript: dogo_in_range(s); break;
    case ST_CutwireAiScript: cutwire_in_range(s); break;
    case ST_NikitaAiScript: nikita_in_range(s); break;
    case ST_DoppelAiScript: doppel_in_range(s); break;
    }
}

static void out_of_range_action(Script *s)
{
    switch (s->type) {
    case ST_StrigaAiScript: pre_action(s); striga_teleport(s); break;
    case ST_DogoAiScript: dogo_out_of_range(s); break;
    }
}

static void single_throw(Script *s, float dx, float dy)
{
    Ai *a = s->self;
    uint32_t field = s->type == ST_StrigaAiScript ? CRC("projectileArrayB") : CRC("projectileArray");
    int n = ref_n(s, field);
    if (!n) return;
    Script *t = script_on(ref_i(s, field, a->throw_index), ST_ThrowableScript);
    float x, y;
    pos(s, &x, &y);
    if (t) throwable_throw(t, x, y, dx, dy);
    a->throw_index = (a->throw_index + 1) % n;
}

void ai_attacked(Script *s, float dx, float dy)
{
    if (s->type == ST_DogoAiScript) dogo_attacked(s, dx, dy);
    else if (s->type == ST_MinionAiScript) { Ai *a = s->self; if (a->hitable) minion_death(s); }
}

void ghost_wipe_event(Script *s)
{
    Ai *a = s->self;
    switch (s->type) {
    case ST_ZmoraAiScript: zmora_wipe(s); break;
    case ST_StrigaAiScript: striga_wipe(s); break;
    case ST_CutwireAiScript: {
        int n = ref_n(s, CRC("projectileArray"));
        for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("projectileArray"), i), 0);
        extern void trap_destroy_all(void);
        trap_destroy_all();
        break;
    }
    case ST_NikitaAiScript: {
        anim_trigger(f_ref(s, CRC("holoAnimator")), pid("wipe"));
        Script *fb = script_ref(s, CRC("fireball"), ST_FireballScript);
        if (fb) projectile_destruction(fb);
        break;
    }
    case ST_DogoAiScript: {
        obj_set_active(f_ref(s, CRC("nova")), 0);
        obj_set_active(s->go, 0);
        return;
    }
    }
    a->busy = 1;
    anim_trigger(s->go, pid("isExorcised"));
    co_stop(&a->post); co_stop(&a->chase); co_stop(&a->act); co_stop(&a->act2); co_stop(&a->act3); co_stop(&a->tthrow);
}

static void ai_update(Script *s, float dt)
{
    Ai *a = s->self;
    if (co_tick(&a->post, dt, T.udt)) {
        co_stop(&a->post);
        if (s->type == ST_DogoAiScript) dogo_post_body(s);
        else post_action_body(s);
    }
    chase_tick(s, dt);
    tthrow_tick(s, dt);
    switch (s->type) {
    case ST_ZmoraAiScript: zmora_act(s, dt); break;
    case ST_StrigaAiScript: striga_act(s, dt); break;
    case ST_MinionAiScript: minion_act(s, dt); break;
    case ST_DogoAiScript: dogo_act(s, dt); break;
    case ST_CutwireAiScript: cutwire_act(s, dt); break;
    case ST_NikitaAiScript: nikita_act(s, dt); break;
    case ST_DoppelAiScript: doppel_update_extra(s); break;
    }
}

static void minion_fixed(Script *s, float dt)
{
    ai_base_fixed(s, dt);
    Ai *a = s->self;
    float x, y;
    pos(s, &x, &y);
    int other = phys_overlap_circle(x, y, 0.1f, -1, 0);
    if (other >= 0) {
        float ox, oy;
        obj_pos(other, &ox, &oy);
        phys_add_force(s->go, (x - ox) * a->move_speed * 0.8f, (y - oy) * a->move_speed * 0.8f);
    }
}

static void minion_collision(Script *s, int other, float rel)
{
    Ai *a = s->self;
    if (rel > 2 && !a->ival2) {
        a->ival2 = 1;
        minion_death(s);
    }
}

static void ai_event(Script *s, int fn, int iarg, float farg)
{
    Ai *a = s->self;
    if (fn == EV_SpawnAudio) {
        if (!a->already_spawned) {
            mgr_short(a->spawn_clip, 0);
            a->already_spawned = 1;
        }
    } else if (fn == EV_Landing) {
        mgr_short(snd_pick(s, CRC("soundArrayLand")), 0);
    } else if (fn == EV_Shake) {
        mgr_short(f_asset(s, CRC("shakeClip")), 0);
    } else if (fn == EV_GrindAudio) {
        a->gen_state = 1;
        a->ival2 = mgr_short(snd_at(s, CRC("grindClip"), a->ival), 0);
        int n = snd_count(s, CRC("grindClip"));
        a->ival = n ? (a->ival + 1) % n : 0;
    }
}

static void ai_init(Script *s)
{
    ai_base_init(s);
    Ai *a = s->self;
    if (s->type == ST_MinionAiScript) {
        a->ival = f_int(s, CRC("available"), 1);
        a->ival2 = 1;   /* immortal */
    }
    if (s->type == ST_NikitaAiScript)
        a->special_ready = f_int(s, CRC("waveReady"), 0);
}

static void dogo_spawn_wrap(Script *s)
{
    Ai *a = s->self;
    extern void hitbox_set_target(Script *h, Script *target);
    hitbox_set_target(script_ref(s, CRC("hitbox"), ST_HitboxScript), s);
    ai_spawn_base(s);
    co_start(&a->act3);
    co_wait(&a->act3, 0.2f, 9);
}

/* Unity stops coroutines when the object is deactivated */
static void ai_disable(Script *s)
{
    Ai *a = s->self;
    co_stop(&a->post); co_stop(&a->chase); co_stop(&a->act); co_stop(&a->act2); co_stop(&a->act3); co_stop(&a->tthrow);
    if (s->type == ST_DoppelAiScript) doppel_grind_end(s);
}

const ScriptVT vt_ZmoraAiScript = {sizeof(Ai), ai_init, ai_start, ai_update, ai_base_fixed, ai_event, 0, 0, 0, ai_disable};
const ScriptVT vt_StrigaAiScript = {sizeof(Ai), ai_init, ai_start, ai_update, ai_base_fixed, ai_event, 0, 0, 0, ai_disable};
const ScriptVT vt_MinionAiScript = {sizeof(Ai), ai_init, ai_start, ai_update, minion_fixed, ai_event, 0, minion_collision, 0, ai_disable};
const ScriptVT vt_DogoAiScript = {sizeof(Ai), ai_init, ai_start, ai_update, ai_base_fixed, ai_event, 0, dogo_collision, 0, ai_disable};
const ScriptVT vt_CutwireAiScript = {sizeof(Ai), ai_init, ai_start, ai_update, ai_base_fixed, ai_event, 0, 0, 0, ai_disable};
const ScriptVT vt_NikitaAiScript = {sizeof(Ai), ai_init, ai_start, ai_update, ai_base_fixed, ai_event, 0, 0, 0, ai_disable};
const ScriptVT vt_DoppelAiScript = {sizeof(Ai), ai_init, ai_start, ai_update, ai_base_fixed, ai_event, 0, 0, 0, ai_disable};

/* AiScript.Spawn (virtual: Minion and Dogo override it) */
void ai_spawn(Script *s)
{
    if (!s) return;
    if (s->type == ST_MinionAiScript) minion_spawn(s);
    else if (s->type == ST_DogoAiScript) dogo_spawn_wrap(s);
    else ai_spawn_base(s);
}
