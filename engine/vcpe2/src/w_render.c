/* Draw list: sprite renderers, tilemaps, trails, lines, particles and canvases, sorted like Unity
 * (sorting layer, order in layer, then the custom Y axis: higher y draws first). */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "w_world.h"
#include "w_render.h"
#include "aw_gfx.h"

enum { IT_SR, IT_TMAP, IT_TRAIL, IT_LINE, IT_CANVAS, IT_PART, IT_TILE };

typedef struct {
    uint8_t kind, l1, l2, pad;
    int16_t o1, o2;
    float y1, y2;
    int idx;
    int seq;
} Item;

static Item *items;
static int nitems, capitems;
static float cam_x, cam_y, pxu, art_k;

static inline float scr_x(float wx) { return SCR_W * 0.5f + (wx - cam_x) * pxu; }
static inline float scr_y(float wy) { return SCR_H * 0.5f - (wy - cam_y) * pxu; }

static void push(int kind, int l1, int o1, float y1, int l2, int o2, float y2, int idx)
{
    if (nitems >= capitems) {
        capitems = capitems ? capitems * 2 : 1024;
        items = realloc(items, capitems * sizeof(Item));
    }
    Item *it = &items[nitems];
    it->kind = kind; it->l1 = l1; it->o1 = o1; it->y1 = y1; it->l2 = l2; it->o2 = o2; it->y2 = y2;
    it->idx = idx; it->seq = nitems;
    nitems++;
}

static int cmp(const void *pa, const void *pb)
{
    const Item *a = pa, *b = pb;
    if (a->l1 != b->l1) return a->l1 - b->l1;
    if (a->o1 != b->o1) return a->o1 - b->o1;
    if (a->y1 != b->y1) return a->y1 > b->y1 ? -1 : 1;
    if (a->l2 != b->l2) return a->l2 - b->l2;
    if (a->o2 != b->o2) return a->o2 - b->o2;
    if (a->y2 != b->y2) return a->y2 > b->y2 ? -1 : 1;
    return a->seq - b->seq;
}

/* ------------------------------------------------------------------ sprites */
static void sprite_bounds_y(int spr, const float m[6], float *cy)
{
    /* Sprite Sort Point = Center: world y of the sprite bounds centre */
    const SprDef *d = &g_sprites[spr];
    float px = d->ox + d->w * 0.5f, py = d->oy + d->h * 0.5f;
    float lx = px / ART_PXU, ly = -py / ART_PXU;
    *cy = m[2] * lx + m[3] * ly + m[5];
}

static void draw_sr(int i)
{
    Sr *s = &W.sr[i];
    Obj *o = &W.obj[s->d.go];
    const float *m = o->m;
    float A = m[0] * art_k, B = -m[1] * art_k, C = -m[2] * art_k, D = m[3] * art_k;
    if (s->d.flags & WSR_FLIPX) { A = -A; C = -C; }
    if (s->d.flags & WSR_FLIPY) { B = -B; D = -D; }
    uint32_t col = RGBA(s->d.r, s->d.g, s->d.b, s->d.a);
    int fl = (s->d.flags & WSR_WHITE) ? GFXF_WHITE : 0;
    if (s->d.flags & WSR_ADD)
        fl |= GFXF_ADD;
    if (o->screen) {
        /* under a screen canvas: local units -> UI pixels through the canvas matrix */
        float k = 1.0f / ART_PXU;
        A = m[0] * k; B = -m[1] * k; C = -m[2] * k; D = m[3] * k;
        if (s->d.flags & WSR_FLIPX) { A = -A; C = -C; }
        if (s->d.flags & WSR_FLIPY) { B = -B; D = -D; }
        gfx_sprite_affine(s->d.spr, A, B, C, D, SCR_W * 0.5f + m[4], SCR_H * 0.5f - m[5], col, fl);
    } else {
        gfx_sprite_affine(s->d.spr, A, B, C, D, scr_x(m[4]), scr_y(m[5]), col, fl);
    }
}

/* ------------------------------------------------------------------ tilemaps */
static void draw_tmap_range(int i, int j0, int j1)
{
    const WTilemap *t = &W.tmap[i];
    const float *m = W.obj[t->go].m;
    float k = art_k;
    for (int j = j0; j < j1; j++) {
        const WTile *tl = &W.tile[t->tile_first + j];
        float wx = m[0] * tl->x + m[1] * tl->y + m[4];
        float wy = m[2] * tl->x + m[3] * tl->y + m[5];
        float A = k, D = k;
        if (tl->flags & 1) A = -A;
        if (tl->flags & 2) D = -D;
        gfx_sprite_tile(tl->spr, A, D, scr_x(wx), scr_y(wy), RGBA(tl->r, tl->g, tl->b, tl->a));
    }
}

