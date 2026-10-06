/* GU renderer for the Awaria port: paletted swizzled pages from AW.PAK, affine sprites, glyph text. */
#ifndef AW_GFX_H
#define AW_GFX_H
#include <stdint.h>
#include "vcpe.h"

#define SCR_W 480
#define SCR_H 272
/* sprites were rasterised at this many PSP pixels per Unity unit (ortho 6 camera over 272 lines) */
#define ART_PXU (272.0f / 12.0f)

#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define WHITE RGBA(255, 255, 255, 255)

enum { BLEND_ALPHA = 0, BLEND_ADD = 1 };

int gfx_init(void);
void gfx_shutdown(void);
void gfx_begin(uint32_t clear);
void gfx_end(void);

int gfx_pak_open(const char *path);
/* language: LANG.PAK at path if valid, else built-in English (fonts + text files + scene string translations) */
int gfx_lang_load(const char *path, int use_file);   /* path 0: the one from the last call */
const char *lang_name(void);
const char *lang_file_name(void);
const char *lang_text(int f, uint32_t *size, uint32_t *nlines);
const char *lang_static(const char *orig);

/* textures: bundle 0 is resident, scene n uses bundle 1 + n */
int gfx_scene_textures(int scene);
int gfx_sprite_ok(int spr);

/* Draw sprite pixels (pivot-relative, y down) through screen = A*px + B*py + TX, C*px + D*py + TY.
 * flags: GFXF_WHITE silhouette, GFXF_ADD additive. */
enum { GFXF_WHITE = 1, GFXF_ADD = 2 };
void gfx_sprite_affine(int spr, float A, float B, float C, float D, float TX, float TY, uint32_t color, int flags);
/* Tilemap tile: like gfx_sprite_affine (axis aligned) but clipped to the sprite rect, unsnapped, no seams */
void gfx_sprite_tile(int spr, float A, float D, float TX, float TY, uint32_t color);
/* UI Image: the sprite's full rect stretched into the screen rectangle (x, y, w, h), optional fill (0..1, from left) */
void gfx_sprite_rect(int spr, float x, float y, float w, float h, uint32_t color, int flags);
void gfx_fill(float x, float y, float w, float h, uint32_t color);
/* untextured triangle strip (screen xy pairs, one colour per vertex) */
void gfx_strip(const float *xy, const uint32_t *colors, int n, int additive);
void gfx_clip(int x, int y, int w, int h);
void gfx_clip_reset(void);

/* text (UTF-8) with a font set; returns width in px. x,y = pen start on the line top. */
float gfx_text(int fset, float x, float y, const char *s, int len, uint32_t color, float spacing);
float gfx_text_mirror(int fset, float x, float y, const char *s, int len, uint32_t color, float spacing, int mirror);
float gfx_text_width(int fset, const char *s, int len, float spacing);
float gfx_font_line(int fset);
float gfx_font_ascent(int fset);
uint32_t utf8_next(const char **ps);

#endif
