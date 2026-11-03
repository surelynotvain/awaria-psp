#ifndef W_RENDER_H
#define W_RENDER_H
#include <stdint.h>
void render_world(float dt);
void render_trail_clear(int go);
void render_push_particles(int idx, int layer, int order, float y);
/* particles (w_part.c) */
void render_particles_items(void);
void render_particles_draw(int idx, float cam_x, float cam_y, float pxu);
void particles_update(float dt);
void particles_play(int go);
void particles_stop(int go);
void particles_reset(void);
/* Effect/draw-list storage retained at the instantiated-component high water mark, in bytes. Allocation failures
 * skip only components/items that did not fit; existing histories remain valid. Main-thread counters. */
uint32_t particles_memory_bytes(void);
uint32_t render_memory_bytes(void);
/* Between frames/loading: return heap payload bytes reclaimed from unused high-water capacity, not live effects. */
uint32_t particles_trim(void);
uint32_t render_trim(void);
extern int particles_alloc_fails, render_alloc_fails;
extern int render_sort_hits, render_sorts, render_culled;
#endif
