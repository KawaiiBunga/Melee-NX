# Dependencies

[Build guide](../BUILDING.md) · [Documentation index](README.md)

`builder/fetch-deps.sh` is the source of truth for dependency acquisition.
It leaves existing directories unchanged. All upstream checkouts live in the
ignored `ref/` directory; Aurora is already vendored in melee-pc.

## Source pins

| Directory | Upstream | Revision |
|---|---|---|
| `ref/melee-pc` | [999sian/melee-pc](https://github.com/999sian/melee-pc) | `7c9a468f4f8206780c4cd762be1da7772daaeabf` |
| `ref/dawn` | [encounter/dawn](https://github.com/encounter/dawn) | `80ee0043018a51532ea0fa2e77496cc66634157e` |
| `ref/SDL-dusklight` | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | `release-3.4.10` |
| `ref/SDL` | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | `release-3.4.4` (legacy fallback) |

The fetcher initializes melee-pc's submodules and Dawn's `third_party/abseil-cpp`
submodule. Other build dependencies retain their upstream CMake configuration.
Fetching an individual tree uses the same pins:

```bash
bash builder/fetch-deps.sh dawn
bash builder/fetch-deps.sh sdl-dusklight
```

Keep Git metadata when transferring a reference checkout. Patch checks depend
on its base objects and repository root. Do not replace a pinned base with a
new commit of already-patched files.

## Patch provenance

- The Dawn and legacy SDL integration derives from KartPad-NX's Switch stack.
- The Dusklight SDL backend patch is copied from
  `dusklight-nx/platforms/switch/patches/sdl3-switch.patch`. Its SHA-256 is
  `61327a8d285880bf4dbf08a7071694f761aa208d00ac6d73160f531697409fcd`.
- The CPU donor audit used Dusklight-NX commit
  `63f8d2ba45431091a4dfb6bbd48dd32ff0a9339d` and Aurora submodule
  `6d9f9d9fe8952aada5274154645610042d0a036e`. Selected changes were adapted to
  melee-pc's Aurora base; the donor is not a build dependency.

The [patch catalog](../switch/patches/README.md) records ownership and the SDL
stack order. `python builder/verify-patches.py` reconstructs the patched
Melee/Aurora and SDL files from their bases without resetting the live trees.

## Toolchain

The [Docker recipe](../switch/docker/Dockerfile) uses
`devkitpro/devkita64:latest` and LLVM 19. GCC is required for the game's
big-endian storage attributes; Clang builds the C++ graphics stack.

The base tag and package repositories can change. Rebuilding the recipe later
does not guarantee the same compiler or system libraries. Preserve a working
image with `docker save`, and record its image ID when comparing builds. The
SDK manifest records the toolchain ID used when it was packaged.

## Mesa

Mesa is supplied as a prebuilt Switch/NVK tree or inside an SDK image. The
SDK contains the same archives, thin-archive members, and Rust compatibility
source consumed by CMake, with a manifest and SHA-256 checksums. The host path
and originating project's name are not part of the build interface.

The working tree originally came from KartPad-NX's `ref/mesa-switch-main`.
Its local overlay snapshot identifies `danfromtico/mesa-switch` and version
`26.2.1`, but records no upstream commit and explicitly describes itself as a
partial recovery snapshot. It is not a verified recipe for rebuilding the
entire driver from a fresh clone.

Packaging that existing build removes the dependency on the original checkout
for subsequent builds. It does not reconstruct the driver's source provenance
or rebuild the Rust NAK compiler. Those require a separate dependency migration
and validation effort. See [the Docker guide](../switch/docker/README.md) for
creating, checking, and transferring an SDK.
