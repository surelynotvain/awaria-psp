#include <string.h>
#include <stdlib.h>
#include <psptypes.h>
#include <psputils.h>
#include <pspkernel.h>
#include "g_core.h"
#include "w_hooks.h"
#include "w_anim.h"
#include "w_render.h"
#include "w_phys.h"
#include "aw_gfx.h"
#include "w_path.h"

GameTime T = {1, 0, 0, 0, 0};
static int requested_scene = -1;

extern const ScriptVT *const g_vt_table[];
const ScriptVT *g_vt[NUM_SCRIPT_TYPES];

static void vt_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < NUM_SCRIPT_TYPES; i++)
        g_vt[i] = g_vt_table[i];
}

static void script_create(Script *s)
{
    const ScriptVT *vt = g_vt[s->type];
    if (!vt)
        return;
    if (vt->size && !s->self)
        s->self = calloc(1, vt->size);
}

/* Awake: the first time the object is active in hierarchy */
static void script_awake(Script *s)
{
    if (s->awake || !s->alive)
        return;
    s->awake = 1;
    const ScriptVT *vt = g_vt[s->type];
    if (vt && vt->init)
        vt->init(s);
}

static void scripts_changed(void);
void hooks_load(void)
{
    scripts_changed();
    vt_init();
    anim_world_init();
    particles_reset();
    phys_world_init();
    path_build();
    for (int i = 0; i < W.nscript; i++)
        if (W.script[i].alive)
            script_create(&W.script[i]);
    /* Awake + OnEnable for scripts on active objects, object by object like Unity */
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (!s->alive || !obj_active(s->go))
            continue;
        script_awake(s);
        if (s->alive && s->enabled && obj_active(s->go) && g_vt[s->type] && g_vt[s->type]->enable)
            g_vt[s->type]->enable(s);
    }
}

void hooks_unload(void)
{
    anim_world_free();
    phys_world_free();
    path_free();
}

void hooks_instantiate(int root)
{
    scripts_changed();
    phys_instantiate(root);
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (s->alive && !s->awake && obj_is_descendant(s->go, root)) {
            script_create(s);
            if (obj_active(s->go)) {
                script_awake(s);
                if (s->alive && s->enabled && g_vt[s->type] && g_vt[s->type]->enable)
                    g_vt[s->type]->enable(s);
            }
        }
    }
}

void hooks_destroy(int obj)
{
    scripts_changed();
    phys_destroy(obj);
    for (int a = 0; a < g_nanim; a++)
        if (g_anim[a].alive && g_anim[a].go == obj)
            g_anim[a].alive = 0;
}

/* scripts by object (ascending script order), rebuilt lazily after loads / instantiates / destroys */
static int16_t *sc_first, *sc_next;
static int sc_cap = -1, sc_valid;
static void scripts_changed(void) { sc_valid = 0; }
static void scripts_index(void)
{
    if (sc_valid)
        return;
    if (sc_cap < W.nscript) {
        sc_cap = W.nscript + 256;
        sc_next = realloc(sc_next, sizeof(int16_t) * sc_cap);
    }
    if (!sc_first)
        sc_first = malloc(sizeof(int16_t) * MAX_OBJS);
    memset(sc_first, 0xFF, sizeof(int16_t) * MAX_OBJS);
    for (int i = W.nscript - 1; i >= 0; i--) {
        Script *s = &W.script[i];
        sc_next[i] = -1;
        if (!s->alive || s->go < 0 || s->go >= MAX_OBJS)
            continue;
        sc_next[i] = sc_first[s->go];
        sc_first[s->go] = (int16_t)i;
    }
    sc_valid = 1;
}

void hooks_enable(int obj)
{
    scripts_index();
    for (int i = sc_first[obj]; i >= 0; i = sc_next[i]) {
        Script *s = &W.script[i];
        if (!s->alive || s->go != obj)
            continue;
        script_awake(s);
        if (s->alive && s->enabled && obj_active(obj) && g_vt[s->type] && g_vt[s->type]->enable)
            g_vt[s->type]->enable(s);
    }
}

void hooks_disable(int obj)
{
    scripts_index();
    for (int i = sc_first[obj]; i >= 0; i = sc_next[i]) {
        Script *s = &W.script[i];
        if (s->alive && s->go == obj && g_vt[s->type] && g_vt[s->type]->disable)
            g_vt[s->type]->disable(s);
    }
    render_trail_clear(obj);
}

