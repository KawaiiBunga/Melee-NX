#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kartpad::deko {
// The CPU uploads twelve tightly packed u32 offsets (48 bytes). A scalar
// uniform array has a 16-byte element stride in GLSL std140; three vec4u words
// preserve the CPU layout under both WGSL and GLSL without repacking uploads.
inline constexpr std::string_view GxArrayStartType = "vec4u, 3";

inline std::string gx_array_start_access(uint32_t index) {
  // Attribute indices are shader-generation constants. Emit a static swizzle,
  // avoiding uam's unsupported dynamic uniform-vector component access too.
  return "ubuf.array_start[" + std::to_string(index / 4) + "]." + "xyzw"[index % 4];
}
} // namespace kartpad::deko
