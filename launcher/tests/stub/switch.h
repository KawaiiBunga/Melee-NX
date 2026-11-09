// Host stand-in for <switch.h>: only what the tested launcher files call.
#pragma once
#include <cstdint>
typedef std::uint32_t Result;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
inline bool envHasNextLoad() { return true; }
inline Result envSetNextLoad(const char*, const char*) { return 0; }
