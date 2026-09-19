#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loaded once before Aurora starts its workers. Missing file selects baseline. */
void melee_nx_perf_init(void);
void melee_nx_perf_thread(const char* name);
uint64_t melee_nx_perf_now(void);
enum MeleePerfStage {
    MELEE_PERF_FIFO, MELEE_PERF_TEXTURE, MELEE_PERF_FINISH,
    MELEE_PERF_UI, MELEE_PERF_DISPATCH, MELEE_PERF_ALARMS,
    MELEE_PERF_RETRACE, MELEE_PERF_STAGE_COUNT
};
/* All timing calls below belong to the game thread, never a render callback. */
void melee_nx_perf_stage(enum MeleePerfStage stage, uint64_t elapsed_ns);
void melee_nx_perf_frame(uint64_t elapsed_ns);

#ifdef __cplusplus
}
#endif
