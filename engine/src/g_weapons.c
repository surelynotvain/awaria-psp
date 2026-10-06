/* Ghost weapons and hazards: projectiles (Throwable, Fireball, Bullet), bullet spreads, shockwaves, thunder,
 * ice waves / spikes, scrap barrage, roto scrap, phase, skull bombs, traps, arms, boilers, coilboxes. */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "g_game.h"
#include "w_render.h"
#include "aw_gfx.h"

static int ref_i(const Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n) ? f_ref_at(s, &e[i]) : -1;
}
static int ref_n(const Script *s, uint32_t field) { int n; f_elems(s, field, &n); return n; }
static int spr_pick(const Script *s, uint32_t field)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && n) ? e[irange(0, n)].v.i : -1;
}
static int is_player(int go) { return go >= 0 && W.obj[go].tag == TAG_Player; }
static Script *player_of(int go) { return script_on(go, ST_PlayerScript); }
static float dist_to(int a, int b)
{
    float ax, ay, bx, by;
    obj_pos(a, &ax, &ay);
    obj_pos(b, &bx, &by);
    return hypotf(ax - bx, ay - by);
}
static void norm(float *x, float *y)
{
    float l = hypotf(*x, *y);
    if (l > 1e-5f) { *x /= l; *y /= l; }
}

/* ================================================================== ProjectileScript (+ Throwable, Fireball, Bullet) */
typedef struct {
    int coll, trail;
    float invoke_t;          /* Invoke("PostAnim", destructTime) */
    float base_x, base_y;    /* Bullet: baseOrigin (local) */
    Co spawn;                /* Bullet.Spawn */
} ProjS;

static void proj_post_anim(Script *s)
{
    ProjS *p = s->self;
    obj_set_active(s->go, 0);
    col_set_enabled(p->coll, 1);
    if (s->type == ST_BulletScript) {
        W.obj[s->go].lx = p->base_x;
        W.obj[s->go].ly = p->base_y;
    }
}

static void proj_init(Script *s)
{
    ProjS *p = s->self;
    p->coll = col_of(s->go, -1);
    p->trail = f_ref(s, CRC("trail"));
    if (s->type == ST_BulletScript) {
        p->base_x = W.obj[s->go].lx;
        p->base_y = W.obj[s->go].ly;
    } else {
        proj_post_anim(s);
    }
}

void projectile_destruction(Script *s)
{
    if (!s) return;
    ProjS *p = s->self;
    if (s->type == ST_BulletScript) {
        int vfx = f_asset(s, CRC("deathVfx"));
        if (vfx >= 0 && W.obj[s->go].active) {
            float x, y;
            obj_pos(s->go, &x, &y);
            w_instantiate(vfx, x, y);
            if (f_int(s, CRC("audible"), 0))
                mgr_short_common(snd_pick(s, CRC("deathrattle")));
        }
    }
    if (p->trail >= 0) render_trail_clear(p->trail);
    if (W.obj[s->go].active) {
        anim_trigger(s->go, P("hit"));
        col_set_enabled(p->coll, 0);
        phys_set_velocity(s->go, 0, 0);
        p->invoke_t = f_float(s, CRC("destructTime"), 0.3f);
    }
}
void fireball_destruction(Script *f) { projectile_destruction(f); }
void bullet_destruction(Script *b) { projectile_destruction(b); }

static void proj_update(Script *s, float dt)
{
    ProjS *p = s->self;
    if (p->invoke_t > 0) {
        p->invoke_t -= dt;
        if (p->invoke_t <= 0) {
            p->invoke_t = 0;
            proj_post_anim(s);
            return;
        }
    }
    if (s->type == ST_BulletScript && co_tick(&p->spawn, dt, T.udt)) {
        if (p->spawn.step == 0) {
            W.obj[s->go].tag = TAG_Untagged;
            col_set_enabled(p->coll, 1);
            co_wait(&p->spawn, 1.0f, 1);
        } else {
            col_set_enabled(p->coll, 0);
            W.obj[s->go].tag = TAG_Deadly;
            col_set_enabled(p->coll, 1);
            anim_trigger(s->go, P("idle"));
            float dx, dy = 0;
            dx = f_vec(s, CRC("bulletDir"), &dy);
            norm(&dx, &dy);
            float sp = f_float(s, CRC("speed"), 8);
            phys_add_impulse(s->go, dx * sp, dy * sp);
            co_stop(&p->spawn);
        }
    }
}

