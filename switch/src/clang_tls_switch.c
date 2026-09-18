// Clang/libnx TLS reconciliation for Switch (aarch64). Copied verbatim from
// KartPad-NX (switch/src/clang_tls_switch.c) -- this melee-nx copy previously
// carried only the pthread-stack-floor and exit() halves of this file and was
// missing the TLS-reconciliation constructor entirely, which is the piece
// that actually matters here: melee-nx hard-crashed (Data Abort, address 0)
// the first time Dawn/abseil touched a thread_local variable
// (absl::container_internal::RandomSeed's counter, during
// InstanceBase::GatherWGSLFeatures) because tpidr_el0 was never initialized.
//
// clang compiles the ELF `thread_local` ABI as a direct `mrs tpidr_el0`, but
// libnx never writes tpidr_el0 — its GCC path builds TLS with -mtp=soft, which
// emits a *call* to libnx's __aarch64_read_tp() (that reads the ELF TLS base
// from ThreadVars via tpidrro_el0). clang rejects -mtp=soft, so every
// clang-compiled thread_local (Dawn, abseil, and the translated MKW shards)
// reads tpidr_el0 == 0 and takes a null Data Abort on first use.
//
// Fix: on each thread, once, copy libnx's TLS base into tpidr_el0. The linker
// assigns tprel offsets uniformly, so after this both GCC and clang TLS resolve
// to the same slots. Memory: clang-tls-tpidr-el0-fix. Confirmed on hardware
// (2026-09-13) for the main thread; worker threads are covered by the
// pthread_create wrap below.
//
// Coverage:
//   * Main thread — constructor(101) runs before default-priority global ctors
//     (Dawn/abseil static initializers), so a thread_local touched in a ctor is
//     safe. libnx sets up the main thread's ThreadVars in crt0 before
//     __libc_init_array, so __aarch64_read_tp() is valid this early.
//   * Worker threads — __wrap_pthread_create (needs -Wl,--wrap=pthread_create)
//     runs the sync on the new thread before its start routine. libnx populates
//     ThreadVars for spawned threads but still never sets tpidr_el0. std::thread
//     and abseil both bottom out in pthread_create, so the wrap covers them.

#include <pthread.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include <stddef.h>

// libnx/devkitPro's default pthread stack is 128 KiB. Dawn's Tint WGSL compiler
// runs on Aurora's pipeline-worker threads and assumes a host-sized stack: a
// single frame of tint::resolver::AnalyzeUniformity is ~54 KiB and the resolver
// recurses, so 128 KiB overflows (hardware crash 2026-09-13, Data Abort with SP
// below the mapped worker stack). Impose a floor matching Dawn's host assumption
// so every clang std::thread worker — Aurora's compilers now, the translated
// game's threads later — gets a stack Tint can run on. Memory: dawn-nvk-link-punchlist.
#define KARTPAD_MIN_THREAD_STACK ((size_t)8 * 1024 * 1024)

extern void* __aarch64_read_tp(void);

void kartpad_sync_clang_tls(void) {
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(__aarch64_read_tp()) : "memory");
}

// Runs before default-priority (65535) constructors. 101 is the lowest number
// available to user code (0-100 are reserved for the implementation).
__attribute__((constructor(101))) static void kartpad_sync_clang_tls_main(void) {
  kartpad_sync_clang_tls();
}

// --- worker-thread coverage via linker --wrap ---------------------------------
extern int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                 void* (*)(void*), void*);

/* switch/src/main_switch.cpp. Its open/write/fsync/close per line is the only
   sink that survives a death this early, and a pthread_create failure is the
   one thing here worth that cost. */
extern void melee_boot_trace(const char* fmt, ...);

/* Running totals, so a failure report says how far the process got rather than
   just that it stopped. Guarded by the same path that mutates them (every
   caller is inside __wrap_pthread_create). */
static int kartpad_threads_created;
static size_t kartpad_stack_bytes;

/* --- pthread_detach emulation via linker --wrap -------------------------------
   devkitA64's pthread_detach is an unconditional ENOSYS stub -- disassembling
   the linked binary shows both paths falling through to `mov w0, #0x58; ret`,
   so it can never succeed. Every std::thread::detach() therefore throws
   std::system_error, and the first one to do so killed the process with an
   uncaught exception (hardware, 2026-09-18: the unwinder backtrace read
   std::thread::detach -> pc_file_cache_start_prewarm -> melee_main). melee-pc
   detaches in three places -- the file-cache prewarm worker, one thread per
   preloaded file, and Aurora's DvdWorker -- so this is not avoidable.

   A no-op detach would compile and run, but libnx threads only release their
   kernel handle when joined, and pc_file_cache_preload_file spawns one thread
   per file: the leak is unbounded. Emulate a real detach instead. Every
   wrapped thread owns a record; on its way out the thread marks its own record
   finished, and a single reaper joins it -- a join that returns immediately,
   because the thread is already exiting.

   Reaping on completion rather than in the order detach() was called is the
   point: the prewarm worker runs for the whole session, and a FIFO reaper
   would park behind it and never reclaim any of the short-lived preload
   threads queued after it.

   Needs -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_join. */
