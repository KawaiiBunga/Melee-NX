# melee-nx

Nintendo Switch homebrew port of [melee-pc](https://github.com/999sian/melee-pc)
(Super Smash Bros. Melee NTSC-U 1.02).

**Status: In development. Not yet functional.**

## Requirements

- Nintendo Switch running [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere)
- Your own Melee disc image (NTSC-U 1.02, GALE01, SHA-1: `d4e70c064cc714ba8400a849cf299dbd1aa326fc`)
- The game data extracted to `sdmc:/switch/melee-nx/`

## Building

See [docs/DEPS.md](docs/DEPS.md) for dependency acquisition and
[docs/PORTING-NOTES.md](docs/PORTING-NOTES.md) for architecture notes.

Requires the `kartpad-dawn` Docker image (devkitA64 GCC + LLVM Clang 19 + Mesa NVK):

```bash
# One-time: build Dawn + SDL3
bash builder/build-graphics.sh all

# Build the NRO
bash builder/build-melee.sh all
```

Output: `build/switch/melee.nro`

## Rendering path

Game C → Aurora GX → Dawn WebGPU → Vulkan (NVK) → libnx NWindow

Same proven stack as [KartPad-NX](https://github.com/999sian/KartPad-NX).

## License

Port code (`switch/`, `builder/`) is GPL-3.0-or-later, matching melee-pc.
Game source in `ref/melee-pc/src/melee` and `ref/melee-pc/src/sysdolphin` is not
licensed and remains the property of Nintendo. No game assets are distributed here.
