// Native deko3d adapter, part 2: shader modules and render pipelines. A pipeline's WGSL stages
// are translated (Tint) and compiled (uam) to native shaders, or found in the shader cache, and
// its WebGPU state is converted to deko3d state. See backend_internal.h for the whole adapter.
#include "backend_internal.h"
#include <sys/stat.h>

namespace kd {
std::unordered_map<std::string, Stage> stageCache;
std::unordered_map<WGPURenderPipeline, std::shared_ptr<Pipeline>> pipelines;
std::unordered_map<WGPUShaderModule, std::pair<std::string, int>> modules;
// A shader Tint or uam refuses is the first thing to look at: keep its text and the reason beside the
// log (Logs/deko-fail-<hash>-<stage>.<ext> and .txt), so it can be run through the translator offline.
void dump_failed_shader(const std::string &key, ShaderStage stage, const char *extension,
                        const std::string &text, const std::string &reason) {
  mkdir(COS_SWITCH_DATA_DIR "/Logs", 0777);
  char path[192];
  const unsigned long long hash = XXH3_64bits(key.data(), key.size());
  snprintf(path, sizeof path, COS_SWITCH_DATA_DIR "/Logs/deko-fail-%016llx-%u.%s", hash,
           static_cast<unsigned>(stage), extension);
  if (FILE *out = fopen(path, "wb")) {
    fwrite(text.data(), 1, text.size(), out);
    fclose(out);
  }
  snprintf(path, sizeof path, COS_SWITCH_DATA_DIR "/Logs/deko-fail-%016llx-%u.txt", hash,
           static_cast<unsigned>(stage));
  if (FILE *out = fopen(path, "wb")) {
    fprintf(out, "%s\nstage cache key: %s\n", reason.c_str(), key.c_str());
    fclose(out);
  }
  swdk::dklog("shader failure saved: %s/Logs/deko-fail-%016llx-%u.*", COS_SWITCH_DATA_DIR, hash,
              static_cast<unsigned>(stage));
}
Stage compile(WGPUShaderModule module, WGPUStringView entry, ShaderStage stage) {
  std::string source;
  {
    Lock l(mutex);
    auto it = modules.find(module);
    if (it == modules.end())
      fatal("Missing native shader module %p for entry %s", static_cast<void *>(module),
            str(entry).c_str());
    source = it->second.first;
  }
  // compilerMutex guards the stage memo only. Tint and the wait for uam run outside it, so several
  // pipeline workers translate at once while uam's one worker compiles (the boot preload builds on
  // every core). Two workers may translate the same stage; the first to finish fills the memo.
  const auto key = std::to_string(XXH3_64bits(source.data(), source.size())) + ":" +
                   std::to_string(static_cast<unsigned>(stage)) + ":" + str(entry);
  const NamedShader* cached = nullptr;
  {
    std::lock_guard compilerLock(compilerMutex);
    if (auto it = stageCache.find(key); it != stageCache.end())
      return it->second;
    if ((cached = shader_cache_named(key.c_str())))
      stageCache.emplace(key, Stage{cached->entry, cached->bindings});
  }
  if (cached) {
    Stage s{cached->entry, cached->bindings};
    shader_note_stage(key, stage, s.shader->glslHash, s.slots);
    return s;
  }
  ShaderCompileTrace trace(key, static_cast<unsigned>(stage), source);
  auto r = wgsl_to_glsl(source, str(entry), stage, BindingScheme::Sequential);
  trace.phase("tint-exit");
  if (!r.ok) {
    dump_failed_shader(key, stage, "wgsl", source, r.error);
    fatal("Tint: %s", r.error.c_str());
  }
  Stage s;
  s.slots = std::move(r.bindings);
  s.shader = shader_entry(stage, XXH3_64bits(r.glsl.data(), r.glsl.size()));
#ifdef COS_DK_DEBUG_LIB
  if (FILE *flag = fopen("sdmc:/switch/melee-nx/deko-capture.flag", "rb")) {
    fclose(flag);
    mkdir("sdmc:/switch/melee-nx/Logs/deko-capture", 0777);
    char path[192];
    for (const auto &item : {std::pair{"wgsl", &source}, std::pair{"glsl", &r.glsl}}) {
      snprintf(path, sizeof path, "sdmc:/switch/melee-nx/Logs/deko-capture/shader-%016llx-%u.%s",
               (unsigned long long)s.shader->glslHash, unsigned(stage), item.first);
      if (FILE *out = fopen(path, "wb")) {
        fwrite(item.second->data(), 1, item.second->size(), out);
        fclose(out);
      }
    }
  }
#endif
  shader_compile(s.shader, r.glsl, true);
  if (!shader_wait(s.shader)) {
    dump_failed_shader(key, stage, "glsl", r.glsl, "uam refused this GLSL (Tint output after the post-pass)");
    fatal("uam failed compiling %s", str(entry).c_str());
  }
  trace.phase("done");
  {
    std::lock_guard compilerLock(compilerMutex);
    if (!stageCache.emplace(key, s).second)
      return stageCache.at(key);
  }
  shader_record_stage(key, stage, s.shader->glslHash, s.slots, source);
  return s;
}
} // namespace kd
// compile()'s stage keys for the GX entry points.
bool kartpad_deko_wgsl_cached(const std::string &wgsl) {
  const std::string hash = std::to_string(XXH3_64bits(wgsl.data(), wgsl.size())) + ":";
  const std::string keys[] = {
      hash + std::to_string(static_cast<unsigned>(swdk::ShaderStage::Vertex)) + ":vs_main",
      hash + std::to_string(static_cast<unsigned>(swdk::ShaderStage::Fragment)) + ":fs_main"};
  std::lock_guard compilerLock(kd::compilerMutex);
  for (const std::string &key : keys)
    if (!kd::stageCache.count(key) && !swdk::shader_cache_named(key.c_str()))
      return false;
  return true;
}
namespace kd {
// Where pipeline creation time goes, summed over all creations and logged every 64 pipelines:
// the native stage lookup or compile, and the rest of the pipeline (state conversion). Shader
// modules and pipelines never reach Dawn (see the wrappers below).
struct BuildTimes {
  std::atomic<uint64_t> moduleUs{0}, modules{0}, stagesUs{0}, stateUs{0}, pipelines{0};
};
BuildTimes buildTimes;
uint64_t micros_since(std::chrono::steady_clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count());
}
void count_pipeline_build(uint64_t stagesUs, uint64_t stateUs) {
  buildTimes.stagesUs += stagesUs;
  buildTimes.stateUs += stateUs;
  const uint64_t n = ++buildTimes.pipelines;
  if (n % 64)
    return;
  const uint64_t modules = std::max<uint64_t>(buildTimes.modules.load(), 1);
  swdk::dklog("pipeline builds: %llu pipelines; per pipeline: native stages %.1f ms, state "
              "%.2f ms; per shader module (%llu): %.2f ms",
              (unsigned long long)n, buildTimes.stagesUs.load() / 1000.0 / n,
              buildTimes.stateUs.load() / 1000.0 / n, (unsigned long long)modules,
              buildTimes.moduleUs.load() / 1000.0 / modules);
}
DkCompareOp compare(WGPUCompareFunction f) {
  return f == WGPUCompareFunction_Undefined
             ? DkCompareOp_Always
             : DkCompareOp(DkCompareOp_Never + int(f) - int(WGPUCompareFunction_Never));
}
DkBlendFactor factor(WGPUBlendFactor f, bool alpha) {
  switch (f) {
  case WGPUBlendFactor_Zero:
    return DkBlendFactor_Zero;
  case WGPUBlendFactor_One:
    return DkBlendFactor_One;
  case WGPUBlendFactor_Src:
    return DkBlendFactor_SrcColor;
  case WGPUBlendFactor_OneMinusSrc:
    return DkBlendFactor_InvSrcColor;
  case WGPUBlendFactor_SrcAlpha:
    return DkBlendFactor_SrcAlpha;
  case WGPUBlendFactor_OneMinusSrcAlpha:
    return DkBlendFactor_InvSrcAlpha;
  case WGPUBlendFactor_Dst:
    return DkBlendFactor_DstColor;
  case WGPUBlendFactor_OneMinusDst:
    return DkBlendFactor_InvDstColor;
  case WGPUBlendFactor_DstAlpha:
    return DkBlendFactor_DstAlpha;
  case WGPUBlendFactor_OneMinusDstAlpha:
    return DkBlendFactor_InvDstAlpha;
  case WGPUBlendFactor_Constant:
    return alpha ? DkBlendFactor_ConstAlpha : DkBlendFactor_ConstColor;
  case WGPUBlendFactor_OneMinusConstant:
    return alpha ? DkBlendFactor_InvConstAlpha : DkBlendFactor_InvConstColor;
  case WGPUBlendFactor_SrcAlphaSaturated:
    return DkBlendFactor_SrcAlphaSaturate;
  case WGPUBlendFactor_Src1:
    return DkBlendFactor_Src1Color;
  case WGPUBlendFactor_OneMinusSrc1:
    return DkBlendFactor_InvSrc1Color;
  case WGPUBlendFactor_Src1Alpha:
    return DkBlendFactor_Src1Alpha;
  case WGPUBlendFactor_OneMinusSrc1Alpha:
    return DkBlendFactor_InvSrc1Alpha;
  default:
    fatal("Unsupported blend factor %d", int(f));
  }
}
DkBlendOp blendop(WGPUBlendOperation o) {
  switch (o) {
  case WGPUBlendOperation_Subtract:
    return DkBlendOp_Sub;
  case WGPUBlendOperation_ReverseSubtract:
    return DkBlendOp_RevSub;
  case WGPUBlendOperation_Min:
    return DkBlendOp_Min;
  case WGPUBlendOperation_Max:
    return DkBlendOp_Max;
  default:
    return DkBlendOp_Add;
  }
}
} // namespace kd