extern int __real_pthread_join(pthread_t, void**);

typedef struct KpThreadRec {
  pthread_t id;
  void* stack_buf; /* the memalign'd stack below; NULL if the caller supplied one */
  int finished;    /* set by the thread itself, just before it returns */
  int detached;    /* set by __wrap_pthread_detach */
  struct KpThreadRec* next;      /* registry, keyed by id for detach/join lookup */
  struct KpThreadRec* reap_next; /* reap queue, finished-and-detached only */
} KpThreadRec;

static pthread_mutex_t kp_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kp_reap_cond = PTHREAD_COND_INITIALIZER;
static KpThreadRec* kp_registry;
static KpThreadRec* kp_reap_head;
static KpThreadRec* kp_reap_tail;
static int kp_reaper_running;

/* All three helpers below require kp_thread_mutex. */
static KpThreadRec* kp_find_locked(pthread_t id) {
  for (KpThreadRec* r = kp_registry; r != NULL; r = r->next) {
    if (pthread_equal(r->id, id)) {
      return r;
    }
  }
  return NULL;
}

static void kp_unlink_locked(KpThreadRec* rec) {
  KpThreadRec** pp = &kp_registry;
  while (*pp != NULL) {
    if (*pp == rec) {
      *pp = rec->next;
      return;
    }
    pp = &(*pp)->next;
  }
}

static void kp_push_reap_locked(KpThreadRec* rec) {
  rec->reap_next = NULL;
  if (kp_reap_tail != NULL) {
    kp_reap_tail->reap_next = rec;
  } else {
    kp_reap_head = rec;
  }
  kp_reap_tail = rec;
  pthread_cond_signal(&kp_reap_cond);
}

static void* kp_reaper_main(void* unused) {
  (void)unused;
  kartpad_sync_clang_tls();
  for (;;) {
    pthread_mutex_lock(&kp_thread_mutex);
    while (kp_reap_head == NULL) {
      pthread_cond_wait(&kp_reap_cond, &kp_thread_mutex);
    }
    KpThreadRec* rec = kp_reap_head;
    kp_reap_head = rec->reap_next;
    if (kp_reap_head == NULL) {
      kp_reap_tail = NULL;
    }
    kp_unlink_locked(rec);
    pthread_mutex_unlock(&kp_thread_mutex);

    /* The thread set `finished` on its last line, so this releases the kernel
       handle rather than waiting on anything. Only after it returns is the
       stack really dead and safe to hand back to the allocator. */
    __real_pthread_join(rec->id, NULL);
    free(rec->stack_buf);
    free(rec);
  }
  return NULL;
}

int __wrap_pthread_detach(pthread_t thread) {
  int reaper_failed = 0;
  pthread_mutex_lock(&kp_thread_mutex);
  if (!kp_reaper_running) {
    /* __real_, not the wrap: the reaper must not be reaped, and a join loop
       has no use for the 8 MiB stack floor below. */
    pthread_t reaper;
    if (__real_pthread_create(&reaper, NULL, kp_reaper_main, NULL) == 0) {
      kp_reaper_running = 1;
    } else {
      reaper_failed = 1;
    }
  }
  KpThreadRec* rec = kp_find_locked(thread);
  if (rec != NULL) {
    rec->detached = 1;
    if (rec->finished) {
      kp_push_reap_locked(rec);
    }
  }
  pthread_mutex_unlock(&kp_thread_mutex);

  if (reaper_failed) {
    /* Report and carry on: leaking thread handles beats throwing out of
       std::thread::detach(), which is exactly what got us here. */
    melee_boot_trace("!! pthread_detach: reaper thread would not start; handles will leak");
  }
  return 0;
}