static void proj_disable(Script *s)
{
    ProjS *p = s->self;
    co_stop(&p->spawn);
    /* Invoke keeps running on inactive objects; PostAnim only deactivates, nothing left to do */
    if (p->invoke_t > 0) {
        p->invoke_t = 0;
        col_set_enabled(p->coll, 1);
        if (s->type == ST_BulletScript) {
            W.obj[s->go].lx = p->base_x;
            W.obj[s->go].ly = p->base_y;
        }
    }
}

void throwable_throw(Script *s, float ox, float oy, float dx, float dy)
{
    if (!s) return;
    ProjS *p = s->self;
    obj_set_pos(s->go, ox, oy);
    if (p->trail >= 0) render_trail_clear(p->trail);
    float a = atan2f(dy, dx) * 57.29578f;
    if (a < 0) a += 360;
    obj_set_rot(f_ref(s, CRC("spriteRotator")), a);
    obj_set_active(s->go, 1);
    anim_trigger(s->go, P("fire"));
    float sp = f_float(s, CRC("speed"), 8);
    phys_add_impulse(s->go, dx * sp, dy * sp);
}

static void proj_collision(Script *s, int other, float rel)
{
    if (s->type == ST_FireballScript) {
        float x, y;
        obj_pos(s->go, &x, &y);
        Script *pool = script_ref(s, CRC("spreadPoolScript"), ST_BulletSpreadPoolScript);
        if (pool) spreadpool_ignite(pool, x, y);
        mgr_short(snd_pick(s, CRC("soundClip")), 0);
    }
    projectile_destruction(s);
}

void bullet_cast(Script *s)
{
    ProjS *p = s->self;
    int bs = f_ref(s, CRC("bulletSprite"));
    if (bs >= 0) {
        int spr = spr_pick(s, CRC("spritePool"));
        if (spr >= 0) sr_set_sprite(sr_of(bs), spr);
    }
    p->invoke_t = 0;
    if (f_asset(s, CRC("deathVfx")) >= 0) {
        col_set_enabled(p->coll, 0);
        obj_set_active(s->go, 1);
        anim_trigger(s->go, P("fire"));
        co_start(&p->spawn);
        return;
    }
    obj_set_active(s->go, 1);
    anim_trigger(s->go, P("fire"));
    if (p->trail >= 0) render_trail_clear(p->trail);
    float dx, dy = 0;
    dx = f_vec(s, CRC("bulletDir"), &dy);
    norm(&dx, &dy);
    float sp = f_float(s, CRC("speed"), 8);
    phys_add_impulse(s->go, dx * sp, dy * sp);
}

static void bullet_trigger(Script *s, int other, int col)
{
    ProjS *p = s->self;
    if (f_asset(s, CRC("deathVfx")) >= 0 && is_player(other)) {
        Script *pl = player_of(other);
        if (pl && player_superdash(pl) == 2 && !player_input_blocked(pl)) {
            co_stop(&p->spawn);
            projectile_destruction(s);
        }
    }
}

const ScriptVT vt_ThrowableScript = {sizeof(ProjS), proj_init, 0, proj_update, 0, 0, 0, proj_collision, 0, proj_disable};
const ScriptVT vt_FireballScript = {sizeof(ProjS), proj_init, 0, proj_update, 0, 0, 0, proj_collision, 0, proj_disable};
const ScriptVT vt_BulletScript = {sizeof(ProjS), proj_init, 0, proj_update, 0, 0, bullet_trigger, proj_collision, 0, proj_disable};

/* ================================================================== BulletSpreadScript / BulletSpreadPoolScript */
#define MAXB 64
typedef struct { Script *b[MAXB]; int n; Co timed; int ti; } SpreadS;

