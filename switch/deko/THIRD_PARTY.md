# Native graphics dependencies

melee-nx took this directory, `../uam` and the shader/seed tooling from
[KartPadNX](https://github.com/KawaiiBunga/KartPadNX) (November 2026), which carries the notices below. The
melee-nx changes are the `SetImmediates` wrapper in `backend_commands.cpp`, the SD paths
(`sdmc:/switch/melee-nx`), and `host/` (the services the adapter expects from the application). Symbols
keep their `kartpad_*` names so the two trees can be compared. The text below describes KartPadNX's
import and is kept as written.

Imported from [centollOS/SwitchWaker](https://github.com/centollOS/SwitchWaker/tree/04372277d03f934e1e563ae302bba9f565c5cb5d), downloaded 2026-10-15 into `ref/SwitchWaker`.

The device, memory allocator, descriptor management, diagnostic pattern,
DKSH file/cache code, shader worker and WGSL-to-GLSL translator came from
`switch/deko`. Texture/view/sampler lifetime tracking came from
`switch/deko/aurora/dk_objects.{cpp,hpp}`. KartPad adaptations include its command
adapter, asynchronous readback completion, presentation shader, cache paths,
thread roles, binding metadata and CMake integration.

SwitchWaker's original work is MIT: see `LICENSE.SwitchWaker`. Files carrying
MPL-2.0 notices retain that license and identify their SwitchWakerHD derivation
at `df8fbde`; see `LICENSE.MPL-2.0`. Keep those source files and notices available
when distributing the port's source. No Wind Waker game code, assets, pipeline
database or shader cache was imported into the KartPad build.

`../uam` is SwitchWaker's vendored uam 1.1.0 compiler, originally devkitPro/uam
`5a5afc2`, with the SwitchWakerHD fixes documented in `../uam/PATCHES.md`.
Its zlib and Mesa-imported MIT notices remain in `../uam/LICENSE` and the files
under `../uam/mesa-imported`. The migration itself changed no compiler code; the later
KartPad resident-compiler ownership fix (2026-10-17) is item 9 in `../uam/PATCHES.md`.

The runtime graphics driver is deko3d from devkitPro. uam contains a Mesa-derived
GLSL compiler and Maxwell code generator; that is a shader compiler, not the
Mesa/NVK Vulkan driver removed from the game's link. Dawn remains as Null-device
object infrastructure and Tint shader translation. The GPU commands and
presentation execute directly through deko3d.
