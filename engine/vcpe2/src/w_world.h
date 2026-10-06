/* "Mini Unity" runtime: GameObjects, components and their data loaded from a scene blob. */
#ifndef W_WORLD_H
#define W_WORLD_H
#include <stdint.h>
#include "awfmt.h"
#include "awdata.h"

#define MAX_OBJS 4400
#define RESERVE 360          /* extra slots per component kind for prefab instances */

typedef struct {
    int16_t parent;
    uint8_t active, layer, tag, flags;
    uint8_t ah;              /* active in hierarchy (updated by w_hierarchy) */
    uint8_t alive;
    uint8_t screen;          /* under a screen-space canvas: matrix is in UI pixels (y up, centred) */
    uint8_t stamp_ok;
    int16_t rect;
    float lx, ly, lsx, lsy, lrot, rx, ry;   /* local TRS; rx/ry: euler x/y (degrees) -> projection flips */
    uint32_t name;
    int16_t sr, img, text, anim, rb, sgrp, canvas, tilemap, trail, line, part, audio;
    int16_t col_first; uint8_t ncol, nscript; int16_t script_first;
    float m[6];              /* local -> world: x' = m0 x + m1 y + m4, y' = m2 x + m3 y + m5 */
    float rw, rh, pvx, pvy;  /* UI rect size and pivot (canvas units) when rect >= 0 */
    uint32_t stamp;
    float destroy_t;         /* > 0: Object.Destroy(obj, t) pending */
} Obj;

typedef struct {
    WSr d;                   /* sprite, colour, order, layer, flags (mutable) */
    int16_t sgroup;          /* nearest sorting group object (-1) */
} Sr;

typedef struct {
    WText d;
    const char *str;         /* current text (pool string or owned copy) */
    char *own;
    int own_cap;
    float alpha_mul;         /* P_TXT_A */
} Text;

typedef struct { WCol d; int16_t enabled_in; } Col;

typedef struct {
    uint16_t type;
    int16_t go;
    uint8_t enabled, started, alive, awake;
    const WField *fields;
    uint16_t nfield;
    int16_t *remap;          /* prefab instance: prefab-local GO index -> world index */
    int remap_n;
    void *self;              /* per-type state */
} Script;

typedef struct {
    int nobj;                /* high water mark of used object slots */
    Obj obj[MAX_OBJS];
    WRect *rect; int nrect, caprect;
    Sr *sr; int nsr, capsr;
    WImg *img; int nimg, capimg;
    Text *text; int ntext, captext;
    WRb *rbd; int nrb, caprb;
    Col *col; int ncol, capcol;
    WWall *wall; int nwall;
    WTilemap *tmap; int ntmap;
    WTile *tile; int ntile;
    WSgrp *sgrp; int nsgrp, capsgrp;
    WCanvas *canvas; int ncanvas, capcanvas;
    WTrail *trail; int ntrail, captrail;
    WLine *line; int nline, capline;
    WPart *part; int npart, cappart;
    WAudio *audio; int naudio, capaudio;
    Script *script; int nscript, capscript;
    int16_t *bind; int nbind, capbind;
    WGrid grid; int has_grid;
    int scene;
    uint32_t frame;
    int camera;              /* Main Camera object */
    float cam_x, cam_y, ortho, pxu;
    float shake_x, shake_y;
} World;

extern World W;

/* load / instance */
int w_load(int scene);
void w_unload(void);
int w_instantiate(int prefab, float x, float y);    /* returns the root object */
void w_destroy(int obj, float delay);
void w_tick_destroy(float dt);

/* hierarchy */
extern int16_t w_first_child[MAX_OBJS];
extern int16_t w_next_sib[MAX_OBJS];
void w_build_children(void);
void w_hierarchy(void);                 /* refresh active-in-hierarchy + screen flags */
void obj_matrix(int o, float m[6]);     /* world matrix (recomputed when needed) */
void w_transforms(void);                /* refresh every alive object's matrix */
void obj_pos(int o, float *x, float *y);
void obj_set_pos(int o, float x, float y);  /* world position */
void obj_set_active(int o, int on);
int obj_active(int o);                  /* activeInHierarchy */
int obj_find(uint32_t name_hash);       /* first object with this name */
int obj_find_child(int parent, uint32_t name_hash);
void obj_scale(int o, float sx, float sy);   /* local scale */
void obj_set_rot(int o, float deg);         /* world/local euler z (2D: local) */
float obj_world_rot(int o);
int obj_is_descendant(int o, int ancestor);

/* components */
void sr_set_sprite(int sr, int spr);
void sr_set_color(int sr, uint32_t rgba);
void text_set(int t, const char *s);
void text_set_color(int t, uint32_t rgba);
uint32_t name_crc(const char *s);
const char *w_string(int id);

/* fields of a script (by name CRC) */
const WField *f_get(const Script *s, uint32_t name);
const WField *f_child(const Script *s, const WField *f);      /* first element of an array / struct */
float f_float(const Script *s, uint32_t name, float def);
int f_int(const Script *s, uint32_t name, int def);
int f_ref(const Script *s, uint32_t name);                    /* object index of a reference (-1) */
int f_ref_at(const Script *s, const WField *f);
int f_asset(const Script *s, uint32_t name);
int f_count(const Script *s, uint32_t name);
const WField *f_elems(const Script *s, uint32_t name, int *count);
float f_vec(const Script *s, uint32_t name, float *y);

/* text files */
const char *txt_line(int file, int line);
void txt_reset(void);
int txt_lines(int file);
void txt_load(void);

#define CRC(s) name_crc(s)
#endif
