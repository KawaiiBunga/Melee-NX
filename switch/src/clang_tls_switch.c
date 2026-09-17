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

typedef struct {
  void* (*start)(void*);
  void* arg;
  void* stack_buf; /* heap-allocated stack; freed by trampoline on thread exit */
} kartpad_tls_trampoline_ctx;

static void* kartpad_tls_thread_trampoline(void* raw) {
  kartpad_sync_clang_tls();  // first: even allocator internals may use TLS
  kartpad_tls_trampoline_ctx ctx = *(kartpad_tls_trampoline_ctx*)raw;
  free(raw);
  void* result = ctx.start(ctx.arg);
  /* Stack memory is owned by this thread; free after the user function returns
     but before we return to pthreads (the stack is still live at this point
     since we're still executing on it — the free just marks the malloc block
     available; the actual memory won't be re-used until a future allocation
     after this thread is joined and its stack reclaimed by the OS). */
  free(ctx.stack_buf);
  return result;
}

// --- exit() teardown guard via linker --wrap ----------------------------------
// On Switch the process-exit chain — C++ static destructors (__libc_fini_array:
// libstdc++ EH globals, abseil, Dawn, Aurora) plus libnx __appExit service
// teardown — recurses/overflows the small (~1 MB) main-thread stack, crashing
// with a User Break instead of exiting (hardware, 2026-09-13). The OS reclaims
// every handle, mapping, and GPU resource at process exit, so running fini is
// unnecessary here. Convert every exit() (crt0's call when main returns, and any
// library exit()) into a clean _exit() that skips fini. Needs -Wl,--wrap=exit.
// Explicit save/flush must not rely on static destructors. Memory:
// switch-worker-thread-stack-size.
extern void _exit(int) __attribute__((noreturn));

__attribute__((noreturn)) void __wrap_exit(int code) { _exit(code); }

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
  ctx->start = start;
  ctx->arg = arg;
  ctx->stack_buf = NULL;

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
  // (which sets BOTH addr and size). The trampoline frees it after the thread exits.
  size_t want_size = KARTPAD_MIN_THREAD_STACK;
  if (attr != NULL) {
    size_t current = 0;
    pthread_attr_getstacksize(attr, &current);
    if (current >= KARTPAD_MIN_THREAD_STACK) {
      // Caller already requested a large-enough stack; honour their attr.
      int rc = __real_pthread_create(thread, attr, kartpad_tls_thread_trampoline, ctx);
      if (rc != 0) free(ctx);
      return rc;
    }
  }

  // Allocate a page-aligned stack buffer the kernel will actually use.
  void* stack_buf = memalign(4096, want_size);
  if (!stack_buf) {
    free(ctx);
    return EAGAIN;
  }
  ctx->stack_buf = stack_buf;

  pthread_attr_t local_attr;
  if (pthread_attr_init(&local_attr) != 0) {
    free(stack_buf);
    free(ctx);
    return EAGAIN;
  }
  // setstack sets BOTH addr and size — kernel gets a valid non-NULL pointer.
  pthread_attr_setstack(&local_attr, stack_buf, want_size);

  int rc = __real_pthread_create(thread, &local_attr, kartpad_tls_thread_trampoline, ctx);
  pthread_attr_destroy(&local_attr);
  if (rc != 0) {
    free(stack_buf);
    free(ctx);
  }
  return rc;
}
