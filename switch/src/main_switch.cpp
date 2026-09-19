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
#include "perf_switch.h"

#include <SDL3/SDL_version.h>
#include <switch.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cxxabi.h>
#include <typeinfo>
#include <unwind.h>
#include <exception>
#include <system_error>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

extern "C" int melee_main_impl(int argc, char** argv);

#ifndef MELEE_NX_ROOT_REV
#define MELEE_NX_ROOT_REV "unknown"
#endif
#ifndef MELEE_NX_MELEE_PC_REV
#define MELEE_NX_MELEE_PC_REV "unknown"
#endif
#ifndef MELEE_NX_PATCHSET_ID
#define MELEE_NX_PATCHSET_ID "unknown"
#endif

namespace {

// Identifies the build, so a run can never be attributed to the wrong NRO.
constexpr const char* kBuildStamp = __DATE__ " " __TIME__;

// A boot trace that does not depend on stdout, on the console, or on the
// process surviving long enough to flush anything.
//
// melee-nx-runtime.log could not answer the one question that mattered on
// 2026-09-17: four hardware runs produced a byte-identical 331-byte file (same
// md5 every time), which the build before this one should have made impossible
// -- it opens that log O_TRUNC, so reaching redirect_stdio_to_sd() at all would
// have emptied it. Either the NRO on the card was stale, or the process was
// dying before the redirect, and nothing on disk could tell the two apart.
//
// Each marker here is its own open/write/fsync/close. A closed file is
// committed by definition, so the trace survives any death at any point after
// it, including one with no Atmosphere crash report because our own
// __wrap_abort took svcExitProcess. Eight markers cost eight SD commits, which
// is nothing next to another blind hardware cycle.
extern "C" void melee_boot_trace(const char* fmt, ...) {
    static bool truncated = false;
    const int flags = O_WRONLY | O_CREAT | (truncated ? O_APPEND : O_TRUNC);
    const int fd = open("sdmc:/switch/melee-nx/melee-nx-boot.log", flags, 0644);
    if (fd < 0) {
        return;
    }
    truncated = true;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t len =
            static_cast<size_t>(n) < sizeof(buf) - 1 ? static_cast<size_t>(n) : sizeof(buf) - 2;
        buf[len] = '\n';
        const char* p = buf;
        size_t left = len + 1;
        while (left > 0) {
            const ssize_t w = write(fd, p, left);
            if (w <= 0) {
                break;
            }
            p += w;
            left -= static_cast<size_t>(w);
        }
    }
    fsync(fd);
    close(fd);
}

// An uncaught C++ exception is how this port dies (hardware, 2026-09-17: two
// runs, both with __cxa_throw -> __cxxabiv1::__terminate -> abort() -> _exit()
// on the main thread's stack). The default terminate handler aborts without
// saying what was thrown, and by the time Atmosphere writes its crash report
// the process is already tearing down -- the report names whichever worker
// thread lost the race on the dismantled fsdev devoptab, never the throw.
//
// Print the exception's type and what() before anything else runs, straight to
// fd 2 with write() so it works whether stderr still points at the libnx
// console or at melee-nx-runtime.log, then fsync so it survives the exit.
// --- throw-site capture via linker --wrap -------------------------------------
// The terminate handler names the exception but never where it came from, and
// an uncaught std::system_error can be thrown from anywhere in Dawn, Aurora or
// the translated game. __cxa_throw still runs on the throwing frame -- nothing
// has been unwound yet -- so its return address IS the throw site.
//
// PCs are printed raw next to melee_boot_trace's own runtime address, because
// hbloader maps the module wherever it likes and the ELF is a PIE. Recover the
// offset to feed addr2line with:
//   offset = pc - ref + <nm value of melee_boot_trace>
// Needs -Wl,--wrap=__cxa_throw.

// __builtin_return_address(0) inside the wrap lands in whichever libstdc++
// helper did the throwing -- std::__throw_system_error is shared by 60-odd
// call sites -- so that address alone names nothing. _Unwind_Backtrace walks
// the real frames using the same .eh_frame data the throw itself is about to
// use, which is exactly the machinery we already know works here.
namespace {
struct MeleeBacktrace {
    const void* pc[16];
    int n;
};

_Unwind_Reason_Code melee_bt_cb(struct _Unwind_Context* ctx, void* arg) {
    auto* bt = static_cast<MeleeBacktrace*>(arg);
    if (bt->n >= 16) {
        return _URC_END_OF_STACK;
    }
    bt->pc[bt->n++] = reinterpret_cast<const void*>(_Unwind_GetIP(ctx));
    return _URC_NO_REASON;
}
}  // namespace

