#!/usr/bin/env python3
"""Stage a copy of melee-pc's Aurora for the native deko3d renderer (MELEE_NX_GFX=deko3d).

    python builder/integrate-deko.py [--src ref/melee-pc/extern/aurora] [--dst build/aurora-deko]

Copies the (already patched) Aurora tree to --dst and edits the copy. Every edit sits behind
AURORA_GFX_DEKO3D, which switch/cmake/Deko3D.cmake defines. The reference tree and the NVK build
never see them, so builder/verify-patches.py keeps passing. Idempotent: an edit whose new text is
already in place is skipped; a missing anchor stops the run (the Aurora pin moved).

What the seams are (docs/DEKO3D.md): Dawn runs its Null backend and keeps the WebGPU objects;
switch/deko executes the recorded frames on deko3d and presents them. So Aurora needs no
wgpu::Surface. Its final present pass draws into an offscreen texture that the adapter blits to the
swapchain, depth peeks use the adapter's own copy instead of a compute shader, and the caches use
their own file names.
"""
from pathlib import Path
import argparse
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]


def edit(root, name, old, new, count=1):
    path = root / name
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    found = text.count(old)
    if found == 0:
        raise SystemExit(f"Missing anchor in {name}:\n{old[:200]}")
    if count == 0:  # every occurrence
        text = text.replace(old, new)
    else:
        text = text.replace(old, new, count)
    path.write_text(text, encoding="utf-8", newline="\n")


