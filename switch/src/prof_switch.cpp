// Sampling profiler for the game, FIFO, render and shader-compile threads (PERFORMANCE.md).
//
// perf.cfg (all default off):
//   prof_seconds N   record N seconds, then stop; 0 = never start
//   prof_start S     begin S seconds after launch (default 60: past boot, into a match)
//   prof_hz H        samples per second per thread (default 750, at most 4000)
//
// A thread on the highest allowed core, above every game thread, pauses each registered thread H
// times per second, reads its PC and LR with svcGetThreadContext3 and resumes it. Records are
// appended to sdmc:/switch/melee-nx/prof.bin once a second. The pause window is register reads only:
// nothing here takes a lock or allocates while a game thread is stopped. builder/prof-report.py
// attributes the samples to functions through the ELF of the same build; the build is gone from the
// header's anchor address, so a profile is only meaningful with its own ELF.
//
// Ported in shape from KartPadNX switch/src/prof_switch.cpp, which also walks frame-pointer chains
// and records race boundaries; Melee's C is built without frame pointers, so PC and LR only.
#include "perf_switch.h"

#include <switch.h>

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" void pc_log_line(const char* fmt, ...);
extern "C" void melee_nx_prof_anchor(void);

namespace {

constexpr const char* kProfPath = "sdmc:/switch/melee-nx/prof.bin";
constexpr int kMaxTargets = 8;

enum Role : u32 { RoleGame = 0, RoleFifo = 1, RoleRender = 2, RoleCompile = 3 };

struct Record {
    u32 tMs;
    u32 role;
    u64 pc;
    u64 lr;
};
static_assert(sizeof(Record) == 24, "record layout is read by builder/prof-report.py");

struct Header {
    char magic[8]; // "MNXPROF1"
    u64 anchor;    // runtime address of melee_nx_prof_anchor
    u32 hz;
    u32 recordSize;
    char stamp[48];
};

struct Target {
    Handle handle;
    u32 role;
};
Target g_targets[kMaxTargets];
std::atomic<int> g_targetCount{0};
std::atomic<bool> g_targetBusy{false};

u32 g_seconds = 0, g_startSeconds = 60, g_hz = 750;
u64 g_initNs = 0;

u64 now_ns() { return armTicksToNs(armGetSystemTick()); }

void sample(const Target& target, u64 t0, std::vector<Record>& out) {
    if (R_FAILED(svcSetThreadActivity(target.handle, ThreadActivity_Paused))) {
        return;
    }
    ThreadContext ctx;
    const Result rc = svcGetThreadContext3(&ctx, target.handle);
    svcSetThreadActivity(target.handle, ThreadActivity_Runnable);
    if (R_SUCCEEDED(rc)) {
        // The vector's capacity was reserved for a full flush interval: no allocation here.
        out.push_back({static_cast<u32>((now_ns() - t0) / 1'000'000ull), target.role, ctx.pc.x, ctx.lr});
    }
}

bool flush(std::vector<Record>& records) {
    if (records.empty()) {
        return true;
    }
    FILE* file = std::fopen(kProfPath, "ab");
    if (file == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(records.data(), sizeof(Record), records.size(), file);
    const bool ok = written == records.size() && std::fclose(file) == 0;
    records.clear();
    return ok;
}

void* prof_main(void*) {
    // Above every game thread (the main thread runs at 0x28), so a sample is never delayed by one.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
    u64 allowed = 0;
    if (R_SUCCEEDED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        int top = -1;
        for (int core = 0; core < 64; ++core) {
            if (allowed & (1ull << core)) {
                top = core;
            }
        }
        if (top >= 0) {
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, top, 1u << top);
        }
    }
    const u64 startAt = g_initNs + static_cast<u64>(g_startSeconds) * 1'000'000'000ull;
    while (now_ns() < startAt) {
        svcSleepThread(250'000'000ll);
    }

    FILE* file = std::fopen(kProfPath, "wb");
    if (file == nullptr) {
        pc_log_line("[prof] cannot create %s", kProfPath);
        return nullptr;
    }
    Header header{};
    std::memcpy(header.magic, "MNXPROF1", 8);
    header.anchor = reinterpret_cast<u64>(&melee_nx_prof_anchor);
    header.hz = g_hz;
    header.recordSize = sizeof(Record);
    std::snprintf(header.stamp, sizeof(header.stamp), "melee-nx %s %s", __DATE__, __TIME__);
    const bool wrote = std::fwrite(&header, sizeof(header), 1, file) == 1;
    std::fclose(file);
    if (!wrote) {
        pc_log_line("[prof] cannot write %s", kProfPath);
        return nullptr;
    }

    const u64 periodNs = 1'000'000'000ull / g_hz;
    const u64 t0 = now_ns();
    const u64 endAt = t0 + static_cast<u64>(g_seconds) * 1'000'000'000ull;
    std::vector<Record> records;
    records.reserve(static_cast<size_t>(g_hz) * kMaxTargets * 2); // two seconds of every slot
    pc_log_line("[prof] capture %u s at %u Hz -> %s", g_seconds, g_hz, kProfPath);
    u64 next = t0 + periodNs, lastFlush = t0, total = 0;
    bool ok = true;
    while (ok && now_ns() < endAt) {
        const u64 now = now_ns();
        if (next > now) {
            svcSleepThread(static_cast<s64>(next - now));
        }
        next = now_ns() + periodNs; // never burst to catch up after an SD write
        const int count = g_targetCount.load(std::memory_order_acquire);
        const size_t before = records.size();
        for (int i = 0; i < count; ++i) {
            sample(g_targets[i], t0, records);
        }
        total += records.size() - before;
        if (now_ns() - lastFlush >= 1'000'000'000ull) {
            ok = flush(records);
            lastFlush = now_ns();
        }
    }
    if (ok) {
        ok = flush(records);
    }
    pc_log_line("[prof] %s: %llu samples", ok ? "saved" : "FILE ERROR", static_cast<unsigned long long>(total));
    return nullptr;
}

} // namespace