int __wrap_pthread_join(pthread_t thread, void** retval) {
  int rc = __real_pthread_join(thread, retval);
  if (rc != 0) {
    return rc;
  }
  pthread_mutex_lock(&kp_thread_mutex);
  KpThreadRec* rec = kp_find_locked(thread);
  /* A detached record belongs to the reaper; joining one is a caller bug, and
     unlinking it here would leave the reap queue pointing at freed memory. */
  if (rec != NULL && rec->detached) {
    rec = NULL;
  }
  if (rec != NULL) {
    kp_unlink_locked(rec);
  }
  pthread_mutex_unlock(&kp_thread_mutex);
  if (rec != NULL) {
    free(rec->stack_buf);
    free(rec);
  }
  return rc;
}

typedef struct {
  void* (*start)(void*);
  void* arg;
  KpThreadRec* rec;
} kartpad_tls_trampoline_ctx;

static void* kartpad_tls_thread_trampoline(void* raw) {
  kartpad_sync_clang_tls();  // first: even allocator internals may use TLS
  kartpad_tls_trampoline_ctx ctx = *(kartpad_tls_trampoline_ctx*)raw;
  free(raw);
  void* result = ctx.start(ctx.arg);
  /* The stack this frame is standing on belongs to ctx.rec. Freeing it here --
     as this trampoline used to -- hands live memory back to the allocator
     while SP still points into it, so the next malloc on any thread can write
     free-list metadata over our own frames. Whoever joins us frees it instead,
     once this thread is genuinely gone. */
  if (ctx.rec != NULL) {
    pthread_mutex_lock(&kp_thread_mutex);
    ctx.rec->finished = 1;
    if (ctx.rec->detached) {
      kp_push_reap_locked(ctx.rec);
    }
    pthread_mutex_unlock(&kp_thread_mutex);
  }
  return result;
}

// --- exit() teardown guard via linker --wrap ----------------------------------
// On Switch the process-exit chain — C++ static destructors (__libc_fini_array:
// libstdc++ EH globals, abseil, Dawn, Aurora) plus libnx __appExit service
// teardown — recurses/overflows the small (~1 MB) main-thread stack, crashing
// with a User Break instead of exiting (hardware, 2026-09-13). The OS reclaims
// every handle, mapping, and GPU resource at process exit, so running fini is
// unnecessary here. Convert every exit() (crt0's call when main returns, and any
// library exit()) into a direct process exit that skips fini. Needs
// -Wl,--wrap=exit. Explicit save/flush must not rely on static destructors.
// Memory: switch-worker-thread-stack-size.
//
// _exit() is NOT enough, and using it produced a crash that looked nothing like
// an exit (hardware, 2026-09-17). _exit() runs libnx's __libnx_exit ->
// __appExit, which unmounts fsdev and NULLs the sdmc entry in newlib's
// devoptab_list, and only then calls svcExitProcess. melee runs nine other
// threads (Aurora's DvdWorker streaming the ISO, the file-cache prewarm worker,
// Aurora's render/pipeline workers, Mesa's util_queue, SDL's input poller) and
// none of them are stopped first. Whichever one touched the SD card inside that
// window took a Data Abort at address 0x30 -- devoptab_t::seek_r read off a
// NULL devoptab -- deep inside newlib's _lseek_r, and Atmosphere reported THAT
// thread. The real reason for the exit never appeared anywhere in the report.
//
// svcExitProcess() kills every thread at once instead: no window, and the
// kernel reclaims the handles __appExit would have closed.
//
// Wrapping exit() alone was NOT enough (hardware, 2026-09-17, second attempt).
// The path that actually runs here is an uncaught C++ exception:
// __cxa_throw -> __cxxabiv1::__terminate -> abort() -> _exit(), and abort()
// never touches exit(). The teardown race came straight back, this time as the
// file-cache prewarm thread Data Aborting at address 0x20 (devoptab_t::write_r
// off a NULL devoptab) inside _write_r, mid-OSReport. Wrap abort() and _exit()
// as well so every route into __libnx_exit is closed. Needs
// -Wl,--wrap=exit -Wl,--wrap=abort -Wl,--wrap=_exit.
//
// fsync() before exiting is what makes the log survive. newlib's write() hands
// the bytes to fsdev, which calls fsFileWrite with FsWriteOption_None -- the
// data sits in Horizon's FS cache and is only committed by fsFileFlush or a
// clean close. Neither happens when the process dies, which is why both
// hardware runs left a 331-byte log that stopped long before the code did,
// even though the message being written at the moment of the crash was visible
// in the Atmosphere stack dump.
#include <stdio.h>
#include <switch.h>
#include <unistd.h>

