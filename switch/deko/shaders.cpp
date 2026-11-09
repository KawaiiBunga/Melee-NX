// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The shader machinery follows SwitchWakerHD (https://github.com/centollOS/SwitchWakerHD) at df8fbde:
// runtime/src/gfx/deko/shaders_dk.cpp (one uam worker with a large stack at priority 0x3B, the queue
// order, the per-frame load budget, skipped draws until a shader is ready, the local cache appended
// as shaders compile, the failure dumps); the registry and the keys are this project's.
// The deko3d renderer's shaders at run time (dk.h):
//   - the registry: every DKSH by (stage, GLSL hash), filled from initial_dksh_cache.bin and
//     dksh_local.bin at start (shader_cache.cpp, shaders_init) and by the uam worker;
//   - the uam worker: one thread (uam is not thread-safe), 8 MiB of stack, priority 0x3B, off the
//     render worker's core (as Aurora's compile thread: COS_SWITCH_COMPILE_CORE); jobs come from
//     Aurora's compile thread (switch/deko/aurora: WGSL -> Tint -> post-pass), foreground (a draw
//     waits for it) before background (the warm-up), the stage that skipped most draws first;
//   - dksh_local.bin (native/user/cache/): every DKSH compiled here, appended as a record with its CRC
//     (dksh_file.h), read back at the next start, ignored as a whole after a compiler change;
//   - the load budget: the render worker copies at most COS_DK_SHADER_BUDGET compiled shaders a frame
//     into code memory (default 64; 0 = no limit); a draw whose shader is not loaded yet is skipped
//     (a brief pop-in) and counted per entry.
#include "dk.h"

#include <pthread.h>
#include <switch.h>
#include <sys/stat.h>

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <unordered_map>

#include "dksh_file.h"
#include "uam_api.h"
#include "cache_journal.h"

extern "C" void cos_switch_thread_role(int role);  // backend_present.cpp
extern "C" const char kartpad_build_stamp[] __attribute__((weak));
constexpr int kThreadRoleCompile = 5;              // COS_SWITCH_THREAD_COMPILE (cos_switch.h)

#ifndef COS_SWITCH_DATA_DIR
#error "COS_SWITCH_DATA_DIR: the port's directory on the SD card (switch/cmake/Deko3D.cmake)"
#endif

