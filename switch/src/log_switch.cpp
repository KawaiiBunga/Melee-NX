#include "log_switch.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace {
constexpr size_t Capacity = 128 * 1024;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wake = PTHREAD_COND_INITIALIZER, flushed = PTHREAD_COND_INITIALIZER;
pthread_t worker;
char buffers[2][Capacity];
size_t used = 0;
unsigned pending = 0;
bool running = false, stopping = false;
uint64_t requested = 0, completed = 0, dropped = 0;
pthread_once_t clockOnce = PTHREAD_ONCE_INIT;
uint64_t clockStart = 0;

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
void init_clock() { clockStart = now_ns(); }
bool write_all(const char* data, size_t length) {
    while (length) {
        const ssize_t n = write(STDOUT_FILENO, data, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        length -= size_t(n);
    }
    return true;
}
void* writer(void*) {
#if defined(__SWITCH__)
    // I/O work should not preempt game/audio/FIFO work. No compiler-core pin.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x32);
#endif
    uint64_t lastSync = now_ns(), lastReport = lastSync;
    uint64_t writeNs = 0, syncNs = 0, bytes = 0, errors = 0;
    bool dirty = false;
    for (;;) {
        pthread_mutex_lock(&mutex);
        if (used < 16384 && requested == completed && !stopping) {
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 100000000;
            if (deadline.tv_nsec >= 1000000000) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&wake, &mutex, &deadline);
        }
        const size_t length = used;
        const unsigned ready = pending;
        pending ^= 1;
        used = 0;
        const uint64_t request = requested, lost = dropped;
        dropped = 0;
        const bool stop = stopping;
        pthread_mutex_unlock(&mutex);

        if (length) {
            const auto start = now_ns();
            if (!write_all(buffers[ready], length))
                ++errors;
            writeNs += now_ns() - start;
            bytes += length;
            dirty = true;
        }
        auto now = now_ns();
        if (lost || now - lastReport >= 2000000000 || stop) {
            char line[256];
            const int n = std::snprintf(
                line, sizeof(line),
                "[%9.3f] LOGIO bytes %llu dropped_records %llu errors %llu write_ms %.3f flush_ms %.3f\n",
                melee_nx_log_now_ms(), (unsigned long long)bytes, (unsigned long long)lost,
                (unsigned long long)errors, writeNs / 1e6, syncNs / 1e6);
            if (n > 0)
                write_all(line, size_t(n));
            bytes = writeNs = syncNs = errors = 0;
            dirty = true;
            lastReport = now;
        }
        if (request != completed || stop || (dirty && now - lastSync >= 1000000000)) {
            const auto start = now_ns();
            if (fsync(STDOUT_FILENO) != 0)
                ++errors;
            syncNs += now_ns() - start;
            lastSync = now_ns();
            dirty = false;
        }
        pthread_mutex_lock(&mutex);
        completed = request;
        pthread_cond_broadcast(&flushed);
        pthread_mutex_unlock(&mutex);
        if (stop)
            break;
    }
    return nullptr;
}
} // namespace

extern "C" double melee_nx_log_now_ms(void) {
    pthread_once(&clockOnce, init_clock);
    return (now_ns() - clockStart) / 1e6;
}
extern "C" void melee_nx_log_init(void) {
    pthread_mutex_lock(&mutex);
    if (!running) {
        stopping = false;
        running = pthread_create(&worker, nullptr, writer, nullptr) == 0;
        if (!running)
            write_all("[LOGIO] writer unavailable; synchronous fallback\n",
                      sizeof("[LOGIO] writer unavailable; synchronous fallback\n") - 1);
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" void melee_nx_log_write(const char* data, size_t size) {
    if (!data || !size)
        return;
    pthread_mutex_lock(&mutex);
    if (!running) {
        write_all(data, size);
    } else if (stopping || size > Capacity - used) {
        // Drop whole records, never block producers on a slow SD card. The
        // writer reports losses explicitly; no unbounded allocation or queue.
        ++dropped;
    } else {
        std::memcpy(buffers[pending] + used, data, size);
        used += size;
        if (used >= 16384)
            pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" void melee_nx_log_flush(void) {
    pthread_mutex_lock(&mutex);
    if (running && !stopping) {
        const uint64_t target = ++requested;
        pthread_cond_signal(&wake);
        while (completed < target)
            pthread_cond_wait(&flushed, &mutex);
    } else if (!running) {
        fsync(STDOUT_FILENO);
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" void melee_nx_log_write_critical(const char* data, size_t size) {
    if (!data || !size)
        return;
    // Critical records may block for space. Routine telemetry never does.
    // Split only if a caller supplies more than the entire buffer capacity.
    while (size) {
        const size_t chunk = size < Capacity ? size : Capacity;
        pthread_mutex_lock(&mutex);
        while (running && !stopping && chunk > Capacity - used) {
            const auto target = ++requested;
            pthread_cond_signal(&wake);
            while (completed < target)
                pthread_cond_wait(&flushed, &mutex);
        }
        if (!running)
            write_all(data, chunk);
        else if (!stopping) {
            std::memcpy(buffers[pending] + used, data, chunk);
            used += chunk;
        }
        pthread_mutex_unlock(&mutex);
        data += chunk;
        size -= chunk;
    }
    melee_nx_log_flush();
}
extern "C" void melee_nx_log_shutdown(void) {
    melee_nx_log_flush();
    pthread_mutex_lock(&mutex);
    const bool join = running;
    stopping = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&mutex);
    if (join)
        pthread_join(worker, nullptr);
    pthread_mutex_lock(&mutex);
    running = false;
    pthread_mutex_unlock(&mutex);
}
