/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Rollback groundwork: snapshots of the game's simulation memory and an on-console determinism
 * self-test (docs/SLIPPI.md, step 3). The patched engine loop (gm_801A4D34 in gmscene.c, from
 * melee-switch-rollback-hooks.patch) calls these around every engine body. With perf.cfg
 * `rollback_selftest 0` (the default) they return at once and change nothing. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start of an engine body; disc_idle is nonzero when no disc request is in flight (a snapshot is
 * only taken or restored then). Returns 1 when this body re-simulates a recorded frame. */
int melee_nx_rb_body_begin(int disc_idle);
/* After the pad renew point. `replay` 0: the game renewed its pads; record them if a test is
 * recording. `replay` 1 (re-simulating): the game skipped its renew; restore the recorded pads. */
void melee_nx_rb_pads(void* lib, unsigned lib_size, void* master, unsigned master_size, int replay);
/* End of an engine body, with Slippi's gameplay checksum of the state it left. Returns 1 to run
 * another body at once (re-simulation, nothing drawn). */
int melee_nx_rb_body_end(int disc_idle, unsigned checksum);

#ifdef __cplusplus
}

#include <cstdint>

namespace melee_nx::rollback {
// Slippi's savestate commands (switch/slippi/slippi_host.cpp): a pool of `count` snapshots. Capture
// keeps the current state under `frame`, in a free slot or else the one holding the oldest frame (a
// frame captured twice reuses its own). Load restores `frame` and then frees every slot; false when
// nothing is kept under it (nothing changes).
void SlotsReset(int count);
bool SlotCapture(std::int32_t frame);
bool SlotLoad(std::int32_t frame);
}  // namespace melee_nx::rollback
#endif