static void draw_tmap(int i) { draw_tmap_range(i, 0, W.tmap[i].ntile); }
/* Individual mode: one tile, idx = tilemap << 16 | tile */
static void draw_tile(int idx) { draw_tmap_range(idx >> 16, idx & 0xFFFF, (idx & 0xFFFF) + 1); }

/* ------------------------------------------------------------------ trails (position history) */
#define TRAIL_PTS 24
typedef struct { float x[TRAIL_PTS], y[TRAIL_PTS], t[TRAIL_PTS]; int n; float clock; int active_prev; } TrailHist;
static TrailHist *trails;
static int ntrails;

void render_trail_clear(int go)
{
    for (int i = 0; i < W.ntrail; i++)
        if (W.trail[i].go == go && i < ntrails)
            trails[i].n = 0;
}

static void trail_update(float dt)
{
    if (ntrails < W.captrail) {
        trails = realloc(trails, sizeof(TrailHist) * W.captrail);
        memset(trails + ntrails, 0, sizeof(TrailHist) * (W.captrail - ntrails));
        ntrails = W.captrail;
    }
    for (int i = 0; i < W.ntrail; i++) {
        WTrail *tr = &W.trail[i];
        TrailHist *h = &trails[i];
        if (tr->go < 0 || !obj_active(tr->go)) {
            h->n = 0;
            continue;
        }
        h->clock += dt;
        float x, y;
        obj_pos(tr->go, &x, &y);
        /* drop old points */
        int keep = 0;
        for (int k = 0; k < h->n; k++) {
            if (h->clock - h->t[k] <= tr->time) {
                h->x[keep] = h->x[k]; h->y[keep] = h->y[k]; h->t[keep] = h->t[k];
                keep++;
            }
        }
        h->n = keep;
        if (h->n == 0 || fabsf(h->x[h->n - 1] - x) + fabsf(h->y[h->n - 1] - y) > 0.03f) {
            if (h->n == TRAIL_PTS) {
                memmove(h->x, h->x + 1, sizeof(float) * (TRAIL_PTS - 1));
                memmove(h->y, h->y + 1, sizeof(float) * (TRAIL_PTS - 1));
                memmove(h->t, h->t + 1, sizeof(float) * (TRAIL_PTS - 1));
                h->n--;
            }
            h->x[h->n] = x; h->y[h->n] = y; h->t[h->n] = h->clock;
            h->n++;
        }
    }
}

static uint32_t lerp_col(const uint8_t *c0, const uint8_t *c1, float f, float amul)
{
    uint8_t r = c0[0] + (c1[0] - c0[0]) * f, g = c0[1] + (c1[1] - c0[1]) * f, b = c0[2] + (c1[2] - c0[2]) * f;
    float a = (c0[3] + (c1[3] - c0[3]) * f) * amul;
    return RGBA(r, g, b, (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a));
}

static void draw_trail(int i)
{
    WTrail *tr = &W.trail[i];
    TrailHist *h = &trails[i];
    if (h->n < 2)
        return;
    float xy[TRAIL_PTS * 4];
    uint32_t col[TRAIL_PTS * 2];
    int nv = 0;
    for (int k = h->n - 1; k >= 0; k--) {
        float age = (h->clock - h->t[k]) / (tr->time > 0 ? tr->time : 1);
        float w = tr->width * (1.0f - age) * 0.5f * pxu;
        int a = k > 0 ? k - 1 : k, b = k > 0 ? k : k + 1;
        float dx = h->x[b] - h->x[a], dy = h->y[b] - h->y[a];
        float l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-5f) { dx = 1; dy = 0; l = 1; }
        float nx = -dy / l * w, ny = dx / l * w;
        float sx = scr_x(h->x[k]), sy = scr_y(h->y[k]);
        xy[nv * 2] = sx + nx; xy[nv * 2 + 1] = sy - ny;
        xy[nv * 2 + 2] = sx - nx; xy[nv * 2 + 3] = sy + ny;
        uint32_t c = lerp_col(&tr->r0, &tr->r1, age, 1.0f);
        col[nv] = c; col[nv + 1] = c;
        nv += 2;
    }
    gfx_strip(xy, col, nv, 0);
}

