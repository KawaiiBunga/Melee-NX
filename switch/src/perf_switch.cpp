#include "perf_switch.h"
#include <switch.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void pc_log_line(const char* fmt, ...);

namespace {
enum class Affinity { Baseline, Main, Split };
Affinity affinity = Affinity::Baseline;
const char* affinity_name = "baseline";
int compile_workers = 1;
constexpr uint64_t BinNs = 250000; // Report percentile upper bounds, 0.25 ms bins.
constexpr size_t BinCount = 4000;  // >=1000 ms is reported as overflow.
std::array<uint32_t, BinCount> bins{};
std::array<uint64_t, MELEE_PERF_STAGE_COUNT> stages{};
std::array<uint32_t, MELEE_PERF_STAGE_COUNT> calls{};
uint64_t window_ns, worst_ns;
uint32_t frames, overflow, late;

double percentile(unsigned percent) {
    const uint32_t rank = (frames * percent + 99) / 100;
    uint32_t seen = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
        seen += bins[i];
        if (seen >= rank)
            return (i + 1) * BinNs / 1e6;
    }
    return -1; // Quantile is beyond the histogram range; never clamp silently.
}
double average(size_t stage) {
    return calls[stage] ? stages[stage] / (double)calls[stage] / 1e6 : 0;
}
} // namespace

extern "C" void melee_nx_perf_init(void) {
    if (FILE* file = std::fopen("sdmc:/switch/melee-nx/perf.cfg", "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), file)) {
            char key[32], value[32], extra[2];
            if (std::sscanf(line, " %31s %31s %1s", key, value, extra) != 2)
                continue;
            if (std::strcmp(key, "affinity") == 0) {
                if (std::strcmp(value, "main") == 0) {
                    affinity = Affinity::Main;
                    affinity_name = "main";
                } else if (std::strcmp(value, "split") == 0) {
                    affinity = Affinity::Split;
                    affinity_name = "split";
                } else if (std::strcmp(value, "baseline") == 0) {
                    affinity = Affinity::Baseline;
                    affinity_name = "baseline";
                }
            } else if (std::strcmp(key, "compile_workers") == 0) {
                compile_workers = (std::strcmp(value, "2") == 0) ? 2 : 1;
            }
        }
        std::fclose(file);
    }
    // Bridge the compile-worker count to aurora's pipeline cache (it reads this
    // env in initialize_pipeline_cache, which runs later in melee's main()).
    setenv("AURORA_COMPILE_WORKERS", compile_workers == 2 ? "2" : "1", 1);
    pc_log_line("SWEEP affinity %s compile_workers %d histogram_bin_ms 0.25 histogram_limit_ms 1000",
                affinity_name, compile_workers);
}

extern "C" void melee_nx_perf_thread(const char* name) {
    int role;
    if (std::strcmp(name, "Main thread") == 0) role = 0;
    else if (std::strcmp(name, "Aurora FIFO processor") == 0) role = 1;
    else if (std::strcmp(name, "Aurora render worker") == 0) role = 2;
    else return;

    u64 allowed = 0, actual = 0;
    s32 preferred = -1;
    s32 priority = 0;
    const Result info_rc = svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int cores[32], count = 0;
    for (int i = 0; i < 32; ++i)
        if (allowed & (1ull << i)) cores[count++] = i;
    int target = -1;
    if (R_SUCCEEDED(info_rc) && count >= 3) {
        if (affinity != Affinity::Baseline && role == 0) target = cores[0];
        if (affinity == Affinity::Split && role == 1) target = cores[1];
        if (affinity == Affinity::Split && role == 2) target = cores[count - 1];
    }
    const Result set_rc = target < 0 ? 0 :
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, target, 1u << target);
    const Result mask_rc = svcGetThreadCoreMask(&preferred, &actual, CUR_THREAD_HANDLE);
    const Result priority_rc = svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    pc_log_line("THREAD %s policy %s allowed 0x%llx requested %d actual 0x%llx preferred %d priority %d rc %x/%x/%x/%x",
                name, affinity_name, (unsigned long long)allowed, target,
                (unsigned long long)actual, preferred, priority,
                info_rc, set_rc, mask_rc, priority_rc);
}

extern "C" uint64_t melee_nx_perf_now(void) {
    return armTicksToNs(armGetSystemTick());
}

extern "C" void melee_nx_perf_stage(MeleePerfStage stage, uint64_t elapsed_ns) {
    stages[stage] += elapsed_ns;
    ++calls[stage];
}

extern "C" void melee_nx_perf_frame(uint64_t elapsed_ns) {
    ++frames;
    window_ns += elapsed_ns;
    if (elapsed_ns > worst_ns) worst_ns = elapsed_ns;
    if (elapsed_ns > 1000000000ull / 60) ++late;
    const size_t bin = elapsed_ns / BinNs;
    if (bin < bins.size()) ++bins[bin]; else ++overflow;
    if (window_ns < 2000000000ull) return;

    pc_log_line("FRAMES n %u span_ms %.1f fps %.2f p50_le_ms %.2f p95_le_ms %.2f p99_le_ms %.2f worst_ms %.2f late60 %u overflow %u",
                frames, window_ns / 1e6, frames * 1e9 / window_ns,
                percentile(50), percentile(95), percentile(99), worst_ns / 1e6, late, overflow);
    pc_log_line("STAGES fifo_ms %.3f texture_ms %.3f finish_ms %.3f ui_ms %.3f dispatch_ms %.3f alarms_ms %.3f retrace_ms %.3f",
                average(MELEE_PERF_FIFO), average(MELEE_PERF_TEXTURE), average(MELEE_PERF_FINISH),
                average(MELEE_PERF_UI), average(MELEE_PERF_DISPATCH),
                average(MELEE_PERF_ALARMS), average(MELEE_PERF_RETRACE));
    bins.fill(0);
    stages.fill(0);
    calls.fill(0);
    frames = overflow = late = 0;
    window_ns = worst_ns = 0;
}