static void spread_collect(Script *s)
{
    SpreadS *sp = s->self;
    sp->n = 0;
    for (int i = 0; i < W.nscript && sp->n < MAXB; i++) {
        Script *b = &W.script[i];
        if (b->alive && b->type == ST_BulletScript && obj_active(b->go) && obj_is_descendant(b->go, s->go))
            sp->b[sp->n++] = b;
    }
}
static void spread_start(Script *s)
{
    SpreadS *sp = s->self;
    spread_collect(s);
    for (int i = 0; i < sp->n; i++) {
        if (!sp->b[i]->self) continue;
        if (!sp->b[i]->awake) continue;
        proj_post_anim(sp->b[i]);
    }
}
void spread_spread(Script *s, float x, float y)
{
    if (!s) return;
    SpreadS *sp = s->self;
    obj_set_pos(s->go, x, y);
    w_transforms();
    for (int i = 0; i < sp->n; i++)
        bullet_cast(sp->b[i]);
}
void spread_wall(Script *s)
{
    SpreadS *sp = s->self;
    sp->ti = 0;
    co_start(&sp->timed);
}
void spread_wipe(Script *s)
{
    if (!s) return;
    SpreadS *sp = s->self;
    for (int i = 0; i < sp->n; i++)
        projectile_destruction(sp->b[i]);
    co_stop(&sp->timed);
}
static void spread_update(Script *s, float dt)
{
    SpreadS *sp = s->self;
    if (co_tick(&sp->timed, dt, T.udt)) {
        if (sp->ti >= sp->n) { co_stop(&sp->timed); return; }
        bullet_cast(sp->b[sp->ti++]);
        co_wait(&sp->timed, 0.05f, 0);
    }
}
static void spread_disable(Script *s) { co_stop(&((SpreadS *)s->self)->timed); }
const ScriptVT vt_BulletSpreadScript = {sizeof(SpreadS), 0, spread_start, spread_update, 0, 0, 0, 0, 0, spread_disable};

#define MAXSP 16
typedef struct { Script *sp[MAXSP]; int n, idx; } PoolS;
static void pool_start(Script *s)
{
    PoolS *p = s->self;
    p->n = 0;
    for (int i = 0; i < W.nscript && p->n < MAXSP; i++) {
        Script *b = &W.script[i];
        if (b->alive && b->type == ST_BulletSpreadScript && obj_active(b->go) && obj_is_descendant(b->go, s->go))
            p->sp[p->n++] = b;
    }
}
void spreadpool_ignite(Script *s, float x, float y)
{
    PoolS *p = s->self;
    if (!p->n) return;
    if (p->idx >= p->n) p->idx = 0;
    spread_spread(p->sp[p->idx], x, y);
    p->idx++;
}
void spreadpool_wipe(Script *s)
{
    PoolS *p = s->self;
    for (int i = 0; i < p->n; i++) spread_wipe(p->sp[i]);
}
const ScriptVT vt_BulletSpreadPoolScript = {sizeof(PoolS), 0, pool_start};

/* ================================================================== ShockwaveScript */
typedef struct { Co dis; int i, number; } ShockS;
void shock_emit(Script *s, float ox, float oy, int advanced, float tx, float ty)
{
    ShockS *st = s->self;
    obj_set_pos(s->go, ox, oy);
    obj_set_active(s->go, 1);
    st->i = 0; st->number = 0;
    co_start(&st->dis);
    co_wait(&st->dis, 0.05f, 0);
    if (advanced) {
        phys_add_force(s->go, tx * 700, ty * 700);
    } else {
        float dx, dy = 0;
        dx = f_vec(s, CRC("dir"), &dy);
        norm(&dx, &dy);
        float sp = f_float(s, CRC("speed"), 8);
        phys_add_impulse(s->go, dx * sp, dy * sp);
    }
}
void shock_destruction(Script *s)
{
    if (!s) return;
    ShockS *st = s->self;
    if (W.obj[s->go].active) {
        co_stop(&st->dis);
        phys_set_velocity(s->go, 0, 0);
        obj_set_active(s->go, 0);
    }
}
static void shock_update(Script *s, float dt)
{
    ShockS *st = s->self;
    if (!co_tick(&st->dis, dt, T.udt)) return;
    int n = ref_n(s, CRC("electroVfx"));
    if (st->i < n) {
        int num = irange(1, 8);
        st->number = num != st->number ? num : irange(1, 8);
        char buf[4];
        snprintf(buf, sizeof buf, "%d", st->number);
        float x, y;
        obj_pos(s->go, &x, &y);
        Script *e = script_on(ref_i(s, CRC("electroVfx"), st->i), ST_ElectroVfxScript);
        if (e) electro_discharge(e, x, y, buf);
        st->i++;
        if (st->i < n) { co_wait(&st->dis, 0.05f, 0); return; }
    }
    shock_destruction(s);
}
static void shock_collision(Script *s, int other, float rel) { shock_destruction(s); }
static void shock_disable(Script *s) { co_stop(&((ShockS *)s->self)->dis); }
const ScriptVT vt_ShockwaveScript = {sizeof(ShockS), 0, 0, shock_update, 0, 0, 0, shock_collision, 0, shock_disable};