__attribute__((noreturn)) static void kartpad_fast_exit(void) {
  fflush(NULL);
  /* fd 1 and fd 2 are melee-nx-runtime.log (main_switch.cpp's
     redirect_stdio_to_sd); commit them before the FS session goes away. */
  fsync(STDOUT_FILENO);
  fsync(STDERR_FILENO);
  svcExitProcess();
  __builtin_unreachable();
}

__attribute__((noreturn)) void __wrap_exit(int code) {
  (void)code; /* Horizon has no exit status to report. */
  kartpad_fast_exit();
}

__attribute__((noreturn)) void __wrap__exit(int code) {
  (void)code;
  kartpad_fast_exit();
}

__attribute__((noreturn)) void __wrap_abort(void) { kartpad_fast_exit(); }

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start)(void*), void* arg) {
  kartpad_tls_trampoline_ctx* ctx =
      (kartpad_tls_trampoline_ctx*)malloc(sizeof(*ctx));
  if (!ctx) {
    // pthread_create returns an error number, rather than setting errno. Never
    // launch an unwrapped thread: callers can handle resource exhaustion, but
    // cannot recover from a worker accessing the wrong TLS base.
    return EAGAIN;
  }
  KpThreadRec* rec = (KpThreadRec*)calloc(1, sizeof(*rec));
  if (!rec) {
    free(ctx);
    return EAGAIN;
  }
  ctx->start = start;
  ctx->arg = arg;
  ctx->rec = rec;

  // Enforce the stack floor. std::thread passes attr==NULL (inheriting the
  // 128 KiB libnx default).
  //
  // IMPORTANT (hardware-proven 2026-09-16): calling only pthread_attr_setstacksize()
  // is NOT sufficient on Switch/libsysbase. pthread_create reads attr[0] (stackAddr)
  // and attr[8] (stackSize) and passes BOTH to __syscall_thread_create. When stackAddr
  // is NULL (as set by pthread_attr_init), the Horizon kernel ignores the requested
  // stackSize and allocates its own 128 KB default. All Atmosphere crash reports showed
  // every std::thread at 132 KB even though setstacksize(8MB) was called.
  // FIX: allocate the stack buffer explicitly with memalign and use pthread_attr_setstack
  // (which sets BOTH addr and size). The reaper -- or whoever joins -- frees it after.
  const size_t want_size = KARTPAD_MIN_THREAD_STACK;
  int honour_attr = 0;
  if (attr != NULL) {
    size_t current = 0;
    pthread_attr_getstacksize(attr, &current);
    // Caller already requested a large-enough stack; honour their attr.
    honour_attr = (current >= KARTPAD_MIN_THREAD_STACK);
  }

  pthread_attr_t local_attr;
  const pthread_attr_t* use_attr = attr;
  if (!honour_attr) {
    // Allocate a page-aligned stack buffer the kernel will actually use.
    void* stack_buf = memalign(4096, want_size);
    if (!stack_buf) {
      melee_boot_trace(
          "!! pthread_create: memalign(%zu KiB) failed after %d threads (%zu MiB of stacks)",
          want_size / 1024, kartpad_threads_created, kartpad_stack_bytes / (1024 * 1024));
      free(rec);
      free(ctx);
      return EAGAIN;
    }
    rec->stack_buf = stack_buf;
    if (pthread_attr_init(&local_attr) != 0) {
      free(stack_buf);
      free(rec);
      free(ctx);
      return EAGAIN;
    }
    // setstack sets BOTH addr and size — kernel gets a valid non-NULL pointer.
    pthread_attr_setstack(&local_attr, stack_buf, want_size);
    use_attr = &local_attr;
  }

  /* Publish the record under the same lock the new thread takes on its way
     out, so a thread that finishes instantly cannot race ahead of its own
     registration and leave detach() with nothing to find. */
  pthread_mutex_lock(&kp_thread_mutex);
  int rc = __real_pthread_create(thread, use_attr, kartpad_tls_thread_trampoline, ctx);
  if (rc == 0) {
    rec->id = *thread;
    rec->next = kp_registry;
    kp_registry = rec;
    kartpad_threads_created++;
    kartpad_stack_bytes += rec->stack_buf ? want_size : 0;
  }
  pthread_mutex_unlock(&kp_thread_mutex);

  if (!honour_attr) {
    pthread_attr_destroy(&local_attr);
  }
  if (rc != 0) {
    melee_boot_trace(
        "!! pthread_create: rc=%d with a %zu KiB stack after %d threads (%zu MiB of stacks)", rc,
        want_size / 1024, kartpad_threads_created, kartpad_stack_bytes / (1024 * 1024));
    free(rec->stack_buf);
    free(rec);
    free(ctx);
  }
  return rc;
}
