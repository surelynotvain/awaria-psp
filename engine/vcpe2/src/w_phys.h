/* Physics2D subset: Rigidbody2D integration, circle/box/capsule colliders, wall boxes, layer matrix,
 * trigger / collision enter events, OverlapCircle / Raycast / CircleCast. */
#ifndef W_PHYS_H
#define W_PHYS_H

void phys_world_init(void);
void phys_world_free(void);
void phys_instantiate(int root);
void phys_destroy(int obj);
void phys_step(float dt);
void phys_forget_pairs(int col);
void phys_dirty(void);              /* invalidate the cached shape list (queries) */

/* bodies (by object) */
int phys_body(int go);
void phys_velocity(int go, float *vx, float *vy);
void phys_set_velocity(int go, float vx, float vy);
void phys_add_force(int go, float fx, float fy);
void phys_add_impulse(int go, float ix, float iy);
void phys_add_torque(int go, float t);
void phys_set_position(int go, float x, float y);   /* rb.position / transform.position */

/* colliders */
void col_set_enabled(int col, int on);
int col_enabled(int col);
int col_of(int go, int kind);           /* first collider of a kind on the object (-1) */
void col_set_radius(int col, float r);
float col_radius(int col);

/* queries; layer masks are Unity bit masks (-1 = everything). Return the collider's object or -1. */
int phys_overlap_circle(float x, float y, float r, int mask, int *col_out);
int phys_raycast(float x, float y, float dx, float dy, float dist, int mask, float *hx, float *hy, int *col_out);
int phys_circlecast(float x, float y, float r, float dx, float dy, float dist, int mask, int *col_out);
#endif
