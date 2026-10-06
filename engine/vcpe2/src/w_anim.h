/* Mecanim reimplementation: one layer per controller, triggers/bools/ints/floats, exit times,
 * write defaults, cubic curves bound to scene objects, clip events. */
#ifndef W_ANIM_H
#define W_ANIM_H
#include <stdint.h>
#include "awdata.h"

typedef struct {
    int16_t go, ctrl;
    uint8_t update;          /* 0 normal (scaled time), 2 unscaled */
    uint8_t enabled, alive, dirty;
    uint16_t bind_first;     /* W.bind[bind_first + path slot] = target object */
    int16_t state;
    float t, prev_t, speed;
    float *defaults;         /* per controller prop (write defaults) */
    float params[NUM_PARAMS];
    uint8_t pending_reset;
} Anim;

extern Anim *g_anim;
extern int g_nanim, g_capanim;

void anim_world_init(void);          /* animators of the loaded scene */
void anim_world_free(void);
int anim_add(int go, int ctrl, int update, int enabled, int bind_first);
void anim_update_all(float dt, float unscaled_dt);
void anim_apply_all(void);
void anim_on_enable(int go);

/* script API (by object) */
int anim_of(int go);
void anim_trigger(int go, int param);
void anim_reset_trigger(int go, int param);
void anim_set_bool(int go, int param, int v);
void anim_set_int(int go, int param, int v);
void anim_set_enabled(int go, int on);
int anim_param_id(const char *name);
void anim_recapture(int a);             /* re-read write-defaults values (after instantiate) */
const char *anim_state_clip(int go);   /* current clip name ("" none) */
float anim_state_length(int go);
float anim_state_time(int go);
void anim_play(int go, const char *clip_name);
/* event sink (implemented by the script layer) */
void anim_event(int go, int fn, int iarg, float farg);
#endif
