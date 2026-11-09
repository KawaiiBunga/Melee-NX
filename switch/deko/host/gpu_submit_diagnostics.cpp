// The release deko3d library turns a failed NVIDIA submission into a generic 2359-0001 abort.
// Preserve the driver result before that path loses it. (From KartPadNX; linked with
// --wrap=nvGpuChannelAppendEntry and --wrap=nvGpuChannelKickoff, switch/CMakeLists.txt.)
#include <switch/types.h>
#include <switch/result.h>
extern "C" {
#include <switch/nvidia/gpu_channel.h>
}
#include <cstdio>

extern "C" Result __real_nvGpuChannelAppendEntry(NvGpuChannel*, iova_t, size_t, u32, u32);
extern "C" Result __real_nvGpuChannelKickoff(NvGpuChannel*);
extern "C" void kartpad_deko_log_failure_context();
extern "C" void kartpad_log_flush_sync(void);

namespace {
void report_failure(const char* operation, NvGpuChannel* channel, Result result,
                    iova_t address = 0, size_t words = 0, u32 flags = 0, u32 threshold = 0) {
    // Do not query/consume the error event here: deko3d's checkError still owns it. Likewise, leave
    // the return value and submission/retry policy intact.
    std::fprintf(stderr,
        "[gpu-submit] %s failed: rc=0x%08x (%04u-%04u), fd=%u, entries=%u/%u, "
        "syncpoint=%u value=%u increment=%u, iova=0x%llx words=%zu flags=0x%x threshold=%u\n",
        operation, result, 2000 + R_MODULE(result), R_DESCRIPTION(result), channel->base.fd,
        channel->num_entries, GPFIFO_QUEUE_SIZE, channel->fence.id, channel->fence.value,
        channel->fence_incr, static_cast<unsigned long long>(address), words, flags, threshold);
    // Save the original result before any optional follow-up diagnostic call.
    std::fflush(stderr);
    kartpad_log_flush_sync();
    // This ioctl reads the channel's error details without waiting on or clearing the notification
    // event. It can tell a GPU fault from a submission error even before the notifier is signalled.
    NvError error{};
    const Result infoResult = nvGpuChannelGetErrorInfo(channel, &error);
    std::fprintf(stderr, "[gpu-submit] error_info rc=0x%08x type=0x%x", infoResult, error.type);
    if (R_SUCCEEDED(infoResult))
        for (u32 word : error.info) std::fprintf(stderr, " %08x", word);
    std::fputc('\n', stderr);
    kartpad_deko_log_failure_context();
    std::fflush(stderr);
    kartpad_log_flush_sync();
}
}  // namespace

extern "C" Result __wrap_nvGpuChannelAppendEntry(NvGpuChannel* channel, iova_t address,
                                                  size_t words, u32 flags, u32 threshold) {
    const Result result = __real_nvGpuChannelAppendEntry(channel, address, words, flags, threshold);
    if (R_FAILED(result)) report_failure("append", channel, result, address, words, flags, threshold);
    return result;
}

extern "C" Result __wrap_nvGpuChannelKickoff(NvGpuChannel* channel) {
    const Result result = __real_nvGpuChannelKickoff(channel);
    if (R_FAILED(result)) report_failure("kickoff", channel, result);
    return result;
}
