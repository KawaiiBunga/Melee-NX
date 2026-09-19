# Performance testing

The recovered build is playable at 720p, but sustained 60 FPS remains open.
Existing `PERF` values are periodic averages. Percentiles of those records are
not individual-frame percentiles, and `submit` covers all of Aurora's end-frame
work, including waits for other threads. Small `wait` values alone do not rule
out a GPU bottleneck because presentation runs on a separate worker.

## Repeatable affinity sweep

The default retains the recovered scheduling policy: the pipeline compiler is
pinned to the highest allowed core, and the other threads retain their existing
affinity. To select an experiment, create `sdmc:/switch/melee-nx/perf.cfg` with
one of these lines, then restart the application:

| Setting | Behavior |
|---|---|
| `affinity baseline` | Recovered scheduling policy; also used when the file is absent |
| `affinity main` | Pin only the game thread to the lowest allowed core |
| `affinity split` | Game on the lowest allowed core, FIFO on the next, render on the highest, sharing with the low-priority compiler |

Experiments require at least three allowed cores and log failures without
preventing launch. `THREAD` records report requested and effective masks,
priorities and syscall results. Neither clocks nor audio/cache settings change.
These layouts are candidates until measured, not performance claims.

1. Save the known-good NRO, logs and configuration. Record each NRO's SHA-256;
   after FTP deployment, download it back and compare hashes.
2. Use the same instrumented binary for baseline/main/split. Keep stock clocks,
   stage, fighters, player count, settings and HUD constant. Record those choices.
3. Use a repeatable CPU-controlled match for 120 seconds after loading; run each
   layout three times, alternating baseline and candidate. Test load transitions
   and four-player/effects stress separately.
4. Exit to FTP and save the runtime and boot logs before the next launch. Preserve
   caches; report first encounters separately from warm runs. A run with skipped
   draws does not establish equivalent rendering performance.
5. Check geometry, input, audio and exit behavior as well as frame time. Return to
   `affinity baseline` if the candidate regresses.

## Reading a capture

```bash
python builder/perf-report.py run.log --start 90 --end 210 --json report.json
```

Times are seconds since application start. Pick an interval wholly within the
same scenario on each run. The report prints interval statistics and preserves
the new frame windows in JSON without averaging their quantiles.

- `PERF`: existing interval averages for simulation, end-frame submission,
  begin-frame wait, event pump, UI and logging. `other` is uninstrumented wall
  time, including callbacks; it is not a direct measurement of CPU starvation.
- `FRAMES`: actual frame counts and histogram-derived p50/p95/p99 upper bounds
  over approximately two seconds. Bins are 0.25 ms; `-1` means a percentile falls
  at or beyond 1000 ms. `worst_ms` retains the full duration, `overflow` counts
  those long frames, and `late60` counts intervals exceeding 1/60 second. Minor
  pacing jitter can cross that exact threshold even during otherwise smooth play.
- `STAGES`: game-thread wall-time averages for FIFO drain/end, texture cleanup,
  graphics finalization, UI recording, frame dispatch, OS alarms/CARD delivery,
  and VI retrace callbacks. Dispatch includes any blocking queue/slot operations.
  These do not measure render-worker encoding or GPU execution separately.
- `PIPELINES`: skipped draws and pending/ready pipelines. A seeded descriptor
  database does not mean all shaders are already compiled.
- `AUDIO`: negotiated device frequency, format, channels and buffer frames,
  plus whether the AX mixer was compiled with NEON. No underrun measurement yet.

## Source verification

```bash
python builder/verify-patches.py
```

This reconstructs every patched Melee/Aurora file from the pinned revision and
the SDL backend from `release-3.4.10`, applies the patches in isolation, compares
with the working sources, and reverse-checks the live patches. Reference trees
and their indices are never reset. Dawn/Mesa still require the separately
documented dependencies in [BUILDING.md](BUILDING.md).
