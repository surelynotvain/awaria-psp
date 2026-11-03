/* VCPE memory for the 32 MB PSP-1000 (see vcpe_mem.h). */

/* HOW THIS FILE WORKS
 *
 * The PSP-1000 has 32 MB of RAM, the later models 64 MB. A homebrew game gets roughly 20 MB of heap on the first and
 * 50 MB on the others. Measuring the largest block malloc can give (vcpe_heap_kb) tells them apart.
 *
 * Every model also has a 4 MB "volatile" partition the system lends to the running game. It is locked once here and
 * kept. Its contents may not survive sleep, so only data that can be read again from the PAK should live there.
 *
 * The arena and the report at the bottom are tools against fragmentation: when blocks of different sizes are freed
 * and allocated level after level, the free memory ends up in pieces too small for the next big block. */
#include <pspkernel.h>
#include <pspsuspend.h>
#include <pspsysmem.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include "vcpe_sys.h"
#include "vcpe_mem.h"

/* Measured once and remembered: later measurements would see the game's own allocations. */
int vcpe_mem_32mb(void)
{
    static int small = -1;
    if (small < 0)
        small = vcpe_heap_kb() < 36000;
    return small;
}

/* Lock the volatile partition on the first call (TryLock: fails instead of waiting if someone else has it). */
static int volatile_tried, volatile_paused;
static void *volatile_ptr;
static int volatile_size;
uint8_t *vcpe_volatile_mem(uint32_t *size)
{
    if (!volatile_tried && !volatile_paused) {
        volatile_tried = 1;
        int st = vcpe_step_begin("Locking volatile memory (4096 KB)");
        if (sceKernelVolatileMemTryLock(0, &volatile_ptr, &volatile_size) < 0 || !volatile_ptr || volatile_size <= 0) {
            volatile_ptr = 0;
            volatile_size = 0;
        }
        vcpe_step_end(st, volatile_ptr != 0);
    }
    if (size)
        *size = volatile_paused ? 0 : (uint32_t)volatile_size;
    return volatile_paused ? 0 : (uint8_t *)volatile_ptr;
}

/* Firmware utilities need this partition too. Yield it before InitStart, reclaim after shutdown.
 * As with sleep, contents are lost; the generation bump makes gfx reload volatile bundles. */
int vcpe_volatile_suspend(void)
{
    if (volatile_paused) return -1;
    if (!volatile_ptr) return 0;
    if (sceKernelVolatileMemUnlock(0) < 0) return -1;
    volatile_paused = 1;
    return 1;
}
int vcpe_volatile_resume(void)
{
    if (!volatile_paused) return 0;
    volatile_paused = 0; volatile_tried = 0;
    vcpe_resume_gen++;
    return vcpe_volatile_mem(0) ? 0 : -1;
}
int vcpe_volatile_suspended(void) { return volatile_paused; }

/* mallinfo().uordblks = bytes currently allocated from the heap; the peak is tracked on every query. */
static int peak;
int vcpe_mem_used_kb(void)
{
    struct mallinfo mi = mallinfo();
    if ((int)mi.uordblks > peak)
        peak = mi.uordblks;
    return mi.uordblks / 1024;
}

int vcpe_mem_peak_kb(void)
{
    vcpe_mem_used_kb();
    return peak / 1024;
}

/* ------------------------------------------------------------------ 4.0 */
int vcpe_mem_largest_free_kb(void) { return vcpe_heap_kb(); }
int vcpe_mem_available_kb(void)
{
    struct mallinfo mi = mallinfo();
    return (int)(((uint32_t)sceKernelTotalFreeMemSize() + (uint32_t)mi.fordblks) / 1024);
}

/* An arena hands out memory by moving a pointer forward; nothing is freed one by one, the whole arena is reset at
 * once. */
int vcpe_arena_init(VcpeArena *a, void *mem, uint32_t size)
{
    a->own = 0;
    if (!mem && size && !(mem = a->own = memalign(64, size)))
        size = 0;
    a->base = mem;
    a->size = size;
    a->used = a->peak = 0;
    return mem ? 0 : -1;
}

/* Align the actual address, including when the caller supplies an unaligned block. Check with subtraction so
 * neither cursor rounding nor size addition can wrap around on the PSP's 32-bit pointers. */
void *vcpe_arena_alloc(VcpeArena *a, uint32_t size, uint32_t align)
{
    if (align < 4)
        align = 4;
    if (!a->base || (align & (align - 1)) || a->used > a->size)
        return 0;
    uintptr_t addr = (uintptr_t)a->base + a->used;
    uint32_t pad = (uint32_t)(-addr & (uintptr_t)(align - 1));
    if (pad > a->size - a->used || size > a->size - a->used - pad)
        return 0;
    uint32_t at = a->used + pad;
    a->used = at + size;
    if (a->used > a->peak)
        a->peak = a->used;
    return a->base + at;
}

void vcpe_arena_free_all(VcpeArena *a)
{
    free(a->own);
    a->own = a->base = 0;
    a->size = a->used = 0;
}

/* modules a game may not link: weak, read only when present */
/* Weak references: the report reads these only if the game links the module that defines them. The address of a
 * missing weak symbol is 0, which is what the &name checks below test. */
extern int vcpe_spr_loads __attribute__((weak)), vcpe_spr_fails __attribute__((weak));
int vcpe_spr_resident_kb(void) __attribute__((weak));
extern int vcpe_stat_reload_fail __attribute__((weak)), vcpe_stat_dlist_peak_kb __attribute__((weak));
extern int vcpe_stat_dlist_full __attribute__((weak));

/* One line describing memory: in use, peak, largest free block, free memory inside the heap ("holes": free but
 * possibly fragmented), then the sprite cache, display list and failed reloads when those modules are linked. */
int vcpe_mem_report(char *out, int n, const char *tag)
{
    struct mallinfo mi = mallinfo();
    int used = mi.uordblks / 1024, holes = mi.fordblks / 1024, largest = vcpe_heap_kb();
    if (used * 1024 > peak)
        peak = used * 1024;
    int len = snprintf(out, n, "mem %s: used %d KB (peak %d) largest free %d KB, holes %d KB", tag ? tag : "", used,
                       peak / 1024, largest, holes);
    if (vcpe_spr_resident_kb && len < n)
        len += snprintf(out + len, n - len, ", sprites %d KB (%d loads, %d failed)", vcpe_spr_resident_kb(),
                        &vcpe_spr_loads ? vcpe_spr_loads : 0, &vcpe_spr_fails ? vcpe_spr_fails : 0);
    if (&vcpe_stat_dlist_peak_kb && len < n)
        len += snprintf(out + len, n - len, ", dlist peak %d KB%s", vcpe_stat_dlist_peak_kb,
                        &vcpe_stat_dlist_full && vcpe_stat_dlist_full ? " FULL" : "");
    if (&vcpe_stat_reload_fail && vcpe_stat_reload_fail && len < n)
        len += snprintf(out + len, n - len, ", %d reloads failed", vcpe_stat_reload_fail);
    return len;
}