/* ================================================================== ThunderScript */
void thunder_bolt(Script *s)
{
    int n;
    const WField *e = f_elems(s, CRC("location"), &n);
    if (e && n) {
        int i = irange(0, 3 < n ? 3 : n);
        float x = 0, y = 0;
        const WField *c = &e[i];
        if (c->kind == F_VEC) { x = c->v.f; y = c->v2; }
        obj_set_pos(s->go, x, y);
    }
    anim_trigger(f_ref(s, CRC("animator")), P("activate"));
}
const ScriptVT vt_ThunderScript = {0};

/* ================================================================== IcewaveScript */
typedef struct { Co dis; int i; } WaveS;
void icewave_throw(Script *s, float ox, float oy, float dx, float dy)
{
    WaveS *w = s->self;
    obj_set_pos(s->go, ox, oy);
    obj_set_active(s->go, 1);
    w->i = 0;
    co_start(&w->dis);
    co_wait(&w->dis, f_float(s, CRC("wait"), 0), 1);
    float sp = f_float(s, CRC("speed"), 8);
    phys_add_impulse(s->go, dx * sp, dy * sp);
}
void icewave_destruction(Script *s)
{
    WaveS *w = s->self;
    if (W.obj[s->go].active) {
        co_stop(&w->dis);
        phys_set_velocity(s->go, 0, 0);
        obj_set_active(s->go, 0);
    }
}
extern void striga_spawn_ice(Script *s, float x, float y);
static void icewave_update(Script *s, float dt)
{
    WaveS *w = s->self;
    if (!co_tick(&w->dis, dt, T.udt)) return;
    if (w->dis.step == 1) {
        co_wait(&w->dis, irange(5, 12) * 0.01f, 2);
        return;
    }
    float x, y;
    obj_pos(s->go, &x, &y);
    Script *striga = script_ref(s, CRC("spikePool"), ST_StrigaAiScript);
    if (striga) striga_spawn_ice(striga, x, y);
    if (++w->i < 5) { co_wait(&w->dis, irange(5, 12) * 0.01f, 2); return; }
    icewave_destruction(s);
}
static void icewave_collision(Script *s, int other, float rel) { icewave_destruction(s); }
static void icewave_disable(Script *s) { co_stop(&((WaveS *)s->self)->dis); }
const ScriptVT vt_IcewaveScript = {sizeof(WaveS), 0, 0, icewave_update, 0, 0, 0, icewave_collision, 0, icewave_disable};

/* ================================================================== IceSpikeScript */
typedef struct { Co life, crack; } SpikeS;
void icespike_spawn(Script *s, float x, float y, int spr)
{
    SpikeS *k = s->self;
    obj_set_pos(s->go, x + frange(-0.1f, 0.1f), y + frange(-0.1f, 0.1f));
    int ss = f_ref(s, CRC("spikeSprite"));
    if (ss >= 0 && spr >= 0) sr_set_sprite(sr_of(ss), spr);
    obj_set_active(s->go, 1);
    anim_trigger(f_ref(s, CRC("animator")), P("spawn"));
    co_start(&k->life);
    co_wait(&k->life, 0.2f, 1);
}
static int spike_hitbox(Script *s) { int h = f_ref(s, CRC("hitbox")); return col_of(h, COL_CIRCLE); }
static void spike_crack(Script *s)
{
    SpikeS *k = s->self;
    int hb = spike_hitbox(s);
    if (col_enabled(hb)) {
        col_set_enabled(hb, 0);
        anim_trigger(f_ref(s, CRC("animator")), P("crack"));
        co_start(&k->crack);
        co_wait(&k->crack, 0.4f, 1);
    }
}
static void spike_update(Script *s, float dt)
{
    SpikeS *k = s->self;
    if (co_tick(&k->life, dt, T.udt)) {
        if (k->life.step == 1) {
            col_set_enabled(spike_hitbox(s), 1);
            co_wait(&k->life, 4.0f, 2);
        } else {
            co_stop(&k->life);
            spike_crack(s);
        }
    }
    if (co_tick(&k->crack, dt, T.udt)) {
        co_stop(&k->crack);
        co_stop(&k->life);
        obj_set_active(s->go, 0);
    }
}
static void spike_trigger(Script *s, int other, int col)
{
    if (is_player(other)) {
        Script *pl = player_of(other);
        if (pl) player_gameover(pl);
        spike_crack(s);
    }
}
static void spike_disable(Script *s) { SpikeS *k = s->self; co_stop(&k->life); co_stop(&k->crack); }
const ScriptVT vt_IceSpikeScript = {sizeof(SpikeS), 0, 0, spike_update, 0, 0, spike_trigger, 0, 0, spike_disable};

