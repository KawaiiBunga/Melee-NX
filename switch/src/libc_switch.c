// Small libc bridges required by the Switch graphics executable.
#include <sys/types.h>
#include <unistd.h>

// Horizon homebrew has no set-user-ID execution or separate effective identity.
// SQLite's Unix VFS queries this before its ownership-preservation operation.
// Keep it consistent with the process identity supplied by the libnx/NVK layer.
uid_t geteuid(void) { return getuid(); }
