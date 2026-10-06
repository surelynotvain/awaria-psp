#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "w_anim.h"
#include "w_world.h"

/* UnityEditor.Animations.AnimatorConditionMode */
enum { COND_IF = 1, COND_IFNOT = 2, COND_GREATER = 3, COND_LESS = 4, COND_EQUALS = 6, COND_NOTEQUAL = 7 };
enum { PK_FLOAT = 1, PK_INT = 3, PK_BOOL = 4, PK_TRIGGER = 9 };

Anim *g_anim;
int g_nanim, g_capanim;

static float clip_len(int clip) { return clip >= 0 ? g_aclips[clip].length : 0; }

static int state_clip(const Anim *a) { return g_astates[g_actrls[a->ctrl].first_state + a->state].clip; }

/* ------------------------------------------------------------------ property access */
static float prop_get(int go, int prop)
{
    if (go < 0 || go >= W.nobj)
        return 0;
    Obj *o = &W.obj[go];
    switch (prop) {
    case P_ACTIVE: return o->active;
    case P_POSX: return o->lx;
    case P_POSY: return o->ly;
    case P_SCX: return o->lsx;
    case P_SCY: return o->lsy;
    case P_ROTX: return o->rx;
    case P_ROTY: return o->ry;
    case P_ROTZ: return o->lrot;
    case P_SR_SPRITE: return o->sr >= 0 ? W.sr[o->sr].d.spr : SPR_NONE;
    case P_SR_R: return o->sr >= 0 ? W.sr[o->sr].d.r / 255.0f : 1;
    case P_SR_G: return o->sr >= 0 ? W.sr[o->sr].d.g / 255.0f : 1;
    case P_SR_B: return o->sr >= 0 ? W.sr[o->sr].d.b / 255.0f : 1;
    case P_SR_A: return o->sr >= 0 ? W.sr[o->sr].d.a / 255.0f : 1;
    case P_SR_EN: return o->sr >= 0 ? ((W.sr[o->sr].d.flags & WSR_ENABLED) ? 1 : 0) : 1;
    case P_SR_ORDER: return o->sr >= 0 ? W.sr[o->sr].d.order : 0;
    case P_G_R: case P_G_G: case P_G_B: case P_G_A: {
        const uint8_t *c = 0;
        if (o->img >= 0) c = &W.img[o->img].r;
        else if (o->text >= 0) c = &W.text[o->text].d.r;
        return c ? c[prop - P_G_R] / 255.0f : 1;
    }
    case P_G_EN:
        if (o->img >= 0) return (W.img[o->img].flags & WIMG_ENABLED) ? 1 : 0;
        if (o->text >= 0) return (W.text[o->text].d.flags & WTXT_ENABLED) ? 1 : 0;
        return 1;
    case P_IMG_SPRITE: return o->img >= 0 ? W.img[o->img].spr : SPR_NONE;
    case P_AP_X: return o->rect >= 0 ? W.rect[o->rect].ap_x : 0;
    case P_AP_Y: return o->rect >= 0 ? W.rect[o->rect].ap_y : 0;
    case P_SD_X: return o->rect >= 0 ? W.rect[o->rect].sd_x : 0;
    case P_SD_Y: return o->rect >= 0 ? W.rect[o->rect].sd_y : 0;
    case P_COL_EN: return o->ncol ? ((W.col[o->col_first].d.flags & WCOL_ENABLED) ? 1 : 0) : 1;
    case P_CAPS_OFFX:
        for (int k = 0; k < o->ncol; k++)
            if (W.col[o->col_first + k].d.kind == COL_CAPSULE)
                return W.col[o->col_first + k].d.ox;
        return 0;
    case P_LINE_W: return o->line >= 0 ? 1.0f : 1.0f;
    case P_TXT_A: return o->text >= 0 ? W.text[o->text].alpha_mul : 1;
    }
    return 0;
}

static int hierarchy_dirty;
static int16_t act_dirty[64];
static int n_act_dirty;

static inline uint8_t u8c(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }

