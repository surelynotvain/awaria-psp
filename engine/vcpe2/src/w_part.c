/* ParticleSystem subset (generator sparks): looping bursts, sphere emitter, gravity, fading squares. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "w_world.h"
#include "w_render.h"
#include "aw_gfx.h"

#ifndef VCPE_PARTICLES_PER_SYSTEM
#define VCPE_PARTICLES_PER_SYSTEM 48
#endif
#define MAXP VCPE_PARTICLES_PER_SYSTEM
#if MAXP < 1
#error VCPE_PARTICLES_PER_SYSTEM must be positive
#endif
typedef struct { float x, y, vx, vy, life, age, size; } P;
typedef struct { float t; int n; uint8_t playing, burst_done; P p[MAXP]; } PSys;
static PSys *ps;
static int nps;
int particles_alloc_fails;

uint32_t particles_memory_bytes(void) { return (uint32_t)nps * sizeof *ps; }
int vcpe_adapt_effect_scale(void) __attribute__((weak));

uint32_t particles_trim(void)
{
    if (W.npart >= nps || W.npart < 0) return 0;
    PSys *next = W.npart ? realloc(ps, (size_t)W.npart * sizeof *ps) : 0;
    if (W.npart && !next) return 0;
    uint32_t freed = (uint32_t)(nps - W.npart) * sizeof *ps;
    if (!W.npart) free(ps);
    ps = next; nps = W.npart;
    return freed;
}

/* Reserve histories for instantiated components, never the world's unused prefab reserve. A grow keeps all
 * existing systems (including live particles); a failed grow leaves the old allocation usable. */
static void particles_reserve(void)
{
    if (W.npart <= nps)
        return;
    PSys *next = realloc(ps, (size_t)W.npart * sizeof *ps);
    if (!next) {
        particles_alloc_fails++;
        return;
    }
    ps = next;
    memset(ps + nps, 0, (size_t)(W.npart - nps) * sizeof *ps);
    nps = W.npart;
}

static float frand(void) { return (rand() & 0xFFFF) / 65535.0f; }

/* Particles are kept per system in a fixed pool; each has a position, velocity, age and life. */
void particles_reset(void)
{
    if (!W.npart) {
        free(ps);
        ps = 0;
        nps = 0;
        return;
    }
    particles_reserve();
    if (ps)
        memset(ps, 0, (size_t)nps * sizeof *ps);
    for (int i = 0; i < W.npart && i < nps; i++)
        if (W.part[i].go >= 0 && W.part[i].awake)
            ps[i].playing = 1;
}

void particles_play(int go)
{
    particles_reserve();
    for (int i = 0; i < W.npart && i < nps; i++)
        if (W.part[i].go == go) {
            ps[i].playing = 1;
            ps[i].t = 0;
            ps[i].burst_done = 0;
        }
}

void particles_stop(int go)
{
    for (int i = 0; i < W.npart && i < nps; i++)
        if (W.part[i].go == go)
            ps[i].playing = 0;
}

/* Spawn count particles at the emitter: a random direction on a sphere projected onto the screen plane (like Unity's
 * sphere shape seen from the front), a random start offset within the radius, and random speed, size and lifetime
 * between the min and max. */
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

/* Bursts at their times (looping systems restart), then move every particle (gravity), age it and drop the dead. */
void particles_update(float dt)
{
    if (!ps)
        particles_reset();
    else
        particles_reserve();
    for (int i = 0; i < W.npart && i < nps; i++) {
        WPart *pd = &W.part[i];
        PSys *s = &ps[i];
        if (pd->go < 0) { s->n = 0; continue; }
        int live = obj_active(pd->go);
        if (s->playing && live) {
            float t0 = s->t;
            s->t += dt;
            for (int b = 0; b < pd->nburst && b < 4; b++) {
                if (!(s->burst_done & (1u << b)) && pd->burst_t[b] >= t0 && pd->burst_t[b] < s->t) {
                    emit(i, pd->burst_n[b]);
                    s->burst_done |= 1u << b;
                }
            }
            if (s->t >= pd->len) {
                if (pd->loop) {
                    s->t -= pd->len;
                    s->burst_done = 0;
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

/* Draw a system's particles as squares that fade out with age. */
void render_particles_draw(int idx, float cam_x, float cam_y, float pxu)
{
    if (idx < 0 || idx >= nps || idx >= W.npart)
        return;
    WPart *pd = &W.part[idx];
    PSys *s = &ps[idx];
    int scale = vcpe_adapt_effect_scale ? vcpe_adapt_effect_scale() : 256;
    for (int k = 0; k < s->n; k++) {
        /* Rendering only: retain simulation and every random call, so adaptive detail cannot change game RNG. */
        if ((k + 1) * scale / 256 == k * scale / 256) continue;
        P *p = &s->p[k];
        float f = 1.0f - p->age / p->life;
        float sz = p->size * pxu * 0.35f * (0.4f + 0.6f * f);
        if (sz < 1.0f) sz = 1.0f;
        float sx = SCR_W * 0.5f + (p->x - cam_x) * pxu, sy = SCR_H * 0.5f - (p->y - cam_y) * pxu;
        gfx_fill(sx - sz * 0.5f, sy - sz * 0.5f, sz, sz, RGBA(pd->r, pd->g, pd->b, (int)(pd->a * f)));
    }
}
