// Native deko3d adapter, part 3: command recording and replay. Encoders and render passes
// record closures; queue submission replays them into the frame's deko3d command buffer, where
// the draw state (bindings, pipeline, split RGB/alpha blending) is applied. See
// backend_internal.h for the whole adapter.
#include "backend_internal.h"
#ifdef COS_DK_DEBUG_LIB
#include "present_vsh_dksh.h"
#include "probe_fsh_dksh.h"
#include <sys/stat.h>
#endif
extern "C" void kartpad_deko_log_failure_context();
// switch/src/transition_guard.cpp; weak, so builds without the game (renderer tests) link.
extern "C" int kartpad_gpu_transition_guard(void) __attribute__((weak));
extern "C" int kartpad_opt(const char *key, int fallback) __attribute__((weak));

namespace kd {
std::unordered_map<WGPUCommandEncoder, std::shared_ptr<Commands>> encoders;
std::unordered_map<WGPUCommandEncoder, int> encoderRefs;
std::unordered_map<WGPUCommandBuffer, std::pair<std::shared_ptr<Commands>, int>> commands;
std::unordered_map<WGPURenderPassEncoder, Pass> passes;
State state;
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
namespace {
const char *gpuOperation = "<none>";
uint64_t gpuArgs[3]{}, gpuOperationSerial = 0, gpuProgressSerial = 0;
struct OperationRecord {
  const char *name = "<none>";
  uint64_t serial = 0, args[3]{};
};
std::array<OperationRecord, 32> gpuRecent{};
struct ImageCopyRecord {
  uint64_t serial = 0, srcAddress = 0, dstAddress = 0;
  uint32_t srcSize = 0, dstSize = 0, srcFormat = 0, dstFormat = 0;
  uint32_t srcMip = 0, dstMip = 0, srcOrigin[3]{}, dstOrigin[3]{};
  char source[96]{}, destination[96]{};
} gpuImageCopy;
// The bytes snprintf(out, N, "%s", label) writes, without the printf machinery: breadcrumbs
// copy two labels twice for every GPU operation on the frame worker.
template <size_t N> void copy_label(char (&out)[N], const char *label) {
  const size_t n = strnlen(label, N - 1);
  std::memcpy(out, label, n);
  out[n] = '\0';
}
#ifdef COS_DK_GPU_BREADCRUMBS
// Reports write a value and timestamp into permanent, CPU-uncached memory.
// Neither this storage nor its CPU history moves while commands are in flight.
// Reuse requires the old GPU-written serial, rather than a CPU submit count.
constexpr size_t kGpuBreadcrumbs = 16384;
struct Breadcrumb {
  OperationRecord operation{nullptr, 0, {}};
  uint64_t frame = 0, vs = 0, fs = 0, targetAddress = 0, depthAddress = 0;
  uint64_t indexAddress = 0, indexBytes = 0;
  uint32_t width = 0, height = 0;
  char pipeline[96]{}, target[96]{};
  ImageCopyRecord copy;
  bool recorded = false;
};
std::array<Breadcrumb, kGpuBreadcrumbs> gpuBreadcrumbs;
Block gpuReports;
void log_gpu_breadcrumbs();
uint64_t breadcrumb_value(uint64_t serial) {
  if (!gpuReports) return 0;
  return reinterpret_cast<volatile const uint64_t *>(gpuReports.cpu)[(serial % kGpuBreadcrumbs) * 2];
}
void breadcrumb_context(Breadcrumb &record) {
  const auto &p = state.pipeline;
  const auto *v = state.target ? find_view(state.target.Get()) : nullptr;
  const auto *d = state.depthTarget ? find_view(state.depthTarget.Get()) : nullptr;
  const auto *t = v ? v->tex : nullptr;
  record.vs = p && p->vs.shader ? p->vs.shader->glslHash : 0;
  record.fs = p && p->fs.shader ? p->fs.shader->glslHash : 0;
  record.targetAddress = t ? dkImageGetGpuAddr(&t->image) : 0;
  record.depthAddress = d ? dkImageGetGpuAddr(&d->tex->image) : 0;
  record.width = t ? t->width : 0; record.height = t ? t->height : 0;
  record.indexAddress = state.index ? state.index->mem.gpu + state.indexOffset : 0;
  record.indexBytes = state.index ? state.index->size - state.indexOffset : 0;
  copy_label(record.pipeline, p ? p->label.c_str() : "<none>");
  copy_label(record.target, t ? t->label.c_str() : "<none>");
}
void breadcrumb_begin() {
  // ReportValue has a 32-bit payload. Stop rather than confuse generations on wrap.
  if (gpuOperationSerial > UINT32_MAX) fatal("GPU breadcrumb serial exhausted");
  auto &record = gpuBreadcrumbs[gpuOperationSerial % kGpuBreadcrumbs];
  if (record.operation.serial &&
      (!record.recorded || breadcrumb_value(record.operation.serial) != record.operation.serial)) {
    log_gpu_breadcrumbs();
    fatal("GPU breadcrumb history full: refusing to overwrite unfinished operation %llu",
          (unsigned long long)record.operation.serial);
  }
  record = {};
  record.operation = {gpuOperation, gpuOperationSerial, {gpuArgs[0], gpuArgs[1], gpuArgs[2]}};
  record.frame = R.frame + 1;
  breadcrumb_context(record);
}
void breadcrumb_end() {
  if (!gpuReports) {
    gpuReports = block_create(kGpuBreadcrumbs * 16, "GPU completion breadcrumbs");
    if (!gpuReports) fatal("Cannot allocate GPU completion breadcrumbs");
    std::memset(gpuReports.cpu, 0, gpuReports.size);
  }
  auto &record = gpuBreadcrumbs[gpuOperationSerial % kGpuBreadcrumbs];
  breadcrumb_context(record);
  if (gpuImageCopy.serial == gpuOperationSerial) record.copy = gpuImageCopy;
  dkCmdBufReportValue(R.cmd, uint32_t(gpuOperationSerial),
                      gpuReports.gpu + (gpuOperationSerial % kGpuBreadcrumbs) * 16);
  record.recorded = true;
  static uint64_t nextSampleFrame = 0;
  if (record.frame >= nextSampleFrame) {
    const uint64_t sample = gpuOperationSerial > 2048 ? gpuOperationSerial - 2048 : 1;
    dklog("GPU progress sample: CPU=%llu sample_serial=%llu GPU_value=%llu",
          (unsigned long long)gpuOperationSerial, (unsigned long long)sample,
          (unsigned long long)breadcrumb_value(sample));
    nextSampleFrame = record.frame + 300;
  }
}
void log_gpu_breadcrumbs() {
  const uint64_t oldest = gpuOperationSerial >= kGpuBreadcrumbs ? gpuOperationSerial - kGpuBreadcrumbs + 1 : 1;
  uint64_t firstPending = 0, lastReported = 0;
  for (uint64_t serial = oldest; serial <= gpuOperationSerial; ++serial) {
    const auto &record = gpuBreadcrumbs[serial % kGpuBreadcrumbs];
    if (record.operation.serial != serial) continue;
    if (record.recorded && breadcrumb_value(serial) == serial)
      lastReported = serial;
    else if (!firstPending)
      firstPending = serial;
  }
  std::fprintf(stderr, "[gpu-progress] CPU=%llu retained_from=%llu last_GPU_report=%llu first_missing=%llu report_addr=0x%llx\n",
      (unsigned long long)gpuOperationSerial, (unsigned long long)oldest,
      (unsigned long long)lastReported, (unsigned long long)firstPending,
      (unsigned long long)gpuReports.gpu);
  const uint64_t focus = firstPending ? firstPending : gpuOperationSerial;
  for (uint64_t serial = std::max(oldest, focus > 8 ? focus - 8 : 1);
       serial <= std::min(gpuOperationSerial, focus + 24); ++serial) {
    const auto &r = gpuBreadcrumbs[serial % kGpuBreadcrumbs];
    const auto &op = r.operation;
    if (op.serial != serial) continue;
    std::fprintf(stderr, "[gpu-breadcrumb] serial=%llu frame=%llu recorded=%u GPU_value=%llu op=%s args=%llu/%llu/%llu pipeline=%s vs=%016llx fs=%016llx target=%s %ux%u addr=0x%llx depth=0x%llx index=0x%llx bytes=%llu\n",
        (unsigned long long)serial, (unsigned long long)r.frame, unsigned(r.recorded),
        (unsigned long long)breadcrumb_value(serial), op.name,
        (unsigned long long)op.args[0], (unsigned long long)op.args[1], (unsigned long long)op.args[2],
        r.pipeline, (unsigned long long)r.vs, (unsigned long long)r.fs, r.target, r.width, r.height,
        (unsigned long long)r.targetAddress, (unsigned long long)r.depthAddress,
        (unsigned long long)r.indexAddress, (unsigned long long)r.indexBytes);
    if (r.copy.serial == serial)
      std::fprintf(stderr, "[gpu-breadcrumb-copy] serial=%llu source=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u destination=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u\n",
          (unsigned long long)serial, r.copy.source, (unsigned long long)r.copy.srcAddress, r.copy.srcSize,
          r.copy.srcFormat, r.copy.srcMip, r.copy.srcOrigin[0], r.copy.srcOrigin[1], r.copy.srcOrigin[2],
          r.copy.destination, (unsigned long long)r.copy.dstAddress, r.copy.dstSize,
          r.copy.dstFormat, r.copy.dstMip, r.copy.dstOrigin[0], r.copy.dstOrigin[1], r.copy.dstOrigin[2]);
  }
}
#endif
#ifdef COS_DK_GPU_FLUSHES
uint32_t gpuOperationsPending = 0;
// Outside course transitions, native submission works as without these diagnostics (candidate C,
// docs/RR-PERFORMANCE-2026-10.md): the frame's own submits, no per-operation report and no pass,
// copy or interval flush. Every recorded 2348-0006 timeout came while a course loaded (the first one
// after the menus, or the next one after a race), where the guard below makes each operation
// complete; v9's policy in steady state cost ~80 submits
// and +3-6 ms of recording per present, and the game thread's GX wait rose from 6 to 45-230 ms/s.
// It did not prevent the timeout either (test 6). perf.cfg `gpu_steady_v9 1` restores it everywhere.
bool steady_v9() {
  static const bool on = kartpad_opt && kartpad_opt("gpu_steady_v9", 0) != 0;
  return on;
}
#ifdef COS_DK_GPU_BREADCRUMBS
bool gpuOperationTracked = false;  // this operation's breadcrumb was begun (gpu_operation_begin)
#endif
// Course loads: the guard (switch/src/transition_guard.cpp) is asked at most every 5 ms;
// R.synchronous follows it.
bool synchronous_operation() {
  if (!kartpad_gpu_transition_guard) return false;  // no scene guard in this build: never synchronous
  static uint64_t nextCheck = 0;
  const uint64_t now = now_ns();
  if (now >= nextCheck) {
    nextCheck = now + 5'000'000;
    const bool synchronous = kartpad_gpu_transition_guard && kartpad_gpu_transition_guard() != 0;
    if (synchronous != R.synchronous)
      dklog(synchronous ? "course transition: each GPU operation completes before the next until a race runs"
                        : "race running: GPU operations asynchronous again");
    R.synchronous = synchronous;
  }
  return R.synchronous;
}
#endif
}
void gpu_operation_begin(const char *name, uint64_t a, uint64_t b, uint64_t c) {
  static const bool announced = [] {
#ifdef COS_DK_GPU_CHECKPOINTS
    swdk::dklog("GPU checkpoint test v3: native API validation and timed per-operation fences enabled");
#else
#ifdef COS_DK_GPU_BREADCRUMBS
    if (steady_v9())
      swdk::dklog("GPU progress test v9: GPU-written completion history; v8 pass/transfer flush policy; no CPU waits");
    else
      swdk::dklog("GPU submission: native outside course transitions; in them each operation completes, with "
                  "GPU-written completion history (perf.cfg gpu_steady_v9 1: v9 everywhere)");
#else
    swdk::dklog("GPU transition test v8: pass/transfer flushes, at most %u draw operations; CPU waits disabled",
               unsigned(COS_DK_GPU_FLUSH_INTERVAL));
#endif
#endif
    return true;
  }();
  (void)announced;
  gpuOperation = name;
  gpuArgs[0] = a; gpuArgs[1] = b; gpuArgs[2] = c;
  ++gpuOperationSerial;
  gpuRecent[gpuOperationSerial % gpuRecent.size()] = {name, gpuOperationSerial, {a, b, c}};
#ifdef COS_DK_GPU_BREADCRUMBS
  // Steady state records no breadcrumb (its report is a GPU write and flush per operation); slots
  // only ever hold completed transition operations, so breadcrumb_begin's reuse check still holds.
  gpuOperationTracked = steady_v9() || synchronous_operation();
  if (gpuOperationTracked) breadcrumb_begin();
#endif
}
void gpu_operation_end(bool flushBoundary) {
#ifdef COS_DK_GPU_BREADCRUMBS
  if (gpuOperationTracked) breadcrumb_end();
#endif
#ifdef COS_DK_GPU_CHECKPOINTS
  (void)flushBoundary;
  DkFence fence{};
  dkCmdBufSignalFence(R.cmd, &fence, true);
  submit_readback("GPU operation checkpoint");
  const DkResult result = dkFenceWait(&fence, 2'000'000'000ll);
  if (result != DkResult_Success) {
    kartpad_deko_log_failure_context();
    fatal("GPU checkpoint stopped after %s: result=%d frame=%llu", gpuOperation, int(result),
          static_cast<unsigned long long>(R.frame + 1));
  }
#else
  if (synchronous_operation()) {
    // v3's checkpoint without its validation: the operation and everything before it finish now.
    DkFence fence{};
    dkCmdBufSignalFence(R.cmd, &fence, true);
    submit_readback("course transition operation");
    gpuOperationsPending = 0;
    const DkResult result = dkFenceWait(&fence, 2'000'000'000ll);
    if (result != DkResult_Success) {
      kartpad_deko_log_failure_context();
      fatal("GPU operation did not complete during a course transition: %s, result=%d frame=%llu", gpuOperation,
            int(result), static_cast<unsigned long long>(R.frame + 1));
    }
    gpuProgressSerial = gpuOperationSerial;
    return;
  }
  if (!steady_v9()) return;  // native: the frame's submits carry it
  // Diagnostic flush policy only; a small submitted batch can still time out.
  if (++gpuOperationsPending < COS_DK_GPU_FLUSH_INTERVAL && !flushBoundary)
    return;
  submit_readback("asynchronous operation flush");
  gpuOperationsPending = 0;
#endif
  gpuProgressSerial = gpuOperationSerial;
}
void gpu_operation_image_copy(const Tex &source, const Tex &destination,
                              const WGPUTexelCopyTextureInfo &src,
                              const WGPUTexelCopyTextureInfo &dst) {
  gpuImageCopy.serial = gpuOperationSerial;
  gpuImageCopy.srcAddress = dkImageGetGpuAddr(&source.image);
  gpuImageCopy.dstAddress = dkImageGetGpuAddr(&destination.image);
  gpuImageCopy.srcSize = source.mem.size; gpuImageCopy.dstSize = destination.mem.size;
  gpuImageCopy.srcFormat = unsigned(source.format); gpuImageCopy.dstFormat = unsigned(destination.format);
  gpuImageCopy.srcMip = src.mipLevel; gpuImageCopy.dstMip = dst.mipLevel;
  gpuImageCopy.srcOrigin[0] = src.origin.x; gpuImageCopy.srcOrigin[1] = src.origin.y; gpuImageCopy.srcOrigin[2] = src.origin.z;
  gpuImageCopy.dstOrigin[0] = dst.origin.x; gpuImageCopy.dstOrigin[1] = dst.origin.y; gpuImageCopy.dstOrigin[2] = dst.origin.z;
  copy_label(gpuImageCopy.source, source.label.c_str());
  copy_label(gpuImageCopy.destination, destination.label.c_str());
}
#ifdef COS_DK_GPU_FLUSHES
void gpu_operation_queue_submitted() {
  // Submit the short tail too: a caller can enqueue fewer than the limit and
  // then wait for a map/readback before producing any further GPU operations.
  if (!steady_v9() && !R.synchronous) return;  // native: no tail to flush (see steady_v9)
  std::lock_guard<std::mutex> lock(fence_mutex());
  dkQueueFlush(R.queue);
  gpuOperationsPending = 0;
  gpuProgressSerial = gpuOperationSerial;
}
#endif
void log_gpu_operation_context() {
#ifdef COS_DK_GPU_BREADCRUMBS
  log_gpu_breadcrumbs();
#endif
#ifdef COS_DK_GPU_CHECKPOINTS
  const char *progress = "completed";
#else
  const char *progress = "flushed";
#endif
  std::fprintf(stderr, "[gpu-operation] serial=%llu %s=%llu op=%s args=%llu/%llu/%llu\n",
      (unsigned long long)gpuOperationSerial, progress, (unsigned long long)gpuProgressSerial, gpuOperation,
      (unsigned long long)gpuArgs[0], (unsigned long long)gpuArgs[1], (unsigned long long)gpuArgs[2]);
  const auto &copy = gpuImageCopy;
  std::fprintf(stderr, "[gpu-image-copy] serial=%llu source=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u destination=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u\n",
      (unsigned long long)copy.serial, copy.source, (unsigned long long)copy.srcAddress, copy.srcSize,
      copy.srcFormat, copy.srcMip, copy.srcOrigin[0], copy.srcOrigin[1], copy.srcOrigin[2], copy.destination,
      (unsigned long long)copy.dstAddress, copy.dstSize, copy.dstFormat, copy.dstMip,
      copy.dstOrigin[0], copy.dstOrigin[1], copy.dstOrigin[2]);
  for (uint64_t serial = gpuOperationSerial > gpuRecent.size() ? gpuOperationSerial - gpuRecent.size() + 1 : 1;
       serial <= gpuOperationSerial; ++serial) {
    const auto &op = gpuRecent[serial % gpuRecent.size()];
    std::fprintf(stderr, "[gpu-operation-recent] serial=%llu op=%s args=%llu/%llu/%llu\n",
        (unsigned long long)op.serial, op.name, (unsigned long long)op.args[0],
        (unsigned long long)op.args[1], (unsigned long long)op.args[2]);
  }
}
#endif
Binding *binding(BoundGroup &g, uint32_t b) {
  if (g.group)
    for (auto &e : g.group->entries)
      if (e.binding == b)
        return &e;
  return nullptr;
}
uint64_t offset(BoundGroup &g, Binding &b) {
  return b.offset +
         (b.dynamic >= 0 && size_t(b.dynamic) < g.offsets.size() ? g.offsets[b.dynamic] : 0);
}
namespace {
Block immediateFallback;
} // namespace
// Address of the uniform block holding the current immediates. A new value gets its own slot of the
// frame's stream, so a draw can never read a later draw's bytes. If the stream is full, one
// persistent block is rewritten with dkCmdBufPushConstants instead, which the GPU applies in
// command order. Called from bind_stage, during replay.
DkGpuAddr current_immediates() {
  if (state.immediateGpu && !state.immediatesDirty)
    return state.immediateGpu;
  if (auto s = stream_alloc(kImmediateSlotBytes, DK_UNIFORM_BUF_ALIGNMENT)) {
    std::memcpy(s.cpu, state.immediates.data(), kImmediateBytes);
    state.immediateGpu = s.gpu;
  } else {
    if (!immediateFallback) {
      immediateFallback = block_create(kImmediateSlotBytes, "draw immediates");
      if (!immediateFallback)
        fatal("Cannot allocate the draw immediates block");
    }
    dkCmdBufPushConstants(R.cmd, immediateFallback.gpu, kImmediateSlotBytes, 0, kImmediateBytes,
                          state.immediates.data());
    state.immediateGpu = immediateFallback.gpu;
  }
  state.immediatesDirty = false;
  return state.immediateGpu;
}
void bind_stage(Stage &s, DkStage stage) {
  const uint32_t cacheStage = stage == DkStage_Vertex ? 0 : 1;
  for (auto &slot : s.slots) {
    if (slot.kind == SlotBinding::Kind::Immediates) {
      const DkGpuAddr address = current_immediates();
      if (state.bindings.uniform(cacheStage, slot.slot, address, kImmediateSlotBytes))
        dkCmdBufBindUniformBuffer(R.cmd, stage, slot.slot, address, kImmediateSlotBytes);
      continue;
    }
    if (slot.group >= state.groups.size())
      fatal("Invalid bind group");
    auto &g = state.groups[slot.group];
    auto *b = binding(g, slot.binding);
    if (slot.kind == SlotBinding::Kind::Sampler) {
      auto *sampler = slot.samplerGroup < state.groups.size()
                          ? binding(state.groups[slot.samplerGroup], slot.samplerBinding)
                          : nullptr;
      const DkResHandle h =
          texture_handle(b ? find_view(b->view.Get()) : nullptr,
                         sampler ? find_sampler(sampler->sampler.Get()) : nullptr);
      if (state.bindings.texture(cacheStage, slot.slot, h))
        dkCmdBufBindTextures(R.cmd, stage, slot.slot, &h, 1);
    } else if (b && b->buffer) {
      uint64_t off = offset(g, *b);
      if (off > b->buffer->size)
        fatal("Buffer binding offset out of bounds");
      uint64_t n = std::min(b->size, b->buffer->size - off);
      if (slot.kind == SlotBinding::Kind::Uniform &&
          state.bindings.uniform(cacheStage, slot.slot, b->buffer->mem.gpu + off, n))
        dkCmdBufBindUniformBuffer(R.cmd, stage, slot.slot, b->buffer->mem.gpu + off, n);
      else if (slot.kind == SlotBinding::Kind::Storage &&
               state.bindings.storage(cacheStage, slot.slot, b->buffer->mem.gpu + off, n))
        dkCmdBufBindStorageBuffer(R.cmd, stage, slot.slot, b->buffer->mem.gpu + off, n);
    } else if (slot.kind != SlotBinding::Kind::Immediates)
      fatal("Missing buffer binding %u:%u", slot.group, slot.binding);
  }
}
bool bind_pipeline() {
  auto p = state.pipeline;
  if (!p)
    fatal("Draw without pipeline");
  if (!shader_usable(p->vs.shader) || !shader_usable(p->fs.shader))
    fatal("Native shader could not be loaded");
  p->vs.shader->usedDraws.fetch_add(1, std::memory_order_relaxed);
  p->fs.shader->usedDraws.fetch_add(1, std::memory_order_relaxed);
  code_make_visible();
  if (state.boundPipeline != p.get()) {
    // Pipeline binds and any fixed/native helper may change driver-side state.
    // A pass reset clears these too; never carry bindings across render passes.
    state.bindings.invalidate();
    const DkShader *shaders[] = {&p->vs.shader->shader, &p->fs.shader->shader};
    dkCmdBufBindShaders(R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindRasterizerState(R.cmd, &p->raster);
    dkCmdBufBindDepthStencilState(R.cmd, &p->depth);
    dkCmdBufBindColorState(R.cmd, &p->color);
    dkCmdBufBindColorWriteState(R.cmd, &p->write);
    auto blend = p->blend;
    if ((p->write.masks & 15) == DkColorMask_A)
      blend.dstColorBlendFactor = blend.dstAlphaBlendFactor;
    dkCmdBufBindBlendStates(R.cmd, 0, &blend, 1);
    dkCmdBufSetDepthBias(R.cmd, p->bias[0], p->bias[1], p->bias[2]);
    dkCmdBufBindVtxAttribState(R.cmd, p->attributes.empty() ? nullptr : p->attributes.data(),
                               p->attributes.size());
    dkCmdBufBindVtxBufferState(R.cmd, p->vertexStates.empty() ? nullptr : p->vertexStates.data(),
                               p->vertexStates.size());
    state.boundPipeline = p.get();
    state.splitAlphaDirty = false;
  } else if (state.splitAlphaDirty) {
    // The second alpha-only draw changed only these three states. Preserve
    // shaders and resource bindings instead of invalidating the full pipeline
    // after every translucent UI/material draw.
    dkCmdBufBindDepthStencilState(R.cmd, &p->depth);
    dkCmdBufBindColorWriteState(R.cmd, &p->write);
    auto blend = p->blend;
    if ((p->write.masks & 15) == DkColorMask_A)
      blend.dstColorBlendFactor = blend.dstAlphaBlendFactor;
    dkCmdBufBindBlendStates(R.cmd, 0, &blend, 1);
    state.splitAlphaDirty = false;
  }
  bind_stage(p->vs, DkStage_Vertex);
  bind_stage(p->fs, DkStage_Fragment);
  return true;
}
#ifdef COS_DK_DEBUG_LIB
#include "gx_probe.inc"
#endif
void draw(bool indexed, uint32_t count, uint32_t instances, uint32_t first, int32_t base,
          uint32_t firstInstance) {
  GpuCheckpoint checkpoint(indexed ? "indexed draw" : "draw", count, instances, first);
  bind_pipeline();
#ifdef COS_DK_DEBUG_LIB
  // Probe before the real draw: the depth comparison must see the same depth
  // image the real draw will see, rather than its own already-written depth.
  probe_draw(indexed, count, instances, first, base, firstInstance);
  bind_pipeline();
#endif
  ++state.draws;
  auto p = state.pipeline;
  auto issue = [&] {
    if (indexed) {
      if (!state.index)
        fatal("Missing index buffer");
      if (state.bindings.index(state.index->mem.gpu + state.indexOffset, state.indexFormat))
        dkCmdBufBindIdxBuffer(R.cmd, state.indexFormat, state.index->mem.gpu + state.indexOffset);
      dkCmdBufDrawIndexed(R.cmd, p->topology, count, instances, first, base, firstInstance);
    } else
      dkCmdBufDraw(R.cmd, p->topology, count, instances, first, firstInstance);
  };
  // deko3d 0.5.0 uses dstColorBlendFactor for destination alpha. Preserve both
  // equations by writing RGB first and alpha second when their factors differ.
  if ((p->color.blendEnableMask & 1) &&
      p->blend.dstColorBlendFactor != p->blend.dstAlphaBlendFactor && (p->write.masks & 15) == 15) {
    auto cw = p->write;
    dkColorWriteStateSetMask(&cw, 0, DkColorMask_RGB);
    dkCmdBufBindColorWriteState(R.cmd, &cw);
    issue();
    dkColorWriteStateSetMask(&cw, 0, DkColorMask_A);
    dkCmdBufBindColorWriteState(R.cmd, &cw);
    auto b = p->blend;
    b.dstColorBlendFactor = b.dstAlphaBlendFactor;
    dkCmdBufBindBlendStates(R.cmd, 0, &b, 1);
    auto d = p->depth;
    if (d.depthWriteEnable)
      d.depthCompareOp = DkCompareOp_Equal;
    d.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(R.cmd, &d);
    issue();
    state.splitAlphaDirty = true;
  } else {
    issue();
  }
#ifdef COS_DK_DEBUG_LIB
  probe_after_draw();
#endif
  checkpoint.finish();
}
} // namespace kd

extern "C" void kartpad_deko_log_failure_context() {
  // A driver failure may arrive while the fence mutex is held. Avoid waiting
  // for the object mutex in the reverse order on this diagnostic path.
  std::unique_lock lock(kd::mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    std::fprintf(stderr, "[gpu-submit] replay context busy\n");
    return;
  }
  const auto stats = swdk::submit_stats();
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
  kd::log_gpu_operation_context();
#endif
  const auto pipeline = kd::state.pipeline;
  const auto *view = kd::state.target ? kd::find_view(kd::state.target.Get()) : nullptr;
  const auto *texture = view ? view->tex : nullptr;
  std::fprintf(stderr,
      "[gpu-submit] frame=%llu submits=%llu pass_draws=%llu pipeline=%s "
      "vs=%016llx fs=%016llx target=%s (%ux%u) buffers=%zu pipelines=%zu\n",
      static_cast<unsigned long long>(swdk::R.frame + 1),
      static_cast<unsigned long long>(stats.submits),
      static_cast<unsigned long long>(kd::state.draws), pipeline ? pipeline->label.c_str() : "<none>",
      static_cast<unsigned long long>(pipeline && pipeline->vs.shader ? pipeline->vs.shader->glslHash : 0),
      static_cast<unsigned long long>(pipeline && pipeline->fs.shader ? pipeline->fs.shader->glslHash : 0),
      texture ? texture->label.c_str() : "<none>", texture ? texture->width : 0,
      texture ? texture->height : 0, kd::buffers.size(), kd::pipelines.size());
}

extern "C" {
WGPUCommandEncoder __wrap_wgpuDeviceCreateCommandEncoder(WGPUDevice d,
                                                         const WGPUCommandEncoderDescriptor *desc) {
  auto h = __real_wgpuDeviceCreateCommandEncoder(d, desc);
  kd::Lock l(kd::mutex);
  kd::encoders[h] = std::make_shared<kd::Commands>();
  kd::encoderRefs[h] = 1;
  return h;
}
void __wrap_wgpuCommandEncoderAddRef(WGPUCommandEncoder h) {
  kd::Lock l(kd::mutex);
  ++kd::encoderRefs[h];
  __real_wgpuCommandEncoderAddRef(h);
}
void __wrap_wgpuCommandEncoderRelease(WGPUCommandEncoder h) {
  kd::Lock l(kd::mutex);
  if (!--kd::encoderRefs[h]) {
    kd::encoders.erase(h);
    kd::encoderRefs.erase(h);
  }
  __real_wgpuCommandEncoderRelease(h);
}
WGPUCommandBuffer __wrap_wgpuCommandEncoderFinish(WGPUCommandEncoder h,
                                                  const WGPUCommandBufferDescriptor *desc) {
  auto b = __real_wgpuCommandEncoderFinish(h, desc);
  kd::Lock l(kd::mutex);
  kd::commands[b] = {kd::required(kd::encoders, h, __func__), 1};
  return b;
}
void __wrap_wgpuCommandBufferAddRef(WGPUCommandBuffer h) {
  kd::Lock l(kd::mutex);
  ++kd::required(kd::commands, h, __func__).second;
  __real_wgpuCommandBufferAddRef(h);
}
void __wrap_wgpuCommandBufferRelease(WGPUCommandBuffer h) {
  kd::Lock l(kd::mutex);
  auto it = kd::commands.find(h);
  if (it != kd::commands.end() && !--it->second.second)
    kd::commands.erase(it);
  __real_wgpuCommandBufferRelease(h);
}
WGPURenderPassEncoder
__wrap_wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e,
                                         const WGPURenderPassDescriptor *desc) {
  auto p = __real_wgpuCommandEncoderBeginRenderPass(e, desc);
  kd::Lock l(kd::mutex);
  auto view = kd::keep(desc->colorAttachments[0].view);
  kd::passes[p] = {kd::required(kd::encoders, e, __func__), view, kd::str(desc->label)};
  auto depth = desc->depthStencilAttachment ? kd::keep(desc->depthStencilAttachment->view)
                                            : wgpu::TextureView{};
  auto color = desc->colorAttachments[0].clearValue;
  bool clear = desc->colorAttachments[0].loadOp == WGPULoadOp_Clear;
  bool clearDepth =
      desc->depthStencilAttachment && desc->depthStencilAttachment->depthLoadOp == WGPULoadOp_Clear;
  float z = desc->depthStencilAttachment ? desc->depthStencilAttachment->depthClearValue : 1;
  kd::record(e, [view, depth, color, clear, clearDepth, z] {
    kd::GpuCheckpoint checkpoint("begin render pass");
    kd::open();
    kd::barrier();
    kd::state = {};
    auto *v = kd::find_view(view.Get());
    auto *d = depth ? kd::find_view(depth.Get()) : nullptr;
    if (!v || !v->tex->valid)
      swdk::fatal("Invalid native render target");
    const DkImageView *targets[] = {&v->view};
    dkCmdBufBindRenderTargets(swdk::R.cmd, targets, 1, d ? &d->view : nullptr);
    swdk::set_view(0, 0, v->tex->width >> v->baseMip, v->tex->height >> v->baseMip);
    kd::state.target = view;
    kd::state.depthTarget = depth;
    kd::state.viewport = {
        0, 0, float(v->tex->width >> v->baseMip), float(v->tex->height >> v->baseMip), 0, 1};
    kd::state.scissor = {0, 0, v->tex->width >> v->baseMip, v->tex->height >> v->baseMip};
    if (clear) {
      const float c[] = {float(color.r), float(color.g), float(color.b), float(color.a)};
      dkCmdBufClearColor(swdk::R.cmd, 0, DkColorMask_RGBA, c);
    }
    if (clearDepth && d)
      dkCmdBufClearDepthStencil(swdk::R.cmd, true, z, 0, 0);
    checkpoint.finish(true);
  });
  return p;
}
void __wrap_wgpuRenderPassEncoderEnd(WGPURenderPassEncoder p) {
  kd::Lock l(kd::mutex);
  const auto &pass = kd::required(kd::passes, p, __func__);
  auto target = pass.target;
  auto label = pass.label;
  kd::record_pass(p, [target, label] {
    kd::GpuCheckpoint checkpoint("end render pass");
    kd::barrier();
    if (swdk::capture_frame()) {
      auto *v = kd::find_view(target.Get());
      if (v && v->tex->valid) {
        swdk::dklog("pixel pass: %s target=%s draws=%llu", label.c_str(), v->tex->label.c_str(),
                    (unsigned long long)kd::state.draws);
        swdk::capture_image(v->view, std::max(1u, v->tex->width >> v->baseMip),
                            std::max(1u, v->tex->height >> v->baseMip), v->tex->format,
                            label.c_str());
      }
      if (auto pipeline = kd::state.pipeline) {
        for (const auto *stage : {&pipeline->vs, &pipeline->fs}) {
          for (const auto &slot : stage->slots) {
            if (slot.kind != swdk::SlotBinding::Kind::Sampler)
              continue;
            auto *b = kd::binding(kd::state.groups[slot.group], slot.binding);
            auto *source = b ? kd::find_view(b->view.Get()) : nullptr;
            swdk::dklog("pixel input: %s binding=%u:%u native_slot=%u texture=%s valid=%d",
                        label.c_str(), slot.group, slot.binding, slot.slot,
                        source ? source->tex->label.c_str() : "MISSING",
                        source && source->tex->valid);
            if (source && source->tex->valid) {
              auto sourceLabel = std::string("input ") + source->tex->label;
              swdk::capture_image(source->view, std::max(1u, source->tex->width >> source->baseMip),
                                  std::max(1u, source->tex->height >> source->baseMip),
                                  source->tex->format, sourceLabel.c_str());
            }
          }
        }
      }
    }
    checkpoint.finish(true);
  });
  __real_wgpuRenderPassEncoderEnd(p);
  kd::passes.erase(p);
}
void __wrap_wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline h) {
  kd::Lock l(kd::mutex);
  auto pipe = kd::required(kd::pipelines, h, __func__);
  kd::record_pass(p, [pipe] { kd::state.pipeline = pipe; });
}
void __wrap_wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder p, uint32_t i, WGPUBindGroup h,
                                              size_t n, const uint32_t *offsets) {
  kd::Lock l(kd::mutex);
  auto g = h ? kd::required(kd::groups, h, __func__) : nullptr;
  std::array<uint32_t, 8> o{};
  if (n > o.size())
    swdk::fatal("Too many dynamic offsets");
  if (n)
    std::copy_n(offsets, n, o.begin());
  kd::record_pass(p, [i, g, o] { kd::state.groups.at(i) = {g, o}; });
}
void __wrap_wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder p, float x, float y, float w,
                                             float h, float near, float far) {
  kd::record_pass(p, [=] {
    DkViewport v{x, y, w, h, near, far};
    kd::state.viewport = v;
    dkCmdBufSetViewports(swdk::R.cmd, 0, &v, 1);
  });
}
void __wrap_wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder p, uint32_t x, uint32_t y,
                                                uint32_t w, uint32_t h) {
  kd::record_pass(p, [=] {
    DkScissor s{x, y, w, h};
    kd::state.scissor = s;
    dkCmdBufSetScissors(swdk::R.cmd, 0, &s, 1);
  });
}
void __wrap_wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder p, const WGPUColor *color) {
  auto c = *color;
  kd::record_pass(p, [=] { dkCmdBufSetBlendConst(swdk::R.cmd, c.r, c.g, c.b, c.a); });
}
// Aurora sets its per-draw DrawImmediateData here. Dawn's Null pass never sees the draws (they are
// recorded natively), so the call is not forwarded.
void __wrap_wgpuRenderPassEncoderSetImmediates(WGPURenderPassEncoder p, uint32_t offset,
                                               const void *data, size_t size) {
  if (offset > kd::kImmediateBytes || size > kd::kImmediateBytes - offset)
    swdk::fatal("Immediate data out of range: offset %u size %zu (limit %u)", offset, size,
                kd::kImmediateBytes);
  std::array<uint8_t, kd::kImmediateBytes> bytes{};
  if (size)
    std::memcpy(bytes.data(), data, size);
  kd::record_pass(p, [bytes, offset, size] {
    std::memcpy(kd::state.immediates.data() + offset, bytes.data(), size);
    kd::state.immediatesDirty = true;
  });
}
void __wrap_wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder p, WGPUBuffer h,
                                                WGPUIndexFormat fmt, uint64_t off, uint64_t size) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  kd::record_pass(p, [=] {
    kd::state.index = b;
    kd::state.indexOffset = off;
    kd::state.indexFormat = fmt == WGPUIndexFormat_Uint32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
  });
}
void __wrap_wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder p, uint32_t slot,
                                                 WGPUBuffer h, uint64_t off, uint64_t size) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  if (off > b->size)
    swdk::fatal("Vertex buffer offset out of bounds");
  if (size == WGPU_WHOLE_SIZE)
    size = b->size - off;
  if (size > UINT32_MAX || size > b->size - off)
    swdk::fatal("Invalid vertex buffer size");
  kd::record_pass(p, [=] {
    DkBufExtents e{b->mem.gpu + off, uint32_t(size)};
    dkCmdBufBindVtxBuffers(swdk::R.cmd, slot, &e, 1);
  });
}
void __wrap_wgpuRenderPassEncoderDraw(WGPURenderPassEncoder p, uint32_t count, uint32_t instances,
                                      uint32_t first, uint32_t firstInstance) {
  kd::record_pass(p, [=] { kd::draw(false, count, instances, first, 0, firstInstance); });
}
void __wrap_wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder p, uint32_t count,
                                             uint32_t instances, uint32_t first, int32_t base,
                                             uint32_t firstInstance) {
  kd::record_pass(p, [=] { kd::draw(true, count, instances, first, base, firstInstance); });
}
void __wrap_wgpuQueueSubmit(WGPUQueue q, size_t n, const WGPUCommandBuffer *b) {
  kd::Lock l(kd::mutex);
  kd::open();
  for (size_t i = 0; i < n; ++i)
    kd::required(kd::commands, b[i], __func__).first->run();
  swdk::submit("Aurora queue submit");
#ifdef COS_DK_GPU_FLUSHES
  kd::gpu_operation_queue_submitted();
#endif
  kd::retire_uploads();
  __real_wgpuQueueSubmit(q, n, b);
}

