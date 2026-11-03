/* Mutable conservative 2D broadphase. Center-cell hash for small AABBs; large shapes live in a global bucket.
 * Queries return an index bitset, so narrowphase can retain its original deterministic pair order. */
#ifndef VCPE_SPATIAL_H
#define VCPE_SPATIAL_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define VCPE_GRID_HASH 512
typedef struct { float x,y,hx,hy; int cx,cy,next,prev,bucket; } VcpeGridNode;
typedef struct { int head[VCPE_GRID_HASH+1], count, cap; VcpeGridNode *nodes; uint32_t *bits; void *own; } VcpeGrid;
static inline int vcpe_grid_begin(VcpeGrid *g, int count)
{
    if (count<0 || count>1048576) return -1;
    if (count>g->cap) {
        int cap=count+64;
        size_t n=(size_t)cap*sizeof(VcpeGridNode)+(size_t)((cap+31)/32)*4;
        void *p=malloc(n);
        if (!p) return -1;
        free(g->own); g->own=p; g->nodes=p; g->cap=cap;
        g->bits=(uint32_t *)((uint8_t *)p+(size_t)cap*sizeof(VcpeGridNode));
    }
    g->count=count;
    for (int i=0;i<=VCPE_GRID_HASH;i++) g->head[i]=-1;
    for (int i=0;i<count;i++) g->nodes[i].bucket=-1;
    return 0;
}
static inline unsigned vcpe_grid_hash(int x,int y) { return ((uint32_t)x*73856093u ^ (uint32_t)y*19349663u)&(VCPE_GRID_HASH-1); }
static inline void vcpe_grid_set(VcpeGrid *g,int id,float x,float y,float hx,float hy)
{
    VcpeGridNode *n=&g->nodes[id];
    if (n->bucket>=0) {
        if (n->prev>=0) g->nodes[n->prev].next=n->next; else g->head[n->bucket]=n->next;
        if (n->next>=0) g->nodes[n->next].prev=n->prev;
    }
    n->x=x; n->y=y; n->hx=hx; n->hy=hy;
    int b=VCPE_GRID_HASH;
    if (fabsf(x)<4000000 && fabsf(y)<4000000 && hx>=0 && hy>=0 && hx<=2 && hy<=2) {
        n->cx=(int)floorf(x*.25f); n->cy=(int)floorf(y*.25f); b=(int)vcpe_grid_hash(n->cx,n->cy);
    }
    n->bucket=b; n->prev=-1; n->next=g->head[b];
    if (n->next>=0) g->nodes[n->next].prev=id;
    g->head[b]=id;
}
static inline uint32_t *vcpe_grid_query(VcpeGrid *g,int id)
{
    VcpeGridNode *a=&g->nodes[id];
    int words=(g->count+31)/32;
    if (a->bucket==VCPE_GRID_HASH) { memset(g->bits,255,(size_t)words*4); return g->bits; }
    memset(g->bits,0,(size_t)words*4);
    /* Extra margin covers cell-boundary float rounding; no exact overlap rejection here. */
    int x0=(int)floorf((a->x-a->hx-2.001f)*.25f), x1=(int)floorf((a->x+a->hx+2.001f)*.25f);
    int y0=(int)floorf((a->y-a->hy-2.001f)*.25f), y1=(int)floorf((a->y+a->hy+2.001f)*.25f);
    for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++) {
        int b=(int)vcpe_grid_hash(x,y);
        for (int j=g->head[b];j>=0;j=g->nodes[j].next)
            if (g->nodes[j].cx==x && g->nodes[j].cy==y) g->bits[j>>5]|=1u<<(j&31);
    }
    for (int j=g->head[VCPE_GRID_HASH];j>=0;j=g->nodes[j].next) g->bits[j>>5]|=1u<<(j&31);
    return g->bits;
}
static inline int vcpe_grid_next(const VcpeGrid *g,int start)
{
    if (start>=g->count) return g->count;
    int word=start>>5; uint32_t b=g->bits[word]&(~0u<<(start&31));
    for (;;) {
        if (b) { int i=(word<<5)+__builtin_ctz(b); return i<g->count?i:g->count; }
        if ((++word<<5)>=g->count) return g->count;
        b=g->bits[word];
    }
}
static inline void vcpe_grid_free(VcpeGrid *g) { free(g->own); memset(g,0,sizeof *g); }
#endif