/* ================================================================== ScrapBarrageScript */
typedef struct { Co drop; } BarS;
void barrage_aim(Script *s, float x, float y)
{
    BarS *b = s->self;
    obj_set_pos(s->go, x, y);
    obj_set_active(s->go, 1);
    anim_trigger(f_ref(s, CRC("animator")), P("spawn"));
    co_start(&b->drop);
    co_wait(&b->drop, f_float(s, CRC("dropTime"), 0.5f), 1);
}
static void barrage_update(Script *s, float dt)
{
    BarS *b = s->self;
    if (!co_tick(&b->drop, dt, T.udt)) return;
    co_stop(&b->drop);
    int pgo = f_ref(s, CRC("player"));
    Script *pl = player_of(pgo);
    if (pl && dist_to(pgo, s->go) < 0.5f) player_gameover(pl);
    float x, y;
    obj_pos(s->go, &x, &y);
    Script *nova = script_ref(s, CRC("nova"), ST_BulletSpreadScript);
    if (nova) spread_spread(nova, x, y);
    mgr_short(snd_pick(s, CRC("impactClip")), 0);
}
void barrage_destruction(Script *s, int perma)
{
    BarS *b = s->self;
    if (!W.obj[s->go].active) return;
    if (perma) obj_set_active(f_ref(s, CRC("nova")), 0);
    else spread_wipe(script_ref(s, CRC("nova"), ST_BulletSpreadScript));
    co_stop(&b->drop);
    obj_set_active(s->go, 0);
}
void barrage_wipe(Script *s)
{
    BarS *b = s->self;
    if (!W.obj[s->go].active) return;
    w_destroy(f_ref(s, CRC("nova")), 0);
    co_stop(&b->drop);
    obj_set_active(s->go, 0);
}
static void barrage_disable(Script *s) { co_stop(&((BarS *)s->self)->drop); }
const ScriptVT vt_ScrapBarrageScript = {sizeof(BarS), 0, 0, barrage_update, 0, 0, 0, 0, 0, barrage_disable};

/* ================================================================== RotoScrapScript */
typedef struct { float cx, cy, ang, spd, rad; } RotoS;
void roto_recast(Script *s, float x, float y)
{
    RotoS *r = s->self;
    int ss = f_ref(s, CRC("scrapSprite"));
    int spr = spr_pick(s, CRC("spritePool"));
    if (ss >= 0 && spr >= 0) sr_set_sprite(sr_of(ss), spr);
    obj_set_pos(s->go, x, y);
    r->cx = x; r->cy = y;
    r->ang = f_float(s, CRC("angle"), 0);
    r->spd = f_float(s, CRC("speed"), 2);
    r->rad = f_float(s, CRC("radius"), 1);
    obj_set_active(s->go, 1);
    anim_trigger(f_ref(s, CRC("anim")), P("recast"));
}
static void roto_update(Script *s, float dt)
{
    RotoS *r = s->self;
    obj_set_pos(s->go, r->cx + cosf(r->ang) * r->rad, r->cy + sinf(r->ang) * r->rad);
    r->spd += 0.5f;
    r->ang += dt * r->spd;
    r->rad -= dt * 0.5f;
}
const ScriptVT vt_RotoScrapScript = {sizeof(RotoS), 0, 0, roto_update};

/* ================================================================== PhaseScript */
typedef struct { float ox, oy, tx, ty, t; int on; } PhaseS;
void phase_phase(Script *s, float tx, float ty, float ox, float oy, float scx, float scy)
{
    PhaseS *p = s->self;
    obj_set_pos(s->go, ox, oy);
    obj_scale(f_ref(s, CRC("spriteTransform")), scx, scy);
    obj_set_active(s->go, 1);
    p->ox = ox; p->oy = oy; p->tx = tx; p->ty = ty; p->t = 0; p->on = 1;
}
static void phase_update(Script *s, float dt)
{
    PhaseS *p = s->self;
    if (!p->on) return;
    if (p->t >= 1) {
        p->on = 0;
        obj_set_active(s->go, 0);
        return;
    }
    p->t += dt / 0.2f;
    float t = p->t > 1 ? 1 : p->t;
    int ps = f_ref(s, CRC("phaseSprite"));
    if (ps >= 0) {
        float a = 1 - p->t;
        if (a < 0) a = 0;
        sr_set_color(sr_of(ps), 0x00FFFFFFu | ((uint32_t)(a * 255 + 0.5f) << 24));
    }
    obj_set_pos(s->go, p->ox + (p->tx - p->ox) * t, p->oy + (p->ty - p->oy) * t);
}
static void phase_disable(Script *s) { ((PhaseS *)s->self)->on = 0; }
const ScriptVT vt_PhaseScript = {sizeof(PhaseS), 0, 0, phase_update, 0, 0, 0, 0, 0, phase_disable};