static void prop_set(int go, int prop, float v)
{
    if (go < 0 || go >= W.nobj || !W.obj[go].alive)
        return;
    Obj *o = &W.obj[go];
    switch (prop) {
    case P_ACTIVE: {
        uint8_t a = v > 0.5f;
        if (o->active != a) {
            o->active = a;
            if (n_act_dirty < (int)(sizeof act_dirty / sizeof act_dirty[0]))
                act_dirty[n_act_dirty++] = (int16_t)go;
            else
                hierarchy_dirty = 1;
        }
        break;
    }
    case P_POSX: o->lx = v; break;
    case P_POSY: o->ly = v; break;
    case P_SCX: o->lsx = v; break;
    case P_SCY: o->lsy = v; break;
    case P_ROTX: o->rx = v; break;
    case P_ROTY: o->ry = v; break;
    case P_ROTZ: o->lrot = v; break;
    case P_SR_SPRITE: if (o->sr >= 0) W.sr[o->sr].d.spr = (uint16_t)(int)v; break;
    case P_SR_R: if (o->sr >= 0) W.sr[o->sr].d.r = u8c(v); break;
    case P_SR_G: if (o->sr >= 0) W.sr[o->sr].d.g = u8c(v); break;
    case P_SR_B: if (o->sr >= 0) W.sr[o->sr].d.b = u8c(v); break;
    case P_SR_A: if (o->sr >= 0) W.sr[o->sr].d.a = u8c(v); break;
    case P_SR_EN:
        if (o->sr >= 0) {
            if (v > 0.5f) W.sr[o->sr].d.flags |= WSR_ENABLED;
            else W.sr[o->sr].d.flags &= ~WSR_ENABLED;
        }
        break;
    case P_SR_ORDER: if (o->sr >= 0) W.sr[o->sr].d.order = (int16_t)(int)v; break;
    case P_G_R: case P_G_G: case P_G_B: case P_G_A:
        if (o->img >= 0) (&W.img[o->img].r)[prop - P_G_R] = u8c(v);
        else if (o->text >= 0) (&W.text[o->text].d.r)[prop - P_G_R] = u8c(v);
        break;
    case P_G_EN:
        if (o->img >= 0) {
            if (v > 0.5f) W.img[o->img].flags |= WIMG_ENABLED; else W.img[o->img].flags &= ~WIMG_ENABLED;
        } else if (o->text >= 0) {
            if (v > 0.5f) W.text[o->text].d.flags |= WTXT_ENABLED; else W.text[o->text].d.flags &= ~WTXT_ENABLED;
        }
        break;
    case P_IMG_SPRITE: if (o->img >= 0) W.img[o->img].spr = (uint16_t)(int)v; break;
    case P_AP_X: if (o->rect >= 0) W.rect[o->rect].ap_x = v; break;
    case P_AP_Y: if (o->rect >= 0) W.rect[o->rect].ap_y = v; break;
    case P_SD_X: if (o->rect >= 0) W.rect[o->rect].sd_x = v; break;
    case P_SD_Y: if (o->rect >= 0) W.rect[o->rect].sd_y = v; break;
    case P_COL_EN:
        for (int k = 0; k < o->ncol; k++) {
            if (v > 0.5f) W.col[o->col_first + k].d.flags |= WCOL_ENABLED;
            else W.col[o->col_first + k].d.flags &= ~WCOL_ENABLED;
        }
        break;
    case P_CAPS_OFFX:
        for (int k = 0; k < o->ncol; k++)
            if (W.col[o->col_first + k].d.kind == COL_CAPSULE)
                W.col[o->col_first + k].d.ox = v;
        break;
    case P_LINE_W: if (o->line >= 0) W.line[o->line].width = v; break;
    case P_TXT_A: if (o->text >= 0) W.text[o->text].alpha_mul = v; break;
    }
}

/* ------------------------------------------------------------------ curves */
static float curve_at(const ACurve *cv, float t)
{
    const AKey *k = &g_akeys[cv->first];
    int n = cv->n;
    if (n == 1 || t <= k[0].t / 1000.0f)
        return k[0].d;
    int ms = (int)(t * 1000.0f + 0.5f);
    int i = 0;
    while (i + 1 < n && k[i + 1].t <= ms)
        i++;
    const AKey *a = &k[i];
    if (a->mode == 1)       /* step (sprite) */
        return a->d;
    float dt = t - a->t / 1000.0f;
    if (a->mode == 2) {     /* linear to the next key */
        if (i + 1 >= n)
            return a->d;
        const AKey *b = &k[i + 1];
        float span = (b->t - a->t) / 1000.0f;
        if (span <= 0)
            return b->d;
        return a->d + (b->d - a->d) * (dt / span);
    }
    if (i + 1 >= n)
        return a->d;
    return ((a->a * dt + a->b) * dt + a->c) * dt + a->d;
}

