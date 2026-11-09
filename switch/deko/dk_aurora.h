// The deko3d renderer's C entry points. The NRO builds Aurora with AURORA_GFX_DEKO3D: Aurora records every frame as
// for WebGPU, Dawn's Null device keeps its object model without GPU work, and switch/deko owns the
// GPU: the device, the swapchain on the default NWindow, the recording of Aurora's frames
// (switch/deko/aurora), the present pass (the game's picture or the test pattern) and the DKSH
// caches. builder/integrate-deko.py adds the calls to Aurora's webgpu::initialize and shutdown.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Device, queue, memory, swapchain, the renderer's own shaders, the DKSH caches and the uam worker.
// From webgpu::initialize on the game thread, before the render worker presents. gxConfigVersion is
// Aurora's gx::GXPipelineConfigVersion (part of the DKSH caches' compiler id).
void aurora_switch_dk_init(uint32_t gxConfigVersion);

// Waits for the GPU and releases the window (Aurora's shutdown).
void aurora_switch_dk_shutdown(void);

// One frame of the boot screen: a title, a progress bar (fraction 0..1; below 0 draws none) and up to
// two lines under it. Only before Aurora's render worker presents (the shader preload in
// pipeline_cache.cpp); the text is the renderer's 3x5 font: capitals, digits and a few signs.
void aurora_switch_dk_boot_screen(const char* title, float fraction, const char* detail, const char* hint);

// The uam worker at the game thread's priority while the boot preload waits for it, back to its
// background priority afterwards.
void aurora_switch_dk_compile_boost(bool on);

#ifdef __cplusplus
}
#endif
