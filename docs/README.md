# Documentation

| Guide | Use it for |
|---|---|
| [Project overview](../README.md) | Status, installation overview, credits |
| [Building](../BUILDING.md) | Setup, build stages, installation, troubleshooting |
| [Contributing](../CONTRIBUTING.md) | Source layout, patch workflow, validation, style |
| [Architecture](ARCHITECTURE.md) | Platform constraints and integration boundaries |
| [Dependencies](DEPS.md) | Source pins, provenance, reproducibility limits |
| [Native deko3d renderer](DEKO3D.md) | The renderer: design, build, shader seeds, risks |
| [perf.cfg keys](PERF-CFG-KEYS.md) | Every runtime switch the code reads (generated) |
| [Launcher](LAUNCHER.md) | The Melee NX launcher: install, mods, Slippi sign-in, game handoff, build and tests |
| [Slippi](SLIPPI.md) | Slippi online play: how to play Direct, how it is built, what is not ported, the first hardware test |
| [Patch catalog](../switch/patches/README.md) | Patch ownership and application order |
| [Docker image](../switch/docker/README.md) | Reusable toolchain image |
| [Cleanup record](CLEANUP.md) | Scope and validation of the repository cleanup |

These guides are maintained with the code. Local `HANDOFF-*`, `PLAN-*`, and
other working notes remain ignored by Git; historical captures and deployment
records belong in `scratch/`. They are evidence for a particular run, rather
than installation instructions.