namespace swdk {
namespace {

constexpr const char* kLocalPath = COS_SWITCH_DATA_DIR "/Cache/deko/dksh_local.bin";
constexpr const char* kFailDir = COS_SWITCH_DATA_DIR "/Cache/deko";
constexpr size_t kWorkerStack = 8u << 20;  // Mesa's GLSL parser and nv50_ir recurse deeply (uam_api.h)
constexpr int kFailDumps = 16;

int stage_index(ShaderStage s) { return s == ShaderStage::Vertex ? 0 : 1; }

// ---- the registry
std::mutex g_mutex;
std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> g_entries[2];

// ---- the worker
struct Job {
    ShaderEntry* entry;
    std::string glsl;
};
std::mutex g_jobMutex;
std::condition_variable g_jobCv;   // a job queued
std::condition_variable g_doneCv;  // an entry left Queued
std::deque<Job> g_foreground, g_background;
bool g_workerUp = false;
std::atomic<Handle> g_workerHandle{INVALID_HANDLE};
uint64_t g_compilerId = 0;
std::mutex g_cacheMutex;
std::atomic<bool> g_cacheWritable{true};
std::string g_sessionPath;
// The journals' SD-card writes. Callers only queue them (g_cacheMutex); the uam worker writes them
// with no lock held, while no draw waits for a compile (flush_writes). A card write can stall for
// hundreds of ms, and every pipeline build takes g_cacheMutex for a cache hit (shader_note_stage).
struct PendingWrite {
    enum Kind { Local, Session, Usage, Source } kind;
    std::string path;
    std::vector<uint8_t> bytes;
};
std::vector<PendingWrite> g_pendingWrites;
// Stage records of cache hits, appended to the session journal by save_usage (g_cacheMutex). A
// hit's record is already in a cache file, so it must not cost the pipeline build SD-card I/O:
// three opens per stage serialized the warm boot preload on the card (108 s for 3550 pipelines).
std::vector<uint8_t> g_sessionStages;
std::unordered_map<ShaderEntry*, uint64_t> g_savedUses;
uint64_t g_lastUsageNs = 0;

void append_cache(const std::vector<uint8_t>& bytes) {
    std::lock_guard lock(g_cacheMutex);
    g_pendingWrites.push_back({PendingWrite::Local, kLocalPath, bytes});
    if (!g_sessionPath.empty()) g_pendingWrites.push_back({PendingWrite::Session, g_sessionPath+"/stages.bin", bytes});
}

// The uam worker only. Records keep their order within each file.
void flush_writes() {
    std::vector<PendingWrite> writes;
    {
        std::lock_guard lock(g_cacheMutex);
        writes.swap(g_pendingWrites);
    }
    bool sessionFailed = false;
    for (const PendingWrite& w : writes) {
        if ((w.kind == PendingWrite::Local && !g_cacheWritable) ||
            ((w.kind == PendingWrite::Session || w.kind == PendingWrite::Usage) && sessionFailed))
            continue;
        if (w.kind == PendingWrite::Source) {  // one file per generated module, kept across sessions
            if (FILE* existing = std::fopen(w.path.c_str(), "rb")) { std::fclose(existing); continue; }
        }
        if (journal::write(w.path, w.bytes, w.kind == PendingWrite::Source ? "wb" : "ab")) continue;
        switch (w.kind) {
        case PendingWrite::Local:
            g_cacheWritable = false;
            dklog("shaders: append failed; cache writes disabled until next launch: %s", kLocalPath);
            break;
        case PendingWrite::Session: {
            dklog("shaders: session journal write failed: %s", w.path.c_str());
            sessionFailed = true;
            std::lock_guard lock(g_cacheMutex);
            g_sessionPath.clear();
            break;
        }
        case PendingWrite::Usage: dklog("shaders: session usage write failed"); break;
        case PendingWrite::Source: dklog("shaders: source capture write failed: %s", w.path.c_str()); break;
        }
    }
}

void save_usage() {
    const auto now=now_ns();
    if (now-g_lastUsageNs < 5000000000ull) return;
    g_lastUsageNs=now;
    std::string rows;
    {
        std::lock_guard lock(g_mutex);
        for (auto& map:g_entries) for (auto& [hash,entry]:map) {
            auto* e=entry.get();
            const uint64_t count=e->usedDraws.load(std::memory_order_relaxed);
            if (count==0 || count==g_savedUses[e]) continue;
            char row[140];
            std::snprintf(row,sizeof row,"%llu,%u,%016llx,%llu\n",
                (unsigned long long)now,unsigned(e->stage),(unsigned long long)hash,(unsigned long long)count);
            rows+=row;g_savedUses[e]=count;
        }
    }
    std::lock_guard lock(g_cacheMutex);
    if (!g_sessionPath.empty()) {
        if (!rows.empty())
            g_pendingWrites.push_back({PendingWrite::Usage, g_sessionPath+"/usage.csv", {rows.begin(), rows.end()}});
        if (!g_sessionStages.empty())
            g_pendingWrites.push_back({PendingWrite::Session, g_sessionPath+"/stages.bin", std::move(g_sessionStages)});
    }
    g_sessionStages.clear();
}

// the session's figures (g_jobMutex)
std::atomic<uint64_t> g_loaded{0};
int g_failDumps = 0;

// ---- the load budget (render worker)
uint32_t g_budget = 0;
uint32_t g_loadsThisFrame = 0;

const char* stage_name(ShaderStage s) { return s == ShaderStage::Vertex ? "vertex" : "fragment"; }

void append_local(const ShaderEntry& e, const std::vector<uint8_t>& dksh) {
    // the header once (a file of another compiler is replaced at start), then the record
    std::vector<uint8_t> bytes;
    DkshShaderRecord r;
    r.stage = e.stage;
    r.glslHash = e.glslHash;
    r.dksh = dksh;
    append_record(&bytes, r);
    append_cache(bytes);
}

void dump_failure(const ShaderEntry& e, const std::string& glsl, const std::string& log) {
    if (g_failDumps >= kFailDumps) return;
    g_failDumps++;
    char path[256];
    snprintf(path, sizeof path, "%s/shaderfail_%016llx_%s.glsl", kFailDir, (unsigned long long)e.glslHash,
             e.stage == ShaderStage::Vertex ? "vs" : "fs");
    if (FILE* f = fopen(path, "wb")) {
        fwrite(glsl.data(), 1, glsl.size(), f);
        fprintf(f, "\n/* uam:\n%s\n*/\n", log.c_str());
        fclose(f);
    }
}

// the next job: foreground first, then the one whose draws were skipped most, then background
bool take_job(Job* out) {
    for (std::deque<Job>* q : {&g_foreground, &g_background}) {
        if (q->empty()) continue;
        auto best = q->begin();
        for (auto it = q->begin(); it != q->end(); ++it)
            if (it->entry->skippedDraws.load(std::memory_order_relaxed) >
                best->entry->skippedDraws.load(std::memory_order_relaxed))
                best = it;
        *out = std::move(*best);
        q->erase(best);
        return true;
    }
    return false;
}

void* worker_main(void*) {
    host_alloc_thread();
    cos_switch_thread_role(kThreadRoleCompile);
    svcSetThreadPriority(CUR_THREAD_HANDLE, kMaxThreadPriority);
    g_workerHandle.store(threadGetCurHandle(), std::memory_order_release);
    const uint64_t t0 = now_ns();
    uam::init(true);
    static const char kTest[] = "#version 460\nvoid main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    const uam::Result test = uam::compile(uam::Stage::Vertex, kTest);
    dklog("shaders: uam worker started (priority 0x%X, %zu MiB stack); self-test %s (%zu bytes) in %.1f ms",
          kMaxThreadPriority, kWorkerStack >> 20, test.ok ? "compiled" : "FAILED", test.dksh.size(),
          double(now_ns() - t0) / 1e6);
    for (;;) {
        save_usage();
        bool drawWaits;
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            drawWaits = !g_foreground.empty();
        }
        if (!drawWaits) flush_writes();
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_jobMutex);
            if (!g_jobCv.wait_for(lock, std::chrono::seconds(5), [] { return !g_foreground.empty() || !g_background.empty(); }))
                continue;
            take_job(&job);
        }
        ShaderEntry* e = job.entry;
        const uint64_t c0 = now_ns();
        uam::Result res = uam::compile(e->stage == ShaderStage::Vertex ? uam::Stage::Vertex : uam::Stage::Fragment,
                                       job.glsl.c_str());
        const uint64_t ns = now_ns() - c0;
        const bool ok = res.ok && !res.dksh.empty();
        if (ok) {
            append_local(*e, res.dksh);
        } else {
            dklog("shaders: uam refused %s shader %016llx (%.0f ms): %s", stage_name(e->stage),
                  (unsigned long long)e->glslHash, double(ns) / 1e6, res.log.substr(0, 400).c_str());
            dump_failure(*e, job.glsl, res.log);
        }
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            if (ok) e->dksh = std::move(res.dksh);
            e->state.store(ok ? ShaderState::Compiled : ShaderState::Failed, std::memory_order_release);
        }
        g_doneCv.notify_all();
    }
    return nullptr;
}

