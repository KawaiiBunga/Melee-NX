// SPDX-License-Identifier: GPL-3.0-or-later
// Rollback snapshots and the determinism self-test. See rollback_switch.h and docs/SLIPPI.md.
//
// What a snapshot covers, following Melee Unlocked's native_savestate (MEM1 plus the game image's
// writable sections), minus what is not simulation state:
//   * MEM1, Aurora's emulated 96 MB block, but only the bytes in use: each heap from its start to
//     its high-water mark (the highest allocation end seen, from the allocator wrappers below), with
//     heaps nested in it counted by their own use; plus every MEM1 byte outside all heaps. Aurora's
//     heap table lives at the start of the arena, inside MEM1, so allocator state is restored too.
//   * The melee_game library's .data/.bss, renamed mgdata/mgbss by switch/CMakeLists.txt.
//   * Never: the audio heap and its heap-table entry (the 5 ms synth callback writes them on the
//     audio thread), melee_game_host's objects (audio driver, render, memory card, debug), ARAM.
// Copies run under melee-pc's interrupt lock, which the audio thread holds while the synth
// callback runs, and are split across worker threads.
//
// The self-test, every `rollback_selftest` frames: capture at a body start, run
// `rollback_selftest_depth` bodies recording the pads each one renewed, note Slippi's gameplay
// checksum and the page hashes, restore, re-run the same bodies with the recorded pads inside the
// same host frame, and compare. Gameplay equality is the pass/fail (render-time state written
// between frames makes whole-memory equality impossible; it is logged as a diagnostic).
#include "rollback_switch.h"

#include "perf_switch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern void* MEM1Start;  // Aurora, lib/dolphin/os/OSMemory.cpp
extern void* MEM1End;
extern "C" {
extern char __start_mgdata[] __attribute__((weak));
extern char __stop_mgdata[] __attribute__((weak));
extern char __start_mgbss[] __attribute__((weak));
extern char __stop_mgbss[] __attribute__((weak));
int OSDisableInterrupts(void);
int OSRestoreInterrupts(int level);
void pc_os_alarms_hold(void);  // src/pc/os.c (melee-switch-slippi-hooks.patch)
void pc_os_alarms_release(void);
extern int HSD_Synth_804D6018 __attribute__((weak));
void* __real_OSInitAlloc(void* start, void* end, int maxHeaps);
int __real_OSCreateHeap(void* start, void* end);
void __real_OSDestroyHeap(int heap);
void* __real_OSAllocFromHeap(int heap, std::uint32_t size);
}

