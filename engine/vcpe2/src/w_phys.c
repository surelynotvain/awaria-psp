#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "w_phys.h"
#include "w_world.h"
#include "g_core.h"

/* Physics2DSettings.m_LayerCollisionMatrix (rows that are not "everything") */
static uint32_t layer_matrix(int l)
{
    switch (l) {
    case 6: return 0xfffffd7fu;   /* PlayerCollision: not EnemyCollision, not Death */
    case 7: return 0xfffffb3fu;   /* EnemyCollision: not PlayerCollision, EnemyCollision, PlayerHitbox */
    case 9: return 0xffffffbfu;   /* Death: not PlayerCollision */
    case 10: return 0xffffff7fu;  /* PlayerHitbox: not EnemyCollision */
    }
    return 0xffffffffu;
}
static int layers_collide(int a, int b) { return (layer_matrix(a) >> b) & 1 && (layer_matrix(b) >> a) & 1; }

typedef struct { float vx, vy, fx, fy, av; } Body;
static Body *bodies;
static int nbodies;
static int16_t *col_body;   /* collider -> rigidbody index (nearest ancestor), -1 static */
static int ncolb;

/* the gathered shape list is reused until something changes (step, activity, colliders, instancing) */
static int gather_valid = -1;
void phys_dirty(void) { gather_valid = -1; }

typedef struct { int col; int go; int body; uint8_t kind, trigger, layer, pad; float x, y, r, hx, hy; } CG;
static CG *cg;
static int ncg, capcg;

static uint32_t *pairs_prev, *pairs_cur;
static int nprev, ncur, cappairs;

static void attach_colliders(void)
{
    if (ncolb < W.capcol) {
        col_body = realloc(col_body, sizeof(int16_t) * W.capcol);
        ncolb = W.capcol;
    }
    for (int c = 0; c < W.ncol; c++) {
        col_body[c] = -1;
        if (W.col[c].d.go < 0)
            continue;
        for (int o = W.col[c].d.go; o >= 0; o = W.obj[o].parent) {
            if (W.obj[o].rb >= 0 && W.rbd[W.obj[o].rb].go == o) {
                col_body[c] = W.obj[o].rb;
                break;
            }
        }
    }
}

void phys_world_init(void)
{
    gather_valid = -1;
    phys_world_free();
    nbodies = W.caprb;
    bodies = calloc(nbodies ? nbodies : 1, sizeof(Body));
    attach_colliders();
}

void phys_world_free(void)
{
    free(bodies); bodies = 0; nbodies = 0;
    free(col_body); col_body = 0; ncolb = 0;
    nprev = ncur = 0;
}

void phys_instantiate(int root)
{
    gather_valid = -1;
    (void)root;
    if (nbodies < W.caprb) {
        bodies = realloc(bodies, sizeof(Body) * W.caprb);
        memset(bodies + nbodies, 0, sizeof(Body) * (W.caprb - nbodies));
        nbodies = W.caprb;
    }
    for (int o = 0; o < W.nobj; o++)
        if (W.obj[o].alive && W.obj[o].rb >= 0 && obj_is_descendant(o, root))
            memset(&bodies[W.obj[o].rb], 0, sizeof(Body));
    attach_colliders();
}

void phys_destroy(int obj)
{
    gather_valid = -1;
    if (W.obj[obj].rb >= 0)
        memset(&bodies[W.obj[obj].rb], 0, sizeof(Body));
}

/* ------------------------------------------------------------------ geometry */

