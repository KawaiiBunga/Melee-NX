// SQLite's HOS backend uses native file handles and offsets. The Unix VFS
// assumes POSIX inode identities, permissions and locking that SD/fsdev lacks.
#include <sqlite3.h>
#include <switch.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <new>
#include <strings.h>
#include <unistd.h>

namespace {
constexpr Result PathNotFound = 0x202;
constexpr Result PathExists = 0x402;
thread_local Result lastResult;

// HOS rejects a second native open while a writable file handle exists. Keep
// one handle per path in this process; SQLite locks still belong to individual
// connections. Serialize native operations and retain the handle until the last
// connection closes, regardless of which connection opened it first.
struct SharedFile {
  FsFile handle;
  FsFileSystem* fs;
  u32 mode;
  unsigned references;
  bool deleteOnClose;
  char path[FS_MAX_PATH];
  SharedFile* next;
};
std::mutex handleMutex;
SharedFile* handles = nullptr;
SharedFile* findHandle(FsFileSystem* fs, const char* path) {
  for (auto* shared = handles; shared; shared = shared->next)
    if (shared->fs == fs && strcasecmp(shared->path, path) == 0) return shared;
  return nullptr;
}

struct HosFile {
  sqlite3_file base;
  SharedFile* shared;
  FsFileSystem* fs;
  int lock;
  bool readOnly;
  char path[FS_MAX_PATH];
  char lockPath[FS_MAX_PATH];
};
HosFile* file(sqlite3_file* value) { return reinterpret_cast<HosFile*>(value); }
int error(Result result, int code, const char* operation, const char* path) {
  lastResult = result;
  std::fprintf(stderr, "[sqlite-hos] %s failed: result=%08x path=%s\n", operation, result, path);
  return code;
}
int unlock(sqlite3_file* base, int level) {
  auto* f = file(base);
  if (level == SQLITE_LOCK_NONE && f->lock != SQLITE_LOCK_NONE) {
    const Result rc = fsFsDeleteDirectory(f->fs, f->lockPath);
    if (R_FAILED(rc) && rc != PathNotFound) return error(rc, SQLITE_IOERR_UNLOCK, "unlock", f->path);
  }
  f->lock = level;
  return SQLITE_OK;
}
int close(sqlite3_file* base) {
  auto* f = file(base);
  const int result = unlock(base, SQLITE_LOCK_NONE);
  std::lock_guard guard(handleMutex);
  auto* shared = f->shared;
  if (--shared->references == 0) {
    fsFileClose(&shared->handle);
    if (shared->deleteOnClose) fsFsDeleteFile(shared->fs, shared->path);
    auto** entry = &handles;
    while (*entry != shared) entry = &(*entry)->next;
    *entry = shared->next;
    delete shared;
  }
  f->shared = nullptr;
  f->base.pMethods = nullptr;
  return result;
}
int read(sqlite3_file* base, void* buffer, int amount, sqlite3_int64 offset) {
  auto* f = file(base);
  std::lock_guard guard(handleMutex);
  s64 length = 0;
  Result rc = fsFileGetSize(&f->shared->handle, &length);
  if (R_FAILED(rc)) return error(rc, SQLITE_IOERR_READ, "read size", f->path);
  const u64 available = offset < length ? static_cast<u64>(length - offset) : 0;
  const u64 requested = available < static_cast<u64>(amount) ? available : static_cast<u64>(amount);
  u64 count = 0;
  if (requested) rc = fsFileRead(&f->shared->handle, offset, buffer, requested, FsReadOption_None, &count);
  if ((rc & 0x3fffff) == 0xd401) {
    // Like fsdev, bounce buffers from memory that HOS cannot map for IPC.
    char bounce[4096];
    count = 0;
    while (count < requested) {
      const u64 chunk = requested - count < sizeof(bounce) ? requested - count : sizeof(bounce);
      u64 received = 0;
      rc = fsFileRead(&f->shared->handle, offset + count, bounce, chunk, FsReadOption_None, &received);
      if (R_FAILED(rc)) break;
      if (received > chunk) return SQLITE_IOERR_READ;
      std::memcpy(static_cast<char*>(buffer) + count, bounce, received);
      count += received;
      if (received < chunk) break;
    }
  }
  if (R_FAILED(rc)) return error(rc, SQLITE_IOERR_READ, "read", f->path);
  if (count < static_cast<u64>(amount)) {
    std::memset(static_cast<char*>(buffer) + count, 0, amount - count);
    return SQLITE_IOERR_SHORT_READ;
  }
  return SQLITE_OK;
}
int write(sqlite3_file* base, const void* buffer, int amount, sqlite3_int64 offset) {
  auto* f = file(base);
  if (f->readOnly) return SQLITE_READONLY;
  std::lock_guard guard(handleMutex);
  Result rc = fsFileWrite(&f->shared->handle, offset, buffer, amount, FsWriteOption_None);
  if ((rc & 0x3fffff) == 0xd401) {
    char bounce[4096];
    int written = 0;
    while (written < amount) {
      const int chunk = amount - written < static_cast<int>(sizeof(bounce)) ? amount - written : sizeof(bounce);
      std::memcpy(bounce, static_cast<const char*>(buffer) + written, chunk);
      rc = fsFileWrite(&f->shared->handle, offset + written, bounce, chunk, FsWriteOption_None);
      if (R_FAILED(rc)) break;
      written += chunk;
    }
  }
  return R_SUCCEEDED(rc) ? SQLITE_OK : error(rc, SQLITE_IOERR_WRITE, "write", f->path);
}
int truncate(sqlite3_file* base, sqlite3_int64 size) {
  auto* f = file(base);
  if (f->readOnly) return SQLITE_READONLY;
  std::lock_guard guard(handleMutex);
  const Result rc = fsFileSetSize(&f->shared->handle, size);
  return R_SUCCEEDED(rc) ? SQLITE_OK : error(rc, SQLITE_IOERR_TRUNCATE, "truncate", f->path);
}
int sync(sqlite3_file* base, int) {
  auto* f = file(base);
  std::lock_guard guard(handleMutex);
  const Result rc = fsFileFlush(&f->shared->handle);
  return R_SUCCEEDED(rc) ? SQLITE_OK : error(rc, SQLITE_IOERR_FSYNC, "flush", f->path);
}
int size(sqlite3_file* base, sqlite3_int64* output) {
  auto* f = file(base);
  std::lock_guard guard(handleMutex);
  s64 value = 0;
  const Result rc = fsFileGetSize(&f->shared->handle, &value);
  *output = value;
  return R_SUCCEEDED(rc) ? SQLITE_OK : error(rc, SQLITE_IOERR_FSTAT, "size", f->path);
}
int lock(sqlite3_file* base, int level) {
  auto* f = file(base);
  if (level <= f->lock) return SQLITE_OK;
  if (f->lock == SQLITE_LOCK_NONE) {
    // Coarse but real exclusion: a read lock also owns the atomic directory.
    // This supports rollback journals and serializes connections to one file.
    const Result rc = fsFsCreateDirectory(f->fs, f->lockPath);
    if (rc == PathExists) return SQLITE_BUSY;
    if (R_FAILED(rc)) return error(rc, SQLITE_IOERR_LOCK, "lock", f->path);
  }
  f->lock = level;
  return SQLITE_OK;
}
int reserved(sqlite3_file* base, int* output) {
  auto* f = file(base);
  if (f->lock != SQLITE_LOCK_NONE) {
    *output = f->lock >= SQLITE_LOCK_RESERVED;
    return SQLITE_OK;
  }
  FsDirEntryType type;
  const Result rc = fsFsGetEntryType(f->fs, f->lockPath, &type);
  *output = R_SUCCEEDED(rc);
  return R_SUCCEEDED(rc) || rc == PathNotFound ? SQLITE_OK
      : error(rc, SQLITE_IOERR_CHECKRESERVEDLOCK, "check lock", f->path);
}
int control(sqlite3_file* base, int operation, void* argument) {
  if (operation == SQLITE_FCNTL_LOCKSTATE) {
    *static_cast<int*>(argument) = file(base)->lock;
    return SQLITE_OK;
  }
  if (operation == SQLITE_FCNTL_HAS_MOVED) {
    *static_cast<int*>(argument) = 0; // private cache paths stay fixed while open
    return SQLITE_OK;
  }
  return SQLITE_NOTFOUND;
}
int sector(sqlite3_file*) { return 4096; }
int characteristics(sqlite3_file*) { return 0; } // do not promise atomic writes
const sqlite3_io_methods methods = {
    1, close, read, write, truncate, sync, size, lock, unlock, reserved, control,
    sector, characteristics, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

int fullPath(sqlite3_vfs*, const char* input, int capacity, char* output) {
  char cwd[FS_MAX_PATH];
  int length;
  if (std::strstr(input, ":/") || input[0] == '/') {
    length = std::snprintf(output, capacity, "%s", input);
  } else {
    if (!getcwd(cwd, sizeof(cwd))) return SQLITE_CANTOPEN;
    length = std::snprintf(output, capacity, "%s/%s", cwd, input);
  }
  return length >= 0 && length < capacity ? SQLITE_OK : SQLITE_CANTOPEN;
}
int open(sqlite3_vfs*, const char* name, sqlite3_file* base, int flags, int* actualFlags) {
  auto* f = file(base);
  std::memset(f, 0, sizeof(*f));
  char temporary[FS_MAX_PATH];
  if (!name) {
    u64 nonce;
    randomGet(&nonce, sizeof(nonce));
    std::snprintf(temporary, sizeof(temporary), "sdmc:/switch/melee-nx/tmp-%016llx.db",
                  static_cast<unsigned long long>(nonce));
    name = temporary;
    flags |= SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE;
  }
  if (fsdevTranslatePath(name, &f->fs, f->path) != 0) return SQLITE_CANTOPEN;
  try {
    const auto normalized = std::filesystem::path(f->path).lexically_normal().generic_string();
    if (normalized.size() >= sizeof(f->path)) return SQLITE_CANTOPEN;
    std::strcpy(f->path, normalized.c_str());
  } catch (const std::bad_alloc&) { return SQLITE_NOMEM; }
  if (std::snprintf(f->lockPath, sizeof(f->lockPath), "%s.lock", f->path) >= FS_MAX_PATH)
    return SQLITE_CANTOPEN;
  // Clear any stale lock directory or file left behind by ungraceful exits or crashes
  fsFsDeleteDirectory(f->fs, f->lockPath);
  fsFsDeleteFile(f->fs, f->lockPath);
  std::lock_guard guard(handleMutex);
  auto* shared = findHandle(f->fs, f->path);
  if (shared && (flags & SQLITE_OPEN_EXCLUSIVE)) return SQLITE_CANTOPEN;
  if (flags & SQLITE_OPEN_CREATE) {
    const Result rc = fsFsCreateFile(f->fs, f->path, 0, 0);
    if (R_FAILED(rc) && (rc != PathExists || (flags & SQLITE_OPEN_EXCLUSIVE)))
      return error(rc, SQLITE_CANTOPEN, "create", f->path);
  }
  if (!shared) {
    shared = new (std::nothrow) SharedFile{};
    if (!shared) return SQLITE_NOMEM;
    // Keep main databases shareable if a read-only connection opens first.
    // Connection-level readOnly below still forbids writes through that view.
    u32 mode = FsOpenMode_Read;
    if (flags & (SQLITE_OPEN_READWRITE | SQLITE_OPEN_MAIN_DB))
      mode |= FsOpenMode_Write | FsOpenMode_Append;
    Result rc = fsFsOpenFile(f->fs, f->path, mode, &shared->handle);
    if (R_FAILED(rc) && (mode & FsOpenMode_Write)) {
      mode = FsOpenMode_Read;
      rc = fsFsOpenFile(f->fs, f->path, mode, &shared->handle);
    }
    if (R_FAILED(rc)) { delete shared; return error(rc, SQLITE_CANTOPEN, "open", f->path); }
    shared->fs = f->fs;
    shared->mode = mode;
    std::strcpy(shared->path, f->path);
    shared->next = handles;
    handles = shared;
  }
  ++shared->references;
  shared->deleteOnClose |= (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
  f->shared = shared;
  f->readOnly = !(flags & SQLITE_OPEN_READWRITE) || !(shared->mode & FsOpenMode_Write);
  if (f->readOnly) flags = (flags & ~SQLITE_OPEN_READWRITE) | SQLITE_OPEN_READONLY;
  f->base.pMethods = &methods;
  if (actualFlags) *actualFlags = flags;
  return SQLITE_OK;
}
int remove(sqlite3_vfs*, const char* name, int) {
  FsFileSystem* fs;
  char path[FS_MAX_PATH];
  if (fsdevTranslatePath(name, &fs, path) != 0) return SQLITE_IOERR_DELETE;
  const Result rc = fsFsDeleteFile(fs, path);
  return R_SUCCEEDED(rc) || rc == PathNotFound ? SQLITE_OK
      : error(rc, SQLITE_IOERR_DELETE, "delete", path);
}
int access(sqlite3_vfs*, const char* name, int flags, int* output) {
  FsFileSystem* fs;
  char path[FS_MAX_PATH];
  *output = 0;
  if (fsdevTranslatePath(name, &fs, path) != 0) return SQLITE_IOERR_ACCESS;
  FsDirEntryType type;
  Result rc = fsFsGetEntryType(fs, path, &type);
  if (rc == PathNotFound) return SQLITE_OK;
  if (R_FAILED(rc)) return error(rc, SQLITE_IOERR_ACCESS, "access", path);
  if (flags == SQLITE_ACCESS_EXISTS) { *output = 1; return SQLITE_OK; }
  std::lock_guard guard(handleMutex);
  if (auto* shared = findHandle(fs, path)) {
    *output = flags != SQLITE_ACCESS_READWRITE || (shared->mode & FsOpenMode_Write);
    return SQLITE_OK;
  }
  FsFile handle;
  rc = fsFsOpenFile(fs, path, FsOpenMode_Read |
      (flags == SQLITE_ACCESS_READWRITE ? FsOpenMode_Write : 0), &handle);
  if (R_SUCCEEDED(rc)) { *output = 1; fsFileClose(&handle); }
  return SQLITE_OK;
}
int randomness(sqlite3_vfs*, int amount, char* bytes) {
  randomGet(bytes, amount);
  return amount;
}
int sleep(sqlite3_vfs*, int microseconds) {
  svcSleepThread(static_cast<s64>(microseconds) * 1000);
  return microseconds;
}
int currentTime64(sqlite3_vfs*, sqlite3_int64* output) {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return SQLITE_ERROR;
  *output = 210866760000000LL + static_cast<sqlite3_int64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
  return SQLITE_OK;
}
int currentTime(sqlite3_vfs* vfs, double* output) {
  sqlite3_int64 value = 0;
  const int result = currentTime64(vfs, &value);
  *output = static_cast<double>(value) / 86400000.0;
  return result;
}
int lastError(sqlite3_vfs*, int length, char* output) {
  if (length > 0) std::snprintf(output, length, "HOS result %08x", lastResult);
  return static_cast<int>(lastResult);
}
sqlite3_vfs vfs = {
    2, sizeof(HosFile), FS_MAX_PATH - 1, nullptr, "hos", nullptr, open, remove, access, fullPath,
    nullptr, nullptr, nullptr, nullptr, randomness, sleep, currentTime, lastError, currentTime64,
    nullptr, nullptr, nullptr};
} // namespace

extern "C" int sqlite3_os_init() { return sqlite3_vfs_register(&vfs, 1); }
extern "C" int sqlite3_os_end() { return SQLITE_OK; }