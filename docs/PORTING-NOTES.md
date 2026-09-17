# Porting notes — melee-pc → Switch (melee-nx)

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

**MEM1 does not need a fixed low address on Switch at all.** `src/pc/disc.h`'s
`pc_encode_dp()`/`pc_resolve_dp()` (backed by `pc_register_ext_ptr`/`pc_resolve_ext_ptr`
in `src/pc/os.c`) already have a generic fallback: any host pointer that doesn't fit in
32 bits is stored as `0x02000000 | index` into a 65536-entry indirection table instead
of being truncated. This exists in upstream melee-pc already (almost certainly for the
Android build, which is PIE/ASLR and can't guarantee sub-4GB addresses either) — it is
not something melee-nx invented.

Consequently `AllocMEM1()` in `extern/aurora/lib/dolphin/os/OSMemory.cpp` only has real
fixed-address logic for `_WIN32` and `__linux__` (x86_64/aarch64); every other platform,
including `__SWITCH__`, already falls through to the generic `calloc(1, size)` branch,
and that's correct — no aurora patch is needed for this. (An earlier version of this
scaffold shipped `switch/src/mem1_switch.c`, which tried to `mmap(..., MAP_FIXED)` at
0x80000000. That was wrong on two counts: devkitA64/libnx has no `<sys/mman.h>` at all
— confirmed by searching the installed toolchain — so it wouldn't have compiled, and it
was solving a problem the ext-pointer fallback already solves. It has been deleted.)

**Watch for on first hardware run:**
- The 65536-slot ext-pointer table is a linear-scan-on-insert array (see
  `pc_register_ext_ptr` in `src/pc/os.c`). It's only hit when a disc pointer is stored
  (archive load/relocation, not per-frame), so it should be a one-time cost per loaded
  archive — but a stage that registers many thousands of unique pointers could get
  slow, and hitting the 65536 cap aborts (`pc_disc_ptr_overflow`). If this becomes a
  real problem, raise `PC_MAX_EXT_PTRS` or switch it to a hash map.
- libnx's default heap sizing (via `__libnx_initheap`) may or may not be large enough
  for MEM1 (96 MB) + ARAM (16 MB) + Dawn/Aurora + game working set. No override has
  been added preemptively since the libnx default usually claims most of the applet's
  available memory automatically — verify actual behavior (or an `abort()`/OOM from
  `calloc`) on the first successful link + hardware boot, and add a heap-size override
  in `main_switch.cpp` (`__nx_heap_size`/`__libnx_initheap`) only if needed.

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
melee-pc reads `launcher.cfg` from `SDL_GetPrefPath(NULL, "melee-pc")`.
`melee-switch-gcc-compat.patch` swaps the app id to `"melee-nx"` under `__SWITCH__`, so
on Switch this resolves (via SDL3's Switch backend) to `sdmc:/switch/melee-nx/`. The
disc image path in the config must point to `sdmc:/switch/melee-nx/disc.iso` or similar.

### 4. SQLite / shader cache on FAT32 — NOT actually wired yet
`Graphics.cmake` optionally includes `${MELEE_AURORA_SOURCE}/cmake/AuroraSwitchSQLite.cmake`
and calls `aurora_configure_switch_sqlite(sqlite3)` if that file exists (this mirrors
KartPad-NX, whose own aurora fork has this file). **melee-pc's vendored aurora does not
have this file** — confirmed, `find` turns up nothing under `extern/aurora`. The
`include(... OPTIONAL)` silently no-ops and the `if(COMMAND ...)` guard skips the call,
so this is safe (won't break the build) but the FAT32-friendly pragmas
(`PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;`) are **not applied**. `sqlite3`
is a real CMake target here (defined in `extern/aurora/extern/CMakeLists.txt`, used for
Dawn's shader cache). Until this is addressed, expect possible slow or unreliable
shader-cache writes to the SD card on hardware — watch for this in first-boot testing,
and if it's a problem, either add a small `AuroraSwitchSQLite.cmake` to melee-pc's
aurora fork (patchable via a new aurora patch) or set the pragmas directly wherever
aurora opens the shader cache DB.

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
far lighter than MKW. GPU is not the bottleneck (same as KartPad-NX; endFrame ≈ 0).

## Bring-up sequence

1. ~~Write `melee-switch-gcc-compat.patch`~~ — done. Renames melee-pc's `main()` to
   `melee_main_impl()` under `__SWITCH__` (avoids a duplicate-`main` link error against
   `switch/src/main_switch.cpp`'s NRO entry), swaps the SDL pref-path app id to
   `melee-nx`, and sets `startFullscreen`. See section 2 above for why no
   MEM1/linker-flag patch was needed.
2. ~~Rewrite the aurora patches against melee-pc's actual vendored aurora~~ — done.
   See section 2b above.
3. ~~Clone `ref/dawn` and `ref/SDL`~~ — done, copied from KartPad-NX per `docs/DEPS.md`.
4. **`builder/build-graphics.sh all`** inside the `kartpad-dawn` Docker image — builds
   Dawn + SDL3 for Switch. Should closely mirror KartPad-NX's known-good build. Not yet
   run (needs the Docker toolchain, not available on this Windows host).
5. **`builder/build-melee.sh all`** — applies the compat patch, configures, and builds
   the NRO. Expect compile errors from melee-pc's game code under
   `aarch64-none-elf-gcc` (struct size assertions via `DISC_ASSERT_SIZE`, warnings
   promoted to errors, etc.) — iterate.
6. **Boot on hardware**: once it links, test disc load, MEM1/ext-pointer behavior under
   real memory pressure, rendering, input.

## Confirmed by an actual build (not just static review)

Docker + the `kartpad-dawn` image were available on the dev machine, so the whole
bring-up sequence was actually run end-to-end, not just planned. Both
`builder/build-graphics.sh all` and `builder/build-melee.sh all` now complete and
produce a real `build/switch/melee.nro` (34 MB) from a `melee.elf` (330 MB,
unstripped). The patch set (`builder/build-graphics.sh prepare`) applies cleanly.
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

This is meaningfully more confidence than the original scaffold had: every part
of the graphics stack, the GCC/Clang toolchain split, the full patch set, and
the final NVK-backed Vulkan link are now proven to compile and link, not just
planned to. Hardware testing (does it actually boot and render) is the next
open question.

## Known risks

- `ASSERT_SIZE` / `ASSERT_OFFSET` in `tools/lint_sweep.py` (-m32 checks): these test
  GameCube ABI struct layouts. They should pass on aarch64 since melee-pc already handles
  LP64 pointer differences, but verify after first build.
- `__builtin_bswap*` in the aurora GX layer: confirmed working on aarch64 Clang/GCC.
- `RmlUi` on Switch: never tested. May need the same `dl` removal as Tracy.
  Watch for `dlopen`/`dlsym` calls in RmlUi's font backend.