void start_worker() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kWorkerStack);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr, worker_main, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        dklog("shaders: cannot start the uam worker (%d): pipelines the caches lack will never draw", rc);
        return;
    }
    pthread_detach(thread);
    g_workerUp = true;
}

// Builds before 2026-10-25 appended every stage of every session to dksh_local.bin, cache hits
// included, so it grew by about 1 MiB a boot with duplicate stage records that every start reads
// and parses again. Rewrite it once with the last record of each name (the one
// shader_cache_add_names keeps). The temporary name is the one read_local completes after a crash.
void compact_local(const DkshFile& file, size_t fileBytes) {
    std::unordered_map<std::string, size_t> last;
    for (size_t i = 0; i < file.named.size(); ++i) last[file.named[i].name] = i;
    if (file.named.size() < last.size() + 4096) return;
    std::vector<uint8_t> bytes = dksh_file_header(file.compilerId);
    for (const DkshShaderRecord& r : file.shaders) append_record(&bytes, r);
    for (const DkshModuleRecord& m : file.modules) append_record(&bytes, m);
    for (size_t i = 0; i < file.named.size(); ++i)
        if (last[file.named[i].name] == i) append_stage_record(&bytes, file.named[i]);
    const std::string temp = std::string(kLocalPath) + ".recover.tmp";
    if (!journal::write(temp, bytes, "wb")) {
        std::remove(temp.c_str());
        dklog("shaders: %s compaction skipped (temporary write failed)", kLocalPath);
        return;
    }
    if (std::remove(kLocalPath) != 0) {
        std::remove(temp.c_str());
        dklog("shaders: %s compaction skipped (cannot replace the file)", kLocalPath);
        return;
    }
    if (std::rename(temp.c_str(), kLocalPath) != 0) {
        g_cacheWritable = false;
        dklog("shaders: %s compaction rename failed; the next start completes it", kLocalPath);
        return;
    }
    dklog("shaders: %s compacted: %zu of %zu stage records were duplicates, %zu -> %zu bytes", kLocalPath,
          file.named.size() - last.size(), file.named.size(), fileBytes, bytes.size());
}

