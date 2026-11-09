#pragma once
#include <string>
#include <webgpu/webgpu.h>
// True when both stages Aurora builds from this GX WGSL (vs_main, fs_main) are in the DKSH caches,
// so its pipeline needs no compile (backend_pipelines.cpp).
bool kartpad_deko_wgsl_cached(const std::string &wgsl);
// Native presentation of a completed KartPad frame (including its UI snapshot).
void kartpad_deko_present(WGPUTexture texture, unsigned width, unsigned height);
struct KartPadDepthParams {
  unsigned dstWidth, dstHeight, srcWidth, srcHeight;
  float offsetX, offsetY, scaleX, scaleY;
};
void kartpad_deko_depth_snapshot(WGPUCommandEncoder encoder, WGPUTextureView view,
                                 KartPadDepthParams params, bool reversed);
bool kartpad_deko_depth_read(unsigned x, unsigned y, unsigned &z);
