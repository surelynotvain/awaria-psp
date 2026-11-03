#ifndef VCPE_DIAG_H
#define VCPE_DIAG_H
#include <stdint.h>
enum { VCPE_COST_CPU, VCPE_COST_GPU, VCPE_COST_IO, VCPE_COST_AUDIO, VCPE_COST_RENDER, VCPE_COST_COUNT };
typedef struct {
    uint32_t samples, average_us, p95_us, p99_us, worst_us, failures;
    uint32_t last_cost[VCPE_COST_COUNT];
} VcpeDiagStats;
void vcpe_diag_cost(int domain, uint32_t amount); /* main-thread us; AUDIO is an underrun count */
void vcpe_diag_failure(const char *module, uint32_t requested_bytes);
void vcpe_diag_frame(uint32_t elapsed_us);
void vcpe_diag_loading(int active);
void vcpe_diag_reset(void);
void vcpe_diag_stats(VcpeDiagStats *out); /* rolling 256 frames, sorted on request, no heap allocations */
int vcpe_diag_line(char *out, int n);
int vcpe_diag_failure_line(int slot, char *out, int n); /* 0 when the fixed 16-module registry has no such slot */
#endif