static void gather(int include_triggers)
{
    if (gather_valid == include_triggers)
        return;
    gather_valid = include_triggers;
    int need = W.ncol + W.nwall;
    if (capcg < need) {
        capcg = need + 64;
        cg = realloc(cg, sizeof(CG) * capcg);
    }
    ncg = 0;
    for (int c = 0; c < W.ncol; c++) {
        Col *co = &W.col[c];
        if (co->d.go < 0 || !(co->d.flags & WCOL_ENABLED) || !obj_active(co->d.go))
            continue;
        if (!include_triggers && (co->d.flags & WCOL_TRIGGER))
            continue;
        Obj *o = &W.obj[co->d.go];
        float m[6];
        obj_matrix(co->d.go, m);
        float sx = sqrtf(m[0] * m[0] + m[2] * m[2]), sy = sqrtf(m[1] * m[1] + m[3] * m[3]);
        CG *g = &cg[ncg++];
        g->col = c; g->go = co->d.go; g->body = col_body[c]; g->kind = co->d.kind;
        g->trigger = (co->d.flags & WCOL_TRIGGER) ? 1 : 0; g->layer = o->layer;
        g->x = m[0] * co->d.ox + m[1] * co->d.oy + m[4];
        g->y = m[2] * co->d.ox + m[3] * co->d.oy + m[5];
        if (co->d.kind == COL_CIRCLE) {
            g->r = co->d.a * (sx > sy ? sx : sy);
            g->hx = g->hy = g->r;
        } else {
            g->hx = co->d.a * 0.5f * sx;
            g->hy = co->d.b * 0.5f * sy;
            g->r = 0;
            if (co->d.kind == COL_CAPSULE)
                g->r = (g->hx < g->hy ? g->hx : g->hy);
        }
    }
    for (int w = 0; w < W.nwall; w++) {
        const WWall *wl = &W.wall[w];
        if (wl->go >= 0 && !obj_active(wl->go))
            continue;
        CG *g = &cg[ncg++];
        g->col = -1 - w; g->go = wl->go; g->body = -1; g->kind = COL_BOX; g->trigger = 0; g->layer = wl->layer;
        g->x = (wl->x0 + wl->x1) * 0.5f; g->y = (wl->y0 + wl->y1) * 0.5f;
        g->hx = (wl->x1 - wl->x0) * 0.5f; g->hy = (wl->y1 - wl->y0) * 0.5f; g->r = 0;
    }
}

/* separation of shape a from b: returns 1 and the push (on a) when they overlap */
static int sep(const CG *a, const CG *b, float *px, float *py)
{
    int ac = a->kind == COL_CIRCLE, bc = b->kind == COL_CIRCLE;
    if (ac && bc) {
        float dx = a->x - b->x, dy = a->y - b->y, d2 = dx * dx + dy * dy, rr = a->r + b->r;
        if (d2 >= rr * rr)
            return 0;
        float d = sqrtf(d2);
        if (d < 1e-6f) { *px = 0; *py = rr; return 1; }
        *px = dx / d * (rr - d); *py = dy / d * (rr - d);
        return 1;
    }
    if (ac || bc) {
        const CG *c = ac ? a : b, *bx = ac ? b : a;
        /* capsules/boxes as rounded rectangles: inner box + radius */
        float rin = bx->kind == COL_CAPSULE ? bx->r : 0;
        float ihx = bx->hx - rin, ihy = bx->hy - rin;
        float qx = c->x - bx->x, qy = c->y - bx->y;
        float cx = qx < -ihx ? -ihx : qx > ihx ? ihx : qx;
        float cy = qy < -ihy ? -ihy : qy > ihy ? ihy : qy;
        float dx = qx - cx, dy = qy - cy, d2 = dx * dx + dy * dy, rr = c->r + rin;
        float ox, oy;
        if (d2 > 1e-12f) {
            if (d2 >= rr * rr)
                return 0;
            float d = sqrtf(d2);
            ox = dx / d * (rr - d); oy = dy / d * (rr - d);
        } else {
            /* centre inside the box: push out along the shallow axis */
            float pxp = ihx - qx + rr, pxn = ihx + qx + rr, pyp = ihy - qy + rr, pyn = ihy + qy + rr;
            float m = pxp; ox = pxp; oy = 0;
            if (pxn < m) { m = pxn; ox = -pxn; oy = 0; }
            if (pyp < m) { m = pyp; ox = 0; oy = pyp; }
            if (pyn < m) { ox = 0; oy = -pyn; }
        }
        if (c == a) { *px = ox; *py = oy; } else { *px = -ox; *py = -oy; }
        return 1;
    }
    /* box vs box */
    float dx = a->x - b->x, dy = a->y - b->y;
    float ox = a->hx + b->hx - fabsf(dx), oy = a->hy + b->hy - fabsf(dy);
    if (ox <= 0 || oy <= 0)
        return 0;
    if (ox < oy) { *px = dx < 0 ? -ox : ox; *py = 0; }
    else { *px = 0; *py = dy < 0 ? -oy : oy; }
    return 1;
}

