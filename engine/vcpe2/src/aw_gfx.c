/* GU renderer: paletted (CLUT) swizzled texture pages streamed from AW.PAK. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "aw_gfx.h"
#include "vcpe.h"
#include "awdata.h"



/* language data (LANG.PAK or the built-in English copy in AW.PAK): fonts, text files, scene string translations */
typedef struct {
    char magic[4];
    uint32_t version;
    uint16_t nfontsets, npages;
    uint32_t nglyphs, fontsets, glyphs, pages, clut, ntext, text, nstatic, statics, kit;
    char name[28];
} LangHdr;
static uint8_t *lang;
static const FontSet *fsets;
static const Glyph *glyphs;
static const FontPage *fpages;
static const uint8_t *font_mem, *fclut;
static uint16_t spr_first[NUM_SPRITES], spr_n[NUM_SPRITES];
static int cur_scene = -1;



int gfx_init(void)
{
    vcpe_gfx_tables((const VcpePage *)g_pages, (const VcpeBundle *)g_bundles, NUM_BUNDLES);
    return vcpe_gfx_init();
}

void gfx_shutdown(void) { vcpe_gfx_shutdown(); }

void gfx_begin(uint32_t clear) { vcpe_gfx_begin(clear); }

void gfx_end(void) { vcpe_gfx_end(); }




int gfx_pak_open(const char *path)
{
    int fd = vcpe_pak_open(path);
    if (fd < 0)
        return fd;
    return vcpe_bundle_load(0);
}

static int lang_ok(const uint8_t *m, uint32_t size)
{
    const LangHdr *h = (const LangHdr *)m;
    return m && size >= sizeof(LangHdr) && !memcmp(h->magic, "AWLG", 4) && h->version == 1 &&
           h->kit == LANG_KIT && h->nfontsets == NUM_FONTSETS && h->ntext == NUM_TEXTFILES && h->statics <= size && h->clut + 1024 <= size;
}

/* LANG.PAK next to the EBOOT wins (a translation); anything missing or broken falls back to built-in English */
static char lang_path[300], lang_file[28];
static int lang_file_ok = -1;

int gfx_lang_load(const char *path, int use_file)
{
    uint8_t *m = 0;
    uint32_t size = 0;
    if (path)
        snprintf(lang_path, sizeof lang_path, "%s", path);
    SceUID fd = sceIoOpen(lang_path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int n = sceIoLseek32(fd, 0, PSP_SEEK_END);
        sceIoLseek32(fd, 0, PSP_SEEK_SET);
        if (n > 0 && (m = memalign(64, n))) {
            int got = 0, r;
            while (got < n && (r = sceIoRead(fd, m + got, n - got)) > 0)
                got += r;
            size = got;
            if (got != n || !lang_ok(m, size)) {
                free(m);
                m = 0;
            }
        }
        sceIoClose(fd);
    }
    lang_file_ok = m != 0;
    if (m)
        snprintf(lang_file, sizeof lang_file, "%s", ((const LangHdr *)m)->name);
    if (m && !use_file) {
        free(m);
        m = 0;
    }
    if (!m) {
        size = LANG_SIZE;
        m = vcpe_pak_read_alloc(LANG_OFF, LANG_SIZE);
        if (!lang_ok(m, size))
            return -1;
    }
    sceKernelDcacheWritebackRange(m, size);
    free(lang);
    lang = m;
    const LangHdr *h = (const LangHdr *)m;
    fsets = (const FontSet *)(m + h->fontsets);
    glyphs = (const Glyph *)(m + h->glyphs);
    fpages = (const FontPage *)(m + h->pages);
    fclut = m + h->clut;
    font_mem = m;
    return 0;
}

const char *lang_name(void) { return lang ? ((const LangHdr *)lang)->name : ""; }
/* the language in LANG.PAK (also while it is switched off), or 0 when there is no usable LANG.PAK */
const char *lang_file_name(void) { return lang_file_ok > 0 ? lang_file : 0; }

/* text file f of the language: '\0' separated lines */
const char *lang_text(int f, uint32_t *size, uint32_t *nlines)
{
    const LangHdr *h = (const LangHdr *)lang;
    if (!lang || f < 0 || (uint32_t)f >= h->ntext)
        return 0;
    const uint32_t *t = (const uint32_t *)(lang + h->text) + 3 * f;
    *size = t[1];
    *nlines = t[2];
    return (const char *)lang + t[0];
}

/* translation of a string baked into a scene, or the string itself */
const char *lang_static(const char *orig)
{
    const LangHdr *h = (const LangHdr *)lang;
    if (!lang || !h->nstatic || !orig || !*orig)
        return orig;
    extern uint32_t name_crc(const char *s);
    uint32_t crc = name_crc(orig);
    const uint32_t *e = (const uint32_t *)(lang + h->statics);
    int lo = 0, hi = (int)h->nstatic - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (e[2 * mid] == crc)
            return (const char *)lang + e[2 * mid + 1];
        if (e[2 * mid] < crc) lo = mid + 1;
        else hi = mid - 1;
    }
    return orig;
}

