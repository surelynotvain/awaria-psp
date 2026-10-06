/* ParticleSystem subset (generator sparks): looping bursts, sphere emitter, gravity, fading squares. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "w_world.h"
#include "w_render.h"
#include "aw_gfx.h"

#define MAXP 48
typedef struct { float x, y, vx, vy, life, age, size; } P;
typedef struct { int playing; float t; int burst_done[4]; P p[MAXP]; int n; } PSys;
static PSys *ps;
static int nps;

static float frand(void) { return (rand() & 0xFFFF) / 65535.0f; }

void particles_reset(void)
{
    free(ps);
    nps = W.cappart;
    ps = calloc(nps ? nps : 1, sizeof(PSys));
    for (int i = 0; i < W.npart; i++)
        if (W.part[i].go >= 0 && W.part[i].awake)
            ps[i].playing = 1;
}

void particles_play(int go)
{
    for (int i = 0; i < W.npart && i < nps; i++)
        if (W.part[i].go == go) {
            ps[i].playing = 1;
            ps[i].t = 0;
            memset(ps[i].burst_done, 0, sizeof ps[i].burst_done);
        }
}

void particles_stop(int go)
{
    for (int i = 0; i < W.npart && i < nps; i++)
        if (W.part[i].go == go)
            ps[i].playing = 0;
}

static void emit(int i, int count)
{
    WPart *pd = &W.part[i];
    PSys *s = &ps[i];
    float ox, oy;
    obj_pos(pd->go, &ox, &oy);
    for (int k = 0; k < count && s->n < MAXP; k++) {
        P *p = &s->p[s->n++];
        /* random direction on a sphere, projected onto the screen plane */
        float z = frand() * 2 - 1, a = frand() * 6.2831853f, r = sqrtf(1 - z * z);
        float dx = r * cosf(a), dy = r * sinf(a);
        float sp = pd->speed0 + (pd->speed1 - pd->speed0) * frand();
        float rad = pd->radius * frand();
        p->x = ox + dx * rad;
        p->y = oy + dy * rad;
        p->vx = dx * sp;
        p->vy = dy * sp;
        p->life = pd->life0 + (pd->life1 - pd->life0) * frand();
        p->age = 0;
        p->size = pd->size0 + (pd->size1 - pd->size0) * frand();
    }
}

void particles_update(float dt)
{
    if (!ps || nps < W.cappart)
        particles_reset();
    for (int i = 0; i < W.npart; i++) {
        WPart *pd = &W.part[i];
        PSys *s = &ps[i];
        if (pd->go < 0) { s->n = 0; continue; }
        int live = obj_active(pd->go);
        if (s->playing && live) {
            float t0 = s->t;
            s->t += dt;
            for (int b = 0; b < pd->nburst && b < 4; b++) {
                if (!s->burst_done[b] && pd->burst_t[b] >= t0 && pd->burst_t[b] < s->t) {
                    emit(i, pd->burst_n[b]);
                    s->burst_done[b] = 1;
                }
            }
            if (s->t >= pd->len) {
                if (pd->loop) {
                    s->t -= pd->len;
                    memset(s->burst_done, 0, sizeof s->burst_done);
                } else {
                    s->playing = 0;
                }
            }
        }
        int keep = 0;
        for (int k = 0; k < s->n; k++) {
            P *p = &s->p[k];
            p->age += dt;
            if (p->age >= p->life)
                continue;
            p->vy -= 9.81f * pd->grav * dt;
            p->x += p->vx * dt;
            p->y += p->vy * dt;
            s->p[keep++] = *p;
        }
        s->n = keep;
    }
}

void render_particles_items(void)
{
    for (int i = 0; i < W.npart && i < nps; i++)
        if (W.part[i].go >= 0 && ps[i].n)
            render_push_particles(i, W.part[i].layer, W.part[i].order, W.obj[W.part[i].go].m[5]);
}

void render_particles_draw(int idx, float cam_x, float cam_y, float pxu)
{
    WPart *pd = &W.part[idx];
    PSys *s = &ps[idx];
    for (int k = 0; k < s->n; k++) {
        P *p = &s->p[k];
        float f = 1.0f - p->age / p->life;
        float sz = p->size * pxu * 0.35f * (0.4f + 0.6f * f);
        if (sz < 1.0f) sz = 1.0f;
        float sx = SCR_W * 0.5f + (p->x - cam_x) * pxu, sy = SCR_H * 0.5f - (p->y - cam_y) * pxu;
        gfx_fill(sx - sz * 0.5f, sy - sz * 0.5f, sz, sz, RGBA(pd->r, pd->g, pd->b, (int)(pd->a * f)));
    }
}
