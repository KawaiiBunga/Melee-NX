#pragma once

#include <cstdint>
#include <limits>

namespace melee_nx {

// Protected by Aurora's pipeline mutex. Keep usage metadata even while the
// worker has removed a descriptor from the queue to compile it.
struct PipelineRequestState {
    uint32_t firstFrameUsed = std::numeric_limits<uint32_t>::max();
    bool known = false;
    bool compiling = false;

    bool remember(uint32_t frame, bool persist) noexcept {
        const bool changed = !known || frame < firstFrameUsed;
        if (changed)
            firstFrameUsed = frame;
        known = true;
        return persist && changed;
    }
};

} // namespace melee_nx
