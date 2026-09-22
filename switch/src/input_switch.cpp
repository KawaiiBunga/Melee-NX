// Compatibility entry point; SDL handles controller enumeration and input.

#include <switch.h>

// Retained for compatibility with platform integration callers.
extern "C" int melee_switch_input_init() {
    // SDL initializes the Switch pads when its gamepad subsystem starts.
    return 0;
}
