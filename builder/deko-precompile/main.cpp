// deko-precompile: compile every recorded GX pipeline's shaders for the deko3d renderer on the PC, so the
// console finds each one in initial_dksh_cache.bin instead of translating and compiling it while the game
// waits for the draw (docs/DEKO3D.md, "Seeds").
//
//   deko-precompile OUT.bin [DB...] [--uber] [--shard I/N] [--limit N]   (--uber: also the uber shader)
//
// DB is an Aurora pipeline recipe database (ref/melee-pc/resources/initial_pipeline_cache.db, or a
// console's pipeline_cache_deko.db). For every GX row of this Aurora's config version it does what the
// console does at pipeline creation: Aurora's own generator makes the WGSL (built from the staged deko
// copy, so the text and therefore the cache key are the console's), the stage key is backend_pipelines.cpp's
// (XXH3 of the WGSL, stage, entry point), Tint translates it with shader_translate.cpp and the vendored
// uam compiles the GLSL. The output carries the console's compiler id. Shards split the rows between
// processes; merge their outputs with `builder/deko-training.py pack-dksh`.
#include "dksh_file.h"
#include "shader_translate.h"
#include "uam_api.h"

#include "gx/gx.hpp"
#include "gx/pipeline.hpp"

#include <sqlite3.h>
#include <xxhash.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The WGSL Aurora handed to wgpuDeviceCreateShaderModule (host_stubs.cpp): how the uber shader's text,
// which Aurora builds only through build_uber_shader, is read.
extern std::string g_capturedWgsl;

namespace {

using swdk::ShaderStage;

struct Options {
    std::string out;
    std::vector<std::string> dbs;
    unsigned shard = 0, shards = 1;
    long limit = -1;
    bool uber = false;
};

bool parse(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shard" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%u/%u", &o->shard, &o->shards) != 2 || o->shards == 0 ||
                o->shard >= o->shards)
                return false;
        } else if (a == "--limit" && i + 1 < argc) {
            o->limit = std::atol(argv[++i]);
        } else if (a == "--uber") {
            o->uber = true;
        } else if (o->out.empty()) {
            o->out = a;
        } else {
            o->dbs.push_back(a);
        }
    }
    return !o->out.empty() && (!o->dbs.empty() || o->uber);
}

struct Totals {
    long rows = 0, stages = 0, translated = 0, compiled = 0, tintFailed = 0, uamFailed = 0, duplicates = 0;
};

class Precompiler {
public:
    void recipe(const aurora::gx::PipelineConfig& config) {
        if (aurora::gx::is_uber_config(config)) {
            uber();
            return;
        }
        const std::string wgsl = aurora::gx::build_shader_source(config.shaderConfig);
        stage(wgsl, "vs_main", ShaderStage::Vertex);
        stage(wgsl, "fs_main", ShaderStage::Fragment);
    }

    // The one shader perf.cfg `uber 1` draws with while a draw's own shader is not ready.
    void uber() {
        g_capturedWgsl.clear();
        (void)aurora::gx::build_uber_shader();
        stage(g_capturedWgsl, "vs_main", ShaderStage::Vertex);
        stage(g_capturedWgsl, "fs_main", ShaderStage::Fragment);
    }

    bool write(const std::string& path) const {
        std::vector<uint8_t> bytes = swdk::dksh_file_header(swdk::dksh_compiler_id(aurora::gx::GXPipelineConfigVersion));
        for (const auto& [id, dksh] : shaders_) {
            swdk::DkshShaderRecord r;
            r.stage = id.first;
            r.glslHash = id.second;
            r.dksh = dksh;
            swdk::append_record(&bytes, r);
        }
        for (const auto& [key, record] : stages_)
            swdk::append_stage_record(&bytes, record);
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        return std::fclose(f) == 0 && ok;
    }