static void locs_apply(int bundle)
{
    const LocRange *r = &g_bundle_locs[bundle];
    for (int i = 0; i < r->n; i++) {
        const SprLoc *l = &g_spr_locs[r->first + i];
        spr_first[l->spr] = l->first;
        spr_n[l->spr] = l->n;
    }
}

int gfx_scene_textures(int scene)
{
    if (scene == cur_scene)
        return 0;
    sceGuSync(0, 0);
    for (int b = 1; b < NUM_BUNDLES; b++)
        if (vcpe_bundle_loaded(b) && b != 1 + scene)
            vcpe_bundle_unload(b);
    memset(spr_n, 0, sizeof spr_n);
    locs_apply(0);
    int b = 1 + scene;
    if (b < NUM_BUNDLES)
        vcpe_bundle_load(b);
    if (b < NUM_BUNDLES && vcpe_bundle_loaded(b))
        locs_apply(b);
    vcpe_unbind();
    cur_scene = scene;
    return (b < NUM_BUNDLES && !vcpe_bundle_loaded(b)) ? -1 : 0;
}

int gfx_sprite_ok(int spr) { return spr >= 0 && spr < NUM_SPRITES && spr_n[spr] > 0; }

/* blend + "white" (Unity Font Material) modes */
static void set_mode(int flags)
{
    vcpe_blend((flags & GFXF_ADD) ? 1 : 0);
    vcpe_tex_white((flags & GFXF_WHITE) ? 1 : 0);
}

void gfx_sprite_affine(int spr, float A, float B, float C, float D, float TX, float TY, uint32_t color, int flags)
{
    if (!gfx_sprite_ok(spr) || (color >> 24) == 0)
        return;
    const SprDef *d = &g_sprites[spr];
    /* reject far off-screen sprites early */
    float ext = (d->w + d->h + 4) * (fabsf(A) + fabsf(B) + fabsf(C) + fabsf(D));
    if (TX < -ext || TX > SCR_W + ext || TY < -ext || TY > SCR_H + ext)
        return;
    set_mode(flags);
    int axis = (B == 0.0f && C == 0.0f);
    int crisp = axis && fabsf(fabsf(A) - 1.0f) < 1e-4f && fabsf(fabsf(D) - 1.0f) < 1e-4f;
    if (crisp) {
        TX = floorf(TX + 0.5f);
        TY = floorf(TY + 0.5f);
    }
    vcpe_filter(crisp);
    int first = spr_first[spr], n = spr_n[spr];
    for (int i = 0; i < n; i++) {
        const SprPiece *p = &g_pieces[first + i];
        if (!vcpe_bind_page(p->page))
            return;
        float lx0 = d->ox + p->dx, ly0 = d->oy + p->dy, lx1 = lx0 + p->w, ly1 = ly0 + p->h;
        float u0 = p->u, v0 = p->v, u1 = p->u + p->w, v1 = p->v + p->h;
        if (axis) {
            vcpe_quad(A * lx0 + TX, D * ly0 + TY, A * lx1 + TX, D * ly1 + TY, u0, v0, u1, v1, color);
            continue;
        }
        vcpe_stat_quads++;
        vcpe_stat_area += fabsf(A * D - B * C) * p->w * p->h;
        VcpeVtx *v = sceGuGetMemory(4 * sizeof(VcpeVtx));
        float px[4] = {lx0, lx1, lx0, lx1}, py[4] = {ly0, ly0, ly1, ly1};
        float uu[4] = {u0, u1, u0, u1}, vv[4] = {v0, v0, v1, v1};
        for (int k = 0; k < 4; k++) {
            v[k].u = uu[k]; v[k].v = vv[k]; v[k].color = color; v[k].z = 0;
            v[k].x = A * px[k] + B * py[k] + TX;
            v[k].y = C * px[k] + D * py[k] + TY;
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, 0, v);
    }
}

/* Tilemap tile: only the sprite's own rect is drawn, at its exact (fractional) size and position,
   so neighbouring tiles share their edges instead of overlapping by a soft fringe. */
void gfx_sprite_tile(int spr, float A, float D, float TX, float TY, uint32_t color)
{
    if (!gfx_sprite_ok(spr) || (color >> 24) == 0)
        return;
    const SprDef *d = &g_sprites[spr];
    float rx0 = d->rx / 4.0f, ry0 = d->ry / 4.0f, rx1 = rx0 + d->rw / 4.0f, ry1 = ry0 + d->rh / 4.0f;
    if (rx1 <= rx0 || ry1 <= ry0)
        return;
    float ext = (d->w + d->h + 4) * (fabsf(A) + fabsf(D));
    if (TX < -ext || TX > SCR_W + ext || TY < -ext || TY > SCR_H + ext)
        return;
    set_mode(0);
    vcpe_filter(0);
    int first = spr_first[spr], n = spr_n[spr];
    for (int i = 0; i < n; i++) {
        const SprPiece *p = &g_pieces[first + i];
        float px0 = d->ox + p->dx, py0 = d->oy + p->dy;
        float x0 = px0 > rx0 ? px0 : rx0, x1 = px0 + p->w < rx1 ? px0 + p->w : rx1;
        float y0 = py0 > ry0 ? py0 : ry0, y1 = py0 + p->h < ry1 ? py0 + p->h : ry1;
        if (x1 <= x0 || y1 <= y0)
            continue;
        if (!vcpe_bind_page(p->page))
            return;
        vcpe_quad(TX + A * x0, TY + D * y0, TX + A * x1, TY + D * y1,
                p->u + (x0 - px0), p->v + (y0 - py0), p->u + (x1 - px0), p->v + (y1 - py0), color);
    }
}