// Address anchor for symbolization: the report subtracts this symbol's ELF address from the recorded
// runtime address to get the NRO's load bias.
extern "C" __attribute__((noinline, used)) void melee_nx_prof_anchor(void) { __asm__ volatile(""); }

// Called on a thread once it is placed (melee_nx_perf_thread, the deko compile worker): it becomes a
// sampling target. Each role registers once.
extern "C" void melee_nx_prof_register(int role) {
    if (g_seconds == 0 || role < 0) {
        return;
    }
    while (g_targetBusy.exchange(true, std::memory_order_acquire)) {
    }
    const int count = g_targetCount.load(std::memory_order_relaxed);
    bool known = false;
    for (int i = 0; i < count; ++i) {
        known |= g_targets[i].role == static_cast<u32>(role);
    }
    if (!known && count < kMaxTargets) {
        g_targets[count] = {threadGetCurHandle(), static_cast<u32>(role)};
        g_targetCount.store(count + 1, std::memory_order_release);
    }
    g_targetBusy.store(false, std::memory_order_release);
}

// After perf.cfg is read, before any worker starts.
extern "C" void melee_nx_prof_init(void) {
    const int seconds = melee_nx_perf_int("prof_seconds", 0);
    if (seconds <= 0) {
        return;
    }
    const int start = melee_nx_perf_int("prof_start", 60);
    const int hz = melee_nx_perf_int("prof_hz", 750);
    g_seconds = static_cast<u32>(seconds);
    g_startSeconds = static_cast<u32>(start > 0 ? start : 0);
    g_hz = static_cast<u32>(hz <= 0 ? 750 : (hz > 4000 ? 4000 : hz));
    g_initNs = now_ns();
    pthread_t thread;
    if (pthread_create(&thread, nullptr, prof_main, nullptr) != 0) {
        pc_log_line("[prof] thread start failed");
        g_seconds = 0;
        return;
    }
    pthread_detach(thread);
    pc_log_line("[prof] armed: %u s at %u Hz starting %u s after launch", g_seconds, g_hz, g_startSeconds);
}
