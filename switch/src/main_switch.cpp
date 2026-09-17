// Switch entry point shim for melee-nx.
// Replaces the Linux/SDL3 main() in melee-pc with a Switch NRO entry that
// initializes libnx services before handing off to the upstream PC main.
//
// melee-pc's main lives in src/pc/main.c (or main.cpp); on Switch we let the
// NRO loader call our romfsInit / appletInit first, then forward to it.
// This file is compiled by Clang (C++) while the game C files use GCC.

#include <switch.h>
#include <cstdlib>
#include <cstdio>

// Declared in melee-pc's src/pc/main.c — forward-declared here so we can call
// it after service init. melee-switch-gcc-compat.patch renames melee-pc's
// main() to melee_main_impl() under __SWITCH__ so we own main() here instead.
extern "C" int melee_main_impl(int argc, char** argv);

// Filesystem root on SD card — passed to melee-pc via MELEE_DVD env equivalent.
// melee-pc reads launcher.cfg from SDL's preference dir; on Switch that maps to
// sdmc:/switch/melee-nx/ via SDL_GetPrefPath.
static void setup_paths() {
    // SDL3 on Switch respects SDL_GetPrefPath("", "melee-nx") →
    // sdmc:/switch/melee-nx/  — no override needed if SDL3 is patched correctly.
}

extern "C" int main(int argc, char** argv) {
    // Standard libnx service init required before any FS/GPU work.
    romfsInit();
    socketInitializeDefault();

    setup_paths();

    int rc = melee_main_impl(argc, argv);

    socketExit();
    romfsExit();
    return rc;
}
