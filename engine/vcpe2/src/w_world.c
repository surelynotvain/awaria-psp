#include <stdio.h>
/* Scene blobs -> runtime arrays; transforms, activity, prefab instances, script fields. */
/* HOW THIS FILE WORKS (the "mini Unity" world store)
 *
 * The 2D runtime replays games made in Unity. The converter turns each Unity scene and prefab into a binary "blob"
 * (format in awfmt.h): a list of tagged sections (GOBJ = GameObjects, SREN = SpriteRenderers, COLL = colliders, SCRP
 * = scripts, FLDS = script field values, ...), each an array of fixed-size records.
 *
 * Loading a scene reads its blob and copies every section into the World W: one array per component kind, with
 * RESERVE free slots at the end for prefab instances created while playing. Every GameObject (Obj) holds indexes into
 * those arrays (-1 = no such component), just as a Unity GameObject has components.
 *
 * Transforms are 2D affine matrices of 6 floats (x' = m0 x + m1 y + m4, y' = m2 x + m3 y + m5). An object's world
 * matrix is its parent's world matrix times its own local one. Matrices are cached per frame (stamp) and invalidated
 * with all descendants when an object moves.
 *
 * Unity's activeInHierarchy (an object counts as active only if it and all its parents are) is kept in Obj.ah and
 * refreshed by w_hierarchy. Script fields are looked up by the CRC32 of their name. */
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include "w_world.h"
#include "w_anim.h"
#include "aw_gfx.h"
#include "w_hooks.h"
#include "vcpe_sys.h"
#include "vcpe_observe.h"

World W;
/* Cached local inputs for world transforms. UI rectangles retain the original full refresh because anchors and
 * parent layout are additional inputs. Sidecars grow with instantiated objects, not MAX_OBJS. */
typedef struct { float local[7]; uint32_t parent_rev, rev; int16_t parent; } TransformCache;
static TransformCache *transform_cache;
static int transform_cap;
static TransformCache *transform_slot(int i)
{
    if (i>=transform_cap) {
        int n=i+64;
        TransformCache *next=realloc(transform_cache,(size_t)n*sizeof *next);
        if (!next) return 0;
        memset(next+transform_cap,0,(size_t)(n-transform_cap)*sizeof *next);
        transform_cache=next; transform_cap=n;
    }
    return &transform_cache[i];
}


/* ------------------------------------------------------------------ CRC32 */
/* Names (objects, fields) are compared as CRC32 hashes, the same function the converter used. */
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
/* The string pool: a count, an offset table, then the 0-terminated strings. Read once, kept. */
static uint8_t *str_mem;
static uint32_t str_count;
const char *w_string(int id)
{
    if (!str_mem || id < 0 || (uint32_t)id >= str_count)
        return "";
    const uint32_t *idx = (const uint32_t *)(str_mem + 4);
    return (const char *)(str_mem + 4 + 4 * str_count + idx[id]);
}

