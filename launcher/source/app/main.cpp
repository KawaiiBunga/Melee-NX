// Melee NX launcher entry point.
//
// The launcher ships no game data. It mounts its romfs (fonts), runs the UI and, when you press
// Play, queues melee.nro with envSetNextLoad (data/launch.h) and returns so the loader starts it.
#include <cstdio>

#include <switch.h>

#include "app/app.h"
#include "data/launch.h"

int main(int argc, char** argv) {
    if (argc > 0) mnx::launch::SetSelfPath(argv[0]);
    if (R_FAILED(romfsInit())) {
        std::fprintf(stderr, "[launcher] romfsInit failed\n");
        return 1;
    }
    // Sockets for Slippi (profile refresh, sign-in from another device).
    const bool sockets = R_SUCCEEDED(socketInitializeDefault());
    int rc = 0;
    {
        app::App launcher;
        rc = launcher.Run();
    }
    if (sockets) socketExit();
    romfsExit();
    return rc;
}