    Totals totals;

private:
    void stage(const std::string& wgsl, const char* entry, ShaderStage stage) {
        // backend_pipelines.cpp kd::compile: the console looks the stage up by exactly this name.
        const std::string key = std::to_string(XXH3_64bits(wgsl.data(), wgsl.size())) + ":" +
                                std::to_string(static_cast<unsigned>(stage)) + ":" + entry;
        ++totals.stages;
        if (!seen_.insert(key).second) {
            ++totals.duplicates;
            return;
        }
        auto r = swdk::wgsl_to_glsl(wgsl, entry, stage, swdk::BindingScheme::Sequential);
        if (!r.ok) {
            if (totals.tintFailed++ < 8)
                std::fprintf(stderr, "Tint refused %s %s: %s\n", key.c_str(), entry, r.error.c_str());
            return;
        }
        ++totals.translated;
        const uint64_t glslHash = swdk::glsl_hash(r.glsl);
        const std::pair<ShaderStage, uint64_t> id{stage, glslHash};
        if (!shaders_.count(id) && !refused_.count(id)) {
            const uam::Result res =
                uam::compile(stage == ShaderStage::Vertex ? uam::Stage::Vertex : uam::Stage::Fragment, r.glsl.c_str());
            if (!res.ok || res.dksh.empty()) {
                refused_.insert(id);
                if (totals.uamFailed++ < 8)
                    std::fprintf(stderr, "uam refused %s: %s\n", key.c_str(), res.log.substr(0, 300).c_str());
                return;
            }
            ++totals.compiled;
            shaders_.emplace(id, res.dksh);
        } else if (refused_.count(id)) {
            return;
        }
        stages_[key] = swdk::DkshNamedRecord{stage, key, glslHash, r.bindings};
    }

    std::set<std::string> seen_;
    std::map<std::pair<ShaderStage, uint64_t>, std::vector<uint8_t>> shaders_;
    std::set<std::pair<ShaderStage, uint64_t>> refused_;
    std::map<std::string, swdk::DkshNamedRecord> stages_;
};

// Rows of the current GX config version, in first-use order (pipeline_cache.cpp loads them so).
bool read_db(const std::string& path, const Options& o, Precompiler* p, long* processed) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "%s: cannot open: %s\n", path.c_str(), sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt* q = nullptr;
    const char* sql = "SELECT config FROM pipeline_cache WHERE type = 1 AND config_version = ? AND config_size = ? "
                      "ORDER BY first_frame_used";
    if (sqlite3_prepare_v2(db, sql, -1, &q, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "%s: not a pipeline recipe database: %s\n", path.c_str(), sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int(q, 1, static_cast<int>(aurora::gx::GXPipelineConfigVersion));
    sqlite3_bind_int(q, 2, static_cast<int>(sizeof(aurora::gx::PipelineConfig)));
    long row = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (o.limit >= 0 && *processed >= o.limit)
            break;
        if (row++ % o.shards != o.shard)
            continue;
        if (sqlite3_column_bytes(q, 0) != static_cast<int>(sizeof(aurora::gx::PipelineConfig)))
            continue;
        aurora::gx::PipelineConfig config;
        std::memcpy(&config, sqlite3_column_blob(q, 0), sizeof(config));
        p->recipe(config);
        ++p->totals.rows;
        ++*processed;
    }
    sqlite3_finalize(q);
    sqlite3_close(db);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, &o)) {
        std::fprintf(stderr, "usage: deko-precompile OUT.bin [DB...] [--uber] [--shard I/N] [--limit N]\n");
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    swdk::translate_init();
    uam::init(true);
    Precompiler p;
    if (o.uber)
        p.uber();
    long processed = 0;
    for (const auto& db : o.dbs)
        if (!read_db(db, o, &p, &processed))
            return 1;
    if (!p.write(o.out)) {
        std::fprintf(stderr, "%s: cannot write\n", o.out.c_str());
        return 1;
    }
    const auto& t = p.totals;
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("{\"recipes\": %ld, \"stages\": %ld, \"duplicate_stages\": %ld, \"translated\": %ld, "
                "\"compiled\": %ld, \"tint_failed\": %ld, \"uam_failed\": %ld, \"shard\": \"%u/%u\", "
                "\"compiler_id\": \"%016llx\", \"seconds\": %.1f}\n",
                t.rows, t.stages, t.duplicates, t.translated, t.compiled, t.tintFailed, t.uamFailed, o.shard,
                o.shards, static_cast<unsigned long long>(swdk::dksh_compiler_id(aurora::gx::GXPipelineConfigVersion)), s);
    return t.translated > 0 ? 0 : 1;
}