static int pair_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void pair_add(int a, int b)
{
    if (ncur >= cappairs) {
        cappairs = cappairs ? cappairs * 2 : 512;
        pairs_cur = realloc(pairs_cur, sizeof(uint32_t) * cappairs);
        pairs_prev = realloc(pairs_prev, sizeof(uint32_t) * cappairs);
    }
    uint32_t ka = (uint32_t)(a + 32768) & 0xFFFF, kb = (uint32_t)(b + 32768) & 0xFFFF;
    pairs_cur[ncur++] = ka < kb ? (ka << 16 | kb) : (kb << 16 | ka);
}

static int pair_was(uint32_t key)
{
    return bsearch(&key, pairs_prev, nprev, sizeof(uint32_t), pair_cmp) != 0;
}

static int dynamic_body(int b)
{
    return b >= 0 && W.rbd[b].go >= 0 && W.rbd[b].type == RB_DYNAMIC && W.rbd[b].sim && obj_active(W.rbd[b].go);
}

void phys_step(float dt)
{
    /* integrate */
    for (int b = 0; b < W.nrb; b++) {
        WRb *rb = &W.rbd[b];
        if (rb->go < 0 || !rb->sim || !obj_active(rb->go) || rb->type == RB_STATIC)
            continue;
        Body *bd = &bodies[b];
        if (rb->type == RB_DYNAMIC) {
            float im = rb->mass > 0 ? 1.0f / rb->mass : 1;
            bd->vx += bd->fx * im * dt;
            bd->vy += (bd->fy * im - 9.81f * rb->grav) * dt;
            float k = 1.0f / (1.0f + dt * rb->drag);
            bd->vx *= k; bd->vy *= k;
            bd->av *= 1.0f / (1.0f + dt * rb->adrag);
        }
        bd->fx = bd->fy = 0;
        if (bd->vx != 0 || bd->vy != 0) {
            float x, y;
            obj_pos(rb->go, &x, &y);
            obj_set_pos(rb->go, x + bd->vx * dt, y + bd->vy * dt);
        }
        if (!(rb->constraints & 4) && bd->av != 0)
            W.obj[rb->go].lrot += bd->av * dt;
    }
    /* contacts */
    extern int g_autotest;
    long long dbg_t0 = g_autotest ? (long long)sceKernelGetSystemTimeWide() : 0;
    gather_valid = -1;
    gather(1);
    long long dbg_t1 = g_autotest ? (long long)sceKernelGetSystemTimeWide() : 0;
    ncur = 0;
    for (int i = 0; i < ncg; i++) {
        CG *a = &cg[i];
        int adyn = dynamic_body(a->body);
        for (int j = i + 1; j < ncg; j++) {
            CG *b = &cg[j];
            if (a->body >= 0 && a->body == b->body)
                continue;
            if (a->col < 0 && b->col < 0)
                continue;
            int bdyn = dynamic_body(b->body);
            int trig = a->trigger || b->trigger;
            if (trig) {
                if (a->body < 0 && b->body < 0)
                    continue;
            } else {
                if (!adyn && !bdyn)
                    continue;
            }
            if (!layers_collide(a->layer, b->layer))
                continue;
            if (fabsf(a->x - b->x) > a->hx + b->hx || fabsf(a->y - b->y) > a->hy + b->hy)
                continue;
            float px, py;
            if (!sep(a, b, &px, &py))
                continue;
            if (trig) {
                pair_add(a->col, b->col);
                uint32_t key = pairs_cur[ncur - 1];
                if (!pair_was(key)) {
                    g_trigger_enter(a->go, b->go, b->col);
                    g_trigger_enter(b->go, a->go, a->col);
                }
                continue;
            }
            Body *ba = adyn ? &bodies[a->body] : 0, *bb = bdyn ? &bodies[b->body] : 0;
            float rvx = (ba ? ba->vx : 0) - (bb ? bb->vx : 0), rvy = (ba ? ba->vy : 0) - (bb ? bb->vy : 0);
            float rel = sqrtf(rvx * rvx + rvy * rvy);
            float share_a = adyn && bdyn ? 0.5f : adyn ? 1.0f : 0.0f;
            float share_b = adyn && bdyn ? 0.5f : bdyn ? 1.0f : 0.0f;
            float pl = sqrtf(px * px + py * py);
            float nx = pl > 0 ? px / pl : 0, ny = pl > 0 ? py / pl : 0;
            if (adyn) {
                float x, y;
                obj_pos(W.rbd[a->body].go, &x, &y);
                obj_set_pos(W.rbd[a->body].go, x + px * share_a, y + py * share_a);
                a->x += px * share_a; a->y += py * share_a;
                float vn = ba->vx * nx + ba->vy * ny;
                if (vn < 0) { ba->vx -= vn * nx; ba->vy -= vn * ny; }
            }
            if (bdyn) {
                float x, y;
                obj_pos(W.rbd[b->body].go, &x, &y);
                obj_set_pos(W.rbd[b->body].go, x - px * share_b, y - py * share_b);
                b->x -= px * share_b; b->y -= py * share_b;
                float vn = -(bb->vx * nx + bb->vy * ny);
                if (vn < 0) { bb->vx += vn * nx; bb->vy += vn * ny; }
            }
            pair_add(a->col, b->col);
            uint32_t key = pairs_cur[ncur - 1];
            if (!pair_was(key)) {
                g_collision_enter(a->go, b->go, rel);
                g_collision_enter(b->go, a->go, rel);
            }
        }
    }
    gather_valid = -1;
    qsort(pairs_cur, ncur, sizeof(uint32_t), pair_cmp);
    uint32_t *t = pairs_prev; pairs_prev = pairs_cur; pairs_cur = t;
    nprev = ncur;
    if (g_autotest) {
        long long t2 = sceKernelGetSystemTimeWide();
        if (t2 - dbg_t0 > 50000)
            printf("[aw] slow phys: gather %lld us, pairs+events %lld us, ncg %d ncol %d pairs %d prev %d\n", dbg_t1 - dbg_t0, t2 - dbg_t1, ncg, W.ncol, ncur, nprev);
    }
}

