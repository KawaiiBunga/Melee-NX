// Compile with switch/src/log_switch.cpp, -pthread and -Wl,--wrap=write.
#include "blob_read_cache.hpp"
#include "log_switch.h"
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static std::mutex ioMutex;
static std::condition_variable ioWake;
static bool blockIO = false, writerBlocked = false;
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
    if (fd == STDOUT_FILENO) {
        std::unique_lock lock(ioMutex);
        if (blockIO) {
            writerBlocked = true;
            ioWake.notify_all();
            ioWake.wait(lock, [] { return !blockIO; });
        }
    }
    return __real_write(fd, data, size);
}

int main() {
    melee_nx::BlobReadCache cache(64, 2);
    auto fill = [](void* out, size_t count) { std::memset(out, 42, count); return true; };
    assert(cache.fill(1, 2, "a", 1, 16, fill));
    assert(!cache.find(1, 2, "b", 1)); // Same complete hash, different full key.
    assert(cache.find(1, 2, "a", 1)->at(15) == 42);
    assert(cache.fill(2, 2, "b", 1, 16, fill));
    assert(cache.find(1, 2, "a", 1)); // Touch a: evict b next.
    assert(cache.fill(3, 2, "c", 1, 16, fill));
    assert(!cache.find(2, 2, "b", 1));
    assert(cache.find(1, 2, "a", 1));
    assert(!cache.fill(1, 2, "a", 1, 128, fill)); // Oversize invalidates old value.
    assert(!cache.find(1, 2, "a", 1));
    assert(!cache.accepts(SIZE_MAX, 1));
    assert(!cache.accepts(1, SIZE_MAX));
    assert(!cache.fill(3, 2, "c", 1, 16, [](void*, size_t) { return false; }));
    assert(!cache.find(3, 2, "c", 1));
    for (uint64_t i = 0; i < 10000; ++i) {
        assert(cache.fill(i, 4, &i, sizeof(i), 24, fill));
        assert(cache.bytes() <= 64 && cache.size() <= 2);
    }
    cache.clear();
    assert(cache.bytes() == 0 && cache.size() == 0);

    char path[] = "/tmp/melee-log-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0 && dup2(fd, STDOUT_FILENO) >= 0);
    close(fd);
    melee_nx_log_init();
    std::array<std::thread, 4> producers;
    for (unsigned t = 0; t < producers.size(); ++t) {
        producers[t] = std::thread([t] {
            for (unsigned i = 0; i < 500; ++i) {
                const auto line = "record " + std::to_string(t) + " " + std::to_string(i) + "\n";
                melee_nx_log_write(line.data(), line.size());
            }
        });
    }
    for (auto& thread : producers) thread.join();
    melee_nx_log_flush();
    {
        std::ifstream file(path);
        std::set<std::string> records;
        std::string line;
        while (std::getline(file, line)) {
            assert(line.find('\0') == std::string::npos);
            if (line.starts_with("record ")) assert(records.insert(line).second);
        }
        assert(records.size() == 2000); // Every producer record survives intact.
    }
    {
        std::lock_guard lock(ioMutex);
        blockIO = true;
    }
    const std::string flood = std::string(999, 'x') + '\n';
    for (unsigned i = 0; i < 20; ++i) melee_nx_log_write(flood.data(), flood.size());
    {
        std::unique_lock lock(ioMutex);
        ioWake.wait(lock, [] { return writerBlocked; });
    }
    // The writer is forcibly stuck in SD I/O. Producers must still complete.
    std::thread producer([&] {
        for (unsigned i = 0; i < 1000; ++i) melee_nx_log_write(flood.data(), flood.size());
    });
    producer.join();
    std::thread critical([] {
        melee_nx_log_write_critical("FATAL test survives saturation\n", 31);
    });
    {
        std::lock_guard lock(ioMutex);
        blockIO = false;
    }
    ioWake.notify_all();
    critical.join();
    melee_nx_log_shutdown();
    std::ifstream file(path);
    const std::string log((std::istreambuf_iterator<char>(file)), {});
    assert(log.find('\0') == std::string::npos);
    assert(log.find("FATAL test survives saturation\n") != std::string::npos);
    size_t dropped = 0, pos = 0;
    while ((pos = log.find("dropped_records ", pos)) != std::string::npos) {
        pos += 16;
        dropped += std::stoul(log.substr(pos));
    }
    assert(dropped > 0);
    unlink(path);
    std::fprintf(stderr, "Blob collision/eviction/bounds and concurrent/saturated/fatal logger checks passed\n");
}
