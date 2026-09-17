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

### 2. No -no-pie / -Ttext-segment=0x10000000
melee-pc pins MEM1 at 0x80000000 using `-no-pie` and places its text at 0x10000000 on
Linux so 32-bit disc pointer slots always fit. On Switch:
- NRO format is always PIE — the loader patches relocations at load time.
- `switch/patches/melee-switch-gcc-compat.patch` removes these linker flags.
- MEM1 at 0x80000000 must be satisfied a different way: either `mmap(0x80000000, ...)`
  with FIXED + anonymous mapping (same trick as KartPad-NX's guest_flat_memory), or
  verifying that libnx's address space leaves 0x80000000 available (on 64-bit HOS it
  usually does in the 39-bit user VA range).

**Status:** patch not yet written — this is the first task for the initial bring-up session.

### 3. Filesystem paths
melee-pc reads `launcher.cfg` from `SDL_GetPrefPath("", "melee-pc")`. On Switch,
SDL3 maps this to `sdmc:/switch/melee-nx/`. The disc image path in the config must
point to `sdmc:/switch/melee-nx/disc.iso` or similar.

### 4. SQLite / shader cache on FAT32
Same issue as KartPad-NX — solved identically:
- `PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;`
- Strip `sdmc:` prefix from paths before passing to SQLite's VFS.
Aurora's `AuroraSwitchSQLite.cmake` already applies these pragmas if included (it's
wired in `Graphics.cmake`).

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

1. **Write `melee-switch-gcc-compat.patch`**: remove `-no-pie`/`-Ttext-segment`,
   add MEM1 mmap shim or verify 0x80000000 is available, fix any `getenv`/path calls
   that assume Linux `/home/` prefixes.
2. **Verify Dawn/SDL3 patches apply** to the same ref/ trees (they're identical to
   KartPad-NX — should be a no-op copy).
3. **First configure attempt**: `builder/build-melee.sh configure` — fix any CMake
   errors (missing includes, wrong arch flags for GCC C vs Clang CXX split).
4. **First build attempt**: expect errors from the melee-pc game code under aarch64-none-elf
   GCC (struct size assertions via `ASSERT_SIZE`/`ASSERT_OFFSET`, any LP64 pointer issues
   the melee-pc porting notes don't cover yet for NRO target).
5. **Boot on hardware**: once it links, test disc load, rendering, input.

## Known risks

- `ASSERT_SIZE` / `ASSERT_OFFSET` in `tools/lint_sweep.py` (-m32 checks): these test
  GameCube ABI struct layouts. They should pass on aarch64 since melee-pc already handles
  LP64 pointer differences, but verify after first build.
- `__builtin_bswap*` in the aurora GX layer: confirmed working on aarch64 Clang/GCC.
- `RmlUi` on Switch: never tested. May need the same `dl` removal as Tracy.
  Watch for `dlopen`/`dlsym` calls in RmlUi's font backend.
