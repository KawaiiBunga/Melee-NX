# Switch integration

melee-nx compiles the melee-pc game sources directly to AArch64 and supplies
Switch platform adapters. Rendering follows:

```text
Game C → Aurora GX → Dawn WebGPU (Null backend) → switch/deko → deko3d → libnx NWindow
```

Tint translates Aurora's WGSL to GLSL, `switch/uam` compiles it to deko3d shaders at run time, and
`switch/deko/` draws; [DEKO3D.md](DEKO3D.md) describes the seams. The launcher (`launcher/`) is a
separate NRO that starts the game (see [LAUNCHER.md](LAUNCHER.md)).

Upstream source changes live in patches. The owned code in `switch/src/`
provides process startup, threading and libc compatibility, disc access,
logging, telemetry, and the SQLite VFS.

`malloc` is routed to newlib's `_malloc_r` (`src/malloc_switch.c`, option `MELEE_NX_MALLOC_WRAP`).
devkitA64's `malloc` sends every request over 31 bytes through `_memalign_r(32, n)`, which asks for
`n + 64` and frees the misaligned head and the cut-off tail. That leaves a small free chunk beside every
block, so the free lists grow over a session and every allocation walks longer bins under newlib's one
heap lock. KartPadNX measured allocator code rising from 4% to 47% of Dawn's frame worker over ten
races. Blocks are 16-byte aligned (`alignof(max_align_t)`); aligned `new` and `memalign` still use
`_memalign_r`. Not yet measured on this port.

## Compiler and linker constraints

GCC compiles C because the game uses `scalar_storage_order("big-endian")`.
Clang 19 compiles the C++ graphics stack against devkitA64's libstdc++.
The game retains its Shift-JIS execution charset, aliasing rules, integer
wrapping, and disabled floating-point contraction.

NROs are position-independent executables. Both application objects and
runtime archives must use the PIC multilib. Clang's bare-metal driver does
not apply `switch.specs`, so `switch/CMakeLists.txt` supplies the linker script,
startup objects, and supported Switch link options explicitly.

Link order is significant:

1. `crti.o` and `crtbegin.o` precede application objects.
2. Application and graphics libraries precede the runtime group.
3. C/C++ runtime libraries share a rescan group for cyclic references.
4. `crtend.o` and `crtn.o` close the startup sections at the end of the link.

The runtime group is set through `CMAKE_CXX_STANDARD_LIBRARIES` after
`project()`. devkitPro platform initialization overwrites an earlier value.
An empty `libunwind.a` satisfies Clang's library lookup; devkitA64's `libgcc`
supplies the actual unwinder. The game's SDK `__assert` definition takes
precedence over newlib's incompatible symbol.

## Startup and thread lifetime

`main_switch.cpp` initializes services, runs the text-console data gate,
redirects logging, releases the console framebuffer, then calls
`melee_main_impl`. The upstream entry-point rename is captured in
`melee-switch-perf-telemetry.patch`.

Clang accesses TLS through `tpidr_el0`; libnx's GCC path uses
`__aarch64_read_tp()`. The constructor with priority 101 synchronizes the main
thread before ordinary static constructors. The `pthread_create` wrapper
does the same before each worker's entry point. Exception tracing installs
at priority 102.

Tint's recursive shader analysis needs more than libnx's default worker
stack. The wrapper supplies an explicitly allocated, page-aligned 8 MiB stack
when needed. Setting only the stack size is insufficient on this platform.
The stack remains owned until a join completes. Detached threads enter a
completion queue whose reaper joins them and releases their stacks; a
long-lived detached worker must not block reclamation of finished workers.

## Logging and exit

newlib's `FILE` objects are per-thread. Redirecting the process-wide file
descriptors with `dup2` keeps workers from writing to a console device after
its framebuffer is released. Boot markers use independent committed writes
so failures before normal logging starts can still be diagnosed.

The wrappers for `exit`, `abort`, and `_exit` bypass static destruction and
libnx service teardown. Workers may still access fsdev, so tearing down its
devices before all threads stop can invalidate live handles.

Before `svcExitProcess`, the wrapper commits Aurora's pending cache batches,
flushes SQLite file handles, shuts down the log writer, and flushes stdio.
Do not move persistence work into destructors: this exit path never runs them.
The SQLite flush uses bounded lock attempts because an abort can originate
inside a locked SQLite operation.

## Disc data and pointers

The C `nod` adapter supports the GameCube ISO/GCM interface used by Melee and
Aurora. Disc handles own their underlying `GcDisc`; partition and file handles
borrow it and must be released first. Extraction preserves the disc FST paths
and publishes a completion manifest after successful extraction.

With a completed `files/` tree and captured `disc.meta` and `disc-boot.bin`,
the data gate returns a sentinel recognized by the patched launcher. Otherwise
it selects a validated disc image or offers extraction. The gate runs before
Aurora initializes graphics.

Game disc-pointer slots are 32-bit values resolved through the MEM1 4 GiB
window. Use the existing `DP()` conversion; do not cast slots directly to host
pointers or impose a fixed low address for MEM1.

## Storage and caches

The native HOS SQLite VFS supplies file access and rollback-journal support
without relying on unavailable Unix mmap and locking behavior. Connections
share a native file handle per path, while retaining their own SQLite lock
state. Writable handles are flushed before closing and during fast exit.

The shared `SwitchIoLock` serializes participating disc and loose-file fsdev
operations across the game and Aurora targets. It is distinct from SQLite's
native handle lock and the log writer's synchronization.

Shader-analysis reuse checks the full configuration, including on hash
collisions. Blob-cache reuse checks full Dawn keys. In-flight pipeline request
state also controls descriptor persistence. These invariants affect rendering
and cache correctness; formatting work must preserve them.

## Dependency boundaries

Dawn and SDL are built separately and imported into the game build. Each SDL
variant has a separate source/build pair. Aurora is built from melee-pc's
vendored tree, staged with the deko3d seams by `builder/integrate-deko.py`.
No GPU driver is linked: deko3d talks to the GPU through libnx.

The [dependency guide](DEPS.md) records pins and provenance. The
[Docker guide](../switch/docker/README.md) describes the toolchain image.
