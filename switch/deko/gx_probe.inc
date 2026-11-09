// Included inside namespace kd, in checked diagnostic builds only. Probes use
// a separate color image and never write the game's depth image or color image.
struct ProbePixels {
  uint32_t changed = 0, rgb = 0, alpha = 0;
  bool ok = false;
};
struct PendingProbe {
  View *target = nullptr;
  uint32_t width = 0, height = 0;
  unsigned draw = 0;
} pendingProbe;
ProbePixels probe_pixels(const DkImageView &view, uint32_t width, uint32_t height, const char *name,
                         unsigned draw) {
  const uint32_t pitch = (width * 4 + 255) & ~255u;
  auto readback = block_create(pitch * height, "GX probe pixels");
  barrier();
  const DkCopyBuf dst{readback.gpu, pitch, pitch * height};
  const DkImageRect rect{0, 0, 0, width, height, 1};
  dkCmdBufCopyImageToBuffer(R.cmd, &view, &rect, &dst, 0);
  DkFence fence{};
  dkCmdBufSignalFence(R.cmd, &fence, true);
  submit_readback("GX isolation probe");
  ProbePixels result;
  if (dkFenceWait(&fence, 1000000000) == DkResult_Success) {
    result.ok = true;
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const auto *p = readback.cpu + y * pitch + x * 4;
        result.changed += p[0] != 8 || p[1] != 16 || p[2] != 24 || p[3] != 32;
        result.rgb += p[0] || p[1] || p[2];
        result.alpha += p[3] != 0;
      }
    char path[192];
    snprintf(path, sizeof path, "sdmc:/switch/melee-nx/Logs/deko-capture/probe-f%04llu-d%02u-%s.ppm",
             (unsigned long long)(R.frame + 1), draw, name);
    if (FILE *out = fopen(path, "wb")) {
      const uint32_t scale = std::max(1u, (width + 319) / 320);
      const uint32_t ow = (width + scale - 1) / scale, oh = (height + scale - 1) / scale;
      fprintf(out, "P6\n%u %u\n255\n", ow, oh);
      for (uint32_t y = 0; y < height; y += scale)
        for (uint32_t x = 0; x < width; x += scale)
          fwrite(readback.cpu + y * pitch + x * 4, 1, 3, out);
      fclose(out);
    }
  }
  dklog("GX probe f%llu d%u %s: read=%d changed=%u/%u rgb=%u alpha=%u",
        (unsigned long long)(R.frame + 1), draw, name, result.ok, result.changed, width * height,
        result.rgb, result.alpha);
  block_free_deferred(readback.block);
  return result;
}

void probe_dump(const char *name, const uint8_t *data, uint64_t size, unsigned draw) {
  char path[192];
  snprintf(path, sizeof path, "sdmc:/switch/melee-nx/Logs/deko-capture/probe-f%04llu-d%02u-%s.bin",
           (unsigned long long)(R.frame + 1), draw, name);
  if (FILE *out = fopen(path, "wb")) {
    const auto written = fwrite(data, 1, size, out);
    fclose(out);
    dklog("GX probe dump %s: %llu/%llu bytes hash=%016llx", name, (unsigned long long)written,
          (unsigned long long)size, (unsigned long long)XXH3_64bits(data, size));
  }
}

