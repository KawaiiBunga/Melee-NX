// Switch entry point shim for melee-nx.
// Replaces the Linux/SDL3 main() in melee-pc with a Switch NRO entry that
// initializes libnx services, runs the on-device game-data gate (search for
// already-extracted data or a disc image; offer to extract; or tell the user
// to supply one), and then hands off to the upstream PC main.
//
// melee-pc's main lives in src/pc/main.c; on Switch it is renamed to
// melee_main_impl() by melee-switch-gcc-compat.patch so we own main() here.
// This file is compiled by Clang (C++) while the game C files use GCC.

#include "game_data/game_data_gate.h"

#include <switch.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

extern "C" int melee_main_impl(int argc, char** argv);

namespace {

// hbmenu (and title takeover) pass the running NRO's own path as argv[0];
// fall back to melee_nx::kDataRoot if that's ever missing.
std::string nro_directory(int argc, char** argv) {
    if (argc > 0 && argv[0] != nullptr) {
        std::string exePath = argv[0];
        auto slash = exePath.find_last_of('/');
        if (slash != std::string::npos) {
            return exePath.substr(0, slash);
        }
    }
    return melee_nx::kDataRoot;
}

// consoleExit() releases the framebuffer/NWindow libnx's software console
// renders into, but stdout/stderr stay bound to that console device unless
// moved first -- confirmed by hardware crash: aurora_initialize()'s very
// first log line (routed through melee-pc's log_callback -> fprintf(stdout))
// ran straight into ConsoleSwRenderer_drawChar against the just-released
// framebuffer and Data Aborted at address 0.
//
// A freopen()-based redirect (KartPad-NX's original pattern) only rebinds the
// CALLING thread's FILE* -- newlib's stdout/stderr are per-_reent fields, and
// every other thread lazily gets its OWN FILE object still bound to fd 1/2's
// ORIGINAL device ("con:"). Hardware-confirmed: the first background thread to
// touch stdout after this ran (melee-pc's file-cache prewarm thread calling
// OSReport(), src/pc/file_cache.cpp:479) read devoptab_list[old_index] == NULL
// once consoleExit() tore "con:" down, and Data Aborted inside _lseek_r.
// Rebinding the RAW fd numbers via open()+dup2() instead updates the
// process-wide fd-to-device table __get_handle() consults for every thread,
// so any thread's stdout/stderr -- however it got initialized -- follows.
bool redirect_stdio_to_sd() {
    std::fflush(stdout);
    std::fflush(stderr);

    int logFd = open("sdmc:/switch/melee-nx/melee-nx-runtime.log",
                      O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logFd < 0) {
        return false;
    }
    if (dup2(logFd, STDOUT_FILENO) < 0 || dup2(logFd, STDERR_FILENO) < 0) {
        close(logFd);
        return false;
    }
    close(logFd);

    std::clearerr(stdout);
    std::clearerr(stderr);
    return true;
}

}  // namespace

extern "C" int main(int argc, char** argv) {
    // Standard libnx service init required before any FS/GPU work.
    romfsInit();
    socketInitializeDefault();

    // Text console for the game-data gate; released before Aurora/Dawn touch
    // the framebuffer, matching KartPad-NX's proven bring-up sequence.
    consoleInit(nullptr);
    std::string discPath = melee_nx::EnsureGameDataAvailable(nro_directory(argc, argv));
    redirect_stdio_to_sd();
    consoleExit(nullptr);

    if (discPath.empty()) {
        socketExit();
        romfsExit();
        return 0;
    }

    // Tell melee-pc's file_cache.cpp (src/pc/file_cache.cpp:resolve_loose_path)
    // exactly where the extracted loose-file cache lives, rather than relying
    // on the NRO's current working directory matching its own folder.
    setenv("MELEE_FILES_DIR", melee_nx::kFilesDir, 1);

    // melee-pc's main() takes the disc path as a positional argument (see
    // src/pc/main.c's arg loop); build a matching argv for melee_main_impl.
    char progName[] = "melee";
    std::vector<char> discArg(discPath.begin(), discPath.end());
    discArg.push_back('\0');
    char* meleeArgv[] = {progName, discArg.data()};

    int rc = melee_main_impl(2, meleeArgv);

    socketExit();
    romfsExit();
    return rc;
}
