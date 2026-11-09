// What Aurora's shader generator and the renderer's cache code reach for outside the files the tool
// compiles. Nothing here draws: the generator only builds text.
#include <webgpu/webgpu.h>
#include <webgpu/webgpu_cpp.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "shim/zlib.h"

#include "gfx/recording.hpp"
#include "gx/gx.hpp"
#include "internal.hpp"
#include "logging.hpp"

std::string g_capturedWgsl;

// Renderer state that shader_info.cpp links against; only its draw-time paths use it, never the generator.
namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel, const char* module, const char* message, unsigned int len) noexcept {
    std::fprintf(stderr, "[%s] %.*s\n", module ? module : "aurora", static_cast<int>(len), message);
}
namespace gfx {
uint32_t align_uniform(uint32_t value) { return (value + 255u) & ~255u; }
Range push_uniform(const uint8_t*, size_t) { return {}; }
} // namespace gfx
namespace gx {
GXState g_gxState{};
const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }
} // namespace gx
} // namespace aurora

// Aurora creates shader modules only through these; keep the WGSL it passes (main.cpp reads it).
extern "C" WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice, const WGPUShaderModuleDescriptor* descriptor) {
    for (const WGPUChainedStruct* c = descriptor ? descriptor->nextInChain : nullptr; c; c = c->next) {
        if (c->sType == WGPUSType_ShaderSourceWGSL) {
            const WGPUStringView code = reinterpret_cast<const WGPUShaderSourceWGSL*>(c)->code;
            g_capturedWgsl.assign(code.data, code.length == WGPU_STRLEN ? std::strlen(code.data) : code.length);
        }
    }
    static int handle;
    return reinterpret_cast<WGPUShaderModule>(&handle);
}
extern "C" void wgpuShaderModuleAddRef(WGPUShaderModule) {}
extern "C" void wgpuShaderModuleRelease(WGPUShaderModule) {}
extern "C" void wgpuDeviceAddRef(WGPUDevice) {}
extern "C" void wgpuDeviceRelease(WGPUDevice) {}

namespace aurora::webgpu {
wgpu::Device g_device;
}

extern "C" uLong crc32(uLong crc, const Bytef* buf, uInt len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = static_cast<uint32_t>(crc) ^ 0xFFFFFFFFu;
    for (uInt i = 0; i < len; ++i)
        c = table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