extern "C" void __real___cxa_throw(void* obj, std::type_info* tinfo, void (*dest)(void*));

static int melee_throw_count;
static const void* melee_last_throw_pc;
static const char* melee_last_throw_type = "<none>";

extern "C" {
__attribute__((noreturn)) void __wrap___cxa_throw(void* obj, std::type_info* tinfo,
                                                  void (*dest)(void*)) {
    const void* pc = __builtin_return_address(0);
    melee_last_throw_pc = pc;
    melee_last_throw_type = (tinfo != nullptr) ? tinfo->name() : "<null-typeinfo>";
    // Bounded. One open/write/fsync/close per line is far too slow to run
    // unthrottled, and a throw that reaches terminate is nearly always among
    // the first handful; past the cap the globals above still hold the last one.
    if (melee_throw_count < 48) {
        melee_boot_trace("   throw #%d %s pc=%p ref=%p", melee_throw_count,
                         melee_last_throw_type, pc,
                         reinterpret_cast<const void*>(&melee_boot_trace));
    }
    if (melee_throw_count < 8) {
        MeleeBacktrace bt{};
        _Unwind_Backtrace(melee_bt_cb, &bt);
        // One line per frame: boot_trace's own buffer is small, and a frame
        // that never reaches the SD card is a frame that never existed.
        for (int i = 0; i < bt.n; ++i) {
            melee_boot_trace("     bt#%d.%d pc=%p", melee_throw_count, i, bt.pc[i]);
        }
    }
    melee_throw_count++;
    __real___cxa_throw(obj, tinfo, dest);
    __builtin_unreachable();
}
}

[[noreturn]] void melee_terminate_handler() {
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), "\n[FATAL] std::terminate: ");
    if (n < 0) {
        n = 0;
    }

    const std::type_info* type = abi::__cxa_current_exception_type();
    if (type == nullptr) {
        n += std::snprintf(buf + n, sizeof(buf) - n, "no active exception\n");
    } else {
        int status = 0;
        char* pretty = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        n += std::snprintf(buf + n, sizeof(buf) - n, "uncaught %s",
                           (status == 0 && pretty != nullptr) ? pretty : type->name());
        std::free(pretty);
        // rethrow_exception on the in-flight exception is the only way to reach
        // what(); a bare `throw;` here would re-enter terminate instead.
        if (const std::exception_ptr active = std::current_exception()) {
            try {
                std::rethrow_exception(active);
            } catch (const std::system_error& e) {
                // what() on this target is just "error" -- newlib has no
                // strerror text for the errno libstdc++ hands it. The code
                // itself is the whole diagnosis, so print the number.
                n += std::snprintf(buf + n, sizeof(buf) - n, ": %s [%s errno=%d: %s]", e.what(),
                                   e.code().category().name(), e.code().value(),
                                   std::strerror(e.code().value()));
            } catch (const std::exception& e) {
                n += std::snprintf(buf + n, sizeof(buf) - n, ": %s", e.what());
            } catch (...) {
                n += std::snprintf(buf + n, sizeof(buf) - n, ": <non-std::exception>");
            }
        }
        n += std::snprintf(buf + n, sizeof(buf) - n, " [throw #%d %s pc=%p ref=%p]",
                           melee_throw_count - 1, melee_last_throw_type, melee_last_throw_pc,
                           reinterpret_cast<const void*>(&melee_boot_trace));
        n += std::snprintf(buf + n, sizeof(buf) - n, "\n");
    }

    if (n > 0) {
        const size_t len = static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                                : sizeof(buf) - 1;
        const char* p = buf;
        size_t left = len;
        while (left > 0) {
            const ssize_t w = write(STDERR_FILENO, p, left);
            if (w <= 0) {
                break;
            }
            p += w;
            left -= static_cast<size_t>(w);
        }
        fsync(STDERR_FILENO);
    }
    // fd 2 may still be the console, or a log nobody will look at. The boot
    // trace is opened, written, fsynced and closed, so this line survives no
    // matter how early the throw happened.
    melee_boot_trace("%s", buf);
    std::abort();  // clang_tls_switch.c's __wrap_abort -> svcExitProcess
}

