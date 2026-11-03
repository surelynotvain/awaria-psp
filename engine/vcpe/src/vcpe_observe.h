/* Optional instrumentation with no required link modules. Call from the main thread, never the audio worker. */
#ifndef VCPE_OBSERVE_H
#define VCPE_OBSERVE_H
#include "vcpe_diag.h"
void vcpe_diag_cost(int,uint32_t) __attribute__((weak));
void vcpe_diag_failure(const char *,uint32_t) __attribute__((weak));
void vcpe_adapt_cost(int,uint32_t) __attribute__((weak));
#include "vcpe_plat.h"
static inline uint32_t vcpe_observe_now(void) { return vcpe_time_us32(); }
static inline uint32_t vcpe_observe_begin(void) { return vcpe_diag_cost || vcpe_adapt_cost ? vcpe_observe_now() : 0; }
static inline void vcpe_observe_cost(int d,uint32_t n)
{
    if (vcpe_diag_cost) vcpe_diag_cost(d,n);
    if (vcpe_adapt_cost) vcpe_adapt_cost(d,n);
}
static inline void vcpe_observe_end(int d,uint32_t start) { if (start) vcpe_observe_cost(d,vcpe_observe_now()-start); }
static inline void vcpe_observe_fail(const char *module,uint32_t n) { if (vcpe_diag_failure) vcpe_diag_failure(module,n); }
#endif
