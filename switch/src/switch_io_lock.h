#pragma once

// Global lock over all sdmc:-backed file I/O on Switch.
//
// Two independent background threads hit the SD card concurrently during
// boot: melee-pc's file_cache.cpp prewarm worker (std::ifstream over loose
// cache files) and Aurora's DvdWorker (reading the game ISO). Both crash the
// same way on hardware -- Data Abort, address 0x30, inside newlib's
// _lseek_r/_read_r/_write_r with a null devoptab pointer -- regardless of
// which thread opened which file, which ruled out a same-thread-only fix
// (see aurora-switch-no-mmap.patch's DvdWorker::runSync, which didn't stop
// this crash on its own). devkitA64/libnx's fsdev devoptab is not proven safe
// under truly concurrent access from multiple threads; serialize it.
//
// Header-only so it can be included from both melee_game (file_cache.cpp) and
// aurora_dvd (dvd.cpp) without a shared library dependency between them --
// the function-local static gives one process-wide instance either way.

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
#define SWITCH_IO_LOCK() \
  do {                   \
  } while (0)
#endif
