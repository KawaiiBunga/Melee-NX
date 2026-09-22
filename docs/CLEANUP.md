# Repository cleanup

This cleanup preserves runtime behavior, dependency versions, compiler flags,
patch contents, and the existing build commands. It does not include performance
work or deployment. Existing changes to the performance report and its tests
belong to separate work and are left intact.

## Plan

1. Record the starting source, patch, and build-artifact hashes. Check the patch
   stack before editing, and keep reference trees and historical captures intact.
2. Give each public document one purpose: project overview, build and install
   guide, contributor workflow, dependency provenance, or architecture. Keep
   local handoffs separate and link the maintained documents from an index.
3. Replace debugging narratives and comments that repeat the code with short
   explanations of constraints. Preserve ABI, lifetime, threading, cache flush,
   and linker-order requirements. Apply consistent formatting to owned C/C++
   sources without changing tokens, include order, or string contents.
4. Separate the reusable Docker toolchain from an optional Mesa SDK layer.
   Package the exact existing archives and compatibility source with hashes,
   retain the external read-only mount workflow, and support builds from the
   SDK image without a neighboring project checkout.
5. Check source equivalence, shell syntax, Docker command construction, SDK
   packaging/integrity, documentation links, and patch reconstruction. Build
   with the existing toolchain and packaged driver, preserving the starting
   artifacts. Record outcomes and limitations below.

## Boundaries

- No game logic, settings, symbols, binary resources, dependency upgrades, or
  patch regeneration.
- No changes to ignored upstream sources for cosmetic purposes.
- No commits, pushes, console deployment, or hardware performance claims.
- Never rewrite a builder script while a container is executing it.
- An SDK snapshot makes existing build inputs portable. Reconstructing the
  Switch Mesa fork from clean upstream sources is separate work; its donor
  snapshot is not a complete, pinned rebuild recipe.

## Results

Completed September 22, 2026.

### Documentation and source

- Reworked the README and build guide; added contributor, architecture, and
  Docker guides with a documentation index. Split the patch catalog by
  dependency and corrected stale descriptions of input, extracted-data boot,
  and the entry-point patch.
- Replaced machine-specific public instructions and the unverified Mesa
  rebuild procedure with the actual dependency contract and its limitations.
- Reduced local agent entry points to working rules and links. Historical
  handoffs, captures, and their evidence remain intact and ignored.
- Shortened debugging narratives in owned runtime and build files. Added
  scoped C/C++ formatting, editor settings, and LF rules for build scripts.

Clang's raw lexer reports identical non-comment tokens across all 22 owned
C/C++ files. Non-comment executable lines are unchanged in the existing
CMake files, graphics/game/fetch scripts, and toolchain Dockerfile. Patch
files are byte-identical to the starting snapshot. Existing performance-report
and test changes are also unchanged.

### Docker reuse

Created `melee-nx-sdk:latest` from the existing toolchain, preserving all linked
Mesa inputs: 25 archives, 68 thin-archive objects, and the compatibility source
(94 files; 195,165,815 bytes). Every packaged file's SHA-256 matches the original.

The SDK records its source toolchain image ID and verifies its manifest during
image creation. It works without a host Mesa checkout; an explicit
`MESA_NVK_ROOT` still supplies the existing read-only mount. Graphics preparation
and a shell also work with a plain toolchain image and no Mesa tree.

The SDK packages an existing driver build. It does not make the partially
recovered Mesa source build reproducible; see [DEPS.md](DEPS.md#mesa).

### Validation

| Check | Result |
|---|---|
| C/C++ lexer comparison and formatting | 22 files unchanged in tokens; formatter check passes |
| Patch reconstruction and live reverse checks | 20 Melee/Aurora patches, 74 files; SDL stack, 19 files |
| Patch helper regression | Nested and standalone Git trees pass |
| Docker wrapper and SDK tests | 9 tests pass, including spaces in paths, SDK mounts, thin archives, and invalid inputs |
| Existing performance-report tests | 6 tests pass |
| Existing GX/cache/log tests | Collision, capacity, invalidation, in-flight persistence, concurrent logging, and blob callbacks pass |
| Shell syntax and Git whitespace | Pass |
| Public Markdown links | All local links and anchors resolve; code fences are balanced |
| SDK build | Manifest verified; all 94 source hashes match |
| SDK-only `graphics prepare` and `melee all` | Configure, compile, link, and NRO packaging pass without a Mesa host mount |

The game build reused existing Dawn and SDL archives. It was not a fresh
graphics dependency rebuild or a hardware retest. Existing compiler and
already-applied Tracy warnings remain outside this cosmetic change.

The rebuilt ELF has **identical machine code** to the starting ELF:
`.text` is 21,253,352 bytes with SHA-256
`81657104ef072240c184b3aaa949b0572a659a69e98a61c31b162ecc116a0eb1`.
Of 21 allocated sections containing data, 19 are byte-identical. The only
differences are the linker build ID and 16 bytes in `.rodata` containing the
build timestamp and recorded repository revision. The NRO remains 85,939,642
bytes; its new SHA-256 is
`b3335b8ca9494fb448cf073b037d609aaee7e50562c689dde0eeee729e6433fe`.

Starting sources and artifacts, build logs, source comparisons, SDK manifest,
and output hashes are preserved locally in `scratch/cleanup-2026-09-22/`.
No console deployment, commits, or pushes were performed.
