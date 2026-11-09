// What the native deko3d renderer (switch/deko) asks of the application, under the names it was
// written against in KartPadNX. Linked into the NRO only when MELEE_NX_GFX=deko3d.
#include "log_switch.h"
#include "perf_switch.h"

#include <switch.h>

#include <cstdio>

extern "C" void pc_log_line(const char* fmt, ...);

// perf.cfg switches (perf_switch.cpp).
extern "C" int kartpad_opt(const char* key, int fallback) { return melee_nx_opt(key, fallback); }
extern "C" int kartpad_perf_int(const char* key, int fallback) { return melee_nx_perf_int(key, fallback); }

// fflush alone only reaches the logger's RAM buffer; a fatal error needs the tail on the card.
extern "C" void kartpad_log_flush_sync(void) {
    std::fflush(stderr);
    melee_nx_log_flush();
}

// Recorded in the shader-training session journal (shaders.cpp).
extern "C" const char kartpad_build_stamp[] = "melee-nx " __DATE__ " " __TIME__;

// The renderer's worker threads (the uam shader compiler) stay off the game thread's core: a cold
// shader compile occupies its core for hundreds of ms, and sharing it with the game thread was the
// stall melee-nx's own affinity work found (docs/HANDOFF-PERF-AUDIO.md). Core placement of the
// game, FIFO and render threads stays with melee_nx_perf_thread.
extern "C" int kartpad_thread_apply(int role, int index) {
    (void)index;
    u64 allowed = 0;
    if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        return -1;
    }
    int top = -1, count = 0;
    for (int core = 0; core < 64; ++core) {
        if (allowed & (1ull << core)) {
            top = core;
            ++count;
        }
    }
    if (count < 3) {
        return 0; // no spare core to move to
    }
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, top, 1u << top);
    pc_log_line("THREAD deko worker role %d core %d rc %x", role, top, rc);
    melee_nx_prof_register(3);
    return 0;
}