/* ================================================================== SkullbombScript */
typedef struct {
    int armed, hittable;
    Co arm, timer, explo;
    int time_left;
    int attack_src;
    float hit_x, hit_y;
} SkullS;
static void skull_counter(Script *s, const char *t) { set_text(f_ref(s, CRC("counter")), t); }
void skull_spawn(Script *s)
{
    SkullS *k = s->self;
    float ox, oy = 0;
    ox = f_vec(s, CRC("origin"), &oy);
    obj_set_pos(s->go, ox, oy);
    obj_set_active(s->go, 1);
    anim_trigger(f_ref(s, CRC("animator")), P("spawn"));
    k->hittable = 1;
    co_start(&k->arm);
    co_wait(&k->arm, 0.7f, 1);
}
static void skull_explosion(Script *s)
{
    SkullS *k = s->self;
    k->armed = 0;
    co_stop(&k->timer);
    skull_counter(s, "");
    anim_trigger(f_ref(s, CRC("animator")), P("explode"));
    k->attack_src = mgr_short(snd_pick(s, CRC("attackClip")), 0);
    co_start(&k->explo);
    co_wait(&k->explo, 0.4f, 1);
}
static void skull_fixed(Script *s, float dt)
{
    SkullS *k = s->self;
    if (!k->armed) return;
    int pgo = f_ref(s, CRC("player"));
    float px, py, x, y;
    obj_pos(pgo, &px, &py);
    obj_pos(s->go, &x, &y);
    if (hypotf(px - x, py - y) < 1.5f) {
        skull_explosion(s);
        return;
    }
    float dx = px - x, dy = py - y;
    norm(&dx, &dy);
    phys_add_force(s->go, dx * 4, dy * 4);
    float vx, vy;
    phys_velocity(s->go, &vx, &vy);
    int st = f_ref(s, CRC("spriteTransform"));
    if (vx > 0.1f) obj_scale(st, -1, 1);
    else if (vx < 0.1f) obj_scale(st, 1, 1);
}
static void skull_update(Script *s, float dt)
{
    SkullS *k = s->self;
    if (co_tick(&k->arm, dt, T.udt)) {
        co_stop(&k->arm);
        k->time_left = f_int(s, CRC("secondsToDie"), 9);
        co_start(&k->timer);
        k->armed = 1;
        k->hittable = 1;
    }
    if (co_tick(&k->timer, dt, T.udt)) {
        if (k->time_left >= 0) {
            char buf[8];
            snprintf(buf, sizeof buf, "%d", k->time_left);
            skull_counter(s, buf);
            k->time_left--;
            co_wait(&k->timer, 1.0f, 0);
        } else {
            co_stop(&k->timer);
            if (k->armed) skull_explosion(s);
        }
    }
    if (co_tick(&k->explo, dt, T.udt)) {
        if (k->explo.step == 1) {
            k->hittable = 0;
            int pgo = f_ref(s, CRC("player"));
            Script *pl = player_of(pgo);
            if (dist_to(pgo, s->go) < 1.0f) { if (pl) player_gameover(pl); }
            else camera_shake(0.15f, 0.15f);
            float x, y;
            obj_pos(s->go, &x, &y);
            Script *nova = script_ref(s, CRC("nova"), ST_BulletSpreadScript);
            if (nova) spread_spread(nova, x, y);
            co_wait(&k->explo, 1.0f, 2);
        } else {
            co_stop(&k->explo);
            obj_set_active(s->go, 0);
        }
    }
}
void skull_dmg(Script *s, int contact)
{
    SkullS *k = s->self;
    if (!contact) spread_wipe(script_ref(s, CRC("nova"), ST_BulletSpreadScript));
    if (!k->hittable) return;
    k->hittable = 0;
    k->armed = 0;
    co_stop(&k->arm); co_stop(&k->timer); co_stop(&k->explo);
    anim_trigger(f_ref(s, CRC("animator")), P("pushed"));
    mgr_short(f_asset(s, CRC("deathClip")), 0);
    if (k->attack_src >= 0) { snd_stop(k->attack_src); k->attack_src = -1; }
    skull_counter(s, "");
    float x, y;
    obj_pos(s->go, &x, &y);
    int vfx = f_asset(s, CRC("dmgVfx"));
    if (vfx >= 0) w_instantiate(vfx, x, y);
    int pgo = f_ref(s, CRC("player"));
    Script *pl = player_of(pgo);
    if (contact) {
        float dx = 0, dy = 0;
        if (pl) player_dash_dir(pl, &dx, &dy);
        phys_add_force(s->go, dx * 500, dy * 500);
    } else if (k->hit_x == 100) {
        k->hit_x = k->hit_y = 0;   /* AddForce(zero) */
    } else {
        obj_pos(pgo, &k->hit_x, &k->hit_y);
        float ox, oy = 0;
        ox = f_vec(s, CRC("origin"), &oy);
        float dx = ox - x, dy = oy - y;
        norm(&dx, &dy);
        phys_add_force(s->go, dx * 500, dy * 500);
    }
}
static void skull_trigger(Script *s, int other, int col)
{
    SkullS *k = s->self;
    Script *pl = player_of(f_ref(s, CRC("player")));
    if (is_player(other) && pl && player_superdash(pl) == 2 && !player_input_blocked(pl)) {
        skull_dmg(s, 1);
    } else if (other >= 0 && W.obj[other].tag == TAG_veryDeadly) {
        k->hit_x = 100; k->hit_y = 0;
        skull_dmg(s, 0);
    }
}
static void skull_event(Script *s, int fn, int iarg, float farg)
{
    if (fn == EV_Death) {
        float x, y;
        obj_pos(s->go, &x, &y);
        int vfx = f_asset(s, CRC("deathVfx"));
        if (vfx >= 0) w_instantiate(vfx, x, y);
        obj_set_active(s->go, 0);
    }
}
void skull_wipe(Script *s)
{
    SkullS *k = s->self;
    co_stop(&k->arm); co_stop(&k->timer); co_stop(&k->explo);
    w_destroy(f_ref(s, CRC("nova")), 0);
    obj_set_active(s->go, 0);
}
static void skull_init(Script *s) { ((SkullS *)s->self)->attack_src = -1; }
static void skull_disable(Script *s) { SkullS *k = s->self; co_stop(&k->arm); co_stop(&k->timer); co_stop(&k->explo); }
const ScriptVT vt_SkullbombScript = {sizeof(SkullS), skull_init, 0, skull_update, skull_fixed, skull_event, skull_trigger, 0, 0, skull_disable};

