#ifndef W_RENDER_H
#define W_RENDER_H
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
#endif