/* ------------------------------------------------------------------ lines */
static void draw_line(int i)
{
    const WLine *l = &W.line[i];
    if (l->npos < 2)
        return;
    const float *m = W.obj[l->go].m;
    float xy[16];
    uint32_t col[8];
    int nv = 0;
    for (int k = 0; k + 1 < l->npos && nv < 8; k++) {
        float p0x = l->pos[k][0], p0y = l->pos[k][1], p1x = l->pos[k + 1][0], p1y = l->pos[k + 1][1];
        if (!l->world) {
            float ax = m[0] * p0x + m[1] * p0y + m[4], ay = m[2] * p0x + m[3] * p0y + m[5];
            float bx = m[0] * p1x + m[1] * p1y + m[4], by = m[2] * p1x + m[3] * p1y + m[5];
            p0x = ax; p0y = ay; p1x = bx; p1y = by;
        }
        float dx = p1x - p0x, dy = p1y - p0y, len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-5f) continue;
        float w = l->width * 0.5f * pxu;
        float nx = -dy / len * w, ny = dx / len * w;
        float ax = scr_x(p0x), ay = scr_y(p0y), bx = scr_x(p1x), by = scr_y(p1y);
        xy[0] = ax + nx; xy[1] = ay - ny; xy[2] = ax - nx; xy[3] = ay + ny;
        xy[4] = bx + nx; xy[5] = by - ny; xy[6] = bx - nx; xy[7] = by + ny;
        col[0] = col[1] = RGBA(l->r0, l->g0, l->b0, l->a0);
        col[2] = col[3] = RGBA(l->r1, l->g1, l->b1, l->a1);
        gfx_strip(xy, col, 4, 0);
    }
}

/* ------------------------------------------------------------------ UI */
static int ui_world;   /* drawing a world-space canvas */

static void ui_to_screen(const float m[6], float x, float y, float *sx, float *sy)
{
    float X = m[0] * x + m[1] * y + m[4], Y = m[2] * x + m[3] * y + m[5];
    if (ui_world) {
        *sx = scr_x(X);
        *sy = scr_y(Y);
    } else {
        *sx = SCR_W * 0.5f + X;
        *sy = SCR_H * 0.5f - Y;
    }
}

static void ui_rect_screen(int o, float *x0, float *y0, float *x1, float *y1, int *mirror_x, int *mirror_y)
{
    Obj *ob = &W.obj[o];
    float lx0 = -ob->rw * ob->pvx, ly0 = -ob->rh * ob->pvy, lx1 = lx0 + ob->rw, ly1 = ly0 + ob->rh;
    float ax, ay, bx, by;
    ui_to_screen(ob->m, lx0, ly1, &ax, &ay);   /* top-left */
    ui_to_screen(ob->m, lx1, ly0, &bx, &by);   /* bottom-right */
    *mirror_x = bx < ax;
    *mirror_y = by < ay;
    *x0 = ax < bx ? ax : bx; *x1 = ax < bx ? bx : ax;
    *y0 = ay < by ? ay : by; *y1 = ay < by ? by : ay;
}

/* rotated UI Image (screen canvas): the rect is drawn through its full matrix */
static int draw_image_rotated(int o, const WImg *im, uint32_t col)
{
    const Obj *ob = &W.obj[o];
    const float *m = ob->m;
    if (ui_world || (fabsf(m[1]) < 1e-4f && fabsf(m[2]) < 1e-4f))
        return 0;
    float lx0 = -ob->rw * ob->pvx, ly0 = -ob->rh * ob->pvy, lx1 = lx0 + ob->rw, ly1 = ly0 + ob->rh;
    if (im->spr == SPR_NONE || !gfx_sprite_ok(im->spr)) {
        if (im->spr != SPR_NONE)
            return 1;
        float xy[8];
        const float lx[4] = {lx0, lx1, lx0, lx1}, ly[4] = {ly1, ly1, ly0, ly0};
        for (int k = 0; k < 4; k++)
            ui_to_screen(m, lx[k], ly[k], &xy[2 * k], &xy[2 * k + 1]);
        const uint32_t cs[4] = {col, col, col, col};
        gfx_strip(xy, cs, 4, 0);
        return 1;
    }
    /* sprite pixel (u, v), y down, rect (rx, ry, rw, rh) -> rect-local (x, y) -> screen */
    const SprDef *d = &g_sprites[im->spr];
    float srw = d->rw / 4.0f, srh = d->rh / 4.0f;
    if (srw <= 0 || srh <= 0)
        return 1;
    float au = (lx1 - lx0) / srw, bu = lx0 - (d->rx / 4.0f) * au;
    float av = -(ly1 - ly0) / srh, bv = ly1 - (d->ry / 4.0f) * av;
    float A = m[0] * au, B = m[1] * av, TX = SCR_W * 0.5f + m[0] * bu + m[1] * bv + m[4];
    float C = -m[2] * au, D = -m[3] * av, TY = SCR_H * 0.5f - (m[2] * bu + m[3] * bv + m[5]);
    gfx_sprite_affine(im->spr, A, B, C, D, TX, TY, col, 0);
    return 1;
}

