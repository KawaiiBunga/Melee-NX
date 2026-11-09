// malloc without devkitA64's forced 32-byte alignment. Its newlib malloc hands every request over
// 31 bytes to _memalign_r(32, n), which asks _malloc_r for n + 64 and frees the misaligned head and
// the tail it cuts off: a 48- or 64-byte free chunk wedged between live blocks per allocation. Over
// a session the free lists fill with them, and every _malloc_r and _free_r walks longer bins under
// newlib's one heap lock. KartPadNX measured it on the same Dawn: the frame worker's 664-byte
// RenderPassEncoder per render pass took allocator code from 4% to 47% of its core over ten races,
// and the game thread and the GX worker queued on the lock behind it.
// _malloc_r's own blocks are 16-byte aligned: alignof(max_align_t) on AArch64, and what calloc and
// realloc already return. Aligned new, memalign and aligned_alloc still go to _memalign_r.
// Linked with -Wl,--wrap=malloc (switch/CMakeLists.txt, option MELEE_NX_MALLOC_WRAP).
#include <stddef.h>
#include <sys/reent.h>

void* _malloc_r(struct _reent*, size_t);

void* __wrap_malloc(size_t n) {
  return _malloc_r(_REENT, n);
}
