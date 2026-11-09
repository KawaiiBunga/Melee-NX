#pragma once
#include <atomic>
#include <cstdio>
#include <string>

#ifndef COS_SWITCH_DATA_DIR
#define COS_SWITCH_DATA_DIR "sdmc:/switch/melee-nx"
#endif

namespace swdk {
// A cache miss can crash before the successful-stage journal is written. Keep
// its exact input before entering Tint. Each compiling thread has its own pair of
// files (tint-request-<n>.*: several pipeline workers translate at once). Never
// publish metadata for an input whose write/rename failed.
class ShaderCompileTrace {
public:
    ShaderCompileTrace(const std::string& key, unsigned stage, const std::string& source,
                       std::string directory = COS_SWITCH_DATA_DIR "/Cache/deko")
        : key_(key), directory_(std::move(directory)), stage_(stage), bytes_(source.size()) {
        static std::atomic<int> threads{0};
        thread_local const int thread = threads.fetch_add(1, std::memory_order_relaxed);
        name_ = directory_ + "/tint-request-" + std::to_string(thread);
        const auto path = name_ + ".wgsl";
        const auto temp = path + ".tmp";
        FILE* file = std::fopen(temp.c_str(), "wb");
        if (!file) return;
        bool ok = std::fwrite(source.data(), 1, source.size(), file) == source.size();
        if (std::fflush(file) != 0) ok = false;
        if (std::fclose(file) != 0) ok = false;
        ready_ = ok && std::rename(temp.c_str(), path.c_str()) == 0;
        phase("tint-enter");
    }
    void phase(const char* state) const {
        if (!ready_) return;
        // Stage keys contain a decimal hash, stage and a validated WGSL entry
        // identifier. Refuse an unexpected JSON delimiter rather than emitting
        // malformed metadata; the source capture remains usable independently.
        if (key_.find_first_of("\"\\\r\n\t") != std::string::npos) return;
        const auto path = name_ + ".json";
        const auto temp = path + ".tmp";
        FILE* file = std::fopen(temp.c_str(), "wb");
        if (!file) return;
        bool ok = std::fprintf(file,
            "{\"schema\":1,\"key\":\"%s\",\"stage\":%u,\"source_bytes\":%zu,\"phase\":\"%s\"}\n",
            key_.c_str(), stage_, bytes_, state) > 0;
        if (std::fflush(file) != 0) ok = false;
        if (std::fclose(file) != 0) ok = false;
        if (ok) std::rename(temp.c_str(), path.c_str());
    }
private:
    std::string key_, directory_, name_;
    unsigned stage_;
    size_t bytes_;
    bool ready_ = false;
};
}