static void draw_image(int o)
{
    WImg *im = &W.img[W.obj[o].img];
    if (!(im->flags & WIMG_ENABLED) || im->a == 0)
        return;
    if (im->type != 3 && !(im->flags & WIMG_PRESERVE) &&
        draw_image_rotated(o, im, RGBA(im->r, im->g, im->b, im->a)))
        return;
    float x0, y0, x1, y1;
    int mx, my;
    ui_rect_screen(o, &x0, &y0, &x1, &y1, &mx, &my);
    uint32_t col = RGBA(im->r, im->g, im->b, im->a);
    if (im->spr == SPR_NONE || !gfx_sprite_ok(im->spr)) {
        if (im->spr == SPR_NONE)
            gfx_fill(x0, y0, x1 - x0, y1 - y0, col);
        return;
    }
    if (im->type == 3 && im->fill < 1.0f) {
        /* filled: horizontal (0) / vertical (1) */
        if (im->fill <= 0) return;
        if (im->fill_method == 0) {
            float w = (x1 - x0) * im->fill;
            gfx_clip((int)floorf(im->fill_origin ? x1 - w : x0), (int)y0 - 1, (int)ceilf(w) + 1, (int)(y1 - y0) + 2);
        } else {
            float h = (y1 - y0) * im->fill;
            gfx_clip((int)x0 - 1, (int)floorf(im->fill_origin ? y0 : y1 - h), (int)(x1 - x0) + 2, (int)ceilf(h) + 1);
        }
    }
    if (im->flags & WIMG_PRESERVE) {
        const SprDef *d = &g_sprites[im->spr];
        float sw = d->rw / 4.0f, sh = d->rh / 4.0f;
        float bw = x1 - x0, bh = y1 - y0;
        if (sw > 0 && sh > 0) {
            float k = fminf(bw / sw, bh / sh);
            float nw = sw * k, nh = sh * k;
            x0 += (bw - nw) * 0.5f; y0 += (bh - nh) * 0.5f;
            x1 = x0 + nw; y1 = y0 + nh;
        }
    }
    if (mx || my) {
        /* mirrored image: draw through the affine path */
        const SprDef *d = &g_sprites[im->spr];
        float sw = d->rw / 4.0f, sh = d->rh / 4.0f;
        float kx = (x1 - x0) / sw, ky = (y1 - y0) / sh;
        float px = x0 - (d->rx / 4.0f) * kx, py = y0 - (d->ry / 4.0f) * ky;
        if (mx) { px = x1 + (d->rx / 4.0f) * kx; kx = -kx; }
        if (my) { py = y1 + (d->ry / 4.0f) * ky; ky = -ky; }
        gfx_sprite_affine(im->spr, kx, 0, 0, ky, px, py, col, 0);
    } else {
        gfx_sprite_rect(im->spr, x0, y0, x1 - x0, y1 - y0, col, 0);
    }
    if (im->type == 3 && im->fill < 1.0f)
        gfx_clip_reset();
}

