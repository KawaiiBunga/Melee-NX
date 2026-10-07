#pragma once

#include <cstdint>

// Layout of the uber-shader's per-draw uniform block. It is a pair of vec4
// arrays so the same bytes read identically from WGSL (std140 uniform rules) and
// from the CPU packer: `f` holds every float, `u` holds the packed configuration.
// WGSL reads these through the same constants (see uber_wgsl.hpp), so a change
// here changes both sides.
namespace melee_nx::uber {

// Float section, in vec4 units.
constexpr uint32_t F_VIEWPORT = 0;                  // render size xy, logical size zw
constexpr uint32_t F_PROJ = 1;                      // mat4x4f, 4 columns
constexpr uint32_t F_POSTEX = F_PROJ + 4;           // 10 position + 10 texture mat3x4f
constexpr uint32_t F_NRM = F_POSTEX + 20 * 3;       // 10 normal mat3x4f
constexpr uint32_t F_PT = F_NRM + 10 * 3;           // 20 post-transform mat3x4f
constexpr uint32_t F_LIGHTS = F_PT + 20 * 3;        // 8 lights x (pos, dir, color, cos_att, dist_att)
constexpr uint32_t F_CC = F_LIGHTS + 8 * 5;         // per channel: amb, mat, amb alpha, mat alpha
constexpr uint32_t F_TEVREG = F_CC + 8;             // TEVPREV, TEVREG0-2
constexpr uint32_t F_KCOLOR = F_TEVREG + 4;         // KCOLOR0-3
constexpr uint32_t F_FOG = F_KCOLOR + 4;            // color; a,b,c,range_center; range_k[3]
constexpr uint32_t F_TCSCALE = F_FOG + 5;           // 8 texcoord scales
constexpr uint32_t F_INDMTX = F_TCSCALE + 8;        // 3 x (2 vec4), reserved for indirect stages
constexpr uint32_t F_TEXSB = F_INDMTX + 6;          // 8 x (width, height, lod bias, 0)
constexpr uint32_t F_COUNT = F_TEXSB + 8;

// Integer section, in vec4 units.
constexpr uint32_t U_HDR0 = 0;   // vtx stride, tev stage count, fog type | range << 8, sampled masks
constexpr uint32_t U_HDR1 = 1;   // alpha compare: comp0, ref0, comp1, ref1
constexpr uint32_t U_HDR2 = 2;   // alpha compare op, alpha compare enabled
constexpr uint32_t U_LIGHTMASK = 3;
constexpr uint32_t U_ATTRS = 4;  // 21 attributes x 2 words
constexpr uint32_t UberAttrCount = 21;
constexpr uint32_t U_SWAP = U_ATTRS + 11;
constexpr uint32_t U_CHAN = U_SWAP + 1;
constexpr uint32_t U_TCG = U_CHAN + 1;  // 8 x vec4u
constexpr uint32_t U_TEV = U_TCG + 8;   // 16 x vec4u
constexpr uint32_t U_COUNT = U_TEV + 16;

constexpr uint32_t MaxTevStagesSupported = 16;
constexpr uint32_t BlockBytes = (F_COUNT + U_COUNT) * 16;

} // namespace melee_nx::uber
