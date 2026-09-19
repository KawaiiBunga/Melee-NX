# Porting notes — melee-pc → Switch (melee-nx)

> Latest implementation and measurement status:
> [HANDOFF-PERF-SWEEP.md](HANDOFF-PERF-SWEEP.md). Older dated checkpoints below
> remain as the diagnosis history.

## Architecture

melee-pc is a **native C port** (not a translator/JIT). The game C code runs directly
as compiled aarch64 — no fiber scheduler, no guest memory mapping, no indirect
dispatch. This makes it significantly simpler to port than KartPad-NX.

Rendering path: **game C → Aurora GX → Dawn WebGPU → Vulkan → NVK → libnx NWindow**
(identical to KartPad-NX).

## Key differences from melee-pc's Linux build

### 1. GCC required for C, Clang for C++
`scalar_storage_order("big-endian")` is a GCC-only extension used on all disc structs.
Solution: `SwitchGCC.cmake` uses `aarch64-none-elf-gcc` for C files and Clang for C++.
Aurora and its dependencies compile fine with either.

### 2. No -no-pie / -Ttext-segment=0x10000000 — and no MEM1 fixed-address mmap either
`switch/CMakeLists.txt` builds `melee_game`/`melee` from globbed `ref/melee-pc/src/**`
sources directly; it never `add_subdirectory()`s or reads melee-pc's own
`CMakeLists.txt`. That file's `-no-pie -Wl,-Ttext-segment=0x10000000` (used on Linux
to keep MEM1 below 4GB for 32-bit disc pointer slots) is simply never invoked for the
Switch build — there is nothing to strip.

**CORRECTED 2026-09-18 by hardware.** The paragraphs that used to sit here said
MEM1 needed no special handling on Switch, because `pc_encode_dp()`/`pc_resolve_dp()`
fall back to a `0x02000000 | index` indirection table for pointers that do not fit in
32 bits. That reasoning was wrong, and the port aborted on the first archive it parsed:

```
archive.c[18] : pointer 0x1f630c3ec0 does not fit a 32-bit disc slot
```

The ext-pointer table only covers pointers written through `DP_SET`. Archive-internal
relocation never goes near it — `Locate()` in `src/sysdolphin/baselib/archive.c` adds
`(u32)(uintptr_t)archive->data` to each slot with raw 32-bit arithmetic, and so do
axdriver's three table fixups and particle.c's three. Seven sites in total, all of which
silently truncate an address above 4GB. So MEM1 genuinely does have to be addressable in
32 bits.

It also cannot be placed low on Horizon. libnx hands out heap far above 4GB, and
`svcMapMemory` only accepts a destination inside the kernel's stack region, which a
39-bit address space places below 4GB with probability under one percent.

**Resolution: the MEM1 4GB window.** A slot now means "offset within whatever 4GB window
MEM1 landed in", and `pc_resolve_dp` restores the high half. `AllocMEM1` gained a
`__SWITCH__` branch that guarantees MEM1 does not straddle a 4GB boundary and starts at
or above `0x03000000` — clear of ARAM offsets (below `0x01000000`) and of the
`0x02xxxxxx` escape prefix. The seven raw relocation sites need no changes at all, since
within one window only the low 32 bits ever differ.

What *did* need changing: **70 sites that cast a disc slot straight to a pointer** with
`(T*)(uintptr_t)`, bypassing `pc_resolve_dp`. These worked by accident while MEM1 was
below 4GB. They are now `DP(T, slot)`, which is also the identity for ARAM offsets and
so is correct on both branches of every `PC_IS_ARAM_ADDR` test.

**The rule for all future work: never cast a disc slot to a pointer directly; use
`DP(T, slot)`.**

Patches: `melee-switch-disc-ptr-window.patch`, `aurora-switch-mem1-window.patch`.
Note that `switch/src/mem1_switch.c` was deleted earlier in the project for trying to
`mmap(..., MAP_FIXED)` at 0x80000000 — that deletion was still correct (devkitA64 has no
`<sys/mman.h>`), it just was not the whole story.

