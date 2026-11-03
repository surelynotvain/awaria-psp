/* VCPE memory for the 32 MB PSP-1000: tell the models apart and use the 4 MB "volatile" partition.
 *
 * A game built with PSP_LARGE_MEMORY gets about 50 MB of heap on a PSP-2000/3000/Street/Go but only about 20 MB on a
 * PSP-1000. Every model also has a 4 MB partition at 0x08400000 that the system lends to the running game
 * (sceKernelVolatileMemLock; commercial games use it too). Whether its contents survive sleep is not verified, so keep
 * only data there that can be read again from the PAK: texture bundles placed with vcpe_bundle_load_at(..., 1) are
 * read again after every resume.
 *
 * Fragmentation is the other PSP-1000 trap: after freeing an 8 MB scene bundle, a 9 MB one may not fit anywhere.
 * vcpe_bundle_slab() (vcpe_gfx.h) gives a range of bundles one fixed block that is reused, so it can't happen. */
#ifndef VCPE_MEM_H
#define VCPE_MEM_H
#include <stdint.h>
/* Native utility handoff. Resume invalidates volatile contents and bumps vcpe_resume_gen (like sleep).
 * Engine bundles reload automatically; game-owned volatile data must be regenerated on generation changes. */
int vcpe_volatile_suspend(void); /* 1 yielded, 0 not locked, -1 error */
int vcpe_volatile_resume(void);  /* 0 reclaimed or no handoff, -1 could not reclaim */
int vcpe_volatile_suspended(void);

/* 1 on a 32 MB console (PSP-1000). Measured once with vcpe_heap_kb(), so call it early, before big allocations */
int vcpe_mem_32mb(void);
/* the 4 MB volatile partition (cached address, 64-byte aligned), locked for the game on the first call and kept.
 * 0 when the system refuses it (another program holds it). *size gets its size in bytes */
uint8_t *vcpe_volatile_mem(uint32_t *size);
/* heap in use and the most it ever was, in KB (mallinfo) */
int vcpe_mem_used_kb(void);
int vcpe_mem_peak_kb(void);

/* 4.0: the largest block malloc can give now (= vcpe_heap_kb). Compared with the free total it shows fragmentation:
 * a heap 40% free that can't fit a 2 MB block */
int vcpe_mem_largest_free_kb(void);
/* Cheap headroom estimate: unallocated user-partition memory plus free space already held by malloc.
 * Includes fragmented space, so this is not a guarantee that one allocation of that size will fit. */
int vcpe_mem_available_kb(void);
/* 4.0: one standard memory line for logs, the same in every game ("mem level 3: used 14210 KB (peak 15002) largest
 * free 5120 KB, holes 830 KB, sprites 2048 KB (31 loads, 0 failed), dlist peak 96 KB"). Parts of modules the game
 * does not link are left out. Print it at scene switches. Probes the heap: not every frame */
int vcpe_mem_report(char *out, int n, const char *tag);

/* 4.0: an arena, for data loaded once per scene (level tables, maps, scripts): allocations are a pointer bump, and
 * the whole scene's data goes at once with vcpe_arena_reset at the next scene switch. Nothing fragments, however
 * the sizes change from level to level. mem 0: the arena allocates size bytes itself (vcpe_arena_free_all frees
 * them); else the game's block, e.g. the volatile partition for data it can read again after a sleep */
typedef struct { uint8_t *base, *own; uint32_t size, used, peak; } VcpeArena;
int vcpe_arena_init(VcpeArena *a, void *mem, uint32_t size);
/* align: a power of two (values below 4 become 4); aligns the address even in caller-supplied memory.
 * Returns 0 without moving the cursor on bad alignment, overflow or exhaustion. */
void *vcpe_arena_alloc(VcpeArena *a, uint32_t size, uint32_t align);
static inline void vcpe_arena_reset(VcpeArena *a) { a->used = 0; }
static inline uint32_t vcpe_arena_mark(const VcpeArena *a) { return a->used; }
static inline void vcpe_arena_release(VcpeArena *a, uint32_t mark) { if (mark <= a->used) a->used = mark; }
void vcpe_arena_free_all(VcpeArena *a);

#endif