int hook_anim_new(int go, int ctrl, int update, int enabled, int bind_first)
{
    return anim_add(go, ctrl, update, enabled, bind_first);
}

/* activity changes (SetActive, animation curves): refresh and send OnEnable / OnDisable */
static uint8_t *prev_ah;
static int prev_n;
int g_prof_act, g_prof_hier;
void w_activity_changed(void)
{
    g_prof_act++;
    if (prev_n < W.nobj) {
        prev_ah = realloc(prev_ah, MAX_OBJS);
        prev_n = MAX_OBJS;
    }
    for (int i = 0; i < W.nobj; i++)
        prev_ah[i] = W.obj[i].ah;
    w_hierarchy();
    phys_dirty();
    for (int i = 0; i < W.nobj; i++) {
        if (!prev_ah[i] && W.obj[i].ah) {
            anim_on_enable(i);
            hooks_enable(i);
        } else if (prev_ah[i] && !W.obj[i].ah) {
            hooks_disable(i);
        }
    }
}

/* SetActive on one object: only its subtree can change activeInHierarchy */
void w_activity_subtree(int root)
{
    if (root < 0 || root >= W.nobj)
        return;
    g_prof_act++;
    Obj *r = &W.obj[root];
    int pa = r->parent >= 0 ? W.obj[r->parent].ah : 1;
    uint8_t ah = r->alive && r->active && pa;
    if (ah == r->ah)
        return;
    /* collect the subtree whose activeInHierarchy flips (parents before children) */
    int16_t stackbuf[256], listbuf[256];
    int16_t *stack = stackbuf, *list = listbuf;
    int cap = 256, nl = 0, ns = 0;
    stack[ns++] = (int16_t)root;
    while (ns) {
        int o = stack[--ns];
        Obj *ob = &W.obj[o];
        int p = ob->parent >= 0 ? W.obj[ob->parent].ah : 1;
        uint8_t a = ob->alive && ob->active && p;
        if (a == ob->ah)
            continue;
        ob->ah = a;
        int nch = 0;
        for (int c = w_first_child[o]; c >= 0; c = w_next_sib[c])
            nch++;
        if (nl + 1 > cap || ns + nch > cap) {
            int ncap = (cap + nch) * 2;
            int16_t *nst = malloc(sizeof(int16_t) * ncap), *nli = malloc(sizeof(int16_t) * ncap);
            memcpy(nst, stack, sizeof(int16_t) * ns);
            memcpy(nli, list, sizeof(int16_t) * nl);
            if (stack != stackbuf) { free(stack); free(list); }
            stack = nst; list = nli; cap = ncap;
        }
        list[nl++] = (int16_t)o;
        /* push children in reverse so they pop in sibling order */
        int base = ns;
        for (int c = w_first_child[o]; c >= 0; c = w_next_sib[c])
            stack[ns++] = (int16_t)c;
        for (int a0 = base, b0 = ns - 1; a0 < b0; a0++, b0--) {
            int16_t t = stack[a0]; stack[a0] = stack[b0]; stack[b0] = t;
        }
    }
    phys_dirty();
    for (int i = 0; i < nl; i++) {
        int o = list[i];
        if (W.obj[o].ah) {
            anim_on_enable(o);
            hooks_enable(o);
        } else {
            hooks_disable(o);
        }
    }
    if (stack != stackbuf) { free(stack); free(list); }
}

const void *w_anim_section(int *count);

Script *script_on(int go, int type)
{
    if (go < 0 || go >= W.nobj)
        return 0;
    Obj *o = &W.obj[go];
    if (o->script_first >= 0) {
        for (int k = 0; k < o->nscript; k++) {
            Script *s = &W.script[o->script_first + k];
            if (s->alive && s->go == go && s->type == type)
                return s;
        }
    }
    for (int i = 0; i < W.nscript; i++)
        if (W.script[i].alive && W.script[i].go == go && W.script[i].type == type)
            return &W.script[i];
    return 0;
}

Script *script_ref(Script *s, uint32_t field, int type) { return script_on(f_ref(s, field), type); }
int sr_of(int go) { return (go >= 0 && go < W.nobj) ? W.obj[go].sr : -1; }
int text_of(int go) { return (go >= 0 && go < W.nobj) ? W.obj[go].text : -1; }
int img_of(int go) { return (go >= 0 && go < W.nobj) ? W.obj[go].img : -1; }