/* a collider disabled and re-enabled in the same frame: Unity sends the enter events again */
void phys_forget_pairs(int col)
{
    uint32_t k = (uint32_t)(col + 32768) & 0xFFFF;
    int j = 0;
    for (int i = 0; i < nprev; i++)
        if ((pairs_prev[i] >> 16) != k && (pairs_prev[i] & 0xFFFF) != k)
            pairs_prev[j++] = pairs_prev[i];
    nprev = j;
}

/* ------------------------------------------------------------------ bodies */
int phys_body(int go) { return (go >= 0 && go < W.nobj) ? W.obj[go].rb : -1; }

void phys_velocity(int go, float *vx, float *vy)
{
    int b = phys_body(go);
    *vx = b >= 0 ? bodies[b].vx : 0;
    *vy = b >= 0 ? bodies[b].vy : 0;
}
void phys_set_velocity(int go, float vx, float vy)
{
    int b = phys_body(go);
    if (b >= 0) { bodies[b].vx = vx; bodies[b].vy = vy; }
}
void phys_add_force(int go, float fx, float fy)
{
    int b = phys_body(go);
    if (b >= 0) { bodies[b].fx += fx; bodies[b].fy += fy; }
}
void phys_add_impulse(int go, float ix, float iy)
{
    int b = phys_body(go);
    if (b >= 0) {
        float im = W.rbd[b].mass > 0 ? 1.0f / W.rbd[b].mass : 1;
        bodies[b].vx += ix * im;
        bodies[b].vy += iy * im;
    }
}
void phys_add_torque(int go, float t)
{
    int b = phys_body(go);
    if (b >= 0) bodies[b].av += t * 0.05f;
}
void phys_set_position(int go, float x, float y) { obj_set_pos(go, x, y); }

/* ------------------------------------------------------------------ colliders */
void col_set_enabled(int c, int on)
{
    gather_valid = -1;
    if (c < 0) return;
    if (on) W.col[c].d.flags |= WCOL_ENABLED;
    else W.col[c].d.flags &= ~WCOL_ENABLED;
}
int col_enabled(int c) { return c >= 0 && (W.col[c].d.flags & WCOL_ENABLED); }
int col_of(int go, int kind)
{
    if (go < 0) return -1;
    Obj *o = &W.obj[go];
    for (int k = 0; k < o->ncol; k++)
        if (kind < 0 || W.col[o->col_first + k].d.kind == kind)
            return o->col_first + k;
    return -1;
}
void col_set_radius(int c, float r) { if (c >= 0) W.col[c].d.a = r; }
float col_radius(int c) { return c >= 0 ? W.col[c].d.a : 0; }