extern "C" {
// A WGSL module is the adapter's own handle, not a Dawn object: nothing downstream uses Dawn's
// module (pipelines, SetPipeline and draws are native; the depth-peek compute pipeline is compiled
// out under deko), and Dawn's WGSL parse, validation and reflection cost 12-33 ms per module on
// the console. The WGSL is kept for compile(), which runs Tint only when the shader cache misses.
WGPUShaderModule __wrap_wgpuDeviceCreateShaderModule(WGPUDevice d,
                                                     const WGPUShaderModuleDescriptor *desc) {
  const WGPUShaderSourceWGSL *wgsl = nullptr;
  for (auto c = desc ? desc->nextInChain : nullptr; c; c = c->next)
    if (c->sType == WGPUSType_ShaderSourceWGSL)
      wgsl = reinterpret_cast<const WGPUShaderSourceWGSL *>(c);
  if (!wgsl)
    return __real_wgpuDeviceCreateShaderModule(d, desc);
  const auto start = std::chrono::steady_clock::now();
  auto h = reinterpret_cast<WGPUShaderModule>(new char);
  {
    kd::Lock l(kd::mutex);
    kd::modules[h] = {kd::str(wgsl->code), 1};
  }
  kd::buildTimes.moduleUs += kd::micros_since(start);
  ++kd::buildTimes.modules;
  return h;
}
void __wrap_wgpuShaderModuleAddRef(WGPUShaderModule h) {
  {
    kd::Lock l(kd::mutex);
    if (auto it = kd::modules.find(h); it != kd::modules.end()) {
      ++it->second.second;
      return;
    }
  }
  __real_wgpuShaderModuleAddRef(h);
}
void __wrap_wgpuShaderModuleRelease(WGPUShaderModule h) {
  {
    kd::Lock l(kd::mutex);
    if (auto it = kd::modules.find(h); it != kd::modules.end()) {
      if (!--it->second.second) {
        kd::modules.erase(it);
        delete reinterpret_cast<char *>(h);
      }
      return;
    }
  }
  __real_wgpuShaderModuleRelease(h);
}
WGPURenderPipeline __wrap_wgpuDeviceCreateRenderPipeline(WGPUDevice d,
                                                         const WGPURenderPipelineDescriptor *desc) {
  // Build privately. A partially translated pipeline is never published, and
  // the render worker can upload, submit and present while this call compiles.
  auto p = std::make_shared<kd::Pipeline>();
  p->label = kd::str(desc->label);
  const auto stagesStart = std::chrono::steady_clock::now();
  p->vs = kd::compile(desc->vertex.module, desc->vertex.entryPoint, swdk::ShaderStage::Vertex);
  if (!desc->fragment)
    swdk::fatal("Pipeline has no fragment stage");
  p->fs =
      kd::compile(desc->fragment->module, desc->fragment->entryPoint, swdk::ShaderStage::Fragment);
  const uint64_t stagesUs = kd::micros_since(stagesStart);
  const auto stateStart = std::chrono::steady_clock::now();
  dkRasterizerStateDefaults(&p->raster);
  dkDepthStencilStateDefaults(&p->depth);
  // WebGPU omitting depthStencil disables both operations. deko3d's defaults
  // enable Less testing and writes, which must never leak into color-only
  // snapshot-copy, display-copy or ImGui passes.
  p->depth.depthTestEnable = false;
  p->depth.depthWriteEnable = false;
  dkColorStateDefaults(&p->color);
  dkColorWriteStateDefaults(&p->write);
  dkBlendStateDefaults(&p->blend);
  p->raster.cullMode = desc->primitive.cullMode == WGPUCullMode_Back    ? DkFace_Back
                       : desc->primitive.cullMode == WGPUCullMode_Front ? DkFace_Front
                                                                        : DkFace_None;
  p->raster.frontFace =
      desc->primitive.frontFace == WGPUFrontFace_CW ? DkFrontFace_CW : DkFrontFace_CCW;
  switch (desc->primitive.topology) {
  case WGPUPrimitiveTopology_PointList:
    p->topology = DkPrimitive_Points;
    break;
  case WGPUPrimitiveTopology_LineList:
    p->topology = DkPrimitive_Lines;
    break;
  case WGPUPrimitiveTopology_LineStrip:
    p->topology = DkPrimitive_LineStrip;
    break;
  case WGPUPrimitiveTopology_TriangleStrip:
    p->topology = DkPrimitive_TriangleStrip;
    break;
  default:
    break;
  }
  if (desc->depthStencil) {
    const auto &ds = *desc->depthStencil;
    p->depth.depthTestEnable = true;
    p->depth.depthWriteEnable = ds.depthWriteEnabled == WGPUOptionalBool_True;
    p->depth.depthCompareOp = kd::compare(ds.depthCompare);
    p->bias[0] = ds.depthBias;
    p->bias[1] = ds.depthBiasClamp;
    p->bias[2] = ds.depthBiasSlopeScale;
    p->raster.depthBiasEnableMask =
        (ds.depthBias || ds.depthBiasSlopeScale) ? DkPolygonFlag_All : 0;
  }
  const auto &target = desc->fragment->targets[0];
  dkColorWriteStateSetMask(&p->write, 0, target.writeMask & 15);
  if (target.blend) {
    const auto &b = *target.blend;
    dkColorStateSetBlendEnable(&p->color, 0, true);
    dkBlendStateSetOps(&p->blend, kd::blendop(b.color.operation), kd::blendop(b.alpha.operation));
    dkBlendStateSetFactors(
        &p->blend, kd::factor(b.color.srcFactor, false), kd::factor(b.color.dstFactor, false),
        kd::factor(b.alpha.srcFactor, true), kd::factor(b.alpha.dstFactor, true));
  }
  p->vertexStates.resize(desc->vertex.bufferCount);
  for (size_t i = 0; i < desc->vertex.bufferCount; ++i) {
    const auto &b = desc->vertex.buffers[i];
    p->vertexStates[i].stride = b.arrayStride;
    p->vertexStates[i].divisor = b.stepMode == WGPUVertexStepMode_Instance ? 1 : 0;
    for (size_t j = 0; j < b.attributeCount; ++j) {
      const auto &a = b.attributes[j];
      if (p->attributes.size() <= a.shaderLocation)
        p->attributes.resize(a.shaderLocation + 1);
      auto &v = p->attributes[a.shaderLocation];
      v.bufferId = i;
      v.offset = a.offset;
      switch (a.format) {
      case WGPUVertexFormat_Float32x2:
        v.size = DkVtxAttribSize_2x32;
        v.type = DkVtxAttribType_Float;
        break;
      case WGPUVertexFormat_Float32x3:
        v.size = DkVtxAttribSize_3x32;
        v.type = DkVtxAttribType_Float;
        break;
      case WGPUVertexFormat_Float32x4:
        v.size = DkVtxAttribSize_4x32;
        v.type = DkVtxAttribType_Float;
        break;
      case WGPUVertexFormat_Unorm8x4:
        v.size = DkVtxAttribSize_4x8;
        v.type = DkVtxAttribType_Unorm;
        break;
      default:
        swdk::fatal("Unsupported vertex attribute %d", int(a.format));
      }
    }
  }
  // The handle is the native pipeline itself (no Dawn pipeline: its modules are not Dawn objects).
  // Its address cannot be reused while a recorded SetPipeline still holds the shared_ptr.
  auto h = reinterpret_cast<WGPURenderPipeline>(p.get());
  kd::count_pipeline_build(stagesUs, kd::micros_since(stateStart));
  kd::Lock l(kd::mutex);
  kd::pipelines[h] = p;
  return h;
}
void __wrap_wgpuRenderPipelineAddRef(WGPURenderPipeline h) {
  {
    kd::Lock l(kd::mutex);
    if (auto it = kd::pipelines.find(h); it != kd::pipelines.end()) {
      ++it->second->refs;
      return;
    }
  }
  __real_wgpuRenderPipelineAddRef(h);
}
void __wrap_wgpuRenderPipelineRelease(WGPURenderPipeline h) {
  {
    kd::Lock l(kd::mutex);
    if (auto it = kd::pipelines.find(h); it != kd::pipelines.end()) {
      if (!--it->second->refs)
        kd::pipelines.erase(it);
      return;
    }
  }
  __real_wgpuRenderPipelineRelease(h);
}
}
