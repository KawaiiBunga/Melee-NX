#include "perf_switch.h"
#include <switch.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" void pc_log_line(const char* fmt, ...);

namespace {
// Every `key value` line of perf.cfg with a numeric value (decimal or 0x hex), for melee_nx_perf_int
// and melee_nx_opt. The named keys parsed in melee_nx_perf_init keep their own handling.
struct CfgEntry {
    char key[32];
    long value;
};
constexpr int MaxCfgEntries = 64;
std::array<CfgEntry, MaxCfgEntries> cfg_entries{};
int cfg_count = 0;
const CfgEntry* cfg_find(const char* key) {
    for (int i = 0; i < cfg_count; ++i)
        if (std::strcmp(cfg_entries[i].key, key) == 0)
            return &cfg_entries[i];
    return nullptr;
}
std::mutex opt_log_mutex;
std::array<char[32], MaxCfgEntries> opt_logged{};
int opt_logged_count = 0;

enum class Affinity { Baseline, Main, Split, Tuned };
Affinity affinity = Affinity::Baseline;
const char* affinity_name = "baseline";
int compile_workers = 1;
int monolithic_cache = 0;
int launch_warm = 1; // 0 skips the startup shader warm-up
int gx_skip_unready = 0;
int warm_helpers = 0;
int uber = 0; // 0 off, 1 hybrid, 2 always (where supported); measured too slow on the GPU, see docs
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
            if (key[0] != '#' && cfg_count < MaxCfgEntries && !cfg_find(key)) {
                char* end = nullptr;
                const long number = std::strtol(value, &end, 0);
                if (end != value && *end == '\0') {
                    std::memcpy(cfg_entries[cfg_count].key, key, sizeof(key));
                    cfg_entries[cfg_count++].value = number;
                }
            }
            if (std::strcmp(key, "affinity") == 0) {
                if (std::strcmp(value, "main") == 0) {
                    affinity = Affinity::Main;
                    affinity_name = "main";
                } else if (std::strcmp(value, "split") == 0) {
                    affinity = Affinity::Split;
                    affinity_name = "split";
                } else if (std::strcmp(value, "tuned") == 0) {
                    affinity = Affinity::Tuned;
                    affinity_name = "tuned";
                } else if (std::strcmp(value, "baseline") == 0) {
                    affinity = Affinity::Baseline;
                    affinity_name = "baseline";
                }
            } else if (std::strcmp(key, "compile_workers") == 0) {
                compile_workers = (std::strcmp(value, "2") == 0) ? 2 : 1;
            } else if (std::strcmp(key, "monolithic_cache") == 0) {
                monolithic_cache = (std::strcmp(value, "1") == 0) ? 1 : 0;
            } else if (std::strcmp(key, "uber") == 0) {
                const int mode = std::atoi(value);
                uber = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
            } else if (std::strcmp(key, "warm_helpers") == 0) {
                const int count = std::atoi(value);
                warm_helpers = count < 0 ? 0 : (count > 2 ? 2 : count);
            } else if (std::strcmp(key, "gx_skip_unready") == 0) {
                gx_skip_unready = (std::strcmp(value, "1") == 0) ? 1 : 0;
            } else if (std::strcmp(key, "launch_warm") == 0) {
                launch_warm = std::atoi(value) != 0 ? 1 : 0;
            }
        }
        std::fclose(file);
    }
    // Bridge perf.cfg knobs to aurora, which reads these env vars when it creates
    // the device / pipeline cache later in melee's main().
    setenv("AURORA_COMPILE_WORKERS", compile_workers == 2 ? "2" : "1", 1);
    // The uber pipelines all share one shader; without a shared Vulkan pipeline cache
    // each would recompile it.
    setenv("AURORA_MONOLITHIC_PIPELINE_CACHE", (monolithic_cache == 1 || uber > 0) ? "1" : "0", 1);
    char warm[16];
    std::snprintf(warm, sizeof(warm), "%d", launch_warm);
    setenv("AURORA_LAUNCH_WARM_SECONDS", warm, 1);
    setenv("AURORA_GX_SKIP_UNREADY", gx_skip_unready == 1 ? "1" : "0", 1);
    char helpers[8];
    std::snprintf(helpers, sizeof(helpers), "%d", warm_helpers);
    setenv("AURORA_LAUNCH_WARM_HELPERS", helpers, 1);
    if (uber > 0) {
        // A previous run turned the uber-shader off after frames collapsed under it.
        if (FILE* marker = std::fopen("sdmc:/switch/melee-nx/uber.tripped", "rb")) {
            std::fclose(marker);
            uber = 0;
            pc_log_line("SWEEP uber disabled by sdmc:/switch/melee-nx/uber.tripped");
        }
    }
    char uberMode[8];
    std::snprintf(uberMode, sizeof(uberMode), "%d", uber);
    setenv("AURORA_UBER", uberMode, 1);
    pc_log_line(
        "SWEEP affinity %s compile_workers %d monolithic_cache %d launch_warm %d gx_skip_unready %d "
        "histogram_bin_ms 0.25 histogram_limit_ms 1000",
        affinity_name, compile_workers, monolithic_cache, launch_warm, gx_skip_unready);
}