void probe_draw(bool indexed, uint32_t count, uint32_t instances, uint32_t first, int32_t base,
                uint32_t firstInstance) {
  pendingProbe = {};
  if (!capture_frame() || (R.frame + 1 != 121 && R.frame + 1 != 601) || !state.pipeline ||
      state.pipeline->label != "GX Pipeline")
    return;
  static uint64_t lastFrame = 0;
  static unsigned draw = 0;
  if (lastFrame != R.frame + 1) {
    lastFrame = R.frame + 1;
    draw = 0;
  }
  if (++draw > 3)
    return;
  const auto p = state.pipeline;
  auto *target = find_view(state.target.Get());
  auto *depth = find_view(state.depthTarget.Get());
  if (!target || !target->tex->valid)
    return;
  const uint32_t width = std::max(1u, target->tex->width >> target->baseMip);
  const uint32_t height = std::max(1u, target->tex->height >> target->baseMip);
  const auto vp = state.viewport;
  const auto sc = state.scissor;
  dklog(
      "GX probe f%llu d%u: vs=%016llx fs=%016llx indexed=%d count=%u instances=%u first=%u base=%d "
      "firstInstance=%u viewport=%g,%g,%g,%g,%g,%g scissor=%u,%u,%u,%u cull=%u front=%u "
      "depth=%u/%u/%u mask=%x blend=%x factors=%u,%u,%u,%u",
      (unsigned long long)(R.frame + 1), draw, (unsigned long long)p->vs.shader->glslHash,
      (unsigned long long)p->fs.shader->glslHash, indexed, count, instances, first, base,
      firstInstance, vp.x, vp.y, vp.width, vp.height, vp.near, vp.far, sc.x, sc.y, sc.width,
      sc.height, p->raster.cullMode, p->raster.frontFace, p->depth.depthTestEnable,
      p->depth.depthWriteEnable, p->depth.depthCompareOp, p->write.masks, p->color.blendEnableMask,
      p->blend.srcColorBlendFactor, p->blend.dstColorBlendFactor, p->blend.srcAlphaBlendFactor,
      p->blend.dstAlphaBlendFactor);
  // QueueWaitIdle flushes and signals a flushing fence. CPU-uncached aliases can
  // then observe the actual GPU copy destinations, not the producer staging data.
  idle();
  for (uint32_t gi = 0; gi < state.groups.size(); ++gi) {
    auto &g = state.groups[gi];
    if (!g.group)
      continue;
    for (auto &b : g.group->entries) {
      if (!b.buffer)
        continue;
      const uint64_t off = offset(g, b);
      if (off > b.buffer->size)
        continue;
      const uint64_t size = std::min(b.size, b.buffer->size - off);
      dklog("GX probe binding %u:%u offset=%llu size=%llu dynamic=%d", gi, b.binding,
            (unsigned long long)off, (unsigned long long)size, b.dynamic);
      // Full storage/index streams once per sampled frame, uniforms per draw.
      if (gi != 1 && draw != 1)
        continue;
      char name[48];
      snprintf(name, sizeof name, "group%u-binding%u", gi, b.binding);
      probe_dump(name, b.buffer->mem.cpu + off, std::min<uint64_t>(size, 8u << 20), draw);
    }
  }
  if (draw == 1 && state.index)
    probe_dump("indices", state.index->mem.cpu, std::min<uint64_t>(state.index->size, 2u << 20),
               draw);

  static DkShader controlVs{}, opaqueFs{};
  static bool loaded = false;
  if (!loaded) {
    loaded = code_load(controlVs, present_vsh_dksh, present_vsh_dksh_size, "probe control VS") &&
             code_load(opaqueFs, probe_fsh_dksh, probe_fsh_dksh_size, "probe opaque FS");
    if (!loaded)
      fatal("GX isolation probe shader failed to load");
  }
  code_make_visible();
  DkImageLayoutMaker maker;
  dkImageLayoutMakerDefaults(&maker, R.device);
  maker.type = DkImageType_2D;
  maker.format = DkImageFormat_RGBA8_Unorm;
  maker.dimensions[0] = width;
  maker.dimensions[1] = height;
  maker.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &maker);
  auto mem = image_alloc(dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));
  DkImage image;
  dkImageInitialize(&image, &layout, mem.block, mem.offset);
  DkImageView view;
  dkImageViewDefaults(&view, &image);
  const DkImageView *targets[] = {&view};
  dkCmdBufBindRenderTargets(R.cmd, targets, 1, depth ? &depth->view : nullptr);
  const char *names[] = {"control", "geometry", "scissor", "cull", "depth", "fragment", "blend"};
  std::array<ProbePixels, 7> results{};
  for (unsigned phase = 0; phase < results.size(); ++phase) {
    barrier();
    set_view(0, 0, width, height);
    DkColorWriteState write;
    dkColorWriteStateDefaults(&write);
    dkColorWriteStateSetMask(&write, 0, DkColorMask_RGBA);
    dkCmdBufBindColorWriteState(R.cmd, &write);
    const float sentinel[] = {8.f / 255, 16.f / 255, 24.f / 255, 32.f / 255};
    dkCmdBufClearColor(R.cmd, 0, DkColorMask_RGBA, sentinel);
    auto raster = p->raster;
    raster.cullMode = phase == 3 || phase == 4 ? p->raster.cullMode : DkFace_None;
    dkCmdBufBindRasterizerState(R.cmd, &raster);
    auto ds = p->depth;
    ds.depthTestEnable = phase == 4 && depth && p->depth.depthTestEnable;
    ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(R.cmd, &ds);
    auto color = p->color;
    dkColorStateSetBlendEnable(&color, 0, phase == 6 && (p->color.blendEnableMask & 1));
    dkCmdBufBindColorState(R.cmd, &color);
    dkCmdBufBindBlendStates(R.cmd, 0, &p->blend, 1);
    const DkShader *stages[] = {phase == 0 ? &controlVs : &p->vs.shader->shader,
                                phase >= 5 ? &p->fs.shader->shader : &opaqueFs};
    dkCmdBufBindShaders(R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, stages, 2);
    if (phase == 0) {
      dkCmdBufBindVtxAttribState(R.cmd, nullptr, 0);
      dkCmdBufBindVtxBufferState(R.cmd, nullptr, 0);
      dkCmdBufDraw(R.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    } else {
      dkCmdBufBindVtxAttribState(R.cmd, p->attributes.data(), p->attributes.size());
      dkCmdBufBindVtxBufferState(R.cmd, p->vertexStates.data(), p->vertexStates.size());
      dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
      if (phase >= 2 && phase <= 4)
        dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
      bind_stage(p->vs, DkStage_Vertex);
      if (phase >= 5)
        bind_stage(p->fs, DkStage_Fragment);
      if (indexed) {
        dkCmdBufBindIdxBuffer(R.cmd, state.indexFormat, state.index->mem.gpu + state.indexOffset);
        dkCmdBufDrawIndexed(R.cmd, p->topology, count, instances, first, base, firstInstance);
      } else {
        dkCmdBufDraw(R.cmd, p->topology, count, instances, first, firstInstance);
      }
    }
    results[phase] = probe_pixels(view, width, height, names[phase], draw);
  }
  dklog("GX probe verdict f%llu d%u: control=%u geometry=%u scissor=%u cull=%u depth=%u "
        "fragment=%u blend=%u "
        "(changed pixels; failed readbacks must be excluded)",
        (unsigned long long)(R.frame + 1), draw, results[0].changed, results[1].changed,
        results[2].changed, results[3].changed, results[4].changed, results[5].changed,
        results[6].changed);
  const DkImageView *original[] = {&target->view};
  dkCmdBufBindRenderTargets(R.cmd, original, 1, depth ? &depth->view : nullptr);
  dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
  dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
  state.boundPipeline = nullptr;
  image_free_later(mem);
  pendingProbe = {target, width, height, draw};
}

void probe_after_draw() {
  if (!pendingProbe.target)
    return;
  probe_pixels(pendingProbe.target->view, pendingProbe.width, pendingProbe.height, "normal",
               pendingProbe.draw);
  pendingProbe = {};
}