// dksh_local.bin: its records into the registry (loaded into code memory as the initial cache's)
void read_local(uint32_t gxConfigVersion) {
    FILE* f = fopen(kLocalPath, "rb");
    // A process can stop between recovery's two renames. The complete prefix
    // was closed in this temporary file before the damaged original moved.
    if (!f) {
        const std::string pending=std::string(kLocalPath)+".recover.tmp";
        if (std::rename(pending.c_str(),kLocalPath)==0) f=fopen(kLocalPath,"rb");
    }
    const uint64_t expected = dksh_compiler_id(gxConfigVersion);
    if (f) {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
        const bool read = size > 0 && fread(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        DkshFile file;
        std::string error;
        if (read && read_dksh_file(data, &file, &error) && file.compilerId == expected) {
            if (file.validBytes != data.size()) {
                data.resize(file.validBytes);
                g_cacheWritable=journal::recover(kLocalPath,data,std::to_string(now_ns()));
                dklog("shaders: recovered complete cache prefix (%zu bytes); append %s",data.size(),g_cacheWritable?"enabled":"disabled (recovery failed)");
            }
            size_t loaded = 0, known = 0;
            for (const DkshShaderRecord& r : file.shaders) {
                if (r.dksh.empty()) continue;
                ShaderEntry* e = shader_entry(r.stage, r.glslHash);
                if (e->state.load() == ShaderState::Ready) {
                    known++;
                    continue;
                }
                if (code_load(e->shader, r.dksh.data(), uint32_t(r.dksh.size()), "local DKSH")) {
                    e->state.store(ShaderState::Ready);
                    loaded++;
                }
            }
            dklog("shaders: %s: %zu shaders compiled on this console loaded (%zu already in the initial cache, %zu "
                  "bad records)",
                  kLocalPath, loaded, known, file.badCrc);
            g_loaded += loaded;
            shader_cache_add_names(file);
            if (g_cacheWritable) compact_local(file, data.size());
            return;
        }
        dklog("shaders: %s %s: started again", kLocalPath,
              read && error.empty() ? "is from another compiler (uam, Tint options or GX config version)"
                                    : "is unreadable");
    }
    // a new file: the header alone
    const auto header=dksh_file_header(expected);
    // Preserve stale compiler caches as training evidence instead of overwriting them.
    g_cacheWritable = f ? journal::recover(kLocalPath,header,std::to_string(now_ns()))
                        : journal::write(kLocalPath,header,"wb");
    if (!g_cacheWritable) dklog("shaders: cannot initialize persistent native cache");
}

}  // namespace

void shader_note_stage(const std::string& key, ShaderStage stage, uint64_t hash,
                       const std::vector<SlotBinding>& slots) {
    DkshNamedRecord record{stage,key,hash,slots};
    std::lock_guard lock(g_cacheMutex);
    if (!g_sessionPath.empty()) append_stage_record(&g_sessionStages,record);
}

void shader_record_stage(const std::string& key, ShaderStage stage, uint64_t hash,
                         const std::vector<SlotBinding>& slots, const std::string& source) {
    std::vector<uint8_t> bytes;
    DkshNamedRecord record{stage,key,hash,slots};
    append_stage_record(&bytes,record);
    append_cache(bytes);
    char path[256];
    std::snprintf(path,sizeof path,"%s/sources/%016llx.wgsl",kFailDir,(unsigned long long)glsl_hash(source));
    std::lock_guard lock(g_cacheMutex);
    g_pendingWrites.push_back({PendingWrite::Source, path, {source.begin(), source.end()}});
}

ShaderEntry* shader_entry(ShaderStage stage, uint64_t glslHash) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& slot = g_entries[stage_index(stage)][glslHash];
    if (!slot) {
        slot = std::make_unique<ShaderEntry>();
        slot->stage = stage;
        slot->glslHash = glslHash;
    }
    return slot.get();
}

bool shader_compile(ShaderEntry* e, const std::string& glsl, bool foreground) {
    if (!g_workerUp) return false;
    std::lock_guard<std::mutex> lock(g_jobMutex);
    ShaderState expected = ShaderState::Missing;
    if (!e->state.compare_exchange_strong(expected, ShaderState::Queued)) {
        // already queued as background: a foreground request moves it up
        if (foreground && expected == ShaderState::Queued) {
            for (auto it = g_background.begin(); it != g_background.end(); ++it) {
                if (it->entry == e) {
                    g_foreground.push_back(std::move(*it));
                    g_background.erase(it);
                    break;
                }
            }
        }
        return false;
    }
    (foreground ? g_foreground : g_background).push_back({e, glsl});
    g_jobCv.notify_one();
    return true;
}