extern "C" int melee_nx_perf_int(const char* key, int fallback) {
    const CfgEntry* entry = cfg_find(key);
    return entry ? static_cast<int>(entry->value) : fallback;
}

// An optimization switch: `opt_safe 1` in perf.cfg turns every opt off except those the file sets
// explicitly. Each key is logged once, with where its value came from, so a capture says what ran.
extern "C" int melee_nx_opt(const char* key, int fallback) {
    const CfgEntry* entry = cfg_find(key);
    const char* source = "default";
    int value = fallback;
    if (entry) {
        value = static_cast<int>(entry->value);
        source = "perf.cfg";
    } else if (melee_nx_perf_int("opt_safe", 0) != 0) {
        value = 0;
        source = "opt_safe";
    }
    std::lock_guard lock(opt_log_mutex);
    for (int i = 0; i < opt_logged_count; ++i)
        if (std::strcmp(opt_logged[i], key) == 0)
            return value;
    if (opt_logged_count < MaxCfgEntries) {
        std::snprintf(opt_logged[opt_logged_count++], sizeof(opt_logged[0]), "%s", key);
        pc_log_line("[opt] %s=%d (%s)", key, value, source);
    }
    return value;
}

extern "C" void melee_nx_perf_thread(const char* name) {
    int role;
    if (std::strcmp(name, "Main thread") == 0)
        role = 0;
    else if (std::strcmp(name, "Aurora FIFO processor") == 0)
        role = 1;
    else if (std::strcmp(name, "Aurora render worker") == 0)
        role = 2;
    else
        return;

    u64 allowed = 0, actual = 0;
    s32 preferred = -1;
    s32 priority = 0;
    const Result info_rc = svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int cores[32], count = 0;
    for (int i = 0; i < 32; ++i)
        if (allowed & (1ull << i))
            cores[count++] = i;
    int target = -1;
    u64 shared_mask = 0; // tuned: a set of cores instead of one
    if (R_SUCCEEDED(info_rc) && count >= 3) {
        if (affinity != Affinity::Baseline && affinity != Affinity::Tuned && role == 0)
            target = cores[0];
        if (affinity == Affinity::Split && role == 1)
            target = cores[1];
        if (affinity == Affinity::Split && role == 2)
            target = cores[count - 1];
        // KartPadNX's tuned placement: the game thread owns the middle core alone, and everything
        // else shares the remaining ones (the lowest for the FIFO thread, the highest for rendering).
        if (affinity == Affinity::Tuned) {
            if (role == 0) {
                target = cores[1];
            } else {
                shared_mask = allowed & ~(1ull << cores[1]);
                target = role == 1 ? cores[0] : cores[count - 1];
            }
        }
    }
    const Result set_rc = target < 0 ? 0
                          : shared_mask != 0
                              ? svcSetThreadCoreMask(CUR_THREAD_HANDLE, target, shared_mask)
                              : svcSetThreadCoreMask(CUR_THREAD_HANDLE, target, 1u << target);
    const Result mask_rc = svcGetThreadCoreMask(&preferred, &actual, CUR_THREAD_HANDLE);
    const Result priority_rc = svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    pc_log_line(
        "THREAD %s policy %s allowed 0x%llx requested %d actual 0x%llx preferred %d priority %d rc %x/%x/%x/%x",
        name, affinity_name, (unsigned long long)allowed, target, (unsigned long long)actual,
        preferred, priority, info_rc, set_rc, mask_rc, priority_rc);
    melee_nx_prof_register(role);
}

extern "C" uint64_t melee_nx_perf_now(void) { return armTicksToNs(armGetSystemTick()); }

extern "C" void melee_nx_perf_stage(MeleePerfStage stage, uint64_t elapsed_ns) {
    stages[stage] += elapsed_ns;
    ++calls[stage];
}

extern "C" void melee_nx_perf_frame(uint64_t elapsed_ns) {
    ++frames;
    window_ns += elapsed_ns;
    if (elapsed_ns > worst_ns)
        worst_ns = elapsed_ns;
    if (elapsed_ns > 1000000000ull / 60)
        ++late;
    const size_t bin = elapsed_ns / BinNs;
    if (bin < bins.size())
        ++bins[bin];
    else
        ++overflow;
    if (window_ns < 2000000000ull)
        return;

    pc_log_line(
        "FRAMES n %u span_ms %.1f fps %.2f p50_le_ms %.2f p95_le_ms %.2f p99_le_ms %.2f worst_ms %.2f late60 %u overflow %u",
        frames, window_ns / 1e6, frames * 1e9 / window_ns, percentile(50), percentile(95),
        percentile(99), worst_ns / 1e6, late, overflow);
    pc_log_line(
        "STAGES fifo_ms %.3f texture_ms %.3f finish_ms %.3f ui_ms %.3f dispatch_ms %.3f alarms_ms %.3f retrace_ms %.3f",
        average(MELEE_PERF_FIFO), average(MELEE_PERF_TEXTURE), average(MELEE_PERF_FINISH),
        average(MELEE_PERF_UI), average(MELEE_PERF_DISPATCH), average(MELEE_PERF_ALARMS),
        average(MELEE_PERF_RETRACE));
    bins.fill(0);
    stages.fill(0);
    calls.fill(0);
    frames = overflow = late = 0;
    window_ns = worst_ns = 0;
}
