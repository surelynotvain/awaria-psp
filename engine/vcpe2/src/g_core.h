/* Script layer: per-type behaviour tables, game time, coroutine helper, physics callbacks. */
#ifndef G_CORE_H
#define G_CORE_H
#include <stddef.h>
#include "w_world.h"

typedef struct {
    size_t size;                                      /* per-instance state */
    void (*init)(Script *s);                          /* Awake (+ field caching) */
    void (*start)(Script *s);                         /* Start (first frame it is active) */
    void (*update)(Script *s, float dt);
    void (*fixed)(Script *s, float dt);
    void (*event)(Script *s, int fn, int iarg, float farg);           /* animation events */
    void (*trigger)(Script *s, int other_go, int other_col);         /* OnTriggerEnter2D */
    void (*collision)(Script *s, int other_go, float rel_speed);     /* OnCollisionEnter2D */
    void (*enable)(Script *s);                        /* OnEnable */
    void (*disable)(Script *s);                       /* OnDisable */
} ScriptVT;

extern const ScriptVT *g_vt[NUM_SCRIPT_TYPES];

/* time */
typedef struct { float scale, dt, udt, t, ut; } GameTime;
extern GameTime T;

/* coroutines: a step machine with a timer (scaled unless realtime) */
typedef struct { int16_t step; uint8_t on, realtime; float wait; } Co;
static inline void co_start(Co *c) { c->on = 1; c->step = 0; c->wait = 0; c->realtime = 0; }
static inline void co_stop(Co *c) { c->on = 0; }
/* returns 1 when the coroutine should run its current step this frame */
static inline int co_tick(Co *c, float dt, float udt)
{
    if (!c->on) return 0;
    if (c->wait > 0) {
        c->wait -= c->realtime ? udt : dt;
        if (c->wait > 0) return 0;
    }
    return 1;
}
static inline void co_wait(Co *c, float secs, int next) { c->wait = secs; c->step = next; c->realtime = 0; }
static inline void co_wait_rt(Co *c, float secs, int next) { c->wait = secs; c->step = next; c->realtime = 1; }

/* helpers for scripts */
Script *script_on(int go, int type);                  /* first script of a type on an object */
Script *script_ref(Script *s, uint32_t field, int type);   /* referenced object's script of a type */
int sr_of(int go);
int text_of(int go);
int img_of(int go);
void g_send_event(int go, int fn, int iarg, float farg);

/* physics callbacks (w_phys.c -> scripts on the objects) */
void g_trigger_enter(int go, int other_go, int other_col);
void g_collision_enter(int go, int other_go, float rel_speed);

void g_update(float udt);                             /* one frame of the game */
void g_scene_request(int scene);
void g_scene_filter(int (*fn)(int scene));   /* game hook: may change a requested scene */
int g_scene_pending(void);
void g_scene_switch(void);
#endif