// 102 runs just after clang_tls_switch.c's TLS sync (101) and before the
// default-priority static constructors, so a throw out of Dawn's or abseil's
// own initializers is covered too.
__attribute__((constructor(102))) void melee_install_terminate_handler() {
    std::set_terminate(melee_terminate_handler);
    melee_boot_trace("00 static-init   build %s", kBuildStamp);
}

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
//
// This descriptor is now the ONLY writer of melee-nx-runtime.log. melee-pc's
// two Switch log sinks (main.c's aurora log_callback and os.c's OSReport) each
// used to fopen() the same path as well; three handles meant three independent
// append offsets clobbering one another, and the log silently truncated to a
// few hundred bytes mid-boot. Both now write(STDOUT_FILENO, ...) through this
// fd -- see main.c's switch_log_write() for the hardware evidence.
bool redirect_stdio_to_sd() {
    std::fflush(stdout);
    std::fflush(stderr);

    // O_TRUNC, not O_APPEND. Three hardware runs in a row produced a
    // byte-identical 331-byte log (same md5, same sub-millisecond timestamps),
    // which is impossible for three separate runs and cost a cycle to see: an
    // append-mode open bumps the file's mtime whether or not anything is ever
    // written to it, so a run that logged nothing was indistinguishable from a
    // run that logged the same thing. One run per file, starting from empty.
    int logFd = open("sdmc:/switch/melee-nx/melee-nx-runtime.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
    // Anything still going through the FILE* layer (SDL_Log, melee-pc's
    // pc_log_line, a stray printf) must reach the fd in the order it was
    // written, or it interleaves with the write()-based sinks at whatever
    // offset a 1 KiB buffer happens to flush at. stderr is already unbuffered
    // on newlib; stdout turns fully-buffered the moment it points at a file.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Prove the descriptor reaches the SD card before anything depends on it,
    // and name the build that is about to run. fsync because newlib's write()
    // only reaches Horizon's FS cache -- see clang_tls_switch.c's
    // kartpad_fast_exit for the full story.
    char stamp[160];
    const int n =
        std::snprintf(stamp, sizeof(stamp), "[melee-nx] build %s -- log opened\n", kBuildStamp);
    if (n > 0) {
        write(STDOUT_FILENO, stamp, static_cast<size_t>(n));
        fsync(STDOUT_FILENO);
    }
    return true;
}

// melee-pc sizes its loose-file cache from detect_system_ram_mb()
// (src/pc/file_cache.cpp:79), which reads sysconf(_SC_PHYS_PAGES). devkitPro's
// newlib has no such counter, so that lookup fails and the function returns its
// "8192 MB" desktop fallback -- on hardware (2026-09-17) the prewarm thread
// announced itself as "[Profile: Desktop (>4GB), Budget: 512 MB]" and set out
// to preload all 889 archives. A 512 MB cache on top of MEM1 (96 MB), ARAM,
// Dawn, Mesa/NVK and an 8 MiB stack per std::thread is not something this
// process's memory allowance can absorb. (aurora::system_info's "Memory: 0 MiB"
// log line is the same detection failing by a different route.)
//
// Ask the kernel instead and hand the answer to file_cache.cpp through the one
// input it already honours ahead of its own detection: MELEE_CACHE_MAX_MB is
// consulted first by BOTH get_effective_profile() and get_default_budget_bytes(),
// so a single number picks a consistent profile and budget. The thresholds below
// mirror file_cache.cpp's own table, just fed real numbers -- which also makes
// this adapt on its own to applet mode (~448 MB allowance -> 24 MB) versus title
// takeover (~3.2 GB -> 64 MB, the Handheld profile melee-pc intends for Switch).
void set_file_cache_budget() {
    u64 totalBytes = 0;
    if (R_FAILED(svcGetInfo(&totalBytes, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) ||
        totalBytes == 0) {
        return;  // Leave melee-pc's own fallback in place rather than guess worse.
    }
    const u64 totalMb = totalBytes / (1024 * 1024);
    const unsigned budgetMb = totalMb <= 2048 ? 24u : (totalMb <= 4096 ? 64u : 512u);

    char value[16];
    std::snprintf(value, sizeof(value), "%u", budgetMb);
    setenv("MELEE_CACHE_MAX_MB", value, 0);  // 0: an explicit override still wins.

    u64 usedBytes = 0;
    svcGetInfo(&usedBytes, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    std::printf("[melee-nx] process memory: %llu MiB total, %llu MiB used -> file cache budget %u MB\n",
                static_cast<unsigned long long>(totalMb),
                static_cast<unsigned long long>(usedBytes / (1024 * 1024)), budgetMb);
}

void log_build_manifest() {
    const int linkedSdl = SDL_GetVersion();
    std::printf(
        "[melee-nx] manifest: build=%s root=%s melee-pc=%s patchset=%.16s\n",
        kBuildStamp, MELEE_NX_ROOT_REV, MELEE_NX_MELEE_PC_REV, MELEE_NX_PATCHSET_ID);
    std::printf(
        "[melee-nx] SDL headers=%d.%d.%d linked=%d.%d.%d revision=%s\n",
        SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
        SDL_VERSIONNUM_MAJOR(linkedSdl), SDL_VERSIONNUM_MINOR(linkedSdl),
        SDL_VERSIONNUM_MICRO(linkedSdl), SDL_GetRevision());
    std::printf(
        "[melee-nx] runtime config: audio_frames=%s fifo=thread fifo_batch=16 input=PADRead-main\n",
        getenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES")
            ? getenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES")
            : "default");
}

}  // namespace

extern "C" int main(int argc, char** argv) {
    melee_boot_trace("01 main          argc=%d argv0=%s", argc,
               (argc > 0 && argv[0] != nullptr) ? argv[0] : "<null>");
    // Standard libnx service init required before any FS/GPU work.
    romfsInit();
    melee_boot_trace("02 romfsInit");
    socketInitializeDefault();
    melee_boot_trace("03 socketInit");

    // Text console for the game-data gate; released before Aurora/Dawn touch
    // the framebuffer, matching KartPad-NX's proven bring-up sequence.
    consoleInit(nullptr);
    melee_boot_trace("04 consoleInit");
    std::string discPath = melee_nx::EnsureGameDataAvailable(nro_directory(argc, argv));
    melee_boot_trace("05 gate          disc=%s", discPath.empty() ? "<none>" : discPath.c_str());

    // This return value used to be discarded, and that hid an entire class of
    // failure: libnx's consoleExit() puts devoptab_list[STD_OUT]/[STD_ERR] back
    // to dotab_stdnull, so once the console is gone a failed redirect sends
    // every subsequent write into a bit bucket -- no log, no error, no crash.
    // Keep the console alive instead and say so on screen; a visible message
    // beats a silent run every time.
    if (!redirect_stdio_to_sd()) {
        melee_boot_trace("06 redirect      FAILED");
        std::printf("\n[melee-nx] build %s\n", kBuildStamp);
        std::printf("[melee-nx] FAILED to open sdmc:/switch/melee-nx/melee-nx-runtime.log\n");
        std::printf("[melee-nx] Not launching: a run with no diagnostics is not worth having.\n");
        consoleUpdate(nullptr);
        svcSleepThread(10ull * 1000000000ull);  // Long enough to read off the screen.
        consoleExit(nullptr);
        socketExit();
        romfsExit();
        return 1;
    }
    melee_boot_trace("06 redirect      ok");
    consoleExit(nullptr);
    melee_boot_trace("07 consoleExit");

    if (discPath.empty()) {
        socketExit();
        romfsExit();
        return 0;
    }

    // Tell melee-pc's file_cache.cpp (src/pc/file_cache.cpp:resolve_loose_path)
    // exactly where the extracted loose-file cache lives, rather than relying
    // on the NRO's current working directory matching its own folder.
    setenv("MELEE_FILES_DIR", melee_nx::kFilesDir, 1);
    // Audio buffer: 1024 sample frames paired with 4 audout buffers ensures continuous, glitch-free output.
    setenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", "1024", 1);
    set_file_cache_budget();
    log_build_manifest();
    melee_nx_perf_init();
    melee_boot_trace("08 cache-budget  MELEE_CACHE_MAX_MB=%s",
               getenv("MELEE_CACHE_MAX_MB") ? getenv("MELEE_CACHE_MAX_MB") : "<unset>");

    // melee-pc's main() takes the disc path as a positional argument (see
    // src/pc/main.c's arg loop); build a matching argv for melee_main_impl.
    char progName[] = "melee";
    std::vector<char> discArg(discPath.begin(), discPath.end());
    discArg.push_back('\0');
    char* meleeArgv[] = {progName, discArg.data()};

    melee_boot_trace("09 melee_main_impl entering");
    int rc = melee_main_impl(2, meleeArgv);
    melee_boot_trace("10 melee_main_impl returned rc=%d", rc);

    socketExit();
    romfsExit();
    return rc;
}