def apply(root):
    # ---- lib/webgpu/gpu.hpp, gpu.cpp: no surface, an offscreen present target, init/shutdown
    edit(root, "lib/webgpu/gpu.hpp", "extern wgpu::Surface g_surface;",
         "extern wgpu::Surface g_surface;\n"
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "// The final present pass draws here; switch/deko blits it to the swapchain (kartpad_deko_present).\n"
         "extern wgpu::Texture g_dekoPresentTexture;\n"
         "#endif")
    edit(root, "lib/webgpu/gpu.cpp", '#include "gpu.hpp"',
         '#include "gpu.hpp"\n#if defined(AURORA_GFX_DEKO3D)\n#include "dk_aurora.h"\n'
         '#include "../gx/pipeline.hpp" // GXPipelineConfigVersion\n#endif')
    edit(root, "lib/webgpu/gpu.cpp", "wgpu::Surface g_surface;",
         "wgpu::Surface g_surface;\n#if defined(AURORA_GFX_DEKO3D)\nwgpu::Texture g_dekoPresentTexture;\n#endif")
    edit(root, "lib/webgpu/gpu.cpp",
         "static bool create_surface() {\n  SDL_Window* window = window::get_sdl_window();",
         "static bool create_surface() {\n#if defined(AURORA_GFX_DEKO3D)\n"
         "  return true; // switch/deko owns the swapchain; there is no wgpu::Surface\n#endif\n"
         "  SDL_Window* window = window::get_sdl_window();")
    edit(root, "lib/webgpu/gpu.cpp",
         "  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);",
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "  const wgpu::BackendType backend = wgpu::BackendType::Null; // deko3d owns the GPU\n"
         "  (void)to_wgpu_backend(auroraBackend);\n"
         "#else\n"
         "  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);\n"
         "#endif")
    edit(root, "lib/webgpu/gpu.cpp",
         "  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);",
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "  aurora_switch_dk_init(gx::GXPipelineConfigVersion);\n"
         "  const auto surfaceFormat = wgpu::TextureFormat::RGBA8Unorm;\n"
         "  g_vsyncEnabled.store(g_config.vsync, std::memory_order_release);\n"
         "  const auto presentMode = wgpu::PresentMode::Fifo;\n"
         "#else\n"
         "  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);")
    edit(root, "lib/webgpu/gpu.cpp",
         "  auto presentMode = select_present_mode(g_surfaceCapabilities);\n",
         "  auto presentMode = select_present_mode(g_surfaceCapabilities);\n#endif\n")
    edit(root, "lib/webgpu/gpu.cpp",
         "void shutdown() {\n  g_initialized = false;",
         "void shutdown() {\n#if defined(AURORA_GFX_DEKO3D)\n  aurora_switch_dk_shutdown();\n"
         "  g_dekoPresentTexture = {};\n#endif\n  g_initialized = false;")
    edit(root, "lib/webgpu/gpu.cpp",
         "  if (!g_surface || !g_device || width == 0 || height == 0 || nativeHeight == 0 || nativeWidth == 0) {",
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "  if (!g_device || width == 0 || height == 0 || nativeHeight == 0 || nativeWidth == 0) {\n"
         "#else\n"
         "  if (!g_surface || !g_device || width == 0 || height == 0 || nativeHeight == 0 || nativeWidth == 0) {\n"
         "#endif")
    edit(root, "lib/webgpu/gpu.cpp",
         "    g_surface.Configure(&surfaceConfiguration);",
         "#if !defined(AURORA_GFX_DEKO3D)\n    g_surface.Configure(&surfaceConfiguration);\n#endif")
    edit(root, "lib/webgpu/gpu.cpp",
         "  g_depthBuffer = create_depth_texture(width, height);\n  g_CopyBindGroup",
         "  g_depthBuffer = create_depth_texture(width, height);\n"
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "  g_dekoPresentTexture = create_render_texture(nativeWidth, nativeHeight, false).texture;\n"
         "#endif\n  g_CopyBindGroup")

    # ---- lib/aurora.cpp: no SDL renderer, no surface to acquire or present
    edit(root, "lib/aurora.cpp", "#include <aurora/aurora.h>",
         "#include <aurora/aurora.h>\n#if defined(AURORA_GFX_DEKO3D)\n#include \"kartpad_backend.h\"\n#endif")
    edit(root, "lib/aurora.cpp",
         "  if (webgpu::g_backendType == wgpu::BackendType::Null) {",
         "#if defined(AURORA_GFX_DEKO3D)\n  if (false) { // deko3d draws the Null device's frames; no SDL_Renderer\n#else\n"
         "  if (webgpu::g_backendType == wgpu::BackendType::Null) {\n#endif")
    edit(root, "lib/aurora.cpp",
         "    if (!g_surface) {\n      webgpu::refresh_surface(true);\n      if (!g_surface) {\n        return false;\n      }\n    }\n",
         "#if !defined(AURORA_GFX_DEKO3D)\n"
         "    if (!g_surface) {\n      webgpu::refresh_surface(true);\n      if (!g_surface) {\n        return false;\n      }\n    }\n"
         "#endif\n")
    edit(root, "lib/aurora.cpp",
         "      if (window::is_presentable() && g_surface) {\n        ZoneScopedN(\"Acquire texture\");",
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "      if (window::is_presentable() && webgpu::g_dekoPresentTexture) {\n"
         "        surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal;\n"
         "        currentTexture = webgpu::g_dekoPresentTexture;\n"
         "        currentView = currentTexture.CreateView();\n"
         "      }\n"
         "      if (false) {\n"
         "#else\n"
         "      if (window::is_presentable() && g_surface) {\n"
         "#endif\n        ZoneScopedN(\"Acquire texture\");")
    edit(root, "lib/aurora.cpp",
         "    if (canPresent && g_surface) {\n      ZoneScopedN(\"Present\");",
         "#if defined(AURORA_GFX_DEKO3D)\n    if (canPresent) {\n#else\n    if (canPresent && g_surface) {\n#endif\n"
         "      ZoneScopedN(\"Present\");")
    edit(root, "lib/aurora.cpp",
         "          status = g_surface.Present();",
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "          kartpad_deko_present(currentTexture.Get(), webgpu::g_graphicsConfig.surfaceConfiguration.width,\n"
         "                               webgpu::g_graphicsConfig.surfaceConfiguration.height);\n"
         "          status = wgpu::Status::Success;\n"
         "#else\n"
         "          status = g_surface.Present();\n"
         "#endif")

    # ---- lib/gx/shader.cpp: no textureNumLevels. Tint's GLSL writer lowers that builtin through a uniform
    # buffer of per-texture level counts, which the adapter does not provide (first deko3d boot: "texture
    # missing from texture_builtins_from_uniform list"). The branch is Aurora's workaround for black
    # samples on Intel Vulkan; its AURORA_LEGACY_TEX_LOD path is the unconditional bias sample.
    edit(root, "lib/gx/shader.cpp",
         '    static const bool legacyTexLod = getenv("AURORA_LEGACY_TEX_LOD") != nullptr;',
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "    static const bool legacyTexLod = true;\n"
         "#else\n"
         '    static const bool legacyTexLod = getenv("AURORA_LEGACY_TEX_LOD") != nullptr;\n'
         "#endif")

    # ---- lib/gfx/depth_peek.cpp: the adapter copies the depth image itself (no compute pass)
    edit(root, "lib/gfx/depth_peek.cpp", '#include "depth_peek.hpp"',
         '#include "depth_peek.hpp"\n#include <cstring>\n#if defined(AURORA_GFX_DEKO3D)\n#include "kartpad_backend.h"\n#endif')
    edit(root, "lib/gfx/depth_peek.cpp",
         "void initialize() {\n  if (!webgpu::g_hasCoreFeatures) {",
         "void initialize() {\n#if defined(AURORA_GFX_DEKO3D)\n"
         "  g_enabled = true; // snapshots are native (switch/deko): no compute pipeline\n  return;\n#endif\n"
         "  if (!webgpu::g_hasCoreFeatures) {")
    edit(root, "lib/gfx/depth_peek.cpp",
         "bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {\n  std::lock_guard lock{g_mutex};",
         "bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {\n#if defined(AURORA_GFX_DEKO3D)\n"
         "  return kartpad_deko_depth_read(x, y, z);\n#endif\n  std::lock_guard lock{g_mutex};")
    edit(root, "lib/gfx/depth_peek.cpp",
         "  const Params params = make_params(sourceSize, dstSize);\n",
         "  const Params params = make_params(sourceSize, dstSize);\n"
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "  static_assert(sizeof(params) == sizeof(KartPadDepthParams));\n"
         "  KartPadDepthParams nativeParams;\n"
         "  std::memcpy(&nativeParams, &params, sizeof(params));\n"
         "  kartpad_deko_depth_snapshot(cmd.Get(), depthView.Get(), nativeParams, gx::UseReversedZ);\n"
         "  return;\n"
         "#endif\n")

    # ---- cache files: the recipes are backend independent, what each backend built is not
    edit(root, "lib/gfx/pipeline_cache.cpp",
         'constexpr const char* InitialPipelineCacheName = "initial_pipeline_cache.db";',
         'constexpr const char* InitialPipelineCacheName = "initial_pipeline_cache.db";\n'
         "#if defined(AURORA_GFX_DEKO3D)\n"
         'constexpr const char* PipelineCacheName = "pipeline_cache_deko.db";\n'
         "#else\n"
         'constexpr const char* PipelineCacheName = "pipeline_cache.db";\n'
         "#endif")
    edit(root, "lib/gfx/pipeline_cache.cpp",
         '/ "pipeline_cache.db"', "/ PipelineCacheName", count=0)
    # ---- uber mode (perf.cfg uber 1): a draw drawn with the uber shader queues its own pipeline here.
    # Demand goes to the background queue, which waits for frame headroom; uber draws use that headroom
    # up (about 0.15 ms of GPU each), so on the console the real pipelines were never built and every
    # frame stayed on the uber shader (15 fps). With the DKSH seed a pipeline costs about 0.1 ms here,
    # so build it at once (Normal: the queue draws wait on); the uber shader only covers a real compile.
    edit(root, "lib/gfx/pipeline_cache.cpp",
         "  return find_pipeline_impl(ShaderType::GX, config, std::move(cb), PipelinePriority::Demand);",
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "  return find_pipeline_impl(ShaderType::GX, config, std::move(cb), PipelinePriority::Normal);\n"
         "#else\n"
         "  return find_pipeline_impl(ShaderType::GX, config, std::move(cb), PipelinePriority::Demand);\n"
         "#endif")
    # ---- FIFO thread CPU (results-screen profile, 2026-11-07: the FIFO thread is the limit there)
    # ByteBuffer::append runs once per index (prepare_idx_buffer) and per uniform field (build_uniform),
    # and resize() was not inlined because of its overflow report: 7.6% of the FIFO thread was the call
    # alone, plus a variable-size memcpy per 2-byte index. Keep "there is room" inline.
    edit(root, "lib/internal.hpp",
         "  void resize(size_t size, bool zeroed) {\n"
         "    if (size == 0) {\n",
         "  void resize(size_t size, bool zeroed) {\n"
         "    if (size != 0 && m_data != nullptr && size <= m_capacity) [[likely]] {\n"
         "      return;\n"
         "    }\n"
         "    resize_slow(size, zeroed);\n"
         "  }\n"
         "  [[gnu::noinline]] void resize_slow(size_t size, bool zeroed) {\n"
         "    if (size == 0) {\n")
    # quick_texture_sig hashes ~1.3 KB on every bind (about 430 binds a frame on the results screen,
    # 4.8% of the FIFO thread). Verify it once per texture per frame: an in-place content change is
    # still seen, at the next frame's first bind (texDataVersion still catches explicit invalidations).
    edit(root, "lib/gx/texture.cpp",
         "  uint64_t quickSig = 0;\n  uint64_t tlutQuickSig = 0;\n};",
         "  uint64_t quickSig = 0;\n  uint64_t tlutQuickSig = 0;\n"
         "  uint64_t sigCheckedFrame = ~uint64_t{0}; // quickSig verified in this frame (non-palette hits)\n};")
    edit(root, "lib/gx/texture.cpp",
         "  entry.quickSig = quick_texture_sig(obj);\n",
         "  entry.quickSig = quick_texture_sig(obj);\n  entry.sigCheckedFrame = s_frameCount;\n")
    edit(root, "lib/gx/texture.cpp",
         "entry.format == obj.format() && entry.quickSig == quick_texture_sig(obj)) {\n"
         "        entry.lastUsedFrame = s_frameCount;\n",
         "entry.format == obj.format() &&\n"
         "          (entry.sigCheckedFrame == s_frameCount || entry.quickSig == quick_texture_sig(obj))) {\n"
         "        entry.sigCheckedFrame = s_frameCount;\n"
         "        entry.lastUsedFrame = s_frameCount;\n")
    # And only a draw that really needs a compile is drawn with the uber shader: one whose shaders are
    # in the DKSH caches gets its real pipeline at once, as without the uber shader (the uber shader is
    # several times the GPU cost of a specialised one). Positive answers are kept per shader config:
    # the WGSL is generated only for configs not seen ready before (the FIFO thread, one caller).
    edit(root, "lib/gx/command_processor.cpp", '#include "command_processor.hpp"',
         '#include "command_processor.hpp"\n'
         "#if defined(AURORA_GFX_DEKO3D)\n"
         '#include "kartpad_backend.h"\n'
         '#include "../gfx/hash.hpp"\n'
         "#include <absl/container/flat_hash_set.h>\n"
         "#endif")
    edit(root, "lib/gx/command_processor.cpp",
         "    if (cache.uberSupported) {\n"
         "      const auto config = cache.config;\n",
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "    if (cache.uberSupported) {\n"
         "      static absl::flat_hash_set<u64> sCachedShaderConfigs;\n"
         "      const u64 shaderKey = xxh3_hash(cache.config.shaderConfig);\n"
         "      if (sCachedShaderConfigs.contains(shaderKey) ||\n"
         "          kartpad_deko_wgsl_cached(build_shader_source(cache.config.shaderConfig))) {\n"
         "        sCachedShaderConfigs.insert(shaderKey);\n"
         "        cache.uberSupported = false;\n"
         "      }\n"
         "    }\n"
         "#endif\n"
         "    if (cache.uberSupported) {\n"
         "      const auto config = cache.config;\n")
    edit(root, "lib/webgpu/gpu_cache.cpp",
         '/ "dawn_cache.db";',
         "/\n#if defined(AURORA_GFX_DEKO3D)\n      \"dawn_null_deko_cache.db\";\n#else\n      \"dawn_cache.db\";\n#endif")

    # ---- cmake: Dawn is built with the Null backend only
    edit(root, "cmake/AuroraDawnProvider.cmake",
         "foreach (_backend D3D12 D3D11 METAL DESKTOP_GL OPENGLES NULL)",
         "foreach (_backend D3D12 D3D11 METAL DESKTOP_GL OPENGLES VULKAN)")
    edit(root, "cmake/AuroraDawnProvider.cmake",
         'set(DAWN_ENABLE_VULKAN ON CACHE INTERNAL "" FORCE)',
         'set(DAWN_ENABLE_NULL ON CACHE INTERNAL "" FORCE)')

    # ---- lib/gfx/texture_convert.cpp: opt-in CMPR -> BC1 (perf.cfg texture_cmpr_bc1 1)
    edit(root, "lib/gfx/texture_convert.cpp",
         "ConvertedTexture convert_texture(u32 format, uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {\n  ZoneScoped;",
         CMPR_BC1_HELPERS +
         "ConvertedTexture convert_texture(u32 format, uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {\n  ZoneScoped;")
    edit(root, "lib/gfx/texture_convert.cpp",
         "  case GX_TF_CMPR:\n    converted = BuildRGBA8FromCMPR(width, height, mips, data);\n    break;",
         "  case GX_TF_CMPR:\n"
         "#if defined(AURORA_GFX_DEKO3D)\n"
         "    if (cmpr_uses_bc1(width, height)) {\n"
         "      // One BC1 block per CMPR block: an eighth of the RGBA8 bytes go to the GPU. The arbitrary-mip\n"
         "      // decision is the RGBA8 path's, from a transient decode.\n"
         "      const bool arbitraryMips =\n"
         "          mips > 1 && arb_mip_check(width, height, mips, BuildRGBA8FromCMPR(width, height, mips, data));\n"
         "      return {\n"
         "          .format = wgpu::TextureFormat::BC1RGBAUnorm,\n"
         "          .width = width,\n"
         "          .height = height,\n"
         "          .mips = mips,\n"
         "          .data = BuildBC1FromCMPR(width, height, mips, data),\n"
         "          .hasArbitraryMips = arbitraryMips,\n"
         "      };\n"
         "    }\n"
         "#endif\n"
         "    converted = BuildRGBA8FromCMPR(width, height, mips, data);\n    break;")


