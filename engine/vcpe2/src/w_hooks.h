/* Callbacks between the world store and the runtime modules (animators, physics, scripts). */
#ifndef W_HOOKS_H
#define W_HOOKS_H
void hooks_load(void);              /* after a scene was loaded */
void hooks_unload(void);
void hooks_instantiate(int root);   /* after a prefab instance was created */
void hooks_destroy(int obj);        /* before an object is destroyed */
void hooks_enable(int obj);         /* object became active in hierarchy (OnEnable on it and children) */
void hooks_disable(int obj);
int hook_anim_new(int go, int ctrl, int update, int enabled, int bind_first);   /* returns animator index */
#endif
