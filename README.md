# melee-nx

A Nintendo Switch homebrew port of [melee-pc](https://github.com/999sian/melee-pc)
— Super Smash Bros. Melee (NTSC-U 1.02) — built as a native NRO.

This is **not an emulator**. melee-pc is a decompilation-derived C port of the
game, and melee-nx compiles that C straight to AArch64 for Horizon. Rendering
goes through Aurora's GX translation layer onto Dawn (WebGPU) → Vulkan → Mesa's
NVK driver → libnx.

You supply your own disc image. No game code or assets are distributed here.

---

## Status

**Playable, improving.** It boots on real hardware, reaches the main menu, and
plays matches at roughly 36 fps (median) / 57 (90th percentile) at 720p on stock
clocks. A locked 60 is the remaining goal.

| | |
|---|---|
| Boots on hardware | yes |
| Menus | render and respond |
| Gameplay | ~36 fps median, up to 60, at 720p stock clocks |
| Audio | works, latent |
| Rendering | can briefly drop geometry while a shader pipeline is still compiling |
| Load transitions | occasional multi-second hitch while streaming assets |

---

## Installing a build

Copy to your Atmosphère SD card:

```
sdmc:/switch/melee-nx/melee.nro
sdmc:/switch/melee-nx/GALE01.iso
```

Launch from the Homebrew Menu. The first run extracts game data from the disc
image into `sdmc:/switch/melee-nx/files/`, which takes a few minutes.

You need:

- A Nintendo Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)
- Your own Melee dump: NTSC-U 1.02, `GALE01`,
  SHA-1 `d4e70c064cc714ba8400a849cf299dbd1aa326fc`

If something goes wrong, `melee-nx-boot.log` and `melee-nx-runtime.log` are
written next to the NRO and are the right place to start.

---

## Building

See **[BUILDING.md](BUILDING.md)** for the full guide. The short version:

```bash
bash builder/fetch-deps.sh              # clone pinned reference sources
export MESA_NVK_ROOT=/path/to/mesa      # see BUILDING.md — this one is manual
bash builder/docker.sh image            # build the container
bash builder/docker.sh graphics all     # Dawn + SDL3 (slow, once)
bash builder/docker.sh melee all        # apply game patches, configure, build
```

Output: `build/switch/melee.nro`.

Everything compiles inside a Docker container defined in
`switch/docker/Dockerfile`, so Docker and git are the only host requirements.

One dependency is **not** automated: Mesa's NVK Vulkan driver. Switch has no
vendor Vulkan driver for homebrew, devkitPro's `switch-mesa` is EGL/GLES-only,
and NVK's Rust shader compiler needs its own cross toolchain to build for
Horizon. BUILDING.md explains how to obtain one. Removing that cross-repo
dependency is open work and the main obstacle to a true one-command build.

---

## Repository layout

```
builder/       fetch-deps, docker wrapper, and the two build stages
switch/        the port itself: NRO entry point, CMake, toolchain, patches
switch/patches every change made to the upstream reference trees
ref/           upstream sources (melee-pc, Dawn, SDL) — gitignored
```

[switch/patches/README.md](switch/patches/README.md) documents what every patch
does and why — big-endian storage order, the MEM1 4 GB pointer window, PIE/PIC
linking, process teardown, and the SQLite cache work.

---

## Credits

- [melee-pc](https://github.com/999sian/melee-pc) by 999sian — the C port this
  builds on, and its vendored Aurora fork
- [Aurora](https://github.com/encounter/aurora) and the
  [Dawn](https://dawn.googlesource.com/dawn) Switch work by encounter
- [Mesa/NVK](https://docs.mesa3d.org/drivers/nvk.html) — the Vulkan driver
- [Dusklight-NX](https://github.com/souldbminerr/dusklight-nx) — SDL3 Switch
  backend and several CPU-side techniques
- [devkitPro](https://devkitpro.org/) — devkitA64 and libnx

---

## License

Port code (`switch/`, `builder/`) is GPL-3.0-or-later, matching melee-pc. Game
source under `ref/melee-pc/src/melee` and `ref/melee-pc/src/sysdolphin` is not
licensed and remains the property of Nintendo. No game assets are distributed by
this repository, and none should ever be committed to it.

## Support

If you'd like to support this and my other projects: https://ko-fi.com/kawaiibunga
