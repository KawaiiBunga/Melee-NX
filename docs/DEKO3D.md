# Native deko3d renderer

melee-nx draws with a native deko3d adapter ported from [KartPadNX](https://github.com/KawaiiBunga/KartPadNX).
It is the only graphics build: the earlier Dawn Vulkan → Mesa NVK path was removed on November 9, 2026.

```text
Game C → Aurora GX → Dawn WebGPU (Null backend: object handles only)
                        │ __wrap_wgpu* link-time wrappers (switch/deko/wrap-symbols.txt)
                        ▼
              switch/deko: replays the recorded frame on deko3d → libnx NWindow
                        Tint (WGSL → GLSL) → uam (GLSL → DKSH), cached
```

## Status

**Runs on the console** (deployed through the launcher since November 7, 2026). The table below is the
record of the first native build, before any hardware run; the NVK comparisons refer to the removed path.

| Check | Result |
|---|---|
| Whole project built with the tools in `E:\tools` (devkitPro GCC 16 for the game C, Clang for C++, CMake, Ninja): configure, 1,543 objects (game, Aurora, RmlUi, the renderer, uam), link, NRO | 0 errors. `melee.nro` 70.9 MB; `melee.elf` 39 MB (the NVK build's is 221 MB) |
| Dawn built from this checkout with `DawnOptionsDeko.cmake` (Null on, Vulkan off; `webgpu_dawn` and the Tint GLSL writer, WGSL reader and inspector, with the `prepare-deko.py` edits), and that build linked into the NRO | yes. A first link against KartPadNX's prebuilt Dawn (same pin) also worked |
| Link inspection: `aurora::gx::render` calls `__wrap_wgpuRenderPassEncoderSetImmediates` and `__wrap_wgpuRenderPassEncoderSetBindGroup`; `__wrap_malloc`, `melee_nx_prof_anchor`, `melee_nx_ensure_controller` present | yes; 0 Vulkan or NVK symbols |
| The renderer's present and text shaders compiled to DKSH by `uam.exe` | yes |
| `builder/integrate-deko.py` on the live (patched) melee-pc Aurora: every anchor found, second run a no-op, balanced `#if`/`#endif`; `prof-report`, `deko-training` and perf.cfg-table tests | pass |
| `build-graphics.sh`, `build-melee.sh` parsed with busybox `ash` | pass (`docker.sh` and `patch-common.sh` use bash arrays, which `ash` cannot parse; their new lines are plain) |
| `docker.sh` and the Docker image, `verify-patches.py` (needs Git), the NVK build after these changes | **not run** |
| Rendering, timing, memory, the GPU timeout below | **not run** |

The first link failed twice and was fixed: archive paths with a space (this checkout's own path) were not
quoted in `Deko3D.cmake`, and `execvp`, `waitpid` and `sysconf`, which the NVK build gets from its toolchain
and Mesa, were missing (`switch/deko/host/posix_stubs_switch.c`, weak).

KartPadNX's figures are the reason to try it, not a promise for Melee: on its hardware the warm start of
~3,600 recorded pipelines took 3.7–7.7 s (108 s before its cache fixes), a pipeline whose shaders are
cached costs about 1 ms instead of NVK's 25–40 ms here, and no Mesa is linked. Melee's shaders are
different. On the console without a seed (second run, NRO `2E032447…`), a pipeline cost 287 ms on average
(Tint plus uam), only 553 of the 6,001 seeded recipes were built, and the worst frame took 9.7 s; the
precompiled seed (below) is the fix.

## Build

```bash
bash builder/docker.sh graphics all      # Dawn Null + Tint GLSL writer -> build/dawn-switch-deko, SDL
bash builder/docker.sh melee all         # stages Aurora, builds build/switch-deko/melee.nro
```


| CMake option | Default | Purpose |
|---|---|---|
| `MELEE_NX_DEKO_GPU_FLUSHES` | ON | Compile the v9 submission policy, inactive until perf.cfg `gpu_steady_v9 1` |
| `MELEE_NX_DEKO_GPU_BREADCRUMBS` | ON | GPU-written completion markers for that policy (about 9 MB of RAM) |
| `MELEE_NX_DEKO_GPU_FLUSH_INTERVAL` | 32 | Operations per diagnostic submit batch (1-64) |
| `MELEE_NX_DEKO_DEBUG` / `_GPU_CHECKPOINTS` | OFF | Checked deko3d library; per-operation GPU waits (slow) |

Pass extras with `MELEE_NX_CMAKE_ARGS`, e.g. `MELEE_NX_CMAKE_ARGS=-DMELEE_NX_DEKO_GPU_BREADCRUMBS=OFF`.

### Checking the build without Docker

The configure and the renderer library were built natively on Windows with the tools in `E:\tools`
(devkitPro with GCC 16, CMake, Ninja, Clang), without touching them. What that setup needed, if you
repeat it: a scratch copy of devkitPro's `cmake/` with the `CMAKE_HOST_WIN32` refusal removed in
`dkp-initialize-path.cmake` (junctions to `devkitA64`, `libnx`, `portlibs`, and a `tools/bin` that
adds `uam.exe` and a `bin2s`; neither ships in `E:\tools\devkitPro`), `-DCMAKE_MAKE_PROGRAM`, the
fetched sources from `build/switch/_deps` as `-DFETCHCONTENT_SOURCE_DIR_*` with
`FETCHCONTENT_FULLY_DISCONNECTED=ON`, a minimal `zstdConfig.cmake`, and the prebuilt Null Dawn as
`MELEE_DAWN_BUILD`. The portlibs' pkg-config files hardcode `/opt/devkitpro`, which has to be rewritten in
`build.ninja`. None of that is needed in the Docker image.

## Where it plugs in

| Piece | Where | Notes |
|---|---|---|
| Adapter, device, memory, shader cache | `switch/deko/` | From KartPadNX (SwitchWaker lineage; licenses in `THIRD_PARTY.md`). Symbols keep their `kartpad_*` names so diffs against KartPadNX stay readable |
| `wgpuRenderPassEncoderSetImmediates` | `switch/deko/backend_commands.cpp` | **New.** melee-pc's Aurora passes per-draw `DrawImmediateData` (64 bytes) as WebGPU immediates; KartPadNX's Aurora does not. Each distinct value is copied into a 256-byte slot of the frame's stream memory and bound as the uniform block Tint emits (slot 15); if the stream is full, one block is rewritten with `dkCmdBufPushConstants` |
| Shader modules and render pipelines | `switch/deko/backend_pipelines.cpp` | **Changed from KartPadNX.** WGSL modules and render pipelines are the adapter's own handles, never Dawn objects: nothing downstream used Dawn's copies, and Dawn's WGSL parse, validation and reflection cost 12–33 ms per module on the console (second run). Non-WGSL modules and foreign handles still go to Dawn |
| Shader journal writes | `switch/deko/shaders.cpp` | **Changed from KartPadNX.** `dksh_local.bin`, the session journal and the WGSL captures are queued and written by the uam worker with no lock held, only while no draw waits for a compile. Before, they were written under `g_cacheMutex`, which every pipeline build takes for a cache hit, and an SD write can stall for hundreds of ms |
| uam shader compiler | `switch/uam/` | Vendored, unmodified from KartPadNX |
| Host services | `switch/deko/host/` | perf.cfg, log flush, worker core placement, NVIDIA error capture |
| Aurora seams | `builder/integrate-deko.py` | Edits a **staged copy** (`build/aurora-deko`) behind `AURORA_GFX_DEKO3D`, so `ref/melee-pc` stays at its pinned, patched state and `verify-patches.py` is unaffected |
| Dawn | `switch/cmake/DawnOptionsDeko.cmake`, `builder/prepare-deko.py` | Null backend, no Vulkan, Tint GLSL writer on; two options added to the GLSL writer for deko3d's `[0, w]` depth and upper-left origin |

The Aurora seams, in `integrate-deko.py`: no `wgpu::Surface` (the present pass draws into an offscreen
texture and `kartpad_deko_present` blits it to the swapchain); the Null adapter; depth peeks use the
adapter's depth copy instead of a compute shader; `pipeline_cache_deko.db` and `dawn_null_deko_cache.db`
keep the backends' bookkeeping apart; the Dawn provider advertises Null instead of Vulkan.

Not carried over from KartPadNX, because melee-pc's Aurora already differs: its uniform-layout rewrite
of `array_start` (melee packs it as three `vec4u` immediates), its shader-module retention fix (melee's
`create_pipeline` releases the module), and its frame-interpolation present path.

## Seeds and first run

A shader the console has to compile costs 40–230 ms in uam's one compiler thread, so an unseeded
session shows late geometry and long loads. `builder/deko-precompile` removes that: a host tool that runs
the console's own code (Aurora's GX generator from the staged copy, `shader_translate.cpp`, the vendored
uam, `dksh_file.cpp`) over every current recipe in `initial_pipeline_cache.db` and writes the DKSH seed.
All 6,001 recipes take about 35 s in six shards on a desktop: 2,572 distinct shaders, 4,470 stage records,
14 MB, 0 Tint or uam failures.

```bash
cmake -S builder/deko-precompile -B build/deko-precompile -G Ninja -DDAWN_HOST_BUILD=<host Dawn build> \
  -DDAWN_GEN_INCLUDE=<any Dawn build>/gen/include -DDEPS_DIR=build/switch-deko/_deps
ninja -C build/deko-precompile
for i in 0 1 2 3 4 5; do build/deko-precompile/deko-precompile shard$i.bin \
  ref/melee-pc/resources/initial_pipeline_cache.db --shard $i/6 & done; wait
python builder/deko-training.py pack-dksh initial_dksh_cache.bin shard*.bin
```

The host Dawn build needs Tint's GLSL writer with the `prepare-deko.py` edits. On llvm-mingw, Abseil's
`time_zone_info.cc` does not compile (`O_NONBLOCK`; build it with `-DO_NONBLOCK=0`), and every Abseil
target Tint uses must be archived, not only compiled.

More recipes than the seed's own: melee-pc's database also holds 5,880 version-13 rows of 2,772 bytes,
4 bytes short of the current layout. With 4 zero bytes inserted at offset 2,728 (every current row holds
zeros there), 1,638 of them become byte-identical to current rows; the other 4,261 are new recipes. A
console's `pipeline_cache_deko.db` adds what that console created on demand. All three together gave
3,401 shaders; pass `--uber` to also compile the uber shader (about 30 s of uam on a PC, minutes on the
console, so it must come from the seed). On the console, the second hardware run's remaining hitches
(0.3–2.1 s) were exactly the 25 shaders no recipe list had: each draw waits for uam.

Copy the result to `sdmc:/switch/melee-nx/initial_dksh_cache.bin`; no rebuild is needed. A file written
by another build of uam, Tint's post-pass or Aurora's GX config version is refused as stale, so
**re-run the tool after any change to those or to the staged Aurora**. Shaders the seed misses are still
compiled on the console and recorded in `Cache/deko/dksh_local.bin`; `pack-dksh` merges such files into
the seed.

## Risks and unknowns

- **GPU timeout at scene loads.** KartPadNX's vanilla build hit NVIDIA channel timeout 2348-0006 on
  course loads. Its cause was never found; per-operation waits (v3), asynchronous flushes (v6) and
  progress markers (v9) each passed limited tests, and its Retro Rewind build runs a "transition guard"
  that makes GPU work synchronous during course loads. melee-nx has **no guard** (it would need a signal
  from the game for scene changes), and `kartpad_gpu_transition_guard` is absent, so work is never
  synchronous. If a stage load times out, set `gpu_steady_v9 1` in perf.cfg (about 80 submits per frame in
  KartPadNX's test, +6-8 ms of recording), and send the `[gpu-submit]` and `[gpu-breadcrumb]` log lines.
- **Shaders Tint or uam may refuse.** Aurora's GX shaders go through the same path as in KartPadNX, but
  melee-pc's Aurora is newer (immediates, different TEV code) and the RmlUi and ImGui pipelines are
  untested. A refusal is a `[dk]` FATAL naming the stage.
- **Vertex formats.** The adapter accepts `Float32x2/3/4` and `Unorm8x4`; the pipelines melee-pc creates
  use those (GX pulls vertices from a storage buffer).
- **`texture_cmpr_bc1` is off.** It keeps CMPR textures compressed as BC1 (an eighth of the RGBA8
  bytes, no decode of the pixels) but changes how every CMPR texture is sampled: GX gives the transparent
  texel the midpoint color, BC1 black. Try it only after the default path renders correctly.
- **Memory.** KartPadNX saw `std::bad_alloc` when every Dawn shader module stayed alive. melee-pc does not
  keep them, but watch `heap never used` in the `[dk] frames` lines on the first long session.

## First hardware test

1. `perf.cfg`: `opt_safe 1` is not needed; leave it empty. Boot; the log must show `[dk]` device, swapchain
   and cache lines and no FATAL. The title screen is the first real test of the present path.
2. Play a match. Note `[dk] pipeline builds` (ms per pipeline), `skipped draws`, FPS, `heap never used`.
3. Load a stage twice, then quit cleanly; keep `Cache/deko/dksh_local.bin`.
4. With a seed in place, repeat; then try `texture_cmpr_bc1 1`. Compare runs at the same clocks, several
   each: a few percent is below what one run can show.