static int slot_of(const Anim *a, uint32_t path)
{
    const ACtrl *c = &g_actrls[a->ctrl];
    const uint32_t *p = &g_actrl_paths[c->first_path];
    int lo = 0, hi = c->npath - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (p[mid] == path)
            return mid;
        if (p[mid] < path)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int target(const Anim *a, int slot)
{
    if (slot < 0)
        return -1;
    return W.bind[a->bind_first + slot];
}

static void capture_defaults(Anim *a)
{
    const ACtrl *c = &g_actrls[a->ctrl];
    if (!a->defaults && c->nprop)
        a->defaults = malloc(sizeof(float) * c->nprop);
    for (int i = 0; i < c->nprop; i++) {
        const ACtrlProp *p = &g_actrl_props[c->first_prop + i];
        a->defaults[i] = prop_get(target(a, p->slot), p->prop);
    }
}

static void reset_params(Anim *a)
{
    memset(a->params, 0, sizeof a->params);
    for (int i = 0; i < NUM_APDEFS; i++)
        if (g_apdefs[i].ctrl == a->ctrl)
            a->params[g_apdefs[i].param] = g_apdefs[i].v;
}

static void reset_state(Anim *a)
{
    a->state = g_actrls[a->ctrl].def;
    a->t = a->prev_t = 0;
    reset_params(a);
    a->dirty = 1;
}

int anim_add(int go, int ctrl, int update, int enabled, int bind_first)
{
    int r = -1;
    for (int i = 0; i < g_nanim; i++)
        if (!g_anim[i].alive) { r = i; break; }
    if (r < 0) {
        if (g_nanim >= g_capanim)
            return -1;
        r = g_nanim++;
    }
    Anim *a = &g_anim[r];
    free(a->defaults);
    memset(a, 0, sizeof *a);
    a->go = go;
    a->ctrl = ctrl;
    a->update = update;
    a->enabled = enabled;
    a->alive = 1;
    a->bind_first = bind_first;
    a->speed = 1;
    reset_state(a);
    capture_defaults(a);
    return r;
}

void anim_world_free(void)
{
    for (int i = 0; i < g_nanim; i++)
        free(g_anim[i].defaults);
    free(g_anim);
    g_anim = 0;
    g_nanim = g_capanim = 0;
}

void anim_world_init(void)
{
    anim_world_free();
    int n = 0;
    for (int i = 0; i < W.nobj; i++)
        if (W.obj[i].anim >= 0)
            n++;
    g_capanim = n + RESERVE;
    g_anim = calloc(g_capanim, sizeof(Anim));
    /* the blob's ANIM section is indexed by Obj.anim */
    extern const void *w_anim_section(int *count);
    int cnt;
    const WAnim *src = w_anim_section(&cnt);
    for (int i = 0; i < cnt; i++) {
        Anim *a = &g_anim[i];
        a->go = src[i].go;
        a->ctrl = src[i].ctrl;
        a->update = src[i].update;
        a->enabled = src[i].enabled;
        a->alive = 1;
        a->bind_first = src[i].bind_first;
        a->speed = 1;
        reset_state(a);
    }
    g_nanim = cnt;
    for (int i = 0; i < cnt; i++)
        capture_defaults(&g_anim[i]);
}

/* ------------------------------------------------------------------ state machine */
static int conds_ok(const Anim *a, const ATrans *t)
{
    for (int i = 0; i < t->ncond; i++) {
        const ACond *c = &g_aconds[t->first_cond + i];
        float v = a->params[c->param];
        switch (c->mode) {
        case COND_IF: if (v == 0) return 0; break;
        case COND_IFNOT: if (v != 0) return 0; break;
        case COND_GREATER: if (!(v > c->thr)) return 0; break;
        case COND_LESS: if (!(v < c->thr)) return 0; break;
        case COND_EQUALS: if ((int)v != (int)c->thr) return 0; break;
        case COND_NOTEQUAL: if ((int)v == (int)c->thr) return 0; break;
        }
    }
    return 1;
}

static void consume(Anim *a, const ATrans *t)
{
    for (int i = 0; i < t->ncond; i++) {
        const ACond *c = &g_aconds[t->first_cond + i];
        if (g_param_kind[c->param] == PK_TRIGGER)
            a->params[c->param] = 0;
    }
}

static void enter(Anim *a, int state)
{
    a->state = state;
    a->t = a->prev_t = 0;
    a->dirty = 1;
}

static void fire_events(Anim *a, int clip, float t0, float t1, int include0)
{
    if (clip < 0)
        return;
    const AClip *c = &g_aclips[clip];
    for (int i = 0; i < c->nevents; i++) {
        const AEvent *e = &g_aevents[c->first_event + i];
        /* half-open window shifted by EPS: events exactly at a boundary (clip end) fire once despite rounding */
        const float EPS = 1e-4f;
        float et = e->t * 0.001f;
        if ((et > t0 + EPS || (include0 && et <= t0 + EPS)) && et <= t1 + EPS)
            anim_event(a->go, e->fn, e->iarg, e->farg);
    }
}

/* exit time: Mecanim fires when the normalized time passes exitTime; 0 means the end of the first loop */
static int exit_reached(const ATrans *t, float n0, float n1, int loop)
{
    float e = t->exit;
    if (e <= 0.0f)
        e = 1.0f;
    if (e < 1.0f && loop) {
        /* fires every loop at the fractional point */
        float b0 = floorf(n0), b1 = floorf(n1);
        for (float k = b0; k <= b1; k += 1.0f) {
            float pt = k + e;
            if (pt > n0 && pt <= n1)
                return 1;
        }
        return n0 == 0 && e == 0;
    }
    return n1 >= e;
}

static void step(Anim *a, float dt)
{
    const ACtrl *cd = &g_actrls[a->ctrl];
    const AState *st = &g_astates[cd->first_state + a->state];
    int clip = st->clip;
    float len = clip_len(clip);
    int loop = clip >= 0 ? g_aclips[clip].loop : 0;
    float p = a->t;
    a->prev_t = p;
    a->t += dt * st->speed * a->speed;
    float n = a->t;
    /* events */
    if (clip >= 0 && len > 0) {
        if (loop) {
            float base = floorf(p / len) * len;
            float lp = p - base, ln = n - base;
            if (ln >= len) {
                fire_events(a, clip, lp, len, p == 0);
                fire_events(a, clip, -1e-6f, ln - len, 1);
            } else {
                fire_events(a, clip, lp, ln, p == 0);
            }
        } else if (p <= len) {
            fire_events(a, clip, p, n > len ? len : n, p == 0);
        }
    } else if (clip >= 0 && p == 0) {
        fire_events(a, clip, 0, 0, 1);
    }
    float n0 = len > 0 ? p / len : 1.0f, n1 = len > 0 ? n / len : 1.0f;
    /* any-state transitions */
    for (int i = 0; i < cd->nany; i++) {
        const ATrans *t = &g_atrans[cd->first_any + i];
        if (!t->ncond && !t->has_exit)
            continue;
        if (!t->self && t->to == a->state)
            continue;
        if (t->has_exit && !exit_reached(t, n0, n1, loop))
            continue;
        if (conds_ok(a, t)) {
            consume(a, t);
            enter(a, t->to);
            return;
        }
    }
    for (int i = 0; i < st->ntrans; i++) {
        const ATrans *t = &g_atrans[st->first_trans + i];
        if (!t->has_exit && !t->ncond)
            continue;
        if (t->has_exit && !exit_reached(t, n0, n1, loop))
            continue;
        if (conds_ok(a, t)) {
            consume(a, t);
            enter(a, t->to);
            return;
        }
    }
}

void anim_update_all(float dt, float unscaled_dt)
{
    for (int i = 0; i < g_nanim; i++) {
        Anim *a = &g_anim[i];
        if (!a->alive)
            continue;
        if (!obj_active(a->go)) {
            a->pending_reset = 1;
            continue;
        }
        if (a->pending_reset) {
            /* Unity resets an Animator when its object is re-enabled */
            a->pending_reset = 0;
            reset_state(a);
        }
        if (!a->enabled)
            continue;
        step(a, a->update == 2 ? unscaled_dt : dt);
    }
}

static void apply(Anim *a)
{
    const ACtrl *cd = &g_actrls[a->ctrl];
    for (int i = 0; i < cd->nprop; i++) {
        const ACtrlProp *p = &g_actrl_props[cd->first_prop + i];
        prop_set(target(a, p->slot), p->prop, a->defaults[i]);
    }
    int clip = state_clip(a);
    if (clip < 0)
        return;
    const AClip *c = &g_aclips[clip];
    float len = c->length;
    float t = a->t;
    if (len > 0) {
        if (c->loop)
            t = fmodf(t, len);
        else if (t > len)
            t = len;
    } else {
        t = 0;
    }
    if (c->nframes) {
        int ms = (int)(t * 1000.0f + 0.5f);
        int spr = g_aframes[c->first_frame].spr;
        for (int i = 0; i < c->nframes; i++) {
            if (g_aframes[c->first_frame + i].t <= ms)
                spr = g_aframes[c->first_frame + i].spr;
            else
                break;
        }
        prop_set(a->go, P_SR_SPRITE, spr);
    }
    for (int i = 0; i < c->ncurves; i++) {
        const ACurve *cv = &g_acurves[c->first_curve + i];
        int go = cv->path == 0 ? a->go : target(a, slot_of(a, cv->path));
        if (go < 0)
            continue;
        prop_set(go, cv->prop, curve_at(cv, t));
    }
}

void anim_apply_all(void)
{
    for (int i = 0; i < g_nanim; i++) {
        Anim *a = &g_anim[i];
        if (!a->alive || !a->enabled || !obj_active(a->go))
            continue;
        apply(a);
    }
    /* activity changes from curves: refresh the touched subtrees (or everything) and run enable hooks;
       hooks may flip more objects, so loop a few times */
    extern void w_activity_changed(void);
    extern void w_activity_subtree(int root);
    for (int pass = 0; pass < 8 && (hierarchy_dirty || n_act_dirty); pass++) {
        int full = hierarchy_dirty, n = n_act_dirty;
        int16_t todo[64];
        memcpy(todo, act_dirty, sizeof(int16_t) * n);
        hierarchy_dirty = 0;
        n_act_dirty = 0;
        if (full)
            w_activity_changed();
        else
            for (int i = 0; i < n; i++)
                w_activity_subtree(todo[i]);
    }
}

/* ------------------------------------------------------------------ script API */
int anim_of(int go) { return (go >= 0 && go < W.nobj) ? W.obj[go].anim : -1; }

/* defaults must be read once the whole prefab instance exists (targets created later in the same
   instantiate still hold the previous occupant's state of a reused slot) */
void anim_recapture(int a)
{
    if (a >= 0 && a < g_nanim && g_anim[a].alive)
        capture_defaults(&g_anim[a]);
}

void anim_on_enable(int go)
{
    int a = anim_of(go);
    if (a >= 0 && a < g_nanim && g_anim[a].alive) {
        g_anim[a].pending_reset = 0;
        reset_state(&g_anim[a]);
    }
}

void anim_trigger(int go, int param)
{
    int a = anim_of(go);
    if (a >= 0 && param >= 0) {
        if (g_anim[a].pending_reset) {
            g_anim[a].pending_reset = 0;
            reset_state(&g_anim[a]);
        }
        g_anim[a].params[param] = 1;
    }
}
void anim_reset_trigger(int go, int param)
{
    int a = anim_of(go);
    if (a >= 0 && param >= 0)
        g_anim[a].params[param] = 0;
}
void anim_set_bool(int go, int param, int v)
{
    int a = anim_of(go);
    if (a >= 0 && param >= 0) {
        if (g_anim[a].pending_reset) {
            g_anim[a].pending_reset = 0;
            reset_state(&g_anim[a]);
        }
        g_anim[a].params[param] = v ? 1 : 0;
    }
}
void anim_set_int(int go, int param, int v)
{
    int a = anim_of(go);
    if (a >= 0 && param >= 0) {
        if (g_anim[a].pending_reset) {
            g_anim[a].pending_reset = 0;
            reset_state(&g_anim[a]);
        }
        g_anim[a].params[param] = (float)v;
    }
}
void anim_set_enabled(int go, int on)
{
    int a = anim_of(go);
    if (a >= 0)
        g_anim[a].enabled = on ? 1 : 0;
}
int anim_param_id(const char *name)
{
    for (int i = 0; i < NUM_PARAMS; i++)
        if (!strcmp(g_param_names[i], name))
            return i;
    return -1;
}
const char *anim_state_clip(int go)
{
    int a = anim_of(go);
    if (a < 0)
        return "";
    int c = state_clip(&g_anim[a]);
    return c >= 0 ? g_aclips[c].name : "";
}
float anim_state_length(int go)
{
    int a = anim_of(go);
    return a >= 0 ? clip_len(state_clip(&g_anim[a])) : 0;
}
float anim_state_time(int go)
{
    int a = anim_of(go);
    return a >= 0 ? g_anim[a].t : 0;
}
void anim_play(int go, const char *clip_name)
{
    int a = anim_of(go);
    if (a < 0)
        return;
    const ACtrl *cd = &g_actrls[g_anim[a].ctrl];
    for (int s = 0; s < cd->nstates; s++) {
        int c = g_astates[cd->first_state + s].clip;
        if (c >= 0 && !strcmp(g_aclips[c].name, clip_name)) {
            enter(&g_anim[a], s);
            return;
        }
    }
}
