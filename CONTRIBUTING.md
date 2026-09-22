# Contributing

Start with [BUILDING.md](BUILDING.md) for a working environment and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the platform constraints.

## Where changes belong

| Change | Location |
|---|---|
| Switch-owned runtime code | `switch/src/` |
| Compiler, linker, graphics configuration | `switch/CMakeLists.txt`, `switch/cmake/` |
| Upstream Melee, Aurora, Dawn, or SDL changes | Working source in `ref/`, captured in `switch/patches/` |
| Dependency and build tooling | `builder/`, `switch/docker/` |
| Reusable instructions and rationale | The guides linked from [docs/README.md](docs/README.md) |

Keep game source, disc images, extracted assets, captures, and build outputs
out of commits. Preserve local edits in reference trees. Never force a patch
failure, reset a reference checkout, or use `git clean` to resolve drift.

## Patch workflow

1. Check the reference tree's revision and worktree before editing. The pins
   are in [docs/DEPS.md](docs/DEPS.md).
2. Find the owning patch in the [catalog](switch/patches/README.md). Melee and
   Aurora patches own disjoint files; the SDL backend is a two-patch stack.
3. Make the source change, then update its owning patch against the pinned
   base. Preserve other local edits. Avoid formatting unrelated upstream code.
4. Run the verifier, then the relevant build stages. A successful
   `git apply --check` alone does not establish that the live source matches.

```bash
python builder/verify-patches.py
bash builder/test-patch-common.sh
```

The verifier checks clean reconstruction, live reverse application, ownership,
and byte equality for Melee/Aurora and SDL. It does not reset the live trees or
validate the separate Dawn/Mesa build. `patch-common.sh` runs Git from the
actual repository root so nested Aurora paths cannot be silently skipped.

Never edit `builder/*.sh` while a container is executing one of those scripts.

## Checks

Run the relevant checks for the change. Python tools use the standard library.
SDK packaging uses the image's Python 3.11+ and GNU `ar`.

```bash
python builder/test-perf-report.py
python builder/verify-patches.py
bash builder/test-patch-common.sh
bash -n builder/docker.sh
```

The Docker wrapper and SDK tests use Bash and `ar`. Run them inside the selected
toolchain or SDK image:

```bash
bash builder/docker.sh shell
python3 builder/test-docker.py
```

For a runtime or build change, finish with the relevant graphics stages and
`bash builder/docker.sh melee all`. A build verifies compilation and linking;
hardware correctness and performance need separate console testing. Save the
tested NRO, configuration, and logs, and verify the deployed NRO by downloading
it and comparing SHA-256.

## Style

- Use `switch/src/.clang-format` for owned C/C++ files: four spaces, 100 columns,
  unchanged include order. Format only the files you are editing.
- Keep shell and CMake indentation at two spaces, Python at four. `.editorconfig`
  defines line endings and whitespace; shell scripts must use LF.
- Explain constraints, ownership, and surprising behavior in comments. Avoid
  narrating the code, repeating a filename, or retaining a debugging diary.
- Keep detailed platform rationale in the architecture guide. Put historical
  measurements and troubleshooting sessions in local working notes.
- Preserve compiler flags, ABI layouts, lifetimes, and shutdown ordering during
  cosmetic cleanup. Keep functional changes separate and validate them directly.

For example:

```bash
clang-format -i switch/src/main_switch.cpp
git diff --check
```