CMPR_BC1_HELPERS = '''#if defined(AURORA_GFX_DEKO3D)
// CMPR tiles hold four DXT1 blocks; each becomes one BC1 block in row-major order.
// The colors are big-endian RGB565 (swapped to little-endian, same order, so the color0 > color1 mode
// choice is unchanged) and GX holds pixel 0 of a row in index bits 7:6 where BC1 holds it in bits 1:0.
// Known difference: in the three-color mode GX gives the transparent texel the midpoint color, BC1 black.
static ByteBuffer BuildBC1FromCMPR(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {
  size_t total = 0;
  for (uint32_t mip = 0, w = width, h = height; mip < mips; ++mip) {
    total += static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 8;
    w = std::max(w >> 1, 1u);
    h = std::max(h >> 1, 1u);
  }
  ByteBuffer buf{total};
  uint8_t* dst = buf.data();
  const uint8_t* src = data.data();
  uint32_t w = width;
  uint32_t h = height;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t blocksWide = (w + 3) / 4;
    for (uint32_t yy = 0; yy < h; yy += 8) {
      for (uint32_t xx = 0; xx < w; xx += 8) {
        for (uint32_t yb = 0; yb < 8; yb += 4) {
          for (uint32_t xb = 0; xb < 8; xb += 4, src += 8) {
            if (xx + xb >= w || yy + yb >= h) {
              continue;
            }
            uint8_t* out = dst + (static_cast<size_t>((yy + yb) / 4) * blocksWide + (xx + xb) / 4) * 8;
            out[0] = src[1];
            out[1] = src[0];
            out[2] = src[3];
            out[3] = src[2];
            for (uint32_t y = 0; y < 4; ++y) {
              const uint8_t b = src[4 + y];
              out[4 + y] = static_cast<uint8_t>((b >> 6) | ((b >> 2) & 0x0C) | ((b << 2) & 0x30) | (b << 6));
            }
          }
        }
      }
    }
    dst += static_cast<size_t>(blocksWide) * ((h + 3) / 4) * 8;
    w = std::max(w >> 1, 1u);
    h = std::max(h >> 1, 1u);
  }
  return buf;
}

extern "C" int melee_nx_perf_int(const char* key, int fallback) __attribute__((weak));

// Off unless perf.cfg says `texture_cmpr_bc1 1`: it changes what every CMPR texture looks like to the GPU.
static bool cmpr_uses_bc1(uint32_t width, uint32_t height) noexcept {
  static const bool enabled = melee_nx_perf_int != nullptr && melee_nx_perf_int("texture_cmpr_bc1", 0) != 0;
  return enabled && webgpu::g_bcTexturesSupported && width % 4 == 0 && height % 4 == 0;
}
#endif

'''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", type=Path, default=ROOT / "ref/melee-pc/extern/aurora")
    parser.add_argument("--dst", type=Path, default=ROOT / "build/aurora-deko")
    args = parser.parse_args()
    src, dst = args.src.resolve(), args.dst.resolve()
    if not (src / "CMakeLists.txt").is_file():
        sys.exit(f"Aurora source not found: {src}")
    if dst == src or src in dst.parents:
        sys.exit("--dst must be outside the reference tree")
    # Refresh the copy from the reference every run: edits are idempotent, but a changed reference
    # (new patch, new pin) must not leave stale files behind.
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns(".git"))
    apply(dst)
    print(f"Aurora staged for deko3d: {dst}")


if __name__ == "__main__":
    main()
