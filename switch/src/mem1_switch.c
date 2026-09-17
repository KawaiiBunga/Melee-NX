// MEM1 address reservation for Switch.
//
// melee-pc's Linux build uses -no-pie + -Ttext-segment=0x10000000 so that
// 32-bit disc pointer slots (which store GameCube addresses like 0x80xxxxxx)
// can be relocated to real host addresses. MEM1 is mapped at 0x80000000 and the
// entire 4 GB range is game-addressable.
//
// On Switch, NRO is always PIE and we cannot force text to 0x10000000. Instead
// we reserve the 4 GB guest VA window at startup using mmap MAP_FIXED_NOREPLACE
// (or MAP_FIXED if unavailable on newlib) before melee-pc's main() initializes
// its heap. The disc pointer relocation in melee-pc's src/pc/ code already handles
// this via the MEM1/DISC_PTR macros — as long as the mapping is in place before
// any disc struct is accessed, it works.
//
// Called from main_switch.cpp before melee_main_impl().

#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>

#define MELEE_GUEST_BASE  ((void*)0x80000000ULL)
#define MELEE_GUEST_SIZE  (0x01800000ULL)  // 24 MiB: MEM1 (24 MiB GameCube RAM)

// Returns 0 on success, -1 on failure.
int melee_switch_reserve_mem1(void) {
    // MAP_FIXED: Switch's 64-bit address space (39-bit user VA on HOS) makes
    // 0x80000000 available. MAP_ANONYMOUS | PROT_READ | PROT_WRITE.
    // melee-pc will later re-mmap this region with its own allocator.
    void* p = mmap(MELEE_GUEST_BASE,
                   MELEE_GUEST_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                   -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[melee-nx] FATAL: failed to reserve MEM1 at 0x80000000\n");
        return -1;
    }
    if (p != MELEE_GUEST_BASE) {
        fprintf(stderr, "[melee-nx] FATAL: mmap returned %p instead of 0x80000000\n", p);
        return -1;
    }
    return 0;
}
