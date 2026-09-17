// GameCube controller input bridge for Switch.
// Maps libnx HID (Pro Controller / Joy-Con pair / Handheld) to SDL3 gamepad
// events that melee-pc's SDL3 PAD layer already knows how to read.
//
// melee-pc remaps SDL gamepads to GC layout in src/pc/ already. This shim only
// ensures that libnx controllers are registered as SDL3 gamepads before melee
// opens them. If SDL3's Switch backend handles this natively, this file is a no-op.
//
// TODO: verify SDL3 Switch backend enumerates Pro Controller as SDL_Gamepad.
//       If not, implement a virtual joystick injection here matching the approach
//       used in KartPad-NX's input_switch.cpp.

#include <switch.h>

// Called once at startup from main_switch.cpp before SDL_Init.
// Returns 0 on success.
extern "C" int melee_switch_input_init() {
    // SDL3's Switch backend calls padInitializeDefault() itself when
    // SDL_INIT_GAMEPAD is requested. If that's confirmed, nothing needed here.
    return 0;
}