/* Text files of the current language: one pointer per line into the language data, built once. */
static const char **txt_idx[32];
static int txt_n[32];
void txt_load(void)
{
    if (!str_mem) {
        int st = vcpe_step_begin("Loading strings: %u KB", (unsigned)((STR_SIZE + 1023) / 1024));
        str_mem = vcpe_pak_read_alloc(STR_OFF, STR_SIZE);
        vcpe_step_end(st, str_mem != 0);
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
        if (!txt_idx[f]) { w_alloc_fails++; vcpe_observe_fail("text lines",(nlines+1)*sizeof(char *)); continue; }
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
    /* A loaded blob: its memory and where each tagged section starts. */
    uint8_t *mem;
    const WSection *sec[24];
    uint32_t tags[24];
    int nsec;
} Blob;

/* Find a section by tag: pointer to its records and their count (0 if the blob has no such section). */
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

/* Read a blob from the PAK and index its sections. A 64-byte header comes first; every section is padded to 64 bytes.
 */
static int blob_load(Blob *b, int index)
{
    memset(b, 0, sizeof *b);
    if (index<0 || index>=NUM_SCENES+NUM_PREFABS) return -1;
    const BlobDef *d = &g_worlds[index];
    if (d->size<64) return -1;
    int st = index < NUM_SCENES ? vcpe_step_begin("Loading scene %d world: %u KB", index, (unsigned)(d->size / 1024))
                                : vcpe_step_begin("Loading prefab %d: %u KB", index - NUM_SCENES, (unsigned)(d->size / 1024));
    b->mem = vcpe_pak_read_alloc(d->off, d->size);
    vcpe_step_end(st, b->mem != 0);
    if (!b->mem)
        return -1;
    const WHeader *h = (const WHeader *)b->mem;
    if (h->magic!=WSEC('A','W','W','1') || h->nsec>24) goto bad_blob;
    uint32_t off = 64;
    for (uint32_t i = 0; i < h->nsec && i < 24; i++) {
        if (off>d->size || sizeof(WSection)>d->size-off) goto bad_blob;
        const WSection *s = (const WSection *)(b->mem + off);
        if (s->bytes>d->size-off-sizeof *s || s->count>INT32_MAX ||
            (s->count && (!s->elem || s->count>s->bytes/s->elem))) goto bad_blob;
        b->sec[b->nsec] = s;
        b->tags[b->nsec] = s->tag;
        b->nsec++;
        off += sizeof(WSection) + s->bytes;
        if (off>UINT32_MAX-63) goto bad_blob;
        off = (off + 63) & ~63u;
    }
    return 0;
bad_blob:
    free(b->mem); memset(b,0,sizeof *b); return -1;
}

/* The current scene's blob, and every prefab's blob (loaded once and kept: instantiating must not read the PAK). */
static Blob scene_blob;
static Blob prefab_blob[NUM_PREFABS];

#define TAG(a, b, c, d) WSEC(a, b, c, d)

/* Per-kind scene capacities stay fixed while scripts can retain raw component pointers. Kinds no prefab uses
 * need no instance reserve. Optional per-kind budgets are chosen before loading; paged VcpeStore serves new
 * systems needing stable-address growth rather than relocating these legacy contiguous arrays. */
static struct { uint32_t tag; int reserve; } budgets[16];
static int nbudgets, world_alloc_failed;
int w_alloc_fails, w_transform_hits, w_transform_rebuilds;
int w_component_reserve(uint32_t tag, int slots)
{
    if (slots < 0 || slots > 32767 || W.nobj) return -1;
    for (int i=0;i<nbudgets;i++) if (budgets[i].tag==tag) { budgets[i].reserve=slots; return 0; }
    if (nbudgets==16) return -1;
    budgets[nbudgets].tag=tag; budgets[nbudgets++].reserve=slots; return 0;
}
static int reserve_for(uint32_t tag, int fallback)
{
    for (int i=0;i<nbudgets;i++) if (budgets[i].tag==tag) return budgets[i].reserve;
    for (int p=0;p<NUM_PREFABS;p++) { int n; blob_sec(&prefab_blob[p],tag,&n); if (n) return fallback; }
    return 0;
}
static void *world_alloc(size_t n, size_t elem)
{
    if (!n) return 0;
    void *p = n <= SIZE_MAX/elem ? calloc(n,elem) : 0;
    if (!p) { w_alloc_fails++; vcpe_observe_fail("world",0); world_alloc_failed=1; }
    return p;
}
typedef struct { uint32_t refs; int16_t map[]; } InstanceMap;
static void map_release(int16_t *map)
{
    if (map) { InstanceMap *m=(InstanceMap *)((uint8_t *)map - offsetof(InstanceMap,map)); if (!--m->refs) free(m); }
}

/* Copy a section into a new array with `reserve` extra empty records (room for instances). */
static void *dup_sec(const Blob *b, uint32_t tag, size_t elem, int *n, int *cap, int reserve)
{
    int cnt;
    const void *src = blob_sec(b, tag, &cnt);
    int c = cnt + reserve_for(tag, reserve);
    void *mem = world_alloc(c, elem);
    if (src && cnt && mem)
        memcpy(mem, src, elem * cnt);
    *n = mem ? cnt : 0;
    if (cap)
        *cap = mem ? c : 0;
    return mem;
}

/* Free every component array of the current scene. */
static void free_world(void)
{
    free(transform_cache); transform_cache=0; transform_cap=0;
    free(W.rect); free(W.sr); free(W.img);
    for (int i = 0; i < W.ntext; i++)
        free(W.text[i].own);
    free(W.text); free(W.rbd); free(W.col); free(W.wall); free(W.tmap); free(W.tile); free(W.sgrp); free(W.canvas);
    free(W.trail); free(W.line); free(W.part); free(W.audio);
    for (int i = 0; i < W.nscript; i++) {
        free(W.script[i].self);
        map_release(W.script[i].remap);
    }
    free(W.script); free(W.bind);
    if (scene_blob.mem)
        free(scene_blob.mem);
    memset(&scene_blob, 0, sizeof scene_blob);
}

int16_t w_first_child[MAX_OBJS];
int16_t w_next_sib[MAX_OBJS];

/* Build first-child / next-sibling lists from the parent indexes, so children can be walked without scanning every
 * object. Going backwards keeps children in their original order. */
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

/* Unload the scene: scripts first (OnDestroy and the like), then the arrays. */
void w_unload(void)
{
    int st = W.nobj ? vcpe_step_begin("Unloading scene %d: %d objects", W.scene, W.nobj) : -1;
    hooks_unload();
    free_world();
    vcpe_step_end(st, 1);
    memset(&W, 0, sizeof W);
    memset(w_first_child, 0xFF, sizeof w_first_child);
    memset(w_next_sib, 0xFF, sizeof w_next_sib);
}

/* A runtime GameObject from its converted record. */
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

/* Initial text of a TextMeshPro component, translated, with its upper/lower case style applied. */
static void text_init(Text *t)
{
    t->str = t->d.str == 0xFFFF ? "" : lang_static(w_string(t->d.str));
    t->own = 0;
    t->own_cap = 0;
    t->alpha_mul = 1.0f;
    if ((t->d.flags & (WTXT_UPPER | WTXT_LOWER)) && *t->str)
        text_set((int)(t - W.text), t->str);
}

/* Load a scene: blob, prefabs, every component array, then link the scripts (Awake/OnEnable/Start run through
 * hooks_load). */
int w_load(int scene)
{
    w_unload();
    world_alloc_failed = 0;
    txt_load();
    if (blob_load(&scene_blob, scene) < 0)
        return -1;
    for (int p = 0; p < NUM_PREFABS; p++)
        if (!prefab_blob[p].mem)
            blob_load(&prefab_blob[p], PREFAB_BASE + p);
    Blob *b = &scene_blob;
    int ngo;
    const WGo *gos = blob_sec(b, TAG('G', 'O', 'B', 'J'), &ngo);
    int st = vcpe_step_begin("Creating scene %d: %d objects", scene, ngo);
    if (ngo<0 || ngo>MAX_OBJS || (ngo && !gos)) goto load_fail;
    W.scene = scene;
    W.nobj = ngo;
    for (int i = 0; i < ngo && i < MAX_OBJS; i++)
        obj_from(&W.obj[i], &gos[i]);
    W.rect = dup_sec(b, TAG('R', 'E', 'C', 'T'), sizeof(WRect), &W.nrect, &W.caprect, RESERVE);
    {
        int n;
        const WSr *src = blob_sec(b, TAG('S', 'R', 'E', 'N'), &n);
        W.capsr = n + reserve_for(TAG('S','R','E','N'), RESERVE);
        W.sr = world_alloc(W.capsr, sizeof(Sr));
        if (W.capsr && !W.sr) goto load_fail;
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
        W.captext = n + reserve_for(TAG('T','E','X','T'), RESERVE);
        W.text = world_alloc(W.captext, sizeof(Text));
        if (W.captext && !W.text) goto load_fail;
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
        W.capcol = n + reserve_for(TAG('C','O','L','L'), RESERVE);
        W.col = world_alloc(W.capcol, sizeof(Col));
        if (W.capcol && !W.col) goto load_fail;
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
        W.capscript = n + reserve_for(TAG('S','C','R','P'), RESERVE);
        W.script = world_alloc(W.capscript, sizeof(Script));
        if (W.capscript && !W.script) goto load_fail;
        for (int i = 0; i < n; i++) {
            Script *s = &W.script[i];
            s->type = src[i].type;
            s->go = src[i].go;
            s->enabled = src[i].enabled;
            s->alive = 1;
            s->fields = fields ? (const WField *)(fields + src[i].field_off) : 0;
            s->nfield = src[i].nfield;
            s->pool = fields;
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
    if (world_alloc_failed) goto load_fail;
    /* Unity's SortingGroup: sprites under one are sorted together as a unit. */
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
    vcpe_step_end(st, 1);
    /* The camera: orthographic size (half the view height in world units) and pixels per world unit for the 272-pixel
     * screen. */
    W.camera = obj_find(name_crc("Main Camera"));
    W.ortho = (scene >= 12) ? 7.0f : 6.0f;
    W.pxu = 272.0f / (2.0f * W.ortho);
    gfx_scene_textures(scene);
    st = vcpe_step_begin("Linking %d scripts", W.nscript);
    w_hierarchy();
    hooks_load();
    vcpe_step_end(st, 1);
    return 0;
load_fail:
    vcpe_step_end(st, 0);
    free_world(); memset(&W,0,sizeof W);
    return -1;
}

/* ------------------------------------------------------------------ prefab instances */
/* A free object slot (a destroyed one, or the next unused one). */
static int find_free_obj(void)
{
    for (int i = 0; i < W.nobj; i++)
        if (!W.obj[i].alive)
            return i;
    if (W.nobj < MAX_OBJS)
        return W.nobj++;
    return -1;
}

/* First free record of a component array: one past the used ones, or one whose freecond holds (its object was
 * destroyed). Grows n when it takes a new one. GCC statement expression. */
#define ALLOC(arr, n, cap, freecond)                       \
    ({ int _r = -1;                                        \
       for (int _i = 0; _i < (cap); _i++) {                \
           if (_i >= (n) || (freecond)) { _r = _i; break; } \
       }                                                   \
       if (_r >= (n)) (n) = _r + 1;                        \
       _r; })

/* Object.Instantiate: copy a prefab's objects and components into free slots, translating every prefab-local index
 * (parents, references) through map[], place its root at (x, y), then run the new scripts. */
int w_instantiate(int prefab, float x, float y)
{
    if (prefab < 0 || prefab >= NUM_PREFABS || !prefab_blob[prefab].mem)
        return -1;
    Blob *b = &prefab_blob[prefab];
    int ngo;
    const WGo *gos = blob_sec(b, TAG('G', 'O', 'B', 'J'), &ngo);
    if (ngo < 1 || ngo > MAX_OBJS) return -1;
    /* Refuse an exhausted object store before reserving any slots. */
    int free_objects=MAX_OBJS-W.nobj;
    for (int i=0;i<W.nobj;i++) if (!W.obj[i].alive) free_objects++;
    if (free_objects<ngo) return -1;
    InstanceMap *instance=malloc(sizeof *instance+(size_t)ngo*sizeof(int16_t));
    if (!instance) { w_alloc_fails++; vcpe_observe_fail("world",0); return -1; }
    instance->refs=1;
    int16_t *map=instance->map;
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
    /* Preflight every required slot before changing the world. Exhaustion must not create half a prefab. */
    int nr=0,ns=0,ni=0,nt=0,nb=0,nc=0,ng=0,nv=0,ntr=0,nl=0,np=0,na=0,nsc=0,bind_need=0;
    for (int i=0;i<ngo;i++) {
        const WGo *g=&gos[i];
        if (g->parent>=ngo || g->rect>=nrect || g->sr>=nsr || g->img>=nimg || g->text>=ntext ||
            g->rb>=nrb || g->anim>=nanim || g->sgrp>=nsgrp || g->canvas>=ncanvas ||
            g->trail>=ntrail || g->line>=nline || g->part>=npart || g->audio>=naudio ||
            (g->ncol && (g->col_first<0 || g->col_first+g->ncol>ncol)) ||
            (g->nscript && (g->script_first<0 || g->script_first+g->nscript>nscript))) { map_release(map); return -1; }
        nr+=g->rect>=0; ns+=g->sr>=0; ni+=g->img>=0; nt+=g->text>=0; nb+=g->rb>=0;
        nc+=g->ncol; ng+=g->sgrp>=0; nv+=g->canvas>=0; ntr+=g->trail>=0; nl+=g->line>=0;
        np+=g->part>=0; na+=g->audio>=0; nsc+=g->nscript;
        if (g->anim>=0) bind_need+=g_actrls[anims[g->anim].ctrl].npath;
    }
#define ROOM(arr,n,cap,freecond,need) do { int room=(cap)-(n); \
    for (int _i=0;_i<(n);_i++) room+=(freecond); \
    if (room<(need)) { map_release(map); return -1; } } while(0)
    ROOM(W.rect,W.nrect,W.caprect,W.rect[_i].go==-2,nr);
    ROOM(W.sr,W.nsr,W.capsr,W.sr[_i].d.go<0,ns);
    ROOM(W.img,W.nimg,W.capimg,W.img[_i].go<0,ni);
    ROOM(W.text,W.ntext,W.captext,W.text[_i].d.go<0,nt);
    ROOM(W.rbd,W.nrb,W.caprb,W.rbd[_i].go<0,nb);
    ROOM(W.col,W.ncol,W.capcol,W.col[_i].d.go<0,nc);
    ROOM(W.sgrp,W.nsgrp,W.capsgrp,W.sgrp[_i].go<0,ng);
    ROOM(W.canvas,W.ncanvas,W.capcanvas,W.canvas[_i].go<0,nv);
    ROOM(W.trail,W.ntrail,W.captrail,W.trail[_i].go<0,ntr);
    ROOM(W.line,W.nline,W.capline,W.line[_i].go<0,nl);
    ROOM(W.part,W.npart,W.cappart,W.part[_i].go<0,np);
    ROOM(W.audio,W.naudio,W.capaudio,W.audio[_i].go<0,na);
    ROOM(W.script,W.nscript,W.capscript,!W.script[_i].alive,nsc);
#undef ROOM
    if (bind_need>W.capbind-W.nbind) { map_release(map); return -1; }
    if (nc) {
        uint8_t *busy=calloc((size_t)W.capcol,1);
        if (!busy) { w_alloc_fails++; vcpe_observe_fail("world",0); map_release(map); return -1; }
        for (int k=0;k<W.ncol;k++) busy[k]=W.col[k].d.go>=0;
        for (int i=0;i<ngo;i++) if (gos[i].ncol) {
            int run=0, found=0;
            for (int k=0;k<W.capcol;k++) {
                run=busy[k]?0:run+1;
                if (run==gos[i].ncol) { memset(busy+k+1-run,1,(size_t)run); found=1; break; }
            }
            if (!found) { free(busy); map_release(map); return -1; }
        }
        free(busy);
    }
    for (int i = 0; i < ngo; i++) {
        map[i] = find_free_obj();
        if (map[i] < 0) {
            map_release(map);
            return -1;
        }
        W.obj[map[i]].alive = 1;   /* reserve */
    }
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
        /* An object's colliders must sit next to each other in the array (col_first + k): find a free run long
         * enough. */
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
        /* Animator: its bindings (which objects each animated path controls) are remapped to the new objects. */
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
        /* Scripts keep the prefab-to-world index map, so their object references resolve to this instance's objects.
         */
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
                map_release(s->remap);
                memset(s, 0, sizeof *s);
                s->type = ws->type;
                s->go = map[i];
                s->enabled = ws->enabled;
                s->alive = 1;
                s->fields = fields ? (const WField *)(fields + ws->field_off) : 0;
                s->nfield = ws->nfield;
                s->pool = fields;
                s->remap = map;
                instance->refs++;
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
    map_release(map);
    w_hierarchy();
    hooks_instantiate(root);
    return root;
}

/* Object.Destroy: children first, then the object's scripts are told (OnDestroy), then every component record is
 * marked free (go = -1). */
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
/* Destroy now, or after `delay` seconds (Object.Destroy(obj, t)). */
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

/* Count down delayed destroys and run the ones that are due. */
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
/* Recompute activeInHierarchy and the "under a screen-space canvas" flag for every object. Instance parents can come
 * after their children in the array, so repeat until nothing changes. */
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

/* GameObject.SetActive: only that object's subtree can change, so only it is refreshed (with OnEnable/OnDisable). */
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

/* UI is laid out for 1080 pixels high in the original; this scales canvas units to the PSP's 272 lines. */
#define UI_S (272.0f / 1080.0f)

/* The parent's RectTransform rectangle, in its local space (origin at its pivot). */
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

/* Local matrix of an object. For UI objects (RectTransform) the position comes from the anchors: anchor points inside
 * the parent's rectangle, plus the anchored offset and size delta, like Unity's layout. A rotation around x or y
 * (used for flips) is approximated by scaling the other axis by its cosine. */
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

/* Product of two 2D affine matrices (out = a * b). */
static void mul(const float a[6], const float b[6], float out[6])
{
    float r0 = a[0] * b[0] + a[1] * b[2], r1 = a[0] * b[1] + a[1] * b[3];
    float r2 = a[2] * b[0] + a[3] * b[2], r3 = a[2] * b[1] + a[3] * b[3];
    float r4 = a[0] * b[4] + a[1] * b[5] + a[4], r5 = a[2] * b[4] + a[3] * b[5] + a[5];
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3; out[4] = r4; out[5] = r5;
}

/* World matrix of object i, computed at most once per frame: parent first (recursively), then parent * local. */
static void update_matrix(int i)
{
    Obj *o = &W.obj[i];
    if (o->stamp == W.frame && o->stamp_ok)
        return;
    if (o->parent >= 0)
        update_matrix(o->parent);
    TransformCache *cache=transform_slot(i);
    /* Reacquire after allocation: a child's sidecar growth can move the whole sidecar array. */
    uint32_t parent_rev=o->parent>=0 && o->parent<transform_cap ? transform_cache[o->parent].rev : 0;
    if (cache && cache->rev && (o->parent<0 || (o->parent<transform_cap && transform_cache[o->parent].rev)) &&
        o->stamp_ok && o->rect<0 && !o->screen && cache->parent==o->parent &&
        cache->parent_rev==parent_rev && !memcmp(cache->local,&o->lx,sizeof cache->local)) {
        o->stamp=W.frame; w_transform_hits++; return;
    }
    float lm[6];
    calc_local(i, lm);
    w_transform_rebuilds++;
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
    if (cache) {
        memcpy(cache->local,&o->lx,sizeof cache->local); cache->parent=o->parent;
        cache->parent_rev=parent_rev; cache->rev++;
    }
}

void obj_matrix(int o, float m[6])
{
    update_matrix(o);
    memcpy(m, W.obj[o].m, sizeof W.obj[o].m);
}

/* New frame for the matrix cache, then refresh every active object. */
void w_transforms(void)
{
    W.frame++;
    for (int i = 0; i < W.nobj; i++)
        if (W.obj[i].alive && W.obj[i].ah)
            update_matrix(i);
}

/* An object moved: its matrix and all its descendants' must be recomputed. */
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

/* Set a world position: convert it into the parent's local space by inverting the parent's 2x2 matrix. */
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

/* Set the world rotation: local = wanted minus the sum of the parents' rotations. */
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
/* Component setters (sprite, colour as 0xAABBGGRR). */
void sr_set_sprite(int sr, int spr) { if (sr >= 0) W.sr[sr].d.spr = spr; }
void sr_set_color(int sr, uint32_t c)
{
    if (sr < 0) return;
    W.sr[sr].d.r = c & 255; W.sr[sr].d.g = (c >> 8) & 255; W.sr[sr].d.b = (c >> 16) & 255; W.sr[sr].d.a = c >> 24;
}

/* TMP FontStyles UpperCase / LowerCase: ASCII + the Polish letters of the credits */
/* TMP UpperCase / LowerCase styles for every script the fonts cover (Latin-1, Latin Extended-A, Greek,
   Cyrillic); 2-byte UTF-8 letters keep their length, so the string is rewritten in place */
/* Upper/lower case of one codepoint. In these Unicode blocks the cases are a fixed distance apart, or alternate (even
 * = upper, odd = lower). */
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

/* Apply case in place to a UTF-8 string; only 1- and 2-byte characters are changed (their length never changes). */
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

/* Set a text component's string: copied into memory the component owns (grown when needed). */
void text_set(int t, const char *s)
{
    if (t < 0 || !s)
        return;
    Text *tx = &W.text[t];
    int n = strlen(s) + 1;
    if (tx->own_cap < n) {
        int cap=n<64?64:n;
        char *next=malloc((size_t)cap);
        if (!next) { w_alloc_fails++; vcpe_observe_fail("text",(uint32_t)cap); return; }
        memcpy(next,s,(size_t)n); free(tx->own); tx->own=next; tx->own_cap=cap;
    } else memmove(tx->own,s,(size_t)n);
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
const uint8_t *w_field_pool(const Script *s);
/* Script fields (the values set in Unity's inspector): find one by name hash. */
const WField *f_get(const Script *s, uint32_t name)
{
    for (int i = 0; i < s->nfield; i++)
        if (s->fields[i].name == name)
            return &s->fields[i];
    return 0;
}

/* First element of an array or struct field: its value is a byte offset into the field pool of the blob the script
 * came from. */
const WField *f_child(const Script *s, const WField *f)
{
    if (!f || (f->kind != F_ARRAY && f->kind != F_STRUCT) || !f->count)
        return 0;
    return (const WField *)(w_field_pool(s) + f->v.off);
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

/* A reference to another object. In a prefab instance it is a prefab-local index, mapped to the world object. */
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

/* Which blob's field pool a script's fields live in: remembered when the script was created (5.6); scripts made
 * some other way are found by address, a prefab's pool if they point inside it, else the scene's. */
const uint8_t *w_field_pool(const Script *s)
{
    if (s->pool)
        return s->pool;
    for (int p = 0; p < NUM_PREFABS; p++) {
        int n;
        const uint8_t *pool = blob_sec(&prefab_blob[p], TAG('F', 'L', 'D', 'S'), &n);
        if (pool && (const uint8_t *)s->fields >= pool && (const uint8_t *)s->fields < pool + n * 16)
            return pool;
    }
    return blob_sec(&scene_blob, TAG('F', 'L', 'D', 'S'), 0);
}
