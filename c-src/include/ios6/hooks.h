#ifndef IOS6_HOOKS_H
#define IOS6_HOOKS_H

#include "ios6/guestmem.h"
#include "ios6/shared_cache.h"
#include <stdint.h>

/*
 * Function hooks: calls into selected guest libSystem functions (malloc, mem*, str*) are
 * intercepted at the JIT dispatcher and executed natively on the host. This avoids needing
 * libSystem's dyld-time initialisation (malloc zones, ...) and NEON-heavy routines.
 */
void hooks_init(guest_arena_t *arena);
int hooks_install_from_cache(shared_cache_t *cache);
/* Returns 1 and performs the "return to caller" when `pc` is a hooked entry point. */
int hooks_dispatch(guest_arena_t *arena, uint32_t *gtc, uint32_t pc);

#endif
