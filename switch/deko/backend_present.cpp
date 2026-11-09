// Native deko3d adapter, part 4: presentation of a finished frame, and depth snapshots that
// serve the game's CPU depth reads a frame late without stalling the GPU. See
// backend_internal.h for the whole adapter.
#include "backend_internal.h"

#include "present_fsh_dksh.h"
#include "present_vsh_dksh.h"
void kartpad_deko_present(WGPUTexture texture, unsigned width, unsigned height) {
  // The swapchain image first, without the renderer's lock: the window's dequeue blocks until the
  // display gives an image back (about 1 ms a present), and the frame worker's next frame waited
  // for the lock all that time (dk.h PresentImage).
  swdk::PresentImage image;
  const bool acquired = swdk::present_acquire(&image);
  kd::Lock l(kd::mutex);
  static bool firstGameSnapshot = true;
  if (firstGameSnapshot) {
    swdk::dklog("first game snapshot: %ux%u, native present", width, height);
    firstGameSnapshot = false;
  }
  kd::open();
  auto *t = kd::find_texture(texture);
  if (!t || !t->valid)
    swdk::fatal("Missing presentation image");
  if (swdk::capture_frame()) {
    auto view = kd::level_view(t, 0);
    swdk::capture_image(view, t->width, t->height, t->format, "snapshot");
  }
  struct Picture {
    kd::Tex *texture;
    DkShader vs, fs;
  };
  static DkShader vs{}, fs{};
  static bool loaded = false;
  if (!loaded) {
    loaded = swdk::code_load(vs, present_vsh_dksh, present_vsh_dksh_size, "present vertex") &&
             swdk::code_load(fs, present_fsh_dksh, present_fsh_dksh_size, "present fragment");
    if (!loaded)
      swdk::fatal("Cannot load presentation shaders");
  }
  Picture picture{t, vs, fs};
  swdk::frame_present(
      [](void *data, int stage) {
        if (stage != 1)
          return;
        kd::GpuCheckpoint checkpoint("presentation draw", swdk::R.width, swdk::R.height);
        auto &p = *static_cast<Picture *>(data);
        kd::barrier();
        swdk::bind_2d_state(false);
        swdk::set_view(0, 0, swdk::R.width, swdk::R.height);
        auto v = kd::level_view(p.texture, 0);
        uint32_t slot = swdk::image_slot_alloc();
        swdk::write_image_view_descriptor(slot, v);
        DkResHandle handle = dkMakeTextureHandle(slot, swdk::kSamplerLinearClamp);
        const DkShader *sh[] = {&p.vs, &p.fs};
        swdk::code_make_visible();
        dkCmdBufBindVtxAttribState(swdk::R.cmd, nullptr, 0);
        dkCmdBufBindVtxBufferState(swdk::R.cmd, nullptr, 0);
        dkCmdBufBindShaders(swdk::R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, sh, 2);
        dkCmdBufBindTextures(swdk::R.cmd, DkStage_Fragment, 0, &handle, 1);
        dkCmdBufDraw(swdk::R.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        swdk::image_slot_free(slot);
        checkpoint.finish(true);
      },
      &picture, acquired ? &image : nullptr);
}

namespace kd {
struct DepthSnapshot {
  Block data;
  DkFence fence{};
  bool pending = false, ready = false, reversed = true;
  uint64_t serial = 0;
  KartPadDepthParams params{};
};
std::array<DepthSnapshot, 3> depthSnapshots;
uint64_t depthSerial = 0;
} // namespace kd
void kartpad_deko_depth_snapshot(WGPUCommandEncoder encoder, WGPUTextureView view,
                                 KartPadDepthParams params, bool reversed) {
  auto hold = kd::keep(view);
  kd::record(encoder, [hold, params, reversed] {
    kd::DepthSnapshot *slot = nullptr;
    for (auto &s : kd::depthSnapshots)
      if (!s.pending || dkFenceWait(&s.fence, 0) == DkResult_Success) {
        slot = &s;
        break;
      }
    if (!slot)
      return;
    const uint32_t bytes = params.srcWidth * params.srcHeight * 4;
    if (slot->data.size < bytes) {
      if (slot->data)
        swdk::block_free_deferred(slot->data.block);
      slot->data = swdk::block_create(bytes, "depth snapshot");
    }
    slot->params = params;
    slot->reversed = reversed;
    slot->serial = ++kd::depthSerial;
    slot->ready = false;
    slot->pending = true;
    kd::barrier();
    auto *v = kd::find_view(hold.Get());
    DkCopyBuf dst{slot->data.gpu, params.srcWidth * 4, bytes};
    DkImageRect rect{0, 0, 0, params.srcWidth, params.srcHeight, 1};
    dkCmdBufCopyImageToBuffer(swdk::R.cmd, &v->view, &rect, &dst, 0);
    kd::barrier();
    dkCmdBufSignalFence(swdk::R.cmd, &slot->fence, true);
  });
}
bool kartpad_deko_depth_read(unsigned x, unsigned y, unsigned &z) {
  kd::Lock l(kd::mutex);
  kd::DepthSnapshot *latest = nullptr;
  for (auto &s : kd::depthSnapshots) {
    if (s.pending && dkFenceWait(&s.fence, 0) == DkResult_Success) {
      s.pending = false;
      s.ready = true;
    }
    if (s.ready && (!latest || s.serial > latest->serial))
      latest = &s;
  }
  if (!latest)
    return false;
  const auto &p = latest->params;
  if (x >= p.dstWidth || y >= p.dstHeight)
    return false;
  int sx = std::clamp(int(std::floor(p.offsetX + (x + 0.5f) * p.scaleX)), 0, int(p.srcWidth) - 1),
      sy = std::clamp(int(std::floor(p.offsetY + (y + 0.5f) * p.scaleY)), 0, int(p.srcHeight) - 1);
  float f;
  memcpy(&f, latest->data.cpu + (uint64_t(sy) * p.srcWidth + sx) * 4, 4);
  if (latest->reversed)
    f = 1 - f;
  z = std::min(uint32_t(std::clamp(f, 0.f, 1.f) * 16777216.f + 0.5f), 0xFFFFFFu);
  return true;
}

// SwitchWaker's host heap and thread hooks have no game-heap equivalent here.
void JKRPcBeginHostAlloc() {}
extern "C" void cos_switch_flush_logs() {
  fflush(stdout);
  fflush(stderr);
}
extern "C" int kartpad_thread_apply(int, int);
extern "C" void cos_switch_thread_role(int) { kartpad_thread_apply(3, 0); }