void __wrap_wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer src, uint64_t so,
                                                 WGPUBuffer dst, uint64_t off, uint64_t n) {
  kd::Lock l(kd::mutex);
  auto s = kd::required(kd::buffers, src, __func__), d = kd::required(kd::buffers, dst, __func__);
  // Pin the mapped generation, not a CPU copy. prepare_map() cannot hand this
  // memory to the producer again before this command and its GPU fence finish.
  auto source = s->active;
  kd::record(e, [=] {
    kd::GpuCheckpoint checkpoint("buffer copy", so, off, n);
    kd::open();
    kd::barrier();
    dkCmdBufCopyBuffer(swdk::R.cmd, source->mem.gpu + so, d->mem.gpu + off, n);
    // DMA writes can replace uniforms/storage at an address the shader has
    // already cached. L2 invalidation alone does not invalidate that cache.
    kd::barrier(DkInvalidateFlags_Shader);
    source->used();
    checkpoint.finish(true);
  });
}
void __wrap_wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder e,
                                                   const WGPUTexelCopyTextureInfo *src,
                                                   const WGPUTexelCopyTextureInfo *dst,
                                                   const WGPUExtent3D *size) {
  auto sh = kd::keep(src->texture), dh = kd::keep(dst->texture);
  auto s = *src, d = *dst;
  auto n = *size;
  kd::record(e, [=] {
    kd::GpuCheckpoint checkpoint("image copy", n.width, n.height, n.depthOrArrayLayers);
    kd::open();
    kd::barrier();
    auto *st = kd::find_texture(sh.Get());
    auto *dt = kd::find_texture(dh.Get());
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
    kd::gpu_operation_image_copy(*st, *dt, s, d);
#endif
    auto sv = kd::level_view(st, s.mipLevel), dv = kd::level_view(dt, d.mipLevel);
    auto sx = s.origin.x, sy = s.origin.y, dx = d.origin.x, dy = d.origin.y, w = n.width,
         h = n.height;
    if (st->compressed) {
      w = (w + st->blockW - 1) / st->blockW;
      h = (h + st->blockH - 1) / st->blockH;
      sx /= st->blockW;
      sy /= st->blockH;
      dx /= dt->blockW;
      dy /= dt->blockH;
    }
    DkImageRect sr{sx, sy, s.origin.z, w, h, n.depthOrArrayLayers},
        dr{dx, dy, d.origin.z, w, h, n.depthOrArrayLayers};
    dkCmdBufCopyImage(swdk::R.cmd, &sv, &sr, &dv, &dr, 0);
    kd::barrier();
    checkpoint.finish(true);
  });
}
void __wrap_wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder e,
                                                  const WGPUTexelCopyBufferInfo *src,
                                                  const WGPUTexelCopyTextureInfo *dst,
                                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  auto s = kd::required(kd::buffers, src->buffer, __func__)->active;
  auto t = kd::keep(dst->texture);
  auto layout = src->layout;
  auto d = *dst;
  auto n = *size;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    auto *dt = kd::find_texture(t.Get());
    kd::GpuCheckpoint checkpoint("buffer to image", n.width, n.height, n.depthOrArrayLayers);
    auto dv = kd::level_view(dt, d.mipLevel);
    uint32_t row = layout.bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED
                       ? ((n.width + dt->blockW - 1) / dt->blockW) * dt->blockBytes
                       : layout.bytesPerRow;
    uint32_t rows = layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED
                        ? (n.height + dt->blockH - 1) / dt->blockH
                        : layout.rowsPerImage;
    DkCopyBuf b{s->mem.gpu + layout.offset, row, row * rows};
    DkImageRect r{d.origin.x, d.origin.y, d.origin.z, n.width, n.height, n.depthOrArrayLayers};
    dkCmdBufCopyBufferToImage(swdk::R.cmd, &b, &dv, &r, 0);
    kd::barrier();
    s->used();
    checkpoint.finish(true);
  });
}
void __wrap_wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder e,
                                                  const WGPUTexelCopyTextureInfo *src,
                                                  const WGPUTexelCopyBufferInfo *dst,
                                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  auto d = kd::required(kd::buffers, dst->buffer, __func__);
  auto t = kd::keep(src->texture);
  auto layout = dst->layout;
  auto s = *src;
  auto n = *size;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    auto *st = kd::find_texture(t.Get());
    kd::GpuCheckpoint checkpoint("image to buffer", n.width, n.height, n.depthOrArrayLayers);
    auto sv = kd::level_view(st, s.mipLevel);
    uint32_t row = layout.bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED ? n.width * st->blockBytes
                                                                    : layout.bytesPerRow;
    uint32_t rows =
        layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? n.height : layout.rowsPerImage;
    DkCopyBuf b{d->mem.gpu + layout.offset, row, row * rows};
    DkImageRect r{s.origin.x, s.origin.y, s.origin.z, n.width, n.height, n.depthOrArrayLayers};
    dkCmdBufCopyImageToBuffer(swdk::R.cmd, &sv, &r, &b, 0);
    kd::barrier(DkInvalidateFlags_Shader);
    checkpoint.finish(true);
  });
}
void __wrap_wgpuCommandEncoderClearBuffer(WGPUCommandEncoder e, WGPUBuffer b, uint64_t off,
                                          uint64_t n) {
  kd::Lock l(kd::mutex);
  auto d = kd::required(kd::buffers, b, __func__);
  if (n == WGPU_WHOLE_SIZE)
    n = d->size - off;
  kd::record(e, [=] {
    kd::GpuCheckpoint checkpoint("buffer clear", off, n);
    kd::open();
    kd::barrier();
    std::vector<uint8_t> zero(n);
    auto temp = swdk::block_create(n, "buffer clear");
    memcpy(temp.cpu, zero.data(), n);
    dkCmdBufCopyBuffer(swdk::R.cmd, temp.gpu, d->mem.gpu + off, n);
    swdk::block_free_deferred(temp.block);
    kd::barrier(DkInvalidateFlags_Shader);
    checkpoint.finish(true);
  });
}

