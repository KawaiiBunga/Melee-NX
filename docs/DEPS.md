# Dependency acquisition

All reference trees go under `ref/`. They are gitignored — never commit them.

## ref/melee-pc — game source port

```bash
git clone https://github.com/999sian/melee-pc ref/melee-pc
```

Aurora is already vendored at `ref/melee-pc/extern/aurora` — no separate clone needed.

## ref/dawn — WebGPU/Vulkan backend

Use the **exact same revision** as KartPad-NX (already battle-tested on Switch/NVK).

```bash
# Get the revision from KartPad-NX's DawnOptions.cmake comment, e.g.:
git clone https://github.com/encounter/dawn ref/dawn
cd ref/dawn
git checkout 80ee0043018a51532ea0fa2e77496cc66634157e
git submodule update --init --recursive third_party/abseil-cpp
```

If you have KartPad-NX checked out, you can copy its `ref/dawn` directly:
```powershell
robocopy C:\Users\Bunga\Documents\GitHub\KartPad-NX\ref\dawn ref\dawn /E /XD .git
# Then re-init the git metadata if you want patch idempotency checks:
git -C ref/dawn init && git -C ref/dawn add -A && git -C ref/dawn commit -m "import"
```

## ref/SDL — SDL3 with Switch external-graphics patch

Same revision as KartPad-NX.

```bash
git clone https://github.com/libsdl-org/SDL ref/SDL
# Check KartPad-NX's sdl-switch-external-graphics.patch header for the revision.
```

Or copy from KartPad-NX:
```powershell
robocopy C:\Users\Bunga\Documents\GitHub\KartPad-NX\ref\SDL ref\SDL /E /XD .git
```

## Docker image

Use the same `kartpad-dawn` Docker image from KartPad-NX — it already has:
- devkitPro + devkitA64 (GCC aarch64-none-elf)
- LLVM/Clang 19
- Ninja, CMake 3.25+
- Mesa NVK patches

No new Docker image needed.
