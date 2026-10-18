#ifndef IOS6_GUESTMEM_H
#define IOS6_GUESTMEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/mman.h>

#define MMAP_MIN_ADDR      0x10000UL        /* 64 KiB */
#define GUEST_SPACE_LIMIT  0x100000000UL    /* 4 GiB */
#define ARENA_SIZE         (GUEST_SPACE_LIMIT - MMAP_MIN_ADDR)

typedef uint32_t guest_addr_t;

typedef struct {
    void *base;
    size_t len;
} guest_arena_t;

int guest_arena_reserve(guest_arena_t *arena);
bool guest_arena_contains(const guest_arena_t *arena, guest_addr_t addr);
int guest_arena_mprotect(guest_arena_t *arena, guest_addr_t start, size_t len, int prot);
void *guest_arena_to_host(const guest_arena_t *arena, guest_addr_t addr);

#endif /* IOS6_GUESTMEM_H */