// Queue writes execute before subsequent command buffers, under the same lock.
void __wrap_wgpuQueueWriteBuffer(WGPUQueue q, WGPUBuffer h, uint64_t off, const void *data,
                                 size_t n) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  kd::GpuCheckpoint checkpoint("buffer upload", off, n);
  kd::open();
  kd::barrier();
  auto temp = kd::upload_allocation(n);
  memcpy(temp->mem.cpu, data, n);
  dkCmdBufCopyBuffer(swdk::R.cmd, temp->mem.gpu, b->mem.gpu + off, n);
  temp->used();
  kd::barrier(DkInvalidateFlags_Shader);
  checkpoint.finish(true);
}
void __wrap_wgpuQueueWriteTexture(WGPUQueue q, const WGPUTexelCopyTextureInfo *dst,
                                  const void *data, size_t bytes,
                                  const WGPUTexelCopyBufferLayout *layout,
                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  kd::open();
  kd::barrier();
  auto *t = kd::find_texture(dst->texture);
  kd::GpuCheckpoint checkpoint("texture upload", size->width, size->height, size->depthOrArrayLayers);
  auto v = kd::level_view(t, dst->mipLevel);
  uint32_t row = ((size->width + t->blockW - 1) / t->blockW) * t->blockBytes,
           rows = (size->height + t->blockH - 1) / t->blockH, pitch = (row + 255) & ~255u;
  uint32_t inPitch = layout->bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED ? row : layout->bytesPerRow;
  uint32_t inRows =
      layout->rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? rows : layout->rowsPerImage;
  auto temp = kd::upload_allocation(uint64_t(pitch) * rows * size->depthOrArrayLayers);
  for (uint32_t z = 0; z < size->depthOrArrayLayers; ++z)
    for (uint32_t y = 0; y < rows; ++y)
      memcpy(temp->mem.cpu + (z * rows + y) * pitch,
             static_cast<const uint8_t *>(data) + layout->offset + (z * inRows + y) * inPitch, row);
  DkCopyBuf b{temp->mem.gpu, pitch, pitch * rows};
  DkImageRect r{dst->origin.x, dst->origin.y, dst->origin.z,
                size->width,   size->height,  size->depthOrArrayLayers};
  dkCmdBufCopyBufferToImage(swdk::R.cmd, &b, &v, &r, 0);
  temp->used();
  kd::barrier();
  checkpoint.finish(true);
}
}
