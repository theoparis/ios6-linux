#include "ios6/guestmem.h"
#include <stdio.h>
#include <errno.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int guest_arena_reserve(guest_arena_t *arena) {
    void *hint = (void *)MMAP_MIN_ADDR;
    size_t len = ARENA_SIZE;

    void *ptr = mmap(hint, len, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                     -1, 0);
    if (ptr == MAP_FAILED) {
        // Fallback if MAP_FIXED_NOREPLACE isn't supported or returns EEXIST:
        ptr = mmap(hint, len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                   -1, 0);
        if (ptr == MAP_FAILED) {
            perror("guest_arena_reserve mmap");
            return -1;
        }
    }

    if ((uintptr_t)ptr != MMAP_MIN_ADDR) {
        fprintf(stderr, "guest_arena_reserve: failed to map at 0x%lx, got %p\n", MMAP_MIN_ADDR, ptr);
        munmap(ptr, len);
        return -1;
    }

    arena->base = ptr;
    arena->len = len;
    return 0;
}

bool guest_arena_contains(const guest_arena_t *arena, guest_addr_t addr) {
    (void)arena;
    return (uintptr_t)addr >= MMAP_MIN_ADDR;
}

int guest_arena_mprotect(guest_arena_t *arena, guest_addr_t start, size_t len, int prot) {
    if (!guest_arena_contains(arena, start)) {
        return -EINVAL;
    }
    if ((uint64_t)start + len > GUEST_SPACE_LIMIT) {
        return -EINVAL;
    }
    // Page-align start and length
    uintptr_t page_mask = 4096 - 1;
    uintptr_t aligned_start = (uintptr_t)start & ~page_mask;
    size_t aligned_len = ((uintptr_t)start + len - aligned_start + page_mask) & ~page_mask;

    if (mprotect((void *)aligned_start, aligned_len, prot) != 0) {
        return -errno;
    }
    return 0;
}

void *guest_arena_to_host(const guest_arena_t *arena, guest_addr_t addr) {
    if (!guest_arena_contains(arena, addr)) {
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}
