#pragma once

// Serialize the participating fsdev disc and loose-file operations.
// The inline function shares one mutex across the game and Aurora targets.

#if defined(__SWITCH__)
#include <mutex>

inline std::mutex& switch_sdmc_io_mutex() {
    static std::mutex m;
    return m;
}

class SwitchIoLock {
  public:
    SwitchIoLock() : lock_(switch_sdmc_io_mutex()) {}

  private:
    std::lock_guard<std::mutex> lock_;
};

#define SWITCH_IO_LOCK() SwitchIoLock _switch_io_lock
#else
#define SWITCH_IO_LOCK()                                                                           \
    do {                                                                                           \
    } while (0)
#endif