#include <stdio.h>
#include "g_input.h"
void anim_event(int go, int fn, int iarg, float farg)
{
    g_send_event(go, fn, iarg, farg);
}

void g_send_event(int go, int fn, int iarg, float farg)
{
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (s->alive && s->go == go && g_vt[s->type] && g_vt[s->type]->event)
            g_vt[s->type]->event(s, fn, iarg, farg);
    }
}

void g_trigger_enter(int go, int other_go, int other_col)
{
    int curr = go;
    while (curr >= 0) {
        scripts_index();
        for (int i = sc_first[curr]; i >= 0; i = sc_next[i]) {
            Script *s = &W.script[i];
            if (s->alive && s->go == curr && s->enabled && g_vt[s->type] && g_vt[s->type]->trigger)
                g_vt[s->type]->trigger(s, other_go, other_col);
        }
        if (W.obj[curr].rb >= 0)
            break;
        curr = W.obj[curr].parent;
    }
}

void g_collision_enter(int go, int other_go, float rel)
{
    int curr = go;
    while (curr >= 0) {
        scripts_index();
        for (int i = sc_first[curr]; i >= 0; i = sc_next[i]) {
            Script *s = &W.script[i];
            if (s->alive && s->go == curr && s->enabled && g_vt[s->type] && g_vt[s->type]->collision)
                g_vt[s->type]->collision(s, other_go, rel);
        }
        if (W.obj[curr].rb >= 0)
            break;
        curr = W.obj[curr].parent;
    }
}

void g_scene_request(int scene)
{
#if AW_DEMO_DATA
    if (scene > 2) scene = 0;            /* chapters 3-13 are not in the demo data */
#endif
    requested_scene = scene;
}
int g_scene_pending(void) { return requested_scene; }

static int lang_pending = -1;
/* Settings > Language: takes effect on the next scene load, when no text points into the old language */
void g_lang_request(int on) { lang_pending = on; }

void g_scene_switch(void)
{
    int sc = requested_scene;
    requested_scene = -1;
    if (sc >= 0 && lang_pending >= 0) {
        w_unload();
        txt_reset();
        gfx_lang_load(0, lang_pending);
        lang_pending = -1;
    }
    if (sc >= 0)
        w_load(sc);
}

#define FIXED_DT 0.02f
static float fixed_acc;

long long g_prof[8];
#define PROF_T() (g_autotest ? (long long)sceKernelGetSystemTimeWide() : 0)
extern int g_autotest;
void g_update(float udt)
{
    long long pt = PROF_T(), pn;
#define PROF(k) do { pn = PROF_T(); g_prof[k] += pn - pt; pt = pn; } while (0)
    T.udt = udt;
    T.dt = udt * T.scale;
    T.ut += udt;
    T.t += T.dt;
    /* Unity runs Start before a script's first FixedUpdate / physics step */
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (s->started || !s->alive || !s->enabled || !g_vt[s->type] || !obj_active(s->go))
            continue;
        s->started = 1;
        if (g_vt[s->type]->start)
            g_vt[s->type]->start(s);
    }
    /* FixedUpdate + physics at 50 Hz */
    fixed_acc += T.dt;
    int steps = 0;
    while (fixed_acc >= FIXED_DT && steps < 4) {
        fixed_acc -= FIXED_DT;
        steps++;
        for (int i = 0; i < W.nscript; i++) {
            Script *s = &W.script[i];
            if (s->alive && s->enabled && g_vt[s->type] && g_vt[s->type]->fixed && obj_active(s->go))
                g_vt[s->type]->fixed(s, FIXED_DT);
        }
        PROF(0);
        phys_step(FIXED_DT);
        PROF(1);
    }
    if (steps == 4)
        fixed_acc = 0;
    phys_dirty();
    PROF(7);
    /* Start + Update */
    for (int i = 0; i < W.nscript; i++) {
        Script *s = &W.script[i];
        if (!s->alive || !s->enabled || !g_vt[s->type] || !obj_active(s->go))
            continue;
        if (!s->started) {
            s->started = 1;
            if (g_vt[s->type]->start)
                g_vt[s->type]->start(s);
            if (!s->alive)
                continue;
        }
        if (g_vt[s->type]->update)
            g_vt[s->type]->update(s, T.dt);
    }
    PROF(2);
    anim_update_all(T.dt, udt);
    PROF(3);
    anim_apply_all();
    PROF(4);
    particles_update(T.dt);
    PROF(5);
    w_tick_destroy(T.dt);
    PROF(6);
}