/* ================================================================== TrapScript (Cutwire's cutterTrap prefab) */
typedef struct { Co setup, act; int set, sprite, player; Script *rot; } TrapS;
#define MAXTRAPS 64
static int traps[MAXTRAPS], ntraps;
void trap_setup(int root, Script *rot, int sprite)
{
    Script *t = 0;
    for (int i = 0; i < W.nscript; i++)
        if (W.script[i].alive && W.script[i].type == ST_TrapScript && obj_is_descendant(W.script[i].go, root)) { t = &W.script[i]; break; }
    if (ntraps < MAXTRAPS) traps[ntraps++] = root;
    if (!t || !t->self) return;
    TrapS *tr = t->self;
    tr->rot = rot;
    tr->sprite = sprite;
}
void trap_destroy_all(void)
{
    for (int i = 0; i < ntraps; i++)
        if (W.obj[traps[i]].alive) w_destroy(traps[i], 0);
    ntraps = 0;
}
static void trap_start(Script *s)
{
    TrapS *t = s->self;
    co_start(&t->setup);
    co_wait(&t->setup, 0.1f, 1);
}
static void trap_update(Script *s, float dt)
{
    TrapS *t = s->self;
    if (co_tick(&t->setup, dt, T.udt)) {
        if (t->setup.step == 1) {
            int ts = f_ref(s, CRC("trapSprite"));
            if (ts >= 0 && t->sprite >= 0) sr_set_sprite(sr_of(ts), t->sprite);
            co_wait(&t->setup, 0.3f, 2);
        } else {
            t->set = 1;
            col_set_enabled(col_of(f_ref(s, CRC("trapCollider")), COL_CIRCLE), 1);
            co_stop(&t->setup);
        }
    }
    if (co_tick(&t->act, dt, T.udt)) {
        if (t->act.step == 1) {
            if (dist_to(t->player, s->go) < 1.0f && t->rot) rotation_gameover(t->rot, t->player, 1, 1);
            co_wait(&t->act, 0.8f, 2);
        } else {
            co_stop(&t->act);
            int root = s->go;
            while (W.obj[root].parent >= 0) root = W.obj[root].parent;
            w_destroy(root, 0);
        }
    }
}
static void trap_trigger(Script *s, int other, int col)
{
    TrapS *t = s->self;
    if (is_player(other) && t->set) {
        t->set = 0;
        t->player = other;
        anim_trigger(f_ref(s, CRC("animator")), P("activate"));
        mgr_short(snd_pick(s, CRC("soundArray")), 0);
        co_start(&t->act);
        co_wait(&t->act, 0.3f, 1);
    }
}
static void trap_init(Script *s) { ((TrapS *)s->self)->sprite = -1; }
const ScriptVT vt_TrapScript = {sizeof(TrapS), trap_init, trap_start, trap_update, 0, 0, trap_trigger};

