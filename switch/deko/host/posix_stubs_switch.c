// POSIX calls that Aurora, Dear ImGui and the game's file cache reference but libnx/newlib may not
// define. The NVK build gets them from the Docker image's toolchain and Mesa; the deko3d build links no
// Mesa, so they are repeated here, weak so that a toolchain definition wins. Behavior matches what the
// NVK build's ELF does: spawning a process is unsupported, and only the page size is known (the file
// cache then sizes itself without a physical-memory figure).
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

__attribute__((weak)) int execvp(const char* file, char* const arguments[]) {
    (void)file;
    (void)arguments;
    errno = ENOSYS;
    return -1;
}

__attribute__((weak)) pid_t waitpid(pid_t pid, int* status, int options) {
    (void)pid;
    (void)status;
    (void)options;
    errno = ENOSYS;
    return -1;
}

__attribute__((weak)) long sysconf(int name) {
    return name == _SC_PAGESIZE ? 4096 : -1;
}
