// Compatibility entry point; SDL handles controller enumeration and input.

#include <switch.h>
#include <cstdio>

// Retained for compatibility with platform integration callers.
extern "C" int melee_switch_input_init() {
    // SDL initializes the Switch pads when its gamepad subsystem starts.
    return 0;
}

// Opens the system controller applet when player 1 has no controller (Joy-Cons detached and not yet
// assigned), the only place they can be paired or assigned; otherwise the game starts without input.
// Four players, Joy-Cons held sideways: on the vertical hold type the applet fails for two or more
// players in handheld mode (0x5D59, as KartPadNX found). SDL configures the pads again when it starts.
extern "C" void melee_nx_ensure_controller(void) {
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    PadState pad;
    padInitializeDefault(&pad);
    // HID reports what is connected a few frames after the configuration.
    bool connected = false;
    for (int i = 0; i < 30 && !connected; ++i) {
        padUpdate(&pad);
        connected = padIsConnected(&pad);
        if (!connected) {
            svcSleepThread(16'000'000);
        }
    }
    if (connected) {
        return;
    }
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_min = 1;
    arg.hdr.player_count_max = 4;
    arg.hdr.enable_permit_joy_dual = 1;
    HidLaControllerSupportResultInfo info{};
    const Result rc = hidLaShowControllerSupport(&info, &arg);
    std::printf("[melee-nx] controller applet: rc=0x%x, %d player(s)\n", rc, static_cast<int>(info.player_count));
}