/* ================================================================== ArmScript (lilArm prefab) */
static void arm_trigger(Script *s, int other, int col)
{
    if (!is_player(other)) return;
    Script *pl = player_of(other);
    if (pl && player_superdash(pl) == 2 && !player_input_blocked(pl)) {
        float x, y;
        obj_pos(s->go, &x, &y);
        int vfx = f_asset(s, CRC("deathVfx"));
        if (vfx >= 0) w_instantiate(vfx, x, y);
        w_destroy(s->go, 0);
    }
}
const ScriptVT vt_ArmScript = {0, 0, 0, 0, 0, 0, arm_trigger};

/* ================================================================== BoilerScript / CoilboxScript */
typedef struct { int index; float ox, oy; } BoilS;
static void boiler_event(Script *s, int fn, int iarg, float farg)
{
    BoilS *b = s->self;
    if (fn == EV_BoilWave) {
        float x, y;
        obj_pos(s->go, &x, &y);
        Script *sp = script_ref(s, CRC("spreadScript"), ST_BulletSpreadScript);
        if (sp) spread_spread(sp, x, y);
    } else if (fn == EV_BoilerStartup) {
        int n = snd_count(s, CRC("soundClip"));
        mgr_short(snd_at(s, CRC("soundClip"), b->index), 0);
        if (++b->index >= n) b->index = 0;
    }
}
void boiler_wipe(Script *s)
{
    anim_trigger(f_ref(s, CRC("animator")), P("off"));
    spread_wipe(script_ref(s, CRC("spreadScript"), ST_BulletSpreadScript));
}
const ScriptVT vt_BoilerScript = {sizeof(BoilS), 0, 0, 0, 0, boiler_event};

static void coil_start(Script *s)
{
    BoilS *b = s->self;
    obj_pos(s->go, &b->ox, &b->oy);
    b->oy -= 0.27f;
    b->index = f_int(s, CRC("index"), 0);
}
static void coil_event(Script *s, int fn, int iarg, float farg)
{
    BoilS *b = s->self;
    if (fn == EV_BoilWave) {
        Script *sh = script_ref(s, CRC("shock"), ST_ShockwaveScript);
        if (sh) shock_emit(sh, b->ox, b->oy, 1, 0, -1);
    } else if (fn == EV_SoundPlay) {
        if (f_int(s, CRC("audible"), 0)) {
            mgr_short(snd_at(s, CRC("soundClip"), b->index), 0);
            if (++b->index == 2) b->index = 0;
        }
    }
}
void coilbox_wipe(Script *s)
{
    anim_trigger(f_ref(s, CRC("animator")), P("off"));
    shock_destruction(script_ref(s, CRC("shock"), ST_ShockwaveScript));
}
const ScriptVT vt_CoilboxScript = {sizeof(BoilS), 0, coil_start, 0, 0, coil_event};

/* ================================================================== ElectroWipeScript */
void electrowipe_activate(Script *s, float x, float y)
{
    obj_set_pos(s->go, x, y);
    obj_set_active(s->go, 1);
    anim_trigger(f_ref(s, CRC("animator")), P("wipe"));
    Script *boss = script_ref(s, CRC("boss"), ST_BigStrigaScript);
    if (boss) boss_electro_wipe(boss);
}
const ScriptVT vt_ElectroWipeScript = {0};
