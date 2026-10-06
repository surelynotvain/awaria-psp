/* A* on the AstarPath grid graph of the scene (2D, 8 neighbours, corner cutting). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "w_path.h"
#include "w_world.h"
#include "w_phys.h"

static int gw, gh;
static float ox, oy, ns;
static uint8_t *walk;
static float *gcost;
static int16_t *came;
static uint8_t *state;   /* 0 new 1 open 2 closed */
static int *heap;
static int heapn;
static float *fcost;

void path_free(void)
{
    free(walk); free(gcost); free(came); free(state); free(heap); free(fcost);
    walk = 0; gcost = 0; came = 0; state = 0; heap = 0; fcost = 0;
    gw = gh = 0;
}

void path_build(void)
{
    path_free();
    if (!W.has_grid)
        return;
    const WGrid *g = &W.grid;
    ns = g->node;
    gw = (int)(g->w / ns + 0.5f);
    gh = (int)(g->h / ns + 0.5f);
    if (gw <= 0 || gh <= 0 || gw * gh > 20000) { gw = gh = 0; return; }
    ox = g->cx - gw * ns * 0.5f;
    oy = g->cy - gh * ns * 0.5f;
    int n = gw * gh;
    walk = malloc(n);
    gcost = malloc(sizeof(float) * n);
    fcost = malloc(sizeof(float) * n);
    came = malloc(sizeof(int16_t) * n);
    state = malloc(n);
    heap = malloc(sizeof(int) * n);
    float r = g->diameter * 0.5f;
    for (int j = 0; j < gh; j++)
        for (int i = 0; i < gw; i++) {
            float x = ox + (i + 0.5f) * ns, y = oy + (j + 0.5f) * ns;
            walk[j * gw + i] = phys_overlap_circle(x, y, r, (int)g->mask, 0) < 0;
        }
}

static int node_at(float x, float y)
{
    int i = (int)floorf((x - ox) / ns), j = (int)floorf((y - oy) / ns);
    if (i < 0) i = 0; if (i >= gw) i = gw - 1;
    if (j < 0) j = 0; if (j >= gh) j = gh - 1;
    int best = j * gw + i;
    if (walk[best])
        return best;
    /* nearest walkable node */
    float bd = 1e30f;
    best = -1;
    for (int k = 0; k < gw * gh; k++) {
        if (!walk[k]) continue;
        float nx = ox + (k % gw + 0.5f) * ns, ny = oy + (k / gw + 0.5f) * ns;
        float d = (nx - x) * (nx - x) + (ny - y) * (ny - y);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

static void hpush(int n)
{
    int i = heapn++;
    heap[i] = n;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (fcost[heap[p]] <= fcost[heap[i]]) break;
        int t = heap[p]; heap[p] = heap[i]; heap[i] = t;
        i = p;
    }
}

static int hpop(void)
{
    int top = heap[0];
    heap[0] = heap[--heapn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < heapn && fcost[heap[l]] < fcost[heap[m]]) m = l;
        if (r < heapn && fcost[heap[r]] < fcost[heap[m]]) m = r;
        if (m == i) break;
        int t = heap[m]; heap[m] = heap[i]; heap[i] = t;
        i = m;
    }
    return top;
}

int path_find(float sx, float sy, float tx, float ty, float *out, int max)
{
    if (!gw)
        return 0;
    int s = node_at(sx, sy), t = node_at(tx, ty);
    if (s < 0 || t < 0)
        return 0;
    int n = gw * gh;
    memset(state, 0, n);
    heapn = 0;
    gcost[s] = 0;
    float txp = ox + (t % gw + 0.5f) * ns, typ = oy + (t / gw + 0.5f) * ns;
    fcost[s] = hypotf(sx - txp, sy - typ);
    came[s] = -1;
    state[s] = 1;
    hpush(s);
    int found = 0;
    static const int di[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (heapn) {
        int c = hpop();
        if (state[c] == 2) continue;
        state[c] = 2;
        if (c == t) { found = 1; break; }
        int ci = c % gw, cj = c / gw;
        for (int k = 0; k < 8; k++) {
            int ni = ci + di[k], nj = cj + dj[k];
            if (ni < 0 || nj < 0 || ni >= gw || nj >= gh) continue;
            int nn = nj * gw + ni;
            if (!walk[nn] || state[nn] == 2) continue;
            if (k >= 4) {
                /* cutCorners: a diagonal needs at least one free side */
                int a = cj * gw + ni, b = nj * gw + ci;
                if (!walk[a] && !walk[b]) continue;
            }
            float step = k >= 4 ? 1.41421356f * ns : ns;
            float g = gcost[c] + step;
            if (state[nn] == 0 || g < gcost[nn]) {
                gcost[nn] = g;
                came[nn] = c;
                float nx = ox + (ni + 0.5f) * ns, ny = oy + (nj + 0.5f) * ns;
                fcost[nn] = g + hypotf(nx - txp, ny - typ);
                state[nn] = 1;
                hpush(nn);
            }
        }
    }
    if (!found)
        return 0;
    /* walk back, then reverse into out (node centres) */
    int cnt = 0;
    for (int c = t; c >= 0; c = came[c])
        cnt++;
    if (cnt > max) cnt = max;
    int k = cnt - 1;
    int idx = 0;
    int nodes[256];
    for (int c = t; c >= 0 && idx < 256; c = came[c])
        nodes[idx++] = c;
    for (int i = 0; i < cnt; i++) {
        int c = nodes[idx - 1 - i];
        out[i * 2] = ox + (c % gw + 0.5f) * ns;
        out[i * 2 + 1] = oy + (c / gw + 0.5f) * ns;
    }
    (void)k;
    return cnt;
}