static void draw_text(int o)
{
    Text *t = &W.text[W.obj[o].text];
    if (!(t->d.flags & WTXT_ENABLED) || !t->str || !*t->str)
        return;
    float a = t->d.a * t->alpha_mul;
    if (a <= 0)
        return;
    uint32_t col = RGBA(t->d.r, t->d.g, t->d.b, (uint8_t)(a > 255 ? 255 : a));
    float x0, y0, x1, y1;
    int mx, my;
    ui_rect_screen(o, &x0, &y0, &x1, &y1, &mx, &my);
    int fs = t->d.font;
    float lh = gfx_font_line(fs) + t->d.line_spacing;
    float maxw = x1 - x0;
    /* word wrap into lines */
    const char *s = t->str;
    int starts[24], lens[24], nl = 0;
    while (*s && nl < 24) {
        const char *line = s, *last_space = 0, *p = s;
        float w = 0;
        while (*p && *p != '\n') {
            const char *q = p;
            utf8_next(&q);
            float cw = gfx_text_width(fs, p, (int)(q - p), t->d.spacing);
            if ((t->d.flags & WTXT_WRAP) && w + cw > maxw + 0.5f && p > line) {
                if (last_space && last_space > line) {
                    p = last_space;
                }
                break;
            }
            if (*p == ' ')
                last_space = p;
            w += cw;
            p = q;
        }
        starts[nl] = (int)(line - t->str);
        lens[nl] = (int)(p - line);
        nl++;
        if (*p == '\n' || *p == ' ')
            p++;
        s = p;
    }
    float total = nl * lh;
    float y;
    if (t->d.valign == 0) y = y0;
    else if (t->d.valign == 1) y = (y0 + y1) * 0.5f - total * 0.5f;
    else if (t->d.valign == 3) y = (y0 + y1) * 0.5f - gfx_font_ascent(fs);           /* TMP Baseline */
    else if (t->d.valign == 4) y = (y0 + y1) * 0.5f - gfx_font_ascent(fs) * 0.5f;    /* TMP Capline */
    else y = y1 - total;
    for (int i = 0; i < nl; i++) {
        const char *ls = t->str + starts[i];
        int len = lens[i];
        while (len > 0 && ls[len - 1] == ' ') len--;
        float w = gfx_text_width(fs, ls, len, t->d.spacing);
        /* a mirrored rect (negative x scale) swaps left / right alignment and flips the glyphs */
        int mirror = mx && !my;   /* both flipped = 180 degree turn: keep it readable */
        int ha = mirror && t->d.halign != 1 ? 2 - t->d.halign : t->d.halign;
        float x = ha == 0 ? x0 : ha == 1 ? (x0 + x1) * 0.5f - w * 0.5f : x1 - w;
        gfx_text_mirror(fs, x, y, ls, len, col, t->d.spacing, mirror);
        y += lh;
    }
}

/* current UI clip (screen pixels) for nested Masks */
static int clip_x0, clip_y0 = 0, clip_x1 = SCR_W, clip_y1 = SCR_H;