/* ------------------------------------------------------------------ queries */
static int mask_ok(int mask, int layer) { return mask == -1 || ((mask >> layer) & 1); }

int phys_overlap_circle(float x, float y, float r, int mask, int *col_out)
{
    gather(1);
    CG q = {0};
    q.kind = COL_CIRCLE; q.x = x; q.y = y; q.r = r; q.hx = q.hy = r;
    for (int i = 0; i < ncg; i++) {
        if (!mask_ok(mask, cg[i].layer))
            continue;
        if (fabsf(q.x - cg[i].x) > q.hx + cg[i].hx || fabsf(q.y - cg[i].y) > q.hy + cg[i].hy)
            continue;
        float px, py;
        if (sep(&q, &cg[i], &px, &py)) {
            if (col_out) *col_out = cg[i].col;
            return cg[i].go;
        }
    }
    return -1;
}

static int ray_shape(const CG *g, float ox, float oy, float dx, float dy, float inflate, float *t)
{
    if (g->kind == COL_CIRCLE) {
        float rr = g->r + inflate;
        float fx = ox - g->x, fy = oy - g->y;
        float b = fx * dx + fy * dy, c = fx * fx + fy * fy - rr * rr;
        if (c <= 0) { *t = 0; return 1; }
        float disc = b * b - c;
        if (disc < 0) return 0;
        float tt = -b - sqrtf(disc);
        if (tt < 0) return 0;
        *t = tt;
        return 1;
    }
    float hx = g->hx + inflate, hy = g->hy + inflate;
    float x0 = g->x - hx, x1 = g->x + hx, y0 = g->y - hy, y1 = g->y + hy;
    if (ox >= x0 && ox <= x1 && oy >= y0 && oy <= y1) { *t = 0; return 1; }
    float tmin = -1e30f, tmax = 1e30f;
    if (fabsf(dx) < 1e-9f) { if (ox < x0 || ox > x1) return 0; }
    else {
        float a = (x0 - ox) / dx, b = (x1 - ox) / dx;
        if (a > b) { float s = a; a = b; b = s; }
        if (a > tmin) tmin = a;
        if (b < tmax) tmax = b;
    }
    if (fabsf(dy) < 1e-9f) { if (oy < y0 || oy > y1) return 0; }
    else {
        float a = (y0 - oy) / dy, b = (y1 - oy) / dy;
        if (a > b) { float s = a; a = b; b = s; }
        if (a > tmin) tmin = a;
        if (b < tmax) tmax = b;
    }
    if (tmax < tmin || tmax < 0) return 0;
    *t = tmin < 0 ? 0 : tmin;
    return 1;
}

static int cast(float x, float y, float r, float dx, float dy, float dist, int mask, float *hx, float *hy, int *col_out)
{
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-9f) return -1;
    dx /= l; dy /= l;
    gather(1);   /* Physics2D.queriesHitTriggers defaults to true */
    int best = -1;
    float bt = dist;
    for (int i = 0; i < ncg; i++) {
        if (!mask_ok(mask, cg[i].layer))
            continue;
        float t;
        if (ray_shape(&cg[i], x, y, dx, dy, r, &t) && t <= bt) {
            bt = t;
            best = i;
        }
    }
    if (best < 0)
        return -1;
    if (hx) *hx = x + dx * bt;
    if (hy) *hy = y + dy * bt;
    if (col_out) *col_out = cg[best].col;
    return cg[best].go;
}

int phys_raycast(float x, float y, float dx, float dy, float dist, int mask, float *hx, float *hy, int *col_out)
{
    return cast(x, y, 0, dx, dy, dist, mask, hx, hy, col_out);
}

int phys_circlecast(float x, float y, float r, float dx, float dy, float dist, int mask, int *col_out)
{
    return cast(x, y, r, dx, dy, dist, mask, 0, 0, col_out);
}
