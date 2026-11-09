#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loaded once before Aurora starts its workers. Missing file selects baseline. */
void melee_nx_perf_init(void);
void melee_nx_perf_thread(const char* name);
/* A numeric perf.cfg value (decimal or 0x hex), or `fallback` when the key is absent. */
int melee_nx_perf_int(const char* key, int fallback);
/* Like melee_nx_perf_int for an optimization switch: `opt_safe 1` turns it off unless the file sets
   it, and the first call per key logs "[opt] key=value (source)". */
int melee_nx_opt(const char* key, int fallback);
/* Sampling profiler (prof_switch.cpp, perf.cfg prof_seconds). Roles: 0 game, 1 FIFO, 2 render,
   3 shader compile. Register from the thread itself once it is placed. */
void melee_nx_prof_init(void);
void melee_nx_prof_register(int role);
uint64_t melee_nx_perf_now(void);
enum MeleePerfStage {
    MELEE_PERF_FIFO,
    MELEE_PERF_TEXTURE,
    MELEE_PERF_FINISH,
    MELEE_PERF_UI,
    MELEE_PERF_DISPATCH,
    MELEE_PERF_ALARMS,
    MELEE_PERF_RETRACE,
    MELEE_PERF_STAGE_COUNT
};
/* All timing calls below belong to the game thread, never a render callback. */
void melee_nx_perf_stage(enum MeleePerfStage stage, uint64_t elapsed_ns);
void melee_nx_perf_frame(uint64_t elapsed_ns);

#ifdef __cplusplus
}
#endif