static void draw_ui_fast(int o, int root_canvas)
{
    Obj *ob = &W.obj[o];
    if (!ob->alive || !ob->ah)
        return;
    if (o != root_canvas && ob->canvas >= 0 && W.canvas[ob->canvas].override)
        return;
    if (ob->flags & 4) {
        /* UI Mask: draw self and children clipped to this rect (scissor; masks here are rectangles) */
        float x0, y0, x1, y1;
        int mx, my;
        ui_rect_screen(o, &x0, &y0, &x1, &y1, &mx, &my);
        int px0 = clip_x0, py0 = clip_y0, px1 = clip_x1, py1 = clip_y1;
        int nx0 = (int)floorf(x0 + 0.5f), ny0 = (int)floorf(y0 + 0.5f), nx1 = (int)floorf(x1 + 0.5f), ny1 = (int)floorf(y1 + 0.5f);
        clip_x0 = nx0 > px0 ? nx0 : px0; clip_y0 = ny0 > py0 ? ny0 : py0;
        clip_x1 = nx1 < px1 ? nx1 : px1; clip_y1 = ny1 < py1 ? ny1 : py1;
        if (clip_x1 > clip_x0 && clip_y1 > clip_y0) {
            gfx_clip(clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
            ob->flags &= ~4;
            uint8_t hide = ob->flags & 8;
            int img = ob->img;
            if (hide) ob->img = -1;
            draw_ui_fast(o, root_canvas);
            ob->img = img;
            ob->flags |= 4;
        }
        clip_x0 = px0; clip_y0 = py0; clip_x1 = px1; clip_y1 = py1;
        if (clip_x0 == 0 && clip_y0 == 0 && clip_x1 == SCR_W && clip_y1 == SCR_H)
            gfx_clip_reset();
        else
            gfx_clip(clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
        return;
    }
    if (ob->img >= 0)
        draw_image(o);
    if (ob->text >= 0)
        draw_text(o);
    if (ob->sr >= 0 && ob->screen) {
        Sr *s = &W.sr[ob->sr];
        if ((s->d.flags & WSR_ENABLED) && s->d.spr != SPR_NONE && s->d.a)
            draw_sr(ob->sr);
    }
    for (int c = w_first_child[o]; c >= 0; c = w_next_sib[c])
        draw_ui_fast(c, root_canvas);
}

static void draw_canvas(int i)
{
    int go = W.canvas[i].go;
    ui_world = W.canvas[i].mode == 2;
    draw_ui_fast(go, go);
    ui_world = 0;
}

/* ------------------------------------------------------------------ frame */
void render_world(float dt)
{
    Obj *cam = W.camera >= 0 ? &W.obj[W.camera] : 0;
    cam_x = cam ? cam->m[4] : 0;
    cam_y = cam ? cam->m[5] : 0;
    cam_x += W.shake_x;
    cam_y += W.shake_y;
    pxu = W.pxu;
    art_k = pxu / ART_PXU;
    trail_update(dt);
    nitems = 0;
    for (int i = 0; i < W.nsr; i++) {
        Sr *s = &W.sr[i];
        if (s->d.go < 0 || !(s->d.flags & WSR_ENABLED) || s->d.spr == SPR_NONE || !s->d.a)
            continue;
        Obj *o = &W.obj[s->d.go];
        if (!o->ah || o->screen)
            continue;
        float y;
        sprite_bounds_y(s->d.spr, o->m, &y);
        if (s->sgroup >= 0 && W.obj[s->sgroup].sgrp >= 0 && W.sgrp[W.obj[s->sgroup].sgrp].enabled) {
            const WSgrp *g = &W.sgrp[W.obj[s->sgroup].sgrp];
            push(IT_SR, g->layer, g->order, W.obj[s->sgroup].m[5], s->d.layer, s->d.order, y, i);
        } else {
            push(IT_SR, s->d.layer, s->d.order, y, 0, 0, 0, i);
        }
    }
    for (int i = 0; i < W.ntmap; i++) {
        const WTilemap *t = &W.tmap[i];
        if (!t->enabled || !obj_active(t->go))
            continue;
        if (t->mode == 1) {
            /* TilemapRenderer Individual: every tile sorts on its own position (custom axis Y) */
            const float *m = W.obj[t->go].m;
            for (int j = 0; j < t->ntile; j++) {
                const WTile *tl = &W.tile[t->tile_first + j];
                push(IT_TILE, t->layer, t->order, m[2] * tl->x + m[3] * tl->y + m[5], 0, 0, 0, i << 16 | j);
            }
            continue;
        }
        push(IT_TMAP, t->layer, t->order, W.obj[t->go].m[5], 0, 0, 0, i);
    }
    for (int i = 0; i < W.ntrail; i++) {
        WTrail *t = &W.trail[i];
        if (t->go < 0 || !t->enabled || !obj_active(t->go))
            continue;
        push(IT_TRAIL, t->layer, t->order, W.obj[t->go].m[5], 0, 0, 0, i);
    }
    for (int i = 0; i < W.nline; i++) {
        WLine *l = &W.line[i];
        if (l->go < 0 || !l->enabled || !obj_active(l->go))
            continue;
        push(IT_LINE, l->layer, l->order, W.obj[l->go].m[5], 0, 0, 0, i);
    }
    for (int i = 0; i < W.ncanvas; i++) {
        WCanvas *c = &W.canvas[i];
        if (c->go < 0 || !obj_active(c->go))
            continue;
        Obj *o = &W.obj[c->go];
        int root = o->parent < 0;
        if (!root) {
            /* nested canvases draw with their parent unless they override sorting */
            int p = o->parent, nested = 0;
            for (; p >= 0; p = W.obj[p].parent)
                if (W.obj[p].canvas >= 0) { nested = 1; break; }
            if (nested && !c->override)
                continue;
        }
        push(IT_CANVAS, c->layer, c->order, c->mode == 2 ? o->m[5] : 1e9f, 0, 0, 0, i);
    }
    render_particles_items();
    qsort(items, nitems, sizeof(Item), cmp);
    for (int i = 0; i < nitems; i++) {
        Item *it = &items[i];
        switch (it->kind) {
        case IT_SR: draw_sr(it->idx); break;
        case IT_TMAP: draw_tmap(it->idx); break;
        case IT_TILE: draw_tile(it->idx); break;
        case IT_TRAIL: draw_trail(it->idx); break;
        case IT_LINE: draw_line(it->idx); break;
        case IT_CANVAS: draw_canvas(it->idx); break;
        case IT_PART: render_particles_draw(it->idx, cam_x, cam_y, pxu); break;
        }
    }
}

void render_push_particles(int idx, int layer, int order, float y)
{
    push(IT_PART, layer, order, y, 0, 0, 0, idx);
}
