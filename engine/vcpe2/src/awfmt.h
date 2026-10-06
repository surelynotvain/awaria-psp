/* Binary scene ("world") format written by tools/awpack.py, read by w_world.c.
 * Little endian, every record naturally aligned, sections located by tag. */
#ifndef AWFMT_H
#define AWFMT_H
#include <stdint.h>

#define WSEC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct { uint32_t tag, count, elem, bytes; } WSection;   /* followed by `bytes` of data (64-byte padded) */

/* GameObject. Component indices are -1 when absent. */
typedef struct {
    int16_t parent;          /* -1 = root */
    uint8_t active, layer;   /* activeSelf, physics layer 0..31 */
    uint8_t tag, flags;      /* tag id (TAG_*), WGO_* */
    int16_t rect;            /* RectTransform index */
    float lx, ly, lsx, lsy, lrot;   /* local position, scale, euler z (degrees) */
    uint32_t name_hash;      /* CRC32 of the name */
    int16_t sr, img, text, anim, rb, sgrp, canvas, tilemap, trail, line, part, audio;
    int16_t col_first; uint8_t ncol, nscript; int16_t script_first, pad;
} WGo;
enum { WGO_ROOTCANVAS = 1, WGO_UI = 2 };

typedef struct { int16_t go, pad; float amin_x, amin_y, amax_x, amax_y, ap_x, ap_y, sd_x, sd_y, pv_x, pv_y; } WRect;

/* SpriteRenderer */
typedef struct {
    int16_t go; uint16_t spr;     /* SPR_NONE = 0xFFFF */
    uint8_t r, g, b, a;
    int16_t order; uint8_t layer; /* sorting layer rank */
    uint8_t flags;                /* WSR_* */
} WSr;
enum { WSR_FLIPX = 1, WSR_FLIPY = 2, WSR_ENABLED = 4, WSR_WHITE = 8, WSR_ADD = 16 };

/* UI Image */
typedef struct { int16_t go; uint16_t spr; uint8_t r, g, b, a; uint8_t type, flags; uint8_t fill_method, fill_origin; float fill; } WImg;
enum { WIMG_ENABLED = 1, WIMG_PRESERVE = 2, WIMG_FILL_CW = 4 };

/* TextMeshPro (UGUI or world) */
typedef struct {
    int16_t go; uint16_t str;     /* initial text: index into the string pool (0xFFFF none) */
    uint16_t font;                /* font set index (font face + pixel size) */
    uint8_t halign, valign;       /* 0 left 1 center 2 right / 0 top 1 middle 2 bottom */
    uint8_t r, g, b, a;
    uint8_t flags, pad[3];        /* WTXT_* */
    float spacing;                /* character spacing in px */
    float line_spacing;           /* extra line spacing in px */
} WText;
enum { WTXT_ENABLED = 1, WTXT_WRAP = 2, WTXT_WORLD = 4, WTXT_UPPER = 8, WTXT_LOWER = 16 };

/* Animator: controller + target objects for every path slot of the controller */
typedef struct { int16_t go; int16_t ctrl; uint8_t update, enabled; uint16_t bind_first; } WAnim;   /* update: 0 normal, 2 unscaled */

typedef struct { int16_t go; uint8_t type, sim; float mass, drag, adrag, grav; uint8_t constraints, detect, pad[2]; } WRb;
enum { RB_DYNAMIC = 0, RB_KINEMATIC = 1, RB_STATIC = 2 };

typedef struct { int16_t go; uint8_t kind, flags; float ox, oy, a, b; uint8_t dir, pad[3]; } WCol;
enum { COL_CIRCLE = 0, COL_BOX = 1, COL_CAPSULE = 2 };
enum { WCOL_TRIGGER = 1, WCOL_ENABLED = 2 };

/* static wall boxes (tilemap colliders), world space */
typedef struct { int16_t go; uint8_t layer, flags; float x0, y0, x1, y1; } WWall;

typedef struct { int16_t go; int16_t order; uint8_t layer, enabled; uint16_t tile_first, ntile, mode; } WTilemap;   /* mode 1: Individual sort */
typedef struct { float x, y; uint16_t spr; uint8_t flags, pad; uint8_t r, g, b, a; } WTile;   /* x,y relative to the tilemap object */

typedef struct { int16_t go; int16_t order; uint8_t layer, enabled, pad[2]; } WSgrp;
typedef struct { int16_t go; uint8_t mode, layer; int16_t order; uint8_t override, pad; } WCanvas;   /* mode 0 overlay 1 camera 2 world */

typedef struct { int16_t go; uint8_t layer, enabled; int16_t order; uint8_t r0, g0, b0, a0, r1, g1, b1, a1; uint16_t pad; float time, width; } WTrail;
typedef struct { int16_t go; uint8_t layer, enabled; int16_t order; uint8_t world, npos; uint8_t r0, g0, b0, a0, r1, g1, b1, a1; float width; float pos[4][2]; } WLine;
typedef struct { int16_t go; uint8_t layer, loop; int16_t order; uint16_t spr; float len, life0, life1, speed0, speed1, size0, size1, grav, angle, radius;
                 uint8_t r, g, b, a; uint8_t nburst, awake, pad[2]; float burst_t[4]; uint16_t burst_n[4]; } WPart;
typedef struct { int16_t go; uint16_t sound; float volume; uint8_t loop, awake, music, pad; } WAudio;

/* MonoBehaviour of a game script: fields in the field pool */
typedef struct { int16_t go; uint16_t type; uint32_t field_off; uint16_t nfield; uint8_t enabled, pad; } WScript;
/* field pool: WField records; arrays point at more values in the same pool */
typedef struct { uint32_t name; uint8_t kind, sub; uint16_t count; union { int32_t i; float f; uint32_t off; } v; float v2; } WField;
enum { F_NULL, F_INT, F_FLOAT, F_STR, F_VEC, F_COLOR, F_REF, F_ARRAY, F_STRUCT, F_ASSET };
/* F_REF sub: what the reference points at (the value is the GameObject index, -1 none) */
enum { R_GO, R_TRANSFORM, R_SR, R_ANIM, R_RB, R_CIRCLE, R_BOX, R_CAPSULE, R_SCRIPT, R_TEXT, R_IMAGE, R_AUDIO, R_PART, R_TRAIL, R_LINE, R_CAMERA, R_OTHER };
/* F_ASSET sub: sprite id / sound id / prefab id / clip id */
enum { A_SPRITE, A_SOUND, A_PREFAB, A_CLIP, A_OTHER };

/* A* grid graph (AstarPath grid, 2D) */
typedef struct { float cx, cy, w, h, node, diameter; uint32_t mask; uint8_t neighbours, cut_corners, pad[2]; } WGrid;

typedef struct { uint32_t magic; uint32_t nsec; } WHeader;   /* magic 'AWW1' */
#endif
