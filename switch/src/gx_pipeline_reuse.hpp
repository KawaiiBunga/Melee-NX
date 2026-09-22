#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

namespace melee_nx {

// GX configurations have unique object representations (also required by
// Aurora's pipeline hash). A hash hit alone must never reuse another shader's
// texture/uniform layout. Keep the full key and check it byte for byte.
template <typename Config>
bool same_pipeline_config(bool valid, const Config& a, const Config& b) noexcept {
    static_assert(std::has_unique_object_representations_v<Config>);
    return valid && std::memcmp(&a, &b, sizeof(Config)) == 0;
}

// Owned only by the FIFO processor. Fixed capacity, no allocations or whole
// cache eviction on the draw path; a collision replaces just one slot.
template <typename Config, typename Info, size_t Capacity>
class ShaderInfoCache {
    static_assert(Capacity > 0);
    struct Entry {
        Config config;
        Info info;
    };
    std::array<std::optional<Entry>, Capacity> entries{};

public:
    template <typename Build>
    Info get(const Config& config, uint64_t hash, Build&& build, bool& hit) {
        auto& entry = entries[hash % Capacity];
        hit = entry && same_pipeline_config(true, entry->config, config);
        if (!hit)
            entry.emplace(Entry{config, std::forward<Build>(build)(config)});
        return entry->info;
    }
};

} // namespace melee_nx
