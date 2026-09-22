// Switch startup, game-data selection, and handoff to melee_main_impl().

#include "game_data/game_data_gate.h"
#include "perf_switch.h"
#include "log_switch.h"

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

// Build timestamp recorded in the startup logs.
constexpr const char* kBuildStamp = __DATE__ " " __TIME__;

// Commit each boot marker independently so early failures survive process exit.
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

// Capture throw sites before unwinding. For a PIE address, recover the ELF offset as
// (offset = pc - ref + the ELF symbol value of melee_boot_trace).

// Backtraces distinguish callers of shared libstdc++ throw helpers.
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
} // namespace

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
    // Limit synchronous SD writes; retain the last throw in memory after the cap.
    if (melee_throw_count < 48) {
        melee_boot_trace("   throw #%d %s pc=%p ref=%p", melee_throw_count, melee_last_throw_type,
                         pc, reinterpret_cast<const void*>(&melee_boot_trace));
    }
    if (melee_throw_count < 8) {
        MeleeBacktrace bt{};
        _Unwind_Backtrace(melee_bt_cb, &bt);
        // Keep each frame within the boot trace buffer.
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
                // Include the error code because newlib may provide no useful strerror text.
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
        const size_t len =
            static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1;
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
    // Also commit to the boot trace in case stdio redirection has not happened.
    melee_boot_trace("%s", buf);
    std::abort(); // clang_tls_switch.c's __wrap_abort -> svcExitProcess
}

// Run after TLS initialization (101) and before default-priority constructors.
__attribute__((constructor(102))) void melee_install_terminate_handler() {
    std::set_terminate(melee_terminate_handler);
    melee_boot_trace("00 static-init   build %s", kBuildStamp);
}

// argv[0] normally names the running NRO; use the data root as a fallback.
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

// Redirect process-wide descriptors before consoleExit() releases the framebuffer.
// newlib FILE objects are per-thread, so freopen() alone leaves workers on the old
// console device. Both log sinks share these descriptors.
bool redirect_stdio_to_sd() {
    std::fflush(stdout);
    std::fflush(stderr);

    // Start a separate runtime log for each launch.
    int logFd =
        open("sdmc:/switch/melee-nx/melee-nx-runtime.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
    // Prevent buffered stdio from reordering records written directly to the fd.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Commit the build stamp before startup continues.
    char stamp[160];
    const int n =
        std::snprintf(stamp, sizeof(stamp), "[melee-nx] build %s -- log opened\n", kBuildStamp);
    if (n > 0) {
        write(STDOUT_FILENO, stamp, static_cast<size_t>(n));
        fsync(STDOUT_FILENO);
    }
    return true;
}

// Use the kernel memory allowance; newlib cannot supply the desktop RAM probe.
// MELEE_CACHE_MAX_MB selects both the upstream cache profile and byte budget.
void set_file_cache_budget() {
    u64 totalBytes = 0;
    if (R_FAILED(svcGetInfo(&totalBytes, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) ||
        totalBytes == 0) {
        return; // Leave melee-pc's own fallback in place rather than guess worse.
    }
    const u64 totalMb = totalBytes / (1024 * 1024);
    // Reserve 24 MB in applet-sized processes, 256 MB for title takeover, and
    // 512 MB for larger allowances. The kernel used-memory value is not free heap.
    const unsigned budgetMb = totalMb <= 2048 ? 24u : (totalMb <= 4096 ? 256u : 512u);

    char value[16];
    std::snprintf(value, sizeof(value), "%u", budgetMb);
    setenv("MELEE_CACHE_MAX_MB", value, 0); // 0: an explicit override still wins.

    u64 usedBytes = 0;
    svcGetInfo(&usedBytes, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    std::printf(
        "[melee-nx] process memory: %llu MiB total, %llu MiB used -> file cache budget %u MB\n",
        static_cast<unsigned long long>(totalMb),
        static_cast<unsigned long long>(usedBytes / (1024 * 1024)), budgetMb);
}

void log_build_manifest() {
    const int linkedSdl = SDL_GetVersion();
    std::printf("[melee-nx] manifest: build=%s root=%s melee-pc=%s patchset=%.16s\n", kBuildStamp,
                MELEE_NX_ROOT_REV, MELEE_NX_MELEE_PC_REV, MELEE_NX_PATCHSET_ID);
    std::printf("[melee-nx] SDL headers=%d.%d.%d linked=%d.%d.%d revision=%s\n", SDL_MAJOR_VERSION,
                SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_VERSIONNUM_MAJOR(linkedSdl),
                SDL_VERSIONNUM_MINOR(linkedSdl), SDL_VERSIONNUM_MICRO(linkedSdl),
                SDL_GetRevision());
    std::printf(
        "[melee-nx] runtime config: audio_frames=%s fifo=thread fifo_batch=16 input=PADRead-main\n",
        getenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES") ? getenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES")
                                                 : "default");
}

} // namespace

extern "C" int main(int argc, char** argv) {
    melee_boot_trace("01 main          argc=%d argv0=%s", argc,
                     (argc > 0 && argv[0] != nullptr) ? argv[0] : "<null>");
    // Initialize services before accessing the filesystem and graphics.
    romfsInit();
    melee_boot_trace("02 romfsInit");
    socketInitializeDefault();
    melee_boot_trace("03 socketInit");

    // Release the extraction console before Aurora takes over the framebuffer.
    consoleInit(nullptr);
    melee_boot_trace("04 consoleInit");
    std::string discPath = melee_nx::EnsureGameDataAvailable(nro_directory(argc, argv));
    melee_boot_trace("05 gate          disc=%s", discPath.empty() ? "<none>" : discPath.c_str());

    // Keep the console alive to report a failed log redirect.
    if (!redirect_stdio_to_sd()) {
        melee_boot_trace("06 redirect      FAILED");
        std::printf("\n[melee-nx] build %s\n", kBuildStamp);
        std::printf("[melee-nx] FAILED to open sdmc:/switch/melee-nx/melee-nx-runtime.log\n");
        std::printf("[melee-nx] Not launching: a run with no diagnostics is not worth having.\n");
        consoleUpdate(nullptr);
        svcSleepThread(10ull * 1000000000ull); // Long enough to read off the screen.
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

    // Resolve extracted files independently of the current working directory.
    setenv("MELEE_FILES_DIR", melee_nx::kFilesDir, 1);
    // Request 1024 audio sample frames from SDL.
    setenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", "1024", 1);
    set_file_cache_budget();
    log_build_manifest();
    melee_nx_log_init();
    melee_nx_perf_init();
    melee_boot_trace("08 cache-budget  MELEE_CACHE_MAX_MB=%s",
                     getenv("MELEE_CACHE_MAX_MB") ? getenv("MELEE_CACHE_MAX_MB") : "<unset>");

    // Pass the selected disc path or loose-boot sentinel to the upstream entry point.
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