namespace {

constexpr size_t kPage = 4096;
constexpr size_t kHeapDescBytes = 24;  // Aurora's HeapDesc: s32 size, Cell* freeList, Cell* allocated
constexpr size_t kAllocSlack = 128;    // header, alignment, canary and the next cell's header

using Byte = std::uint8_t;

struct Span {
    Byte* lo;
    Byte* hi;
};

// ---- heap tracking (allocator wrappers) -------------------------------------------------------

struct Heap {
    int handle = -1;
    Byte* start = nullptr;
    Byte* end = nullptr;
    std::atomic<Byte*> high{nullptr};
    bool live = false;
};
Heap g_heaps[32];
std::mutex g_heapMutex;  // create/destroy only; the high-water mark is atomic
Byte* g_heapTable = nullptr;
int g_heapTableCount = 0;

Heap* FindHeap(int handle) {
    for (Heap& h : g_heaps) {
        if (h.live && h.handle == handle) return &h;
    }
    return nullptr;
}

// ---- span arithmetic --------------------------------------------------------------------------

void Normalize(std::vector<Span>& v) {
    std::sort(v.begin(), v.end(), [](const Span& a, const Span& b) { return a.lo < b.lo; });
    std::vector<Span> out;
    for (const Span& s : v) {
        if (s.hi <= s.lo) continue;
        if (!out.empty() && s.lo <= out.back().hi)
            out.back().hi = std::max(out.back().hi, s.hi);
        else
            out.push_back(s);
    }
    v.swap(out);
}

void Subtract(std::vector<Span>& v, Span cut) {
    std::vector<Span> out;
    for (const Span& s : v) {
        if (cut.hi <= s.lo || cut.lo >= s.hi) {
            out.push_back(s);
            continue;
        }
        if (s.lo < cut.lo) out.push_back({s.lo, cut.lo});
        if (cut.hi < s.hi) out.push_back({cut.hi, s.hi});
    }
    v.swap(out);
}

// ---- worker pool for copies -------------------------------------------------------------------

class Pool {
public:
    void Run(const std::vector<Span>& spans, Byte* buffer, bool toBuffer) {
        if (workers_.empty()) Start();
        // Split the work into equal byte shares, one per worker plus the calling thread.
        size_t total = 0;
        for (const Span& s : spans) total += static_cast<size_t>(s.hi - s.lo);
        const size_t parts = workers_.size() + 1;
        jobs_.assign(parts, {});
        const size_t share = (total + parts - 1) / parts;
        size_t part = 0, filled = 0, offset = 0;
        for (const Span& s : spans) {
            Byte* p = s.lo;
            while (p < s.hi) {
                const size_t take = std::min(static_cast<size_t>(s.hi - p), share - filled);
                jobs_[part].push_back({p, buffer + offset, take});
                p += take;
                offset += take;
                filled += take;
                if (filled == share && part + 1 < parts) {
                    ++part;
                    filled = 0;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            toBuffer_ = toBuffer;
            left_ = static_cast<int>(workers_.size());
            ++generation_;
        }
        cv_.notify_all();
        Do(jobs_.back());
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [this] { return left_ == 0; });
    }

private:
    struct Job {
        Byte* live;
        Byte* saved;
        size_t size;
    };
    void Do(const std::vector<Job>& jobs) {
        for (const Job& j : jobs) {
            if (toBuffer_)
                std::memcpy(j.saved, j.live, j.size);
            else
                std::memcpy(j.live, j.saved, j.size);
        }
    }
    void Start() {
        for (int i = 0; i < 2; ++i) {
            workers_.emplace_back([this, i] {
                std::uint64_t seen = 0;
                for (;;) {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait(lk, [&] { return generation_ != seen; });
                    seen = generation_;
                    lk.unlock();
                    Do(jobs_[static_cast<size_t>(i)]);
                    lk.lock();
                    if (--left_ == 0) done_.notify_one();
                }
            });
            workers_.back().detach();
        }
    }
    std::vector<std::thread> workers_;
    std::vector<std::vector<Job>> jobs_;
    std::mutex mu_;
    std::condition_variable cv_, done_;
    std::uint64_t generation_ = 0;
    int left_ = 0;
    bool toBuffer_ = true;
};

// ---- snapshot ---------------------------------------------------------------------------------

struct Snapshot {
    std::vector<Span> spans;
    std::vector<Byte> bytes;
    size_t size = 0;
};

Pool g_pool;

std::vector<Span> SnapshotSpans(size_t* audioOut = nullptr) {
    std::vector<Span> spans;
    auto* mem1 = static_cast<Byte*>(MEM1Start);
    auto* mem1End = static_cast<Byte*>(MEM1End);
    if (mem1 && mem1End > mem1) {
        std::lock_guard<std::mutex> lk(g_heapMutex);
        spans.push_back({mem1, mem1End});
        for (const Heap& h : g_heaps) {
            if (h.live) Subtract(spans, {h.start, h.end});
        }
        for (const Heap& h : g_heaps) {
            if (!h.live) continue;
            std::vector<Span> used{{h.start, std::min(h.end, h.high.load(std::memory_order_relaxed) + kAllocSlack)}};
            for (const Heap& c : g_heaps) {
                if (&c != &h && c.live && c.start >= h.start && c.end <= h.end) Subtract(used, {c.start, c.end});
            }
            spans.insert(spans.end(), used.begin(), used.end());
        }
        Normalize(spans);
        if (&HSD_Synth_804D6018) {
            for (const Heap& h : g_heaps) {
                if (h.live && h.handle == HSD_Synth_804D6018) {
                    Subtract(spans, {h.start, h.end});
                    if (audioOut) *audioOut = static_cast<size_t>(h.end - h.start);
                }
            }
            if (g_heapTable && HSD_Synth_804D6018 >= 0 && HSD_Synth_804D6018 < g_heapTableCount) {
                Byte* desc = g_heapTable + static_cast<size_t>(HSD_Synth_804D6018) * kHeapDescBytes;
                Subtract(spans, {desc, desc + kHeapDescBytes});
            }
        }
    }
    if (+__start_mgdata && +__stop_mgdata > +__start_mgdata)
        spans.push_back({reinterpret_cast<Byte*>(__start_mgdata), reinterpret_cast<Byte*>(__stop_mgdata)});
    if (+__start_mgbss && +__stop_mgbss > +__start_mgbss)
        spans.push_back({reinterpret_cast<Byte*>(__start_mgbss), reinterpret_cast<Byte*>(__stop_mgbss)});
    Normalize(spans);
    return spans;
}

void Capture(Snapshot& s) {
    s.spans = SnapshotSpans();
    s.size = 0;
    for (const Span& sp : s.spans) s.size += static_cast<size_t>(sp.hi - sp.lo);
    if (s.bytes.size() < s.size) s.bytes.resize(s.size + (s.size >> 3));  // headroom for growth
    const int level = OSDisableInterrupts();
    g_pool.Run(s.spans, s.bytes.data(), true);
    OSRestoreInterrupts(level);
}

void Restore(Snapshot& s) {
    const int level = OSDisableInterrupts();
    // Alarms are timers, not game state: they keep their schedule through the load, or the pad
    // alarm would fire again for every frame the snapshot went back.
    pc_os_alarms_hold();
    g_pool.Run(s.spans, s.bytes.data(), false);
    pc_os_alarms_release();
    OSRestoreInterrupts(level);
}

// ---- self-test --------------------------------------------------------------------------------

enum class Mode { Off, Idle, Recording, Resim };

struct State {
    bool initialised = false;
    Mode mode = Mode::Off;
    int interval = 0;
    int depth = 7;
    unsigned frame = 0;
    unsigned nextTest = 0;
    int step = 0;
    Snapshot snap;
    std::vector<std::uint64_t> refHashes;
    std::vector<std::vector<Byte>> pads;
    double captureMs = 0, restoreMs = 0;
    std::chrono::steady_clock::time_point resimStart;
    unsigned tests = 0, identical = 0, gameplayPasses = 0;
    unsigned refChecksum = 0;
};

State g;

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

std::uint64_t HashBytes(const Byte* p, size_t n) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

void HashPages(const std::vector<Span>& spans, std::vector<std::uint64_t>& out) {
    out.clear();
    for (const Span& s : spans) {
        for (Byte* p = s.lo; p < s.hi; p += kPage) out.push_back(HashBytes(p, std::min(kPage, static_cast<size_t>(s.hi - p))));
    }
}

// "mem1+0x..", "mgdata+0x..", "mgbss+0x.." for a live address.
std::string Name(const Byte* p) {
    char buf[48];
    auto* mem1 = static_cast<Byte*>(MEM1Start);
    if (p >= mem1 && p < static_cast<Byte*>(MEM1End))
        std::snprintf(buf, sizeof buf, "mem1+0x%zx", static_cast<size_t>(p - mem1));
    else if (+__start_mgdata && p >= reinterpret_cast<Byte*>(__start_mgdata) && p < reinterpret_cast<Byte*>(__stop_mgdata))
        std::snprintf(buf, sizeof buf, "mgdata+0x%zx", static_cast<size_t>(p - reinterpret_cast<Byte*>(__start_mgdata)));
    else
        std::snprintf(buf, sizeof buf, "mgbss+0x%zx", static_cast<size_t>(p - reinterpret_cast<Byte*>(__start_mgbss)));
    return buf;
}

void Init() {
    g.initialised = true;
    g.interval = melee_nx_perf_int("rollback_selftest", 0);
    g.depth = std::clamp(melee_nx_perf_int("rollback_selftest_depth", 7), 1, 30);
    if (g.interval <= 0) return;
    g.interval = std::max(g.interval, g.depth + 2);
    if (!+__start_mgdata || !+__start_mgbss || !MEM1Start) {
        std::printf("[rollback] snapshot regions missing; self-test off\n");
        return;
    }
    size_t audio = 0;
    const std::vector<Span> spans = SnapshotSpans(&audio);
    size_t total = 0;
    for (const Span& s : spans) total += static_cast<size_t>(s.hi - s.lo);
    int heaps = 0;
    for (const Heap& h : g_heaps) heaps += h.live ? 1 : 0;
    std::printf("[rollback] self-test every %d frames, depth %d; snapshot %zu KB in %zu spans now (%d heaps tracked, "
                "audio heap %zu KB %s)\n",
                g.interval, g.depth, total >> 10, spans.size(), heaps, audio >> 10, audio ? "excluded" : "NOT FOUND");
    g.pads.resize(static_cast<size_t>(g.depth));
    g.nextTest = static_cast<unsigned>(g.interval);
    g.mode = Mode::Idle;
}

void Report(unsigned checksum) {
    std::vector<std::uint64_t> live;
    const auto t0 = std::chrono::steady_clock::now();
    HashPages(g.snap.spans, live);
    const double hashMs = Ms(t0, std::chrono::steady_clock::now());
    const double resimMs = Ms(g.resimStart, t0);
    ++g.tests;
    size_t differing = 0, shown = 0, page = 0;
    std::string ranges;
    for (const Span& s : g.snap.spans) {
        for (Byte* p = s.lo; p < s.hi; p += kPage, ++page) {
            if (page >= live.size() || page >= g.refHashes.size() || live[page] == g.refHashes[page]) continue;
            ++differing;
            if (shown++ < 8) ranges += " " + Name(p);
        }
    }
    if (differing == 0) ++g.identical;
    const bool gameplay = checksum == g.refChecksum;
    if (gameplay) ++g.gameplayPasses;
    std::printf("[rollback] self-test %u at frame %u: gameplay %s (%08x/%08x, %u/%u passed); snapshot %zu KB in %zu spans, "
                "capture %.2f ms, restore %.2f ms, resim %d frames %.2f ms, hash %.2f ms; memory %s%s%s\n",
                g.tests, g.frame, gameplay ? "match" : "MISMATCH", g.refChecksum, checksum, g.gameplayPasses, g.tests,
                g.snap.size >> 10, g.snap.spans.size(), g.captureMs, g.restoreMs, g.depth, resimMs, hashMs,
                differing ? std::to_string(differing).c_str() : "identical", differing ? " pages differ:" : "",
                ranges.c_str());
}

}  // namespace

// ---- Slippi savestate slots -------------------------------------------------------------------

namespace {
std::map<std::int32_t, std::unique_ptr<Snapshot>> g_activeSlots;
std::vector<std::unique_ptr<Snapshot>> g_freeSlots;
}  // namespace

void melee_nx::rollback::SlotsReset(int count) {
    for (auto& kv : g_activeSlots) g_freeSlots.push_back(std::move(kv.second));
    g_activeSlots.clear();
    while (static_cast<int>(g_freeSlots.size()) < count) g_freeSlots.push_back(std::make_unique<Snapshot>());
    while (static_cast<int>(g_freeSlots.size()) > count) g_freeSlots.pop_back();
}

bool melee_nx::rollback::SlotCapture(std::int32_t frame) {
    std::unique_ptr<Snapshot> s;
    if (auto it = g_activeSlots.find(frame); it != g_activeSlots.end()) {
        s = std::move(it->second);
        g_activeSlots.erase(it);
    } else if (!g_freeSlots.empty()) {
        s = std::move(g_freeSlots.back());
        g_freeSlots.pop_back();
    } else if (!g_activeSlots.empty()) {
        s = std::move(g_activeSlots.begin()->second);
        g_activeSlots.erase(g_activeSlots.begin());
    } else {
        return false;
    }
    Capture(*s);
    g_activeSlots[frame] = std::move(s);
    return true;
}

bool melee_nx::rollback::SlotLoad(std::int32_t frame) {
    auto it = g_activeSlots.find(frame);
    if (it == g_activeSlots.end()) return false;
    Restore(*it->second);
    for (auto& kv : g_activeSlots) g_freeSlots.push_back(std::move(kv.second));
    g_activeSlots.clear();
    return true;
}

// ---- allocator wrappers (--wrap in switch/CMakeLists.txt) ---------------------------------------

extern "C" void* __wrap_OSInitAlloc(void* start, void* end, int maxHeaps) {
    void* rest = __real_OSInitAlloc(start, end, maxHeaps);
    if (rest) {
        g_heapTable = static_cast<Byte*>(start);
        g_heapTableCount = maxHeaps;
        std::lock_guard<std::mutex> lk(g_heapMutex);
        for (Heap& h : g_heaps) h.live = false;
    }
    return rest;
}

extern "C" int __wrap_OSCreateHeap(void* start, void* end) {
    const int handle = __real_OSCreateHeap(start, end);
    if (handle < 0) return handle;
    std::lock_guard<std::mutex> lk(g_heapMutex);
    for (Heap& h : g_heaps) {
        if (h.live && h.handle == handle) h.live = false;
    }
    for (Heap& h : g_heaps) {
        if (h.live) continue;
        h.handle = handle;
        h.start = static_cast<Byte*>(start);
        h.end = static_cast<Byte*>(end);
        h.high.store(h.start, std::memory_order_relaxed);
        h.live = true;
        break;
    }
    return handle;
}

extern "C" void __wrap_OSDestroyHeap(int heap) {
    __real_OSDestroyHeap(heap);
    std::lock_guard<std::mutex> lk(g_heapMutex);
    for (Heap& h : g_heaps) {
        if (h.live && h.handle == heap) h.live = false;
    }
}

extern "C" void* __wrap_OSAllocFromHeap(int heap, std::uint32_t size) {
    void* p = __real_OSAllocFromHeap(heap, size);
    if (p) {
        if (Heap* h = FindHeap(heap)) {
            Byte* end = static_cast<Byte*>(p) + size;
            Byte* cur = h->high.load(std::memory_order_relaxed);
            while (end > cur && !h->high.compare_exchange_weak(cur, end, std::memory_order_relaxed)) {
            }
        }
    }
    return p;
}

// ---- engine-loop hooks ------------------------------------------------------------------------

extern "C" int melee_nx_rb_body_begin(int discIdle) {
    if (!g.initialised) Init();
    switch (g.mode) {
        case Mode::Off:
            return 0;
        case Mode::Idle: {
            ++g.frame;
            if (g.frame < g.nextTest || !discIdle) return 0;
            g.nextTest = g.frame + static_cast<unsigned>(g.interval);
            const auto t0 = std::chrono::steady_clock::now();
            Capture(g.snap);
            g.captureMs = Ms(t0, std::chrono::steady_clock::now());
            g.step = 0;
            g.mode = Mode::Recording;
            return 0;
        }
        case Mode::Recording:
            ++g.frame;
            return 0;
        case Mode::Resim:
            return 1;
    }
    return 0;
}

extern "C" void melee_nx_rb_pads(void* lib, unsigned libSize, void* master, unsigned masterSize, int replay) {
    if (g.mode != Mode::Recording && g.mode != Mode::Resim) return;
    if (g.step < 0 || g.step >= g.depth) return;
    auto& blob = g.pads[static_cast<size_t>(g.step)];
    if (!replay) {
        blob.resize(libSize + masterSize);
        std::memcpy(blob.data(), lib, libSize);
        std::memcpy(blob.data() + libSize, master, masterSize);
    } else if (blob.size() == static_cast<size_t>(libSize) + masterSize) {
        std::memcpy(lib, blob.data(), libSize);
        std::memcpy(master, blob.data() + libSize, masterSize);
    }
}

extern "C" int melee_nx_rb_body_end(int discIdle, unsigned checksum) {
    if (g.mode == Mode::Recording) {
        if (!discIdle) {  // a disc load or a scene change inside the window: never restore across it
            std::printf("[rollback] self-test at frame %u skipped: disc load or scene change\n", g.frame);
            g.mode = Mode::Idle;
            return 0;
        }
        if (++g.step < g.depth) return 0;
        g.refChecksum = checksum;
        HashPages(g.snap.spans, g.refHashes);
        const auto t0 = std::chrono::steady_clock::now();
        Restore(g.snap);
        g.restoreMs = Ms(t0, std::chrono::steady_clock::now());
        g.step = 0;
        g.mode = Mode::Resim;
        g.resimStart = std::chrono::steady_clock::now();
        return 1;
    }
    if (g.mode == Mode::Resim) {
        if (++g.step < g.depth) return 1;
        Report(checksum);
        g.mode = Mode::Idle;
        return 0;
    }
    return 0;
}
