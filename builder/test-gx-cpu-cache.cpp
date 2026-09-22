// Host-side policy regression checks; no game data or GPU is needed.
// clang++ -std=c++20 -O2 -I switch/src builder/test-gx-cpu-cache.cpp -o /tmp/test-gx-cpu-cache
#include "gx_pipeline_reuse.hpp"
#include "pipeline_request_state.hpp"

#include <cassert>
#include <iostream>

int main() {
    using Config = std::array<uint32_t, 16>;
    Config a{}, b{};
    assert(!melee_nx::same_pipeline_config(false, a, b));
    assert(melee_nx::same_pipeline_config(true, a, b));
    // A difference anywhere in the key, including its tail, must invalidate.
    for (size_t i = 0; i < sizeof(b); ++i) {
        reinterpret_cast<unsigned char*>(b.data())[i] = 1;
        assert(!melee_nx::same_pipeline_config(true, a, b));
        b = a;
    }

    melee_nx::ShaderInfoCache<Config, uint32_t, 4> cache;
    unsigned builds = 0;
    auto analyze = [&](const Config& c) { ++builds; return c.back(); };
    bool hit = true;
    assert(cache.get(a, 5, analyze, hit) == 0 && !hit);
    assert(cache.get(a, 5, analyze, hit) == 0 && hit && builds == 1);
    // Deliberately collide the entire hash, not just the bucket index.
    b.back() = 123;
    assert(cache.get(b, 5, analyze, hit) == 123 && !hit && builds == 2);
    assert(cache.get(a, 5, analyze, hit) == 0 && !hit && builds == 3);
    // Churn beyond capacity. Every replacement must return its own analysis.
    for (unsigned i = 1; i < 10000; ++i) {
        b.back() = i;
        assert(cache.get(b, i, analyze, hit) == i && !hit);
        assert(cache.get(b, i, analyze, hit) == i && hit);
    }

    melee_nx::PipelineRequestState fresh;
    assert(fresh.remember(100, true));
    fresh.compiling = true;
    for (unsigned i = 100; i < 10000; ++i)
        assert(!fresh.remember(i, true));
    assert(fresh.compiling && fresh.firstFrameUsed == 100);
    assert(fresh.remember(50, true));
    assert(!fresh.remember(50, true));
    fresh.compiling = false;
    assert(fresh.firstFrameUsed == 50 && !fresh.remember(200, true));

    melee_nx::PipelineRequestState seeded;
    assert(!seeded.remember(500, false));
    assert(!seeded.remember(600, true));
    assert(seeded.remember(400, true));
    assert(!seeded.remember(400, true));
    seeded = {};
    assert(seeded.remember(600, true));
    std::cout << "GX cache collision/capacity/invalidation and in-flight persistence checks passed\n";
}
