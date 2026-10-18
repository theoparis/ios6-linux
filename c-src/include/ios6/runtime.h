#ifndef IOS6_RUNTIME_H
#define IOS6_RUNTIME_H

#include "ios6/guestmem.h"
#include "ios6/context.h"
#include <stdint.h>
#include <stdbool.h>

#define TLS_REGISTER_ADDR       0x21005000u
#define PROCESS_INFO_BASE       0x21007000u
#define HOST_TRAMPOLINE_BASE    0x22000000u
#define HOST_TRAMPOLINE_STRIDE  8

typedef struct {
    guest_arena_t *arena;
    uint32_t tramp_addrs[256];
    size_t tramp_count;
} host_stubs_t;

int runtime_install_stubs(guest_arena_t *arena, host_stubs_t *stubs);
int runtime_patch_for_cache(guest_arena_t *arena);
uint32_t runtime_resolve_symbol(const char *name, void *user_data);
int runtime_setup_stack(guest_arena_t *arena, int argc, char **argv,
                        uint32_t *out_sp, uint32_t *out_argv_addr);

int runtime_handle_trampoline(guest_arena_t *arena, uint32_t *gtc);
int runtime_handle_syscall(guest_arena_t *arena, uint32_t *gtc);

#endif /* IOS6_RUNTIME_H */