void gfx_sprite_rect(int spr, float x, float y, float w, float h, uint32_t color, int flags)
{
    if (!gfx_sprite_ok(spr) || (color >> 24) == 0)
        return;
    const SprDef *d = &g_sprites[spr];
    float rw = d->rw / 4.0f, rh = d->rh / 4.0f;
    if (rw <= 0 || rh <= 0)
        return;
    float kx = w / rw, ky = h / rh;
    float bx = d->ox - d->rx / 4.0f, by = d->oy - d->ry / 4.0f;
    set_mode(flags);
    vcpe_filter(fabsf(kx - 1.0f) < 0.02f && fabsf(ky - 1.0f) < 0.02f);
    int first = spr_first[spr], n = spr_n[spr];
    for (int i = 0; i < n; i++) {
        const SprPiece *p = &g_pieces[first + i];
        if (!vcpe_bind_page(p->page))
            return;
        vcpe_quad(x + (bx + p->dx) * kx, y + (by + p->dy) * ky, x + (bx + p->dx + p->w) * kx, y + (by + p->dy + p->h) * ky,
                p->u, p->v, p->u + p->w, p->v + p->h, color);
    }
}

void gfx_fill(float x, float y, float w, float h, uint32_t color) { vcpe_fill(x, y, w, h, color); }

void gfx_strip(const float *xy, const uint32_t *colors, int n, int additive) { vcpe_strip(xy, colors, n, additive); }

void gfx_clip(int x, int y, int w, int h) { vcpe_clip(x, y, w, h); }

void gfx_clip_reset(void) { vcpe_clip_reset(); }

/* ------------------------------------------------------------------ text */
uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    if (c >= 0xF0) {
        c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63);
        s += 3;
    } else if (c >= 0xE0) {
        c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63);
        s += 2;
    } else if (c >= 0xC0) {
        c = ((c & 31) << 6) | (s[0] & 63);
        s += 1;
    }
    *ps = (const char *)s;
    return c;
}

static const Glyph *glyph(int fset, uint32_t cp)
{
    const FontSet *f = &fsets[fset];
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const Glyph *g = &glyphs[f->first + mid];
        if (g->cp == cp)
            return g;
        if (g->cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (cp != '?' && cp > ' ')
        return glyph(fset, '?');
    return 0;
}

float gfx_font_line(int fset) { return fsets ? fsets[fset].line / 16.0f : 0; }
float gfx_font_ascent(int fset) { return fsets ? fsets[fset].ascent / 16.0f : 0; }

float gfx_text_width(int fset, const char *s, int len, float spacing)
{
    float w = 0;
    const char *end = s + len;
    while (s < end && *s) {
        const Glyph *g = glyph(fset, utf8_next(&s));
        if (g)
            w += g->adv / 16.0f + spacing;
    }
    return w;
}

float gfx_text(int fset, float x, float y, const char *s, int len, uint32_t color, float spacing)
{
    return gfx_text_mirror(fset, x, y, s, len, color, spacing, 0);
}

/* mirror: draw the run flipped horizontally inside its own box (UI under a negative x scale) */
float gfx_text_mirror(int fset, float x, float y, const char *s, int len, uint32_t color, float spacing, int mirror)
{
    if (!s || !font_mem || (color >> 24) == 0)
        return 0;
    float flip = mirror ? 2.0f * floorf(x + 0.5f) + gfx_text_width(fset, s, len, spacing) : 0;
    const char *end = s + len;
    float x0 = x;
    x = floorf(x + 0.5f);
    float baseline = floorf(y + gfx_font_ascent(fset) + 0.5f);
    set_mode(0);
    vcpe_filter(1);
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, fclut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
    int cur = -1;
    vcpe_unbind();
    while (s < end && *s) {
        const Glyph *g = glyph(fset, utf8_next(&s));
        if (!g)
            continue;
        if (g->w) {
            if (g->page != cur) {
                const FontPage *fp = &fpages[g->page];
                sceGuTexImage(0, fp->w, fp->h, fp->w, font_mem + fp->off);
                sceGuTexFlush();
                cur = g->page;
            }
            float gx = floorf(x + g->bx / 16.0f + 0.5f), gy = baseline + floorf(g->by / 16.0f + 0.5f);
            if (mirror)
                vcpe_quad(flip - gx, gy, flip - gx - g->w, gy + g->h, g->u, g->v, g->u + g->w, g->v + g->h, color);
            else
                vcpe_quad(gx, gy, gx + g->w, gy + g->h, g->u, g->v, g->u + g->w, g->v + g->h, color);
        }
        x += g->adv / 16.0f + spacing;
    }
    return x - x0;
}
