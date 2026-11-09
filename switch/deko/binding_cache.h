#pragma once
#include <array>
#include <cstdint>

namespace kd {
// Valid only within the adapter's render pass. Out-of-range slots deliberately
// bypass caching, so the native API still receives and validates those calls.
struct BindingCache {
  struct Buffer { uint64_t address = 0, bytes = 0; uint32_t generation = 0; };
  struct Texture { uint32_t handle = 0, generation = 0; };
  struct Stage {
    std::array<Buffer, 32> uniforms{}, storage{};
    std::array<Texture, 32> textures{};
  };
  std::array<Stage, 2> stages{};
  uint64_t indexAddress = 0;
  uint32_t indexFormat = 0;
  uint32_t generation = 1, indexGeneration = 0;

  void invalidate() {
    if (++generation == 0) {
      stages = {};
      indexGeneration = 0;
      generation = 1;
    }
  }

  bool buffer(std::array<Buffer, 32>& slots, uint32_t slot,
                     uint64_t address, uint64_t bytes) {
    if (slot >= slots.size()) return true;
    auto& old = slots[slot];
    if (old.generation == generation && old.address == address && old.bytes == bytes) return false;
    old = {address, bytes, generation};
    return true;
  }
  bool texture(uint32_t stage, uint32_t slot, uint32_t handle) {
    if (stage >= stages.size() || slot >= stages[stage].textures.size()) return true;
    auto& old = stages[stage].textures[slot];
    if (old.generation == generation && old.handle == handle) return false;
    old = {handle, generation};
    return true;
  }
  bool uniform(uint32_t stage, uint32_t slot, uint64_t address, uint64_t bytes) {
    return stage >= stages.size() || buffer(stages[stage].uniforms, slot, address, bytes);
  }
  bool storage(uint32_t stage, uint32_t slot, uint64_t address, uint64_t bytes) {
    return stage >= stages.size() || buffer(stages[stage].storage, slot, address, bytes);
  }
  bool index(uint64_t address, uint32_t format) {
    if (indexGeneration == generation && indexAddress == address && indexFormat == format) return false;
    indexAddress = address;
    indexFormat = format;
    indexGeneration = generation;
    return true;
  }
};
} // namespace kd