bool shader_wait(ShaderEntry* e) {
    std::unique_lock<std::mutex> lock(g_jobMutex);
    g_doneCv.wait(lock, [e] { return e->state.load(std::memory_order_acquire) != ShaderState::Queued; });
    return e->state.load(std::memory_order_acquire) != ShaderState::Failed;
}

bool shader_usable(ShaderEntry* e) {
    const ShaderState s = e->state.load(std::memory_order_acquire);
    if (s == ShaderState::Ready) return true;
    if (s == ShaderState::Compiled && (g_budget == 0 || g_loadsThisFrame < g_budget)) {
        std::vector<uint8_t> dksh;
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            dksh.swap(e->dksh);
        }
        g_loadsThisFrame++;
        char name[48];
        snprintf(name, sizeof name, "%s %016llx", stage_name(e->stage), (unsigned long long)e->glslHash);
        if (!dksh.empty() && code_load(e->shader, dksh.data(), uint32_t(dksh.size()), name)) {
            e->state.store(ShaderState::Ready, std::memory_order_release);
            g_loaded++;
            return true;
        }
        e->state.store(ShaderState::Failed, std::memory_order_release);
        return false;
    }
    e->skippedDraws.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void shaders_frame_start() { g_loadsThisFrame = 0; }

void shader_compile_boost(bool on) {
    const Handle worker = g_workerHandle.load(std::memory_order_acquire);
    if (worker == INVALID_HANDLE) return;
    const Result rc = svcSetThreadPriority(worker, on ? kBoostThreadPriority : kMaxThreadPriority);
    dklog("shaders: uam worker priority 0x%X (%s)%s", on ? kBoostThreadPriority : kMaxThreadPriority,
          on ? "boot preload" : "background", R_FAILED(rc) ? " FAILED" : "");
}

void shaders_init(uint32_t gxConfigVersion) {
    g_compilerId = dksh_compiler_id(gxConfigVersion);
    mkdir(COS_SWITCH_DATA_DIR "/Cache", 0777);
    mkdir(COS_SWITCH_DATA_DIR "/Cache/deko", 0777);
    g_budget = uint32_t(std::max(0L, env_long("COS_DK_SHADER_BUDGET", 0)));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint64_t ready = 0;
        for (auto& map : g_entries)
            for (auto& [hash, e] : map) ready += e->state.load() == ShaderState::Ready;
        g_loaded = ready;
    }
    read_local(gxConfigVersion);
    mkdir(COS_SWITCH_DATA_DIR "/Cache/deko/sessions",0777);
    mkdir(COS_SWITCH_DATA_DIR "/Cache/deko/sources",0777);
    g_sessionPath=std::string(kFailDir)+"/sessions/"+std::to_string(std::time(nullptr))+"-"+std::to_string(now_ns());
    if (mkdir(g_sessionPath.c_str(),0777)!=0 ||
        !journal::write(g_sessionPath+"/stages.bin",dksh_file_header(g_compilerId),"wb")) {
        dklog("shaders: cannot create session journal");g_sessionPath.clear();
    } else {
        const std::string csv="time_ns,stage,glsl_hash,draws_total\n";
        journal::write(g_sessionPath+"/usage.csv",{csv.begin(),csv.end()},"wb");
        char info[384];
        std::snprintf(info,sizeof info,"{\"build_stamp\":\"%s\",\"compiler_id\":\"%016llx\",\"loaded_stages\":%llu,\"usage_interval_seconds\":5}\n",
            kartpad_build_stamp?kartpad_build_stamp:"unknown",(unsigned long long)g_compilerId,(unsigned long long)g_loaded.load());
        const std::string metadata=info;
        if (!journal::write(g_sessionPath+"/session.json",{metadata.begin(),metadata.end()},"wb"))
            dklog("shaders: session identity write failed");
        dklog("shader training: session=%s; new stage records saved immediately, cached ones and use counts every 5s",g_sessionPath.c_str());
    }
    translate_init();
    start_worker();
    dklog("shaders: %llu loaded; misses compiled by the uam worker and appended to %s; load budget %u per frame "
          "(COS_DK_SHADER_BUDGET, 0 = no limit); a draw whose shader is not ready is skipped",
          (unsigned long long)g_loaded.load(), kLocalPath, g_budget);
    sched_report();
}

}  // namespace swdk