**Still true, and still worth watching:**
- The 65536-slot ext-pointer table is a linear-scan-on-insert array (see
  `pc_register_ext_ptr` in `src/pc/os.c`). It is only hit when a pointer outside MEM1's
  window is stored in a slot, so it should stay a one-time cost per loaded archive — but
  hitting the 65536 cap aborts (`pc_disc_ptr_overflow`). If it becomes a problem, raise
  `PC_MAX_EXT_PTRS` or make it a hash map.
- libnx's default heap sizing has proven large enough for MEM1 (96 MB) + ARAM (16 MB) +
  Dawn/Aurora + the game working set: the console reports 3189 MiB total. Note that
  `AllocMEM1`'s retry loop can transiently hold up to 4 rejected 96 MB blocks while
  searching for a usable window; a fresh heap has always answered on the first attempt.

### 2b. melee-pc's aurora fork has drifted from KartPad-NX's — patches were rewritten
`ref/melee-pc/extern/aurora` is melee-pc's own vendored aurora checkout, not the same
snapshot KartPad-NX built against (different namespace style in `BackendBinding.cpp`,
a simpler `create_window()` that already sets the external-graphics-context property
generically, and a `DawnCacheDeviceDescriptor` construction that's already past the API
shape KartPad's `aurora-switch-dawn-api.patch` guarded against). Applying KartPad-NX's
three aurora patches verbatim failed outright (`git apply --check` fails, not just
"already applied" — confirmed by testing the reverse-check too). Concretely:
- `aurora-switch-dawn-api.patch` — **not needed**. melee-pc's aurora already builds
  `DawnCacheDeviceDescriptor` with only `.nextInChain`, matching the newer Dawn API
  the patch was trying to guard against.
- Window/fullscreen handling — **not needed as an aurora patch**. melee-pc's aurora
  already sets the SDL external-graphics-context property for any non-null backend,
  and already exposes `AuroraConfig::startFullscreen`. `melee-switch-gcc-compat.patch`
  just sets `.startFullscreen = true` under `__SWITCH__` in `main.c` instead of
  touching `lib/window.cpp`.
- `aurora-switch-surface.patch` and the CMake/present-mode half of
  `aurora-switch-window.patch` — **rewritten** against melee-pc's actual file layout as
  `aurora-switch-surface.patch` (constructs `SurfaceSourceSwitchNativeWindow` from
  `nwindowGetDefault()` in `BackendBinding.cpp`) and `aurora-switch-dawn-backends.patch`
  (Vulkan-only `DAWN_ENABLE_*` for `CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch"`, plus
  forcing present mode to `Fifo` on Switch for bring-up, same rationale as KartPad-NX).
  Both verified with `git apply --check` against the cloned tree.

### 3. Filesystem paths
The current SDL fallback has no usable Switch `SDL_GetPrefPath()` implementation.
Under `__SWITCH__`, melee-pc therefore pins both Aurora user and cache paths to
`sdmc:/switch/melee-nx/`; launcher persistence uses explicit Horizon-safe file
handling. The disc image path in the config must point to a Horizon path such as
`sdmc:/switch/melee-nx/GALE01.iso`.

### 4. SQLite / shader cache on FAT32 — corruption root-caused 2026-09-19

`switch/src/sqlite_vfs.cpp` now provides a native Horizon SQLite VFS and SQLite
is compiled with `SQLITE_OS_OTHER=1`, pthread mutexes, no WAL, no mmap, and
temporary storage in memory. Aurora selects FAT32-safe DELETE journaling and
NORMAL synchronization, and the NRO carries the initial pipeline descriptor
database in RomFS.

That fixed the original “unable to open database file” startup failure but not
durability: every captured `dawn_cache.db` has been malformed.

**Root cause, 2026-09-19 afternoon.** The header of the downloaded file declares
4569 pages of 4096 bytes; the file is 18,706,432 bytes, which is 4567 pages. It
is two pages short of what it claims — a torn tail, not random corruption. The
console also still held `pipeline_cache.db-journal` and `pipeline_cache.db.lock`,
which only an abandoned transaction leaves behind.

The cause is §5's own exit wrapper. `kartpad_fast_exit()` goes straight to
`svcExitProcess` and skips all fini processing on purpose, so
`shutdown_pipeline_cache()` and `cache_shutdown()` never run. Horizon commits
written file data only on `fsFileFlush` or a clean close — the same property
that made the runtime log stop mid-line before `fsync` was added here. So each
run lost its last transaction's pages *and* the rollback journal that could have
repaired them, leaving a header written by an earlier flush over a shorter file.
SQLite then rejects the database at `PRAGMA journal_mode`, Aurora latches
`cache_broken`, and every pipeline is compiled from scratch on first use. The
symptom split the user reported — menus fine, gameplay ~15-20 FPS — is what that
looks like, because a match keeps meeting pipelines a menu never needs.

Two changes, both in the current build:

1. `sqlite_vfs.cpp` flushes writable handles before `fsFileClose`, honours
   `SQLITE_FCNTL_SIZE_HINT` (one explicit `fsFileSetSize` instead of implicit
   per-page extension), and exports `melee_nx_sqlite_flush_all()`, which
   `kartpad_fast_exit()` calls before its own `fsync`. It must never block exit,
   so the handle mutex is taken with a bounded `try_lock` — `abort()` from
   inside SQLite reaches the same path — and a failed acquisition skips the
   flush rather than deadlocking.
2. `aurora-switch-cache-recovery.patch` quarantines an unreadable dawn or
   pipeline cache to `<name>.corrupt`, drops its stale journal and lock, and
   retries once on a fresh database. Derived caches are rebuildable; losing one
   launch's compilation is far cheaper than latching caching off forever. The
   bad copy is kept for diagnosis. A second failure means the cache path itself,
   so it latches as before.

Still unproven on hardware, and worth checking in the next capture: that a
quiescent snapshot passes `PRAGMA quick_check`, that the second launch reports
real hits rather than re-seeding, and that skipped-pipeline draws (now counted —
see the `PIPELINES` log line) fall toward zero in warm steady state. Pipeline
descriptor-cache existence is still not proof that Dawn's compiled cache is
valid or that pipeline hits occurred.

### 5. pthread stack size + exit() wrapping
Dawn/Tint WGSL compiler needs ~54 KB stack frames. libnx default pthread stack is
128 KB — dangerously close. `clang_tls_switch.c` wraps `pthread_create` to floor at
8 MB (identical to KartPad-NX). `exit()` is also wrapped to `_exit()` to skip C++
fini teardown (confirmed crash class in KartPad-NX).

### 6. Input
melee-pc uses SDL3 `SDL_Gamepad` directly. SDL3's Switch backend should enumerate
the Pro Controller and Joy-Con pair as SDL gamepads. melee-pc's gamepad remapping
(stored per-device in aurora's `.controller` files) maps SDL buttons to GC layout.
This likely works without modification — needs hardware verification.

### 7. Performance expectations
Switch Tegra X1 = Cortex-A57 @ ~1 GHz (4 cores). melee-pc's Android build targets
Cortex-A73 at similar clocks. Melee at 60fps should be within reach — the game is
far lighter than MKW.

**Updated 2026-09-18, after the first hardware boot:** the port reaches the menu but
does not hit target frame rate, and audio is laggy. The claim that "GPU is not the
bottleneck" was inherited from KartPad-NX and has never been measured for melee-nx —
treat it as untested. Known contaminants must be removed before any measurement is
   believable, chiefly the `fsync` per log line added during bring-up. The original
   cache-open failure has since been replaced by an integrity problem in the latest
   captured Dawn cache. See
[HANDOFF-PERF-AUDIO.md](HANDOFF-PERF-AUDIO.md).

## Bring-up sequence

1. ~~Write `melee-switch-gcc-compat.patch`~~ — done. Renames melee-pc's `main()` to
   `melee_main_impl()` under `__SWITCH__` (avoids a duplicate-`main` link error against
   `switch/src/main_switch.cpp`'s NRO entry), swaps the SDL pref-path app id to
   `melee-nx`, and sets `startFullscreen`. See section 2 above for why no
   MEM1/linker-flag patch was needed.
2. ~~Rewrite the aurora patches against melee-pc's actual vendored aurora~~ — done.
   See section 2b above.
3. ~~Clone `ref/dawn` and `ref/SDL`~~ — done, copied from KartPad-NX per `docs/DEPS.md`.
4. ~~Build Dawn + SDL3 inside the `kartpad-dawn` Docker image~~ — completed.
5. ~~Configure, compile, link and package the NRO with GCC C + Clang C++~~ — completed.
   On the current layered snapshot use `build-graphics.sh prepare`, then separate
   `build-melee.sh configure` and `build-melee.sh build`; do not force the old
   monolithic compatibility patch through a failed reverse check.
6. ~~Boot on hardware and reach the main menu~~ — completed 2026-09-18. Performance,
   cache integrity, missing async-pipeline geometry, controller breadth and audio
   latency remain active validation work.

## Confirmed by an actual build (not just static review)

Docker + the `kartpad-dawn` image were available on the dev machine, so the whole
bring-up sequence was actually run end-to-end, not just planned. The original
scripts produced a real NRO/ELF and the result later booted on hardware. The
current 2026-09-19 optimized build is 85,879,957 bytes; its exact hash and
deployment record are documented below. Dependency/Aurora preparation still
reverse-detects cleanly, while the historical melee compatibility patch now has
the overlap caveat described in `switch/patches/README.md`.
Along the way, three real, non-obvious problems had to be found and fixed —
none of them things static review would have caught:

**1. Dawn's renderdoc header didn't know `__SWITCH__`.** Vendored
`third_party/renderdoc/renderdoc/api/app/renderdoc_app.h` only recognizes
Windows/Linux/BSD/Apple for its `RENDERDOC_CC` calling-convention macro and hits
`#error "Unknown platform"` otherwise. Because `RENDERDOC_CC` then expands to
nothing predictable, every subsequent `typedef` in the header cascades into bogus
"typedef redefinition"/"expected ')'" errors that look unrelated to the real cause.
Fixed with `dawn-switch-renderdoc.patch` (adds `__SWITCH__` next to `__linux__` —
Switch needs no special calling-convention keyword either).

**2. Clang silently ignores `-specs=switch.specs` for `aarch64-none-elf`.**
Confirmed with `-v`: neither the linker-script (`-T switch.ld`), the PIE/`-z`
flags, nor the `crti.o`/`crtbegin.o` startfile objects it's supposed to inject
ever appear in the generated link line, regardless of `-specs=`/`--rtlib=`/
`--unwindlib=`. GCC (which every "official" devkitPro Switch project uses,
including KartPad-NX) honors `-specs=` fine — this is specifically a Clang gap,
hit here because melee's game code needs GCC for
`__attribute__((scalar_storage_order(...)))` while everything else is Clang.
Symptom without the fix: undefined `__tls_start`/`__tls_end`/`__bss_start__`/
`__got_start__`/`_DYNAMIC`/`__argdata__`/etc. — all section-boundary symbols
`switch.ld` defines and libnx's `crt0`/TLS setup expects. Fixed by hand-expanding
`switch.specs`' `*link`/`*startfile` blocks directly into `melee`'s
`target_link_options` in `switch/CMakeLists.txt` (`-Wl,-T,switch.ld -Wl,-pie
-Wl,--no-dynamic-linker -Wl,-z,text -Wl,-z,now -Wl,--build-id=sha1
-Wl,--require-defined=main` plus literal `crti.o`/`crtbegin.o` objects), and by
switching the linker Clang invokes from its default (this container's system
lld, which doesn't support several of those flags at all) to devkitA64's own
`aarch64-none-elf-ld` via `-fuse-ld=`.

That fix then exposed **PIC/PIE mismatch**: linking against devkitA64's default
(non-PIC) `libstdc++`/`libc`/`libgcc`/etc. produced "read-only segment has
dynamic relocations" from a PIE final link. devkitPro's own
`Platform/Generic-dkP.cmake` adds `-fPIC` to every NintendoSwitch target's arch
flags for exactly this reason; our toolchain file didn't. Fixed by adding
`-fPIC` to `SwitchGCC.cmake`'s common flags and switching every `-L` search
path (and the libgcc probe) to devkitA64's PIC multilib
(`aarch64-none-elf/lib/pic`, `lib/gcc/.../pic`) instead of the default one —
which meant Dawn (built via this same toolchain file) needed a rebuild too,
since its archives were already compiled non-PIC.

**3. Switch has no real Vulkan implementation available in the `kartpad-dawn`
image.** devkitPro's own `switch-mesa` package, installed there, is EGL/GLES
only (`libEGL.a`/`libGLESv2.a`, no `libvulkan.a`) — confirmed by inspecting its
pacman file list in-container. Real Switch homebrew Vulkan comes only from
Mesa's NVK driver, which needs its own Rust-enabled cross-build (NAK, NVK's
shader compiler, is written in Rust, which has no Horizon/`aarch64-none-elf`
target — the same gap `nod` hit, see `switch/src/nod/`). That build is
substantial and out of scope to reproduce here a second time; KartPad-NX
already has it built (`switch/overlays/mesa-switch/`), so melee-nx reuses that
prebuilt tree directly via a second read-only bind mount rather than rebuilding
it — see `docs/DEPS.md`'s "Mesa/NVK" section for the exact command and the
caveat that this is a dev-machine-specific shortcut, not a portable solution.
Once mounted, the remaining undefined symbols (`vkGetInstanceProcAddr`,
`sysconf`, `fchown`, `waitpid`, `execvp`) all resolved for free from files
KartPad-NX had already written for the exact same problem
(`rust_switch_stubs.c`, plus a small local `geteuid()` shim in
`switch/src/libc_switch.c`).

This was meaningfully more confidence than the original scaffold had: every part
of the graphics stack, the GCC/Clang toolchain split, the full patch set, and
the final NVK-backed Vulkan link are now proven to compile and link, not just
planned to. Hardware testing subsequently confirmed boot and rendering through
the Melee main menu on 2026-09-18; later sections supersede this historical
bring-up checkpoint.

## Known risks

- `ASSERT_SIZE` / `ASSERT_OFFSET` in `tools/lint_sweep.py` (-m32 checks): these test
  GameCube ABI struct layouts. They should pass on aarch64 since melee-pc already handles
  LP64 pointer differences, but verify after first build.
- `__builtin_bswap*` in the aurora GX layer: confirmed working on aarch64 Clang/GCC.
- `RmlUi` on Switch: never tested. May need the same `dl` removal as Tracy.
  Watch for `dlopen`/`dlsym` calls in RmlUi's font backend.


## Confirmed on hardware (2026-09-18) — first boot to the main menu

melee-nx now reaches the Melee main menu on a real console. Getting there took five
distinct fixes, each found by instrumenting rather than guessing. Recorded here because
every one of them is a Switch-specific trap the next port will hit too.

### The logging pipeline was broken three ways before anything could be diagnosed
Three writers shared one file, nothing was ever committed to the SD card, and the file
being read was a stale local copy that had never been on the console. **Lesson: verify
the build stamp in the log against the binary you just uploaded before believing
anything the log says.** `kBuildStamp` is printed as line `00` for exactly this reason.

### Crash reports named the wrong thread
The process-exit path (`_exit` → `__libnx_exit` → `__appExit`) unmounts fsdev and NULLs
the `sdmc` entry in newlib's `devoptab_list` while nine other threads are still running.
Whichever one next touched the SD card took a Data Abort on a NULL devoptab, and
Atmosphère reported *that* thread — the real reason for the exit never appeared. Fixed by
wrapping `exit`, `abort` and `_exit` to call `svcExitProcess` directly. Note `abort()`
does not go through `exit()`, so wrapping `exit` alone is not enough; an uncaught C++
exception reaches `__terminate` → `abort()` → `_exit()`.

### `pthread_detach` is an unconditional ENOSYS stub on devkitA64
Disassembling the linked binary shows both paths falling through to `mov w0, #0x58; ret`.
So **every `std::thread::detach()` throws `std::system_error`**, and melee-pc detaches in
three places. Emulated in `clang_tls_switch.c` with a thread registry and a reaper:
threads mark themselves finished on the way out and the reaper joins them, which returns
immediately. A no-op detach would have leaked one kernel handle per preloaded file.
`pthread_join` is real and works.

### Disc pointers — see §2 above.

### Finding the throw site
`__builtin_return_address(0)` inside a `__cxa_throw` wrap is useless on its own: it lands
in whichever libstdc++ helper threw, and `std::__throw_system_error` alone has 60+
callers. `_Unwind_Backtrace` from inside the wrap gives the real chain, using the same
`.eh_frame` data the throw is about to use. That is what named
`std::thread::detach` → `pc_file_cache_start_prewarm`.

### Performance optimizations and port menu (2026-09-18)

Following the initial hardware boot, five major performance and diagnostic milestones were achieved:

1. **SD Card Logging Decoupling:**
   - Synchronous `fsync` on every `OSReport` and Aurora log line was eliminated. Writes are now buffered in memory and flushed periodically (1 Hz / 64 lines) or on fatal abort/panic (`OSPanic`, `melee_terminate_handler`). This prevents SD card serialization on the main render thread.

2. **Native Horizon SQLite VFS (`"hos"`):**
   - SQLite DB initialization failures in Dawn and Aurora pipeline caches (`unable to open database file`) were resolved by integrating a custom libnx-native VFS (`switch/src/sqlite_vfs.cpp`).
   - SQLite is compiled with `SQLITE_OS_OTHER=1` and `SQLITE_OMIT_WAL`, using `fsFs*` calls directly with atomic file replaces and custom lock tracking.
   - Cache files can now be created, but the latest captured Dawn DB is malformed. Persistence, hits, and reduced compilation remain unproven until integrity tests pass.

3. **RomFS Port Menu & Lightweight Profiler:**
   - All RmlUi UI assets (`resources/`) are packed directly into the NRO's RomFS partition via `switch/CMakeLists.txt` (`ROMFS "${MELEE_PC_ROOT}/resources"`). `launcher.cpp` loads from `romfs:/resources/`.
   - Settings persistence bug fixed: `save_preferences` writes directly using POSIX `O_TRUNC` to prevent FAT32 rename errors on Horizon.
   - Controller chord **`- + R3`** (Minus/Select + Right Stick Click) opens the in-game port menu on console.
   - Built-in lightweight profiler tracks real-time microsecond metrics:
     - `Sim`: Melee GameCube simulation and GX display list generation.
     - `Submit`: WebGPU/Aurora command submission.
     - `Wait`: Swapchain acquisition and VSync synchronization.
   - Profiler is visible live inside the Port Menu and toggleable as an on-screen HUD (`fps=0` Off, `fps=1` FPS, `fps=2` Full Breakdown).

4. **Audio Latency Tuning:**
   - The current build forces 1024 frames (`SDL_AUDIO_DEVICE_SAMPLE_FRAMES=1024`). Earlier notes claiming 512 was deployed were stale. Buffer tuning follows negotiated-spec and underrun instrumentation.

5. **Input Worker Removal (2026-09-19):**
   - Horizon controllers are already sampled through `PADRead` on the main thread. The auxiliary SDL keyboard/touch worker is no longer created on Switch, avoiding a kernel handle and the port's 8 MiB stack floor.

6. **Aurora encoder-state suppression (2026-09-19):**
   - Dusklight-derived render-pass state caches suppress identical texture bind-group and destination-alpha blend-constant calls. They reset at every pass and after custom draws. The change is built and deployed; visual and timing validation on hardware is pending.

### Latest deployed optimization build (2026-09-19)

The Release build completed with Cortex-A57, `-O3`, `-DNDEBUG`, PIC/PIE, Dawn,
Aurora and NVK. It was uploaded to
`ftp://192.168.1.171:5000/sdmc:/switch/melee-nx/melee.nro`, downloaded again,
and verified byte-for-byte:

- Size: 85,879,957 bytes
- SHA-256: `184480f708c5f26fd22bc115ca5319b42c9e922e67d2ec07948bdf5b7742ece5`
- Embedded patch-set ID: `d3b6815a581b4d1f401c25ad22684136acd646385a64f7af545e7b610b4d26bb`
- SDL fallback version: 3.4.4; the pinned 3.4.10/Dusklight migration is not complete.

No frame-rate, visual, input, or audio result has yet been recorded for this
specific binary. Verify the runtime manifest before interpreting the next
hardware logs.


### SDL 3.4.10 / Dusklight migration and cache repair (2026-09-19, afternoon)

Deployed NRO: 85,896,341 bytes, SHA-256
`84d88dd602959e1abef4c6d35220b7838a1ed977cfb44b7c0a289d9afc3b1342`, verified by
downloading it back and comparing bytes.

- SQLite cache flush-on-exit and quarantine/rebuild — see §4 above.
- Draws dropped because their pipeline was still compiling are counted in
  `get_pipeline()` and reported every 60 frames with pending/ready counts.
  Switch requests `PipelinePriority::Normal`, so those draws are simply skipped;
  a fast frame with missing geometry is not a win, and nothing measured it.
- melee-pc's frame profiler in `src/pc/vi.c` was gated on `getenv("MELEE_FPS")`.
  Nothing on a Switch sets environment variables, so every hardware log to date
  contained no frame timing whatsoever — which is why P0 of the performance plan
  still had no baseline after a full day of work. It now defaults on under
  `__SWITCH__` and writes its per-second `PERF` summary through `pc_log_line`,
  so it lands in `melee-nx-runtime.log` next to whatever Aurora logged in the
  same second. `MELEE_NO_FPS` disables it.
- SDL moved from the 3.4.4 tree to `ref/SDL-dusklight`: pinned `release-3.4.10`,
  plus Dusklight's Switch backend verbatim, plus a melee-nx layer that adds
  external-graphics mode (Aurora/Dawn keeps sole ownership of
  `nwindowGetDefault()`; no EGL/GLES is compiled in), four-player pad
  enumeration with per-device state, instance IDs, connect/disconnect events and
  GC analog triggers mapped to axes 4/5, and audren open/close error handling
  with short-buffer padding and bounded failure cleanup. `MELEE_SDL_VARIANT`
  selects `dusklight` (default) or `legacy`, each with its own build root so a
  library is never linked against another variant's headers.

One consequence to watch: the Dusklight tree builds `src/filesystem/unix`
instead of falling through to the dummy backend, so `SDL_GetPrefPath` may now
return a path rather than NULL. Aurora's user/cache paths on Switch are
hardcoded to `sdmc:/switch/melee-nx/` in `src/pc/main.c` and are unaffected, but
`music_stream.cpp` and `textures.cpp` call `SDL_GetPrefPath("", "melee-pc")`
directly for optional custom music and textures and could now look somewhere
new.
