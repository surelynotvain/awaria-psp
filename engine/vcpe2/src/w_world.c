#include <stdio.h>
/* Scene blobs -> runtime arrays; transforms, activity, prefab instances, script fields. */
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include "w_world.h"
#include "w_anim.h"
#include "aw_gfx.h"
#include "w_hooks.h"

World W;

/* ------------------------------------------------------------------ CRC32 */
static uint32_t crc_tab[256];
uint32_t name_crc(const char *s)
{
    if (!crc_tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_tab[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        c = crc_tab[(c ^ *p) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ strings and text files */
static uint8_t *str_mem;
static uint32_t str_count;
const char *w_string(int id)
{
    if (!str_mem || id < 0 || (uint32_t)id >= str_count)
        return "";
    const uint32_t *idx = (const uint32_t *)(str_mem + 4);
    return (const char *)(str_mem + 4 + 4 * str_count + idx[id]);
}

static const char **txt_idx[32];
static int txt_n[32];
void txt_load(void)
{
    if (!str_mem) {
        str_mem = vcpe_pak_read_alloc(STR_OFF, STR_SIZE);
        if (str_mem)
            str_count = *(uint32_t *)str_mem;
    }
    for (int f = 0; f < NUM_TEXTFILES && f < 32; f++) {
        if (txt_idx[f])
            continue;
        uint32_t size, nlines;
        const char *p = lang_text(f, &size, &nlines);
        if (!p)
            continue;
        txt_idx[f] = malloc(sizeof(char *) * (nlines + 1));
        for (uint32_t i = 0; i < nlines; i++) {
            txt_idx[f][i] = p;
            p += strlen(p) + 1;
        }
        txt_n[f] = nlines;
    }
}
/* after a language switch: forget the line tables (they point into the old language data) */
void txt_reset(void)
{
    for (int f = 0; f < 32; f++) {
        free(txt_idx[f]);
        txt_idx[f] = 0;
        txt_n[f] = 0;
    }
}

const char *txt_line(int file, int line)
{
    if (file < 0 || file >= 32 || !txt_idx[file] || line < 0 || line >= txt_n[file])
        return "";
    return txt_idx[file][line];
}
int txt_lines(int file) { return (file >= 0 && file < 32) ? txt_n[file] : 0; }

/* ------------------------------------------------------------------ blobs */
typedef struct {
    uint8_t *mem;
    const WSection *sec[24];
    uint32_t tags[24];
    int nsec;
} Blob;

static const void *blob_sec(const Blob *b, uint32_t tag, int *count)
{
    for (int i = 0; i < b->nsec; i++) {
        if (b->tags[i] == tag) {
            if (count)
                *count = b->sec[i]->count;
            return (const uint8_t *)b->sec[i] + sizeof(WSection);
        }
    }
    if (count)
        *count = 0;
    return 0;
}

static int blob_load(Blob *b, int index)
{
    memset(b, 0, sizeof *b);
    const BlobDef *d = &g_worlds[index];
    b->mem = vcpe_pak_read_alloc(d->off, d->size);
    if (!b->mem)
        return -1;
    const WHeader *h = (const WHeader *)b->mem;
    uint32_t off = 64;
    for (uint32_t i = 0; i < h->nsec && i < 24; i++) {
        const WSection *s = (const WSection *)(b->mem + off);
        b->sec[b->nsec] = s;
        b->tags[b->nsec] = s->tag;
        b->nsec++;
        off += sizeof(WSection) + s->bytes;
        off = (off + 63) & ~63u;
    }
    return 0;
}

static Blob scene_blob;
static Blob prefab_blob[NUM_PREFABS];

#define TAG(a, b, c, d) WSEC(a, b, c, d)

static void *dup_sec(const Blob *b, uint32_t tag, size_t elem, int *n, int *cap, int reserve)
{
    int cnt;
    const void *src = blob_sec(b, tag, &cnt);
    int c = cnt + reserve;
    void *mem = calloc(c ? c : 1, elem);
    if (src && cnt)
        memcpy(mem, src, elem * cnt);
    *n = cnt;
    if (cap)
        *cap = c;
    return mem;
}

static void free_world(void)
{
    free(W.rect); free(W.sr); free(W.img);
    for (int i = 0; i < W.ntext; i++)
        free(W.text[i].own);
    free(W.text); free(W.rbd); free(W.col); free(W.wall); free(W.tmap); free(W.tile); free(W.sgrp); free(W.canvas);
    free(W.trail); free(W.line); free(W.part); free(W.audio);
    for (int i = 0; i < W.nscript; i++) {
        free(W.script[i].self);
        free(W.script[i].remap);
    }
    free(W.script); free(W.bind);
    if (scene_blob.mem)
        free(scene_blob.mem);
    memset(&scene_blob, 0, sizeof scene_blob);
}

int16_t w_first_child[MAX_OBJS];
int16_t w_next_sib[MAX_OBJS];

void w_build_children(void)
{
    memset(w_first_child, 0xFF, sizeof(int16_t) * (W.nobj > 0 ? W.nobj : MAX_OBJS));
    memset(w_next_sib, 0xFF, sizeof(int16_t) * (W.nobj > 0 ? W.nobj : MAX_OBJS));
    for (int i = W.nobj - 1; i >= 0; i--) {
        if (!W.obj[i].alive) continue;
        int p = W.obj[i].parent;
        if (p >= 0 && p < W.nobj) {
            w_next_sib[i] = w_first_child[p];
            w_first_child[p] = (int16_t)i;
        }
    }
}

void w_unload(void)
{
    hooks_unload();
    free_world();
    memset(&W, 0, sizeof W);
    memset(w_first_child, 0xFF, sizeof w_first_child);
    memset(w_next_sib, 0xFF, sizeof w_next_sib);
}

static void obj_from(Obj *o, const WGo *g)
{
    memset(o, 0, sizeof *o);
    o->parent = g->parent;
    o->active = g->active; o->layer = g->layer; o->tag = g->tag; o->flags = g->flags;
    o->rect = g->rect;
    o->lx = g->lx; o->ly = g->ly; o->lsx = g->lsx; o->lsy = g->lsy; o->lrot = g->lrot;
    o->name = g->name_hash;
    o->sr = g->sr; o->img = g->img; o->text = g->text; o->anim = g->anim; o->rb = g->rb; o->sgrp = g->sgrp;
    o->canvas = g->canvas; o->tilemap = g->tilemap; o->trail = g->trail; o->line = g->line; o->part = g->part; o->audio = g->audio;
    o->col_first = g->col_first; o->ncol = g->ncol; o->nscript = g->nscript; o->script_first = g->script_first;
    o->alive = 1;
}

static void text_init(Text *t)
{
    t->str = t->d.str == 0xFFFF ? "" : lang_static(w_string(t->d.str));
    t->own = 0;
    t->own_cap = 0;
    t->alpha_mul = 1.0f;
    if ((t->d.flags & (WTXT_UPPER | WTXT_LOWER)) && *t->str)
        text_set((int)(t - W.text), t->str);
}

int w_load(int scene)
{
    w_unload();
    txt_load();
    if (blob_load(&scene_blob, scene) < 0)
        return -1;
    for (int p = 0; p < NUM_PREFABS; p++)
        if (!prefab_blob[p].mem)
            blob_load(&prefab_blob[p], PREFAB_BASE + p);
    Blob *b = &scene_blob;
    int ngo;
    const WGo *gos = blob_sec(b, TAG('G', 'O', 'B', 'J'), &ngo);
    W.scene = scene;
    W.nobj = ngo;
    for (int i = 0; i < ngo && i < MAX_OBJS; i++)
        obj_from(&W.obj[i], &gos[i]);
    W.rect = dup_sec(b, TAG('R', 'E', 'C', 'T'), sizeof(WRect), &W.nrect, &W.caprect, RESERVE);
    {
        int n;
        const WSr *src = blob_sec(b, TAG('S', 'R', 'E', 'N'), &n);
        W.capsr = n + RESERVE;
        W.sr = calloc(W.capsr, sizeof(Sr));
        for (int i = 0; i < n; i++)
            W.sr[i].d = src[i];
        for (int i = n; i < W.capsr; i++)
            W.sr[i].d.go = -1;
        W.nsr = n;
    }
    W.img = dup_sec(b, TAG('I', 'M', 'A', 'G'), sizeof(WImg), &W.nimg, &W.capimg, RESERVE);
    {
        int n;
        const WText *src = blob_sec(b, TAG('T', 'E', 'X', 'T'), &n);
        W.captext = n + RESERVE;
        W.text = calloc(W.captext, sizeof(Text));
        for (int i = 0; i < n; i++) {
            W.text[i].d = src[i];
            text_init(&W.text[i]);
        }
        for (int i = n; i < W.captext; i++)
            W.text[i].d.go = -1;
        W.ntext = n;
    }
    W.rbd = dup_sec(b, TAG('R', 'B', 'D', 'Y'), sizeof(WRb), &W.nrb, &W.caprb, RESERVE);
    {
        int n;
        const WCol *src = blob_sec(b, TAG('C', 'O', 'L', 'L'), &n);
        W.capcol = n + RESERVE;
        W.col = calloc(W.capcol, sizeof(Col));
        for (int i = 0; i < n; i++)
            W.col[i].d = src[i];
        for (int i = n; i < W.capcol; i++)
            W.col[i].d.go = -1;
        W.ncol = n;
    }
    W.wall = dup_sec(b, TAG('W', 'A', 'L', 'L'), sizeof(WWall), &W.nwall, 0, 0);
    W.tmap = dup_sec(b, TAG('T', 'M', 'A', 'P'), sizeof(WTilemap), &W.ntmap, 0, 0);
    W.tile = dup_sec(b, TAG('T', 'I', 'L', 'E'), sizeof(WTile), &W.ntile, 0, 0);
    W.sgrp = dup_sec(b, TAG('S', 'G', 'R', 'P'), sizeof(WSgrp), &W.nsgrp, &W.capsgrp, RESERVE);
    W.canvas = dup_sec(b, TAG('C', 'A', 'N', 'V'), sizeof(WCanvas), &W.ncanvas, &W.capcanvas, RESERVE);
    W.trail = dup_sec(b, TAG('T', 'R', 'A', 'L'), sizeof(WTrail), &W.ntrail, &W.captrail, RESERVE);
    W.line = dup_sec(b, TAG('L', 'I', 'N', 'E'), sizeof(WLine), &W.nline, &W.capline, RESERVE);
    W.part = dup_sec(b, TAG('P', 'A', 'R', 'T'), sizeof(WPart), &W.npart, &W.cappart, RESERVE);
    W.audio = dup_sec(b, TAG('A', 'U', 'D', 'I'), sizeof(WAudio), &W.naudio, &W.capaudio, RESERVE);
    W.bind = dup_sec(b, TAG('B', 'I', 'N', 'D'), sizeof(int16_t), &W.nbind, &W.capbind, RESERVE * 8);
    {
        int n;
        const WScript *src = blob_sec(b, TAG('S', 'C', 'R', 'P'), &n);
        const uint8_t *fields = blob_sec(b, TAG('F', 'L', 'D', 'S'), 0);
        W.capscript = n + RESERVE;
        W.script = calloc(W.capscript, sizeof(Script));
        for (int i = 0; i < n; i++) {
            Script *s = &W.script[i];
            s->type = src[i].type;
            s->go = src[i].go;
            s->enabled = src[i].enabled;
            s->alive = 1;
            s->fields = (const WField *)(fields + src[i].field_off);
            s->nfield = src[i].nfield;
        }
        W.nscript = n;
    }
    {
        int n;
        const WGrid *g = blob_sec(b, TAG('G', 'R', 'I', 'D'), &n);
        W.has_grid = n > 0;
        if (n)
            W.grid = *g;
    }
    /* sorting group of each sprite renderer: nearest ancestor (or self) with a SortingGroup */
    for (int i = 0; i < W.nsr; i++) {
        W.sr[i].sgroup = -1;
        for (int o = W.sr[i].d.go; o >= 0; o = W.obj[o].parent) {
            if (W.obj[o].sgrp >= 0) {
                W.sr[i].sgroup = o;
                break;
            }
        }
    }
    W.camera = obj_find(name_crc("Main Camera"));
    W.ortho = (scene >= 12) ? 7.0f : 6.0f;
    W.pxu = 272.0f / (2.0f * W.ortho);
    gfx_scene_textures(scene);
    w_hierarchy();
    hooks_load();
    return 0;
}

/* ------------------------------------------------------------------ prefab instances */
static int find_free_obj(void)
{
    for (int i = 0; i < W.nobj; i++)
        if (!W.obj[i].alive)
            return i;
    if (W.nobj < MAX_OBJS)
        return W.nobj++;
    return -1;
}

#define ALLOC(arr, n, cap, freecond)                       \
    ({ int _r = -1;                                        \
       for (int _i = 0; _i < (cap); _i++) {                \
           if (_i >= (n) || (freecond)) { _r = _i; break; } \
       }                                                   \
       if (_r >= (n)) (n) = _r + 1;                        \
       _r; })

int w_instantiate(int prefab, float x, float y)
{
    if (prefab < 0 || prefab >= NUM_PREFABS || !prefab_blob[prefab].mem)
        return -1;
    Blob *b = &prefab_blob[prefab];
    int ngo;
    const WGo *gos = blob_sec(b, TAG('G', 'O', 'B', 'J'), &ngo);
    int16_t *map = malloc(sizeof(int16_t) * (ngo ? ngo : 1));
    for (int i = 0; i < ngo; i++) {
        map[i] = find_free_obj();
        if (map[i] < 0) {
            free(map);
            return -1;
        }
        W.obj[map[i]].alive = 1;   /* reserve */
    }
    int nrect, nsr, nimg, ntext, nanim, nrb, ncol, nsgrp, ncanvas, ntrail, nline, npart, naudio, nscript;
    const WRect *rects = blob_sec(b, TAG('R', 'E', 'C', 'T'), &nrect);
    const WSr *srs = blob_sec(b, TAG('S', 'R', 'E', 'N'), &nsr);
    const WImg *imgs = blob_sec(b, TAG('I', 'M', 'A', 'G'), &nimg);
    const WText *texts = blob_sec(b, TAG('T', 'E', 'X', 'T'), &ntext);
    const WAnim *anims = blob_sec(b, TAG('A', 'N', 'I', 'M'), &nanim);
    const int16_t *binds = blob_sec(b, TAG('B', 'I', 'N', 'D'), 0);
    const WRb *rbs = blob_sec(b, TAG('R', 'B', 'D', 'Y'), &nrb);
    const WCol *cols = blob_sec(b, TAG('C', 'O', 'L', 'L'), &ncol);
    const WSgrp *sgrps = blob_sec(b, TAG('S', 'G', 'R', 'P'), &nsgrp);
    const WCanvas *canv = blob_sec(b, TAG('C', 'A', 'N', 'V'), &ncanvas);
    const WTrail *trails = blob_sec(b, TAG('T', 'R', 'A', 'L'), &ntrail);
    const WLine *lines = blob_sec(b, TAG('L', 'I', 'N', 'E'), &nline);
    const WPart *parts = blob_sec(b, TAG('P', 'A', 'R', 'T'), &npart);
    const WAudio *auds = blob_sec(b, TAG('A', 'U', 'D', 'I'), &naudio);
    const WScript *scrs = blob_sec(b, TAG('S', 'C', 'R', 'P'), &nscript);
    const uint8_t *fields = blob_sec(b, TAG('F', 'L', 'D', 'S'), 0);
    (void)nrect; (void)nimg; (void)ntext; (void)nanim; (void)nrb; (void)nsgrp; (void)ncanvas; (void)ntrail; (void)nline; (void)npart; (void)naudio;
    for (int i = 0; i < ngo; i++) {
        const WGo *g = &gos[i];
        Obj *o = &W.obj[map[i]];
        obj_from(o, g);
        o->parent = g->parent >= 0 ? map[g->parent] : -1;
        if (g->parent < 0) {
            o->lx = x;
            o->ly = y;
        }
        o->sr = o->img = o->text = o->anim = o->rb = o->sgrp = o->canvas = o->tilemap = o->trail = o->line = o->part = o->audio = -1;
        o->col_first = -1; o->ncol = 0; o->rect = -1; o->script_first = -1;
        o->stamp = 0;
        if (g->rect >= 0) {
            int r = ALLOC(W.rect, W.nrect, W.caprect, W.rect[_i].go == -2);
            if (r >= 0) { W.rect[r] = rects[g->rect]; W.rect[r].go = map[i]; o->rect = r; }
        }
        if (g->sr >= 0) {
            int r = ALLOC(W.sr, W.nsr, W.capsr, W.sr[_i].d.go < 0);
            if (r >= 0) { W.sr[r].d = srs[g->sr]; W.sr[r].d.go = map[i]; W.sr[r].sgroup = -1; o->sr = r; }
        }
        if (g->img >= 0) {
            int r = ALLOC(W.img, W.nimg, W.capimg, W.img[_i].go < 0);
            if (r >= 0) { W.img[r] = imgs[g->img]; W.img[r].go = map[i]; o->img = r; }
        }
        if (g->text >= 0) {
            int r = ALLOC(W.text, W.ntext, W.captext, W.text[_i].d.go < 0);
            if (r >= 0) { free(W.text[r].own); W.text[r].d = texts[g->text]; W.text[r].d.go = map[i]; text_init(&W.text[r]); o->text = r; }
        }
        if (g->rb >= 0) {
            int r = ALLOC(W.rbd, W.nrb, W.caprb, W.rbd[_i].go < 0);
            if (r >= 0) { W.rbd[r] = rbs[g->rb]; W.rbd[r].go = map[i]; o->rb = r; }
        }
        if (g->ncol) {
            int first = -1;
            /* colliders of one object must be contiguous: find a free run */
            for (int s = 0; s + g->ncol <= W.capcol && first < 0; s++) {
                int ok = 1;
                for (int k = 0; k < g->ncol; k++)
                    if (s + k < W.ncol && W.col[s + k].d.go >= 0) { ok = 0; break; }
                if (ok) first = s;
            }
            if (first >= 0) {
                for (int k = 0; k < g->ncol; k++) {
                    W.col[first + k].d = cols[g->col_first + k];
                    W.col[first + k].d.go = map[i];
                }
                if (first + g->ncol > W.ncol) W.ncol = first + g->ncol;
                o->col_first = first;
                o->ncol = g->ncol;
            }
        }
        if (g->sgrp >= 0) {
            int r = ALLOC(W.sgrp, W.nsgrp, W.capsgrp, W.sgrp[_i].go < 0);
            if (r >= 0) { W.sgrp[r] = sgrps[g->sgrp]; W.sgrp[r].go = map[i]; o->sgrp = r; }
        }
        if (g->canvas >= 0) {
            int r = ALLOC(W.canvas, W.ncanvas, W.capcanvas, W.canvas[_i].go < 0);
            if (r >= 0) { W.canvas[r] = canv[g->canvas]; W.canvas[r].go = map[i]; o->canvas = r; }
        }
        if (g->trail >= 0) {
            int r = ALLOC(W.trail, W.ntrail, W.captrail, W.trail[_i].go < 0);
            if (r >= 0) { W.trail[r] = trails[g->trail]; W.trail[r].go = map[i]; o->trail = r; }
        }
        if (g->line >= 0) {
            int r = ALLOC(W.line, W.nline, W.capline, W.line[_i].go < 0);
            if (r >= 0) { W.line[r] = lines[g->line]; W.line[r].go = map[i]; o->line = r; }
        }
        if (g->part >= 0) {
            int r = ALLOC(W.part, W.npart, W.cappart, W.part[_i].go < 0);
            if (r >= 0) { W.part[r] = parts[g->part]; W.part[r].go = map[i]; o->part = r; }
        }
        if (g->audio >= 0) {
            int r = ALLOC(W.audio, W.naudio, W.capaudio, W.audio[_i].go < 0);
            if (r >= 0) { W.audio[r] = auds[g->audio]; W.audio[r].go = map[i]; o->audio = r; }
        }
        if (g->anim >= 0) {
            const WAnim *a = &anims[g->anim];
            int np = g_actrls[a->ctrl].npath;
            if (W.nbind + np <= W.capbind) {
                int first = W.nbind;
                for (int k = 0; k < np; k++) {
                    int lo = binds[a->bind_first + k];
                    W.bind[first + k] = lo >= 0 ? map[lo] : -1;
                }
                W.nbind += np;
                o->anim = hook_anim_new(map[i], a->ctrl, a->update, a->enabled, first);
            }
        }
        if (g->nscript) {
            for (int k = 0; k < g->nscript; k++) {
                const WScript *ws = &scrs[g->script_first + k];
                int r = -1;
                for (int s = 0; s < W.capscript; s++)
                    if (s >= W.nscript || !W.script[s].alive) { r = s; break; }
                if (r < 0)
                    break;
                if (r >= W.nscript)
                    W.nscript = r + 1;
                Script *s = &W.script[r];
                free(s->self);
                free(s->remap);
                memset(s, 0, sizeof *s);
                s->type = ws->type;
                s->go = map[i];
                s->enabled = ws->enabled;
                s->alive = 1;
                s->fields = (const WField *)(fields + ws->field_off);
                s->nfield = ws->nfield;
                s->remap = malloc(sizeof(int16_t) * ngo);
                memcpy(s->remap, map, sizeof(int16_t) * ngo);
                s->remap_n = ngo;
                if (k == 0)
                    o->script_first = r;
            }
        }
    }
    int root = map[0];
    for (int i = 0; i < ngo; i++) {
        Obj *o = &W.obj[map[i]];
        if (o->sr >= 0) {
            W.sr[o->sr].sgroup = -1;
            for (int p = map[i]; p >= 0; p = W.obj[p].parent)
                if (W.obj[p].sgrp >= 0) { W.sr[o->sr].sgroup = p; break; }
        }
    }
    for (int i = 0; i < ngo; i++)
        if (W.obj[map[i]].anim >= 0)
            anim_recapture(W.obj[map[i]].anim);
    free(map);
    w_hierarchy();
    hooks_instantiate(root);
    return root;
}

static void destroy_now(int o)
{
    Obj *ob = &W.obj[o];
    if (!ob->alive)
        return;
    for (int c = w_first_child[o]; c >= 0; c = w_next_sib[c])
        destroy_now(c);
    hooks_destroy(o);
    if (ob->sr >= 0) W.sr[ob->sr].d.go = -1;
    if (ob->img >= 0) W.img[ob->img].go = -1;
    if (ob->text >= 0) { W.text[ob->text].d.go = -1; }
    if (ob->rb >= 0) W.rbd[ob->rb].go = -1;
    for (int k = 0; k < ob->ncol; k++) W.col[ob->col_first + k].d.go = -1;
    if (ob->sgrp >= 0) W.sgrp[ob->sgrp].go = -1;
    if (ob->canvas >= 0) W.canvas[ob->canvas].go = -1;
    if (ob->trail >= 0) W.trail[ob->trail].go = -1;
    if (ob->line >= 0) W.line[ob->line].go = -1;
    if (ob->part >= 0) W.part[ob->part].go = -1;
    if (ob->audio >= 0) W.audio[ob->audio].go = -1;
    if (ob->rect >= 0) W.rect[ob->rect].go = -2;
    for (int s = 0; s < W.nscript; s++)
        if (W.script[s].alive && W.script[s].go == o)
            W.script[s].alive = 0;
    ob->alive = 0;
    ob->ah = 0;
}

static int destroy_pending;
void w_destroy(int o, float delay)
{
    if (o < 0 || o >= W.nobj || !W.obj[o].alive)
        return;
    if (delay > 0) {
        W.obj[o].destroy_t = delay;
        destroy_pending = 1;
        return;
    }
    destroy_now(o);
    w_hierarchy();
}

void w_tick_destroy(float dt)
{
    if (!destroy_pending)
        return;
    int any = 0, left = 0;
    for (int i = 0; i < W.nobj; i++) {
        Obj *o = &W.obj[i];
        if (o->alive && o->destroy_t > 0) {
            left = 1;
            o->destroy_t -= dt;
            if (o->destroy_t <= 0) {
                o->destroy_t = 0;
                destroy_now(i);
                any = 1;
            }
        }
    }
    destroy_pending = left;
    if (any)
        w_hierarchy();
}

/* ------------------------------------------------------------------ hierarchy and transforms */
void w_hierarchy(void)
{
    extern int g_prof_hier; g_prof_hier++;
    /* parents may come after children for prefab instances: iterate until stable (depth is small) */
    for (int pass = 0; pass < 8; pass++) {
        int changed = 0;
        for (int i = 0; i < W.nobj; i++) {
            Obj *o = &W.obj[i];
            if (!o->alive) { o->ah = 0; continue; }
            int pa = o->parent >= 0 ? W.obj[o->parent].ah : 1;
            uint8_t ah = o->active && pa;
            uint8_t sc = 0;
            if (o->canvas >= 0 && W.canvas[o->canvas].mode != 2 && (o->parent < 0 || !W.obj[o->parent].screen))
                sc = 1;
            else if (o->parent >= 0)
                sc = W.obj[o->parent].screen;
            if (ah != o->ah || sc != o->screen) {
                o->ah = ah;
                o->screen = sc;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
    w_build_children();
}

int obj_active(int o) { return o >= 0 && o < W.nobj && W.obj[o].alive && W.obj[o].ah; }

void w_activity_changed(void);
void w_activity_subtree(int root);

void obj_set_active(int o, int on)
{
    if (o < 0 || o >= W.nobj || !W.obj[o].alive)
        return;
    on = on ? 1 : 0;
    if (W.obj[o].active == on)
        return;
    W.obj[o].active = on;
    w_activity_subtree(o);
}

const void *w_anim_section(int *count)
{
    return blob_sec(&scene_blob, TAG('A', 'N', 'I', 'M'), count);
}

#define UI_S (272.0f / 1080.0f)

static void parent_rect(int p, float *x0, float *y0, float *w, float *h)
{
    /* parent rect in the parent's local space (origin = its pivot) */
    if (p >= 0 && W.obj[p].rect >= 0) {
        Obj *po = &W.obj[p];
        *w = po->rw; *h = po->rh;
        *x0 = -po->rw * po->pvx;
        *y0 = -po->rh * po->pvy;
        return;
    }
    *x0 = *y0 = *w = *h = 0;
}

static void calc_local(int i, float lm[6])
{
    Obj *o = &W.obj[i];
    float lx = o->lx, ly = o->ly;
    if (o->rect >= 0) {
        const WRect *r = &W.rect[o->rect];
        int p = o->parent;
        float px0, py0, pw, ph;
        if (o->canvas >= 0 && W.canvas[o->canvas].mode != 2 && o->screen && (p < 0 || !W.obj[p].screen)) {
            /* root screen canvas: the whole screen in canvas units, centred */
            o->rw = SCR_W / UI_S;
            o->rh = SCR_H / UI_S;
            o->pvx = o->pvy = 0.5f;
            lx = ly = 0;
        } else {
            parent_rect(p, &px0, &py0, &pw, &ph);
            float ax0 = px0 + r->amin_x * pw, ay0 = py0 + r->amin_y * ph;
            float ax1 = px0 + r->amax_x * pw, ay1 = py0 + r->amax_y * ph;
            o->rw = (ax1 - ax0) + r->sd_x;
            o->rh = (ay1 - ay0) + r->sd_y;
            o->pvx = r->pv_x;
            o->pvy = r->pv_y;
            if (p >= 0 && W.obj[p].rect >= 0) {
                lx = ax0 + (ax1 - ax0) * r->pv_x + r->ap_x;
                ly = ay0 + (ay1 - ay0) * r->pv_y + r->ap_y;
            }
        }
    }
    float sx = o->lsx, sy = o->lsy;
    if (o->rx != 0.0f) sy *= cosf(o->rx * 0.01745329f);
    if (o->ry != 0.0f) sx *= cosf(o->ry * 0.01745329f);
    float c = 1, s = 0;
    if (o->lrot != 0.0f) {
        c = cosf(o->lrot * 0.01745329f);
        s = sinf(o->lrot * 0.01745329f);
    }
    lm[0] = c * sx; lm[1] = -s * sy;
    lm[2] = s * sx; lm[3] = c * sy;
    lm[4] = lx; lm[5] = ly;
}

static void mul(const float a[6], const float b[6], float out[6])
{
    float r0 = a[0] * b[0] + a[1] * b[2], r1 = a[0] * b[1] + a[1] * b[3];
    float r2 = a[2] * b[0] + a[3] * b[2], r3 = a[2] * b[1] + a[3] * b[3];
    float r4 = a[0] * b[4] + a[1] * b[5] + a[4], r5 = a[2] * b[4] + a[3] * b[5] + a[5];
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3; out[4] = r4; out[5] = r5;
}

static void update_matrix(int i)
{
    Obj *o = &W.obj[i];
    if (o->stamp == W.frame && o->stamp_ok)
        return;
    float lm[6];
    if (o->parent >= 0)
        update_matrix(o->parent);
    calc_local(i, lm);
    if (o->parent >= 0) {
        Obj *p = &W.obj[o->parent];
        if (o->screen && !p->screen) {
            /* screen canvas root under a world object (unlikely): treat as root */
            memcpy(o->m, lm, sizeof lm);
        } else {
            mul(p->m, lm, o->m);
        }
    } else {
        memcpy(o->m, lm, sizeof lm);
    }
    if (o->screen && o->canvas >= 0 && (o->parent < 0 || !W.obj[o->parent].screen)) {
        /* screen canvas root: canvas units -> UI pixels (y up, origin at the screen centre) */
        float m[6] = {UI_S, 0, 0, UI_S, 0, 0};
        memcpy(o->m, m, sizeof m);
    }
    o->stamp = W.frame;
    o->stamp_ok = 1;
}

void obj_matrix(int o, float m[6])
{
    update_matrix(o);
    memcpy(m, W.obj[o].m, sizeof W.obj[o].m);
}

void w_transforms(void)
{
    W.frame++;
    for (int i = 0; i < W.nobj; i++)
        if (W.obj[i].alive && W.obj[i].ah)
            update_matrix(i);
}

static void invalidate(int o)
{
    W.obj[o].stamp_ok = 0;
    for (int c = w_first_child[o]; c >= 0; c = w_next_sib[c])
        invalidate(c);
}

void obj_pos(int o, float *x, float *y)
{
    if (o < 0) { *x = *y = 0; return; }
    update_matrix(o);
    *x = W.obj[o].m[4];
    *y = W.obj[o].m[5];
}

void obj_set_pos(int o, float x, float y)
{
    if (o < 0)
        return;
    Obj *ob = &W.obj[o];
    if (ob->parent >= 0) {
        float pm[6];
        obj_matrix(ob->parent, pm);
        float det = pm[0] * pm[3] - pm[1] * pm[2];
        if (det == 0)
            det = 1e-6f;
        float dx = x - pm[4], dy = y - pm[5];
        ob->lx = (pm[3] * dx - pm[1] * dy) / det;
        ob->ly = (-pm[2] * dx + pm[0] * dy) / det;
    } else {
        ob->lx = x;
        ob->ly = y;
    }
    invalidate(o);
}

void obj_scale(int o, float sx, float sy)
{
    if (o < 0) return;
    W.obj[o].lsx = sx;
    W.obj[o].lsy = sy;
    invalidate(o);
}

void obj_set_rot(int o, float deg)
{
    if (o < 0) return;
    float parent = 0;
    if (W.obj[o].parent >= 0)
        parent = obj_world_rot(W.obj[o].parent);
    W.obj[o].lrot = deg - parent;
    invalidate(o);
}

float obj_world_rot(int o)
{
    float r = 0;
    for (; o >= 0; o = W.obj[o].parent)
        r += W.obj[o].lrot;
    return r;
}

int obj_find(uint32_t h)
{
    for (int i = 0; i < W.nobj; i++)
        if (W.obj[i].alive && W.obj[i].name == h)
            return i;
    return -1;
}

int obj_find_child(int parent, uint32_t h)
{
    if (parent < 0 || parent >= W.nobj) return -1;
    for (int c = w_first_child[parent]; c >= 0; c = w_next_sib[c])
        if (W.obj[c].alive && W.obj[c].name == h)
            return c;
    return -1;
}

int obj_is_descendant(int o, int ancestor)
{
    for (; o >= 0; o = W.obj[o].parent)
        if (o == ancestor)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ components */
void sr_set_sprite(int sr, int spr) { if (sr >= 0) W.sr[sr].d.spr = spr; }
void sr_set_color(int sr, uint32_t c)
{
    if (sr < 0) return;
    W.sr[sr].d.r = c & 255; W.sr[sr].d.g = (c >> 8) & 255; W.sr[sr].d.b = (c >> 16) & 255; W.sr[sr].d.a = c >> 24;
}

/* TMP FontStyles UpperCase / LowerCase: ASCII + the Polish letters of the credits */
/* TMP UpperCase / LowerCase styles for every script the fonts cover (Latin-1, Latin Extended-A, Greek,
   Cyrillic); 2-byte UTF-8 letters keep their length, so the string is rewritten in place */
static uint32_t case_cp(uint32_t c, int upper)
{
    if (upper) {
        if (c >= 'a' && c <= 'z') return c - 32;
        if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
        if (c == 0xFF) return 0x178;
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & ~1u;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
        if (c == 0x3C2) return 0x3A3;
        if (c >= 0x430 && c <= 0x44F) return c - 32;
        if (c >= 0x450 && c <= 0x45F) return c - 80;
        if (c >= 0x460 && c <= 0x4FF && (c & 1)) return c - 1;
    } else {
        if (c >= 'A' && c <= 'Z') return c + 32;
        if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
        if (c == 0x178) return 0xFF;
        if ((c >= 0x100 && c <= 0x137 && c != 0x130) || (c >= 0x14A && c <= 0x177)) return c | 1u;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
        if (c >= 0x410 && c <= 0x42F) return c + 32;
        if (c >= 0x400 && c <= 0x40F) return c + 80;
        if (c >= 0x460 && c <= 0x4FF && !(c & 1)) return c + 1;
    }
    return c;
}

static void text_case(char *p, int upper)
{
    while (*p) {
        unsigned char b = (unsigned char)*p;
        if (b < 0x80) {
            *p = (char)case_cp(b, upper);
            p++;
            continue;
        }
        if ((b & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            uint32_t c = ((b & 31u) << 6) | (p[1] & 63u);
            uint32_t m = case_cp(c, upper);
            if (m != c && m >= 0x80 && m < 0x800) {
                p[0] = (char)(0xC0 | (m >> 6));
                p[1] = (char)(0x80 | (m & 63));
            }
            p += 2;
            continue;
        }
        p++;
        while ((*p & 0xC0) == 0x80) p++;
    }
}

void text_set(int t, const char *s)
{
    if (t < 0 || !s)
        return;
    Text *tx = &W.text[t];
    int n = strlen(s) + 1;
    if (tx->own_cap < n) {
        free(tx->own);
        tx->own_cap = n < 64 ? 64 : n;
        tx->own = malloc(tx->own_cap);
    }
    memcpy(tx->own, s, n);
    tx->str = tx->own;
    if (tx->d.flags & (WTXT_UPPER | WTXT_LOWER))
        text_case(tx->own, tx->d.flags & WTXT_UPPER);
}

void text_set_color(int t, uint32_t c)
{
    if (t < 0) return;
    W.text[t].d.r = c & 255; W.text[t].d.g = (c >> 8) & 255; W.text[t].d.b = (c >> 16) & 255; W.text[t].d.a = c >> 24;
}

/* ------------------------------------------------------------------ script fields */
const WField *f_get(const Script *s, uint32_t name)
{
    for (int i = 0; i < s->nfield; i++)
        if (s->fields[i].name == name)
            return &s->fields[i];
    return 0;
}

static const uint8_t *field_base(const Script *s)
{
    /* the pool start: fields of every script point into the same FLDS block of their blob */
    (void)s;
    return 0;
}

const WField *f_child(const Script *s, const WField *f)
{
    (void)field_base;
    if (!f || (f->kind != F_ARRAY && f->kind != F_STRUCT) || !f->count)
        return 0;
    /* f->v.off is the byte offset into the blob's field pool; the script's own fields start at its
     * field_off, so walk back from the script's first field to the pool start */
    const uint8_t *pool = (const uint8_t *)s->fields - (s->fields[0].name == 0 ? 0 : 0);
    extern const uint8_t *w_field_pool(const Script *s);
    pool = w_field_pool(s);
    return (const WField *)(pool + f->v.off);
}

float f_float(const Script *s, uint32_t name, float def)
{
    const WField *f = f_get(s, name);
    if (!f) return def;
    if (f->kind == F_FLOAT) return f->v.f;
    if (f->kind == F_INT) return (float)f->v.i;
    return def;
}

int f_int(const Script *s, uint32_t name, int def)
{
    const WField *f = f_get(s, name);
    if (!f) return def;
    if (f->kind == F_INT) return f->v.i;
    if (f->kind == F_FLOAT) return (int)f->v.f;
    return def;
}

int f_ref_at(const Script *s, const WField *f)
{
    if (!f || f->kind != F_REF || f->v.i < 0)
        return -1;
    int g = f->v.i;
    if (s->remap)
        return g < s->remap_n ? s->remap[g] : -1;
    return g;
}

int f_ref(const Script *s, uint32_t name) { return f_ref_at(s, f_get(s, name)); }

int f_asset(const Script *s, uint32_t name)
{
    const WField *f = f_get(s, name);
    return (f && f->kind == F_ASSET) ? f->v.i : -1;
}

int f_count(const Script *s, uint32_t name)
{
    const WField *f = f_get(s, name);
    return (f && (f->kind == F_ARRAY || f->kind == F_STRUCT)) ? f->count : 0;
}

const WField *f_elems(const Script *s, uint32_t name, int *count)
{
    const WField *f = f_get(s, name);
    if (count)
        *count = (f && (f->kind == F_ARRAY || f->kind == F_STRUCT)) ? f->count : 0;
    return f_child(s, f);
}

float f_vec(const Script *s, uint32_t name, float *y)
{
    const WField *f = f_get(s, name);
    if (!f || f->kind != F_VEC) { if (y) *y = 0; return 0; }
    if (y) *y = f->v2;
    return f->v.f;
}

const uint8_t *w_field_pool(const Script *s)
{
    /* scene scripts use the scene blob pool, prefab scripts their prefab's pool */
    for (int p = 0; p < NUM_PREFABS; p++) {
        const uint8_t *pool = blob_sec(&prefab_blob[p], TAG('F', 'L', 'D', 'S'), 0);
        int n;
        blob_sec(&prefab_blob[p], TAG('F', 'L', 'D', 'S'), &n);
        if (pool && (const uint8_t *)s->fields >= pool && (const uint8_t *)s->fields < pool + n * 16)
            return pool;
    }
    return blob_sec(&scene_blob, TAG('F', 'L', 'D', 'S'), 0);
}
