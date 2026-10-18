#ifndef IOS6_JIT_H
#define IOS6_JIT_H

#include "ios6/context.h"
#include "ios6/guestmem.h"
#include "ios6/disasm.h"
#include <lightning.h>

#define BLOCK_CACHE_CAPACITY 65536

typedef struct {
    uint32_t pc;
    arm_mode_t mode;
    jit_block_fn fn;
    bool occupied;
} block_cache_entry_t;

typedef struct {
    guest_arena_t *arena;
    disassembler_t dis;
    jit_state_t *_jit;
    block_cache_entry_t cache[BLOCK_CACHE_CAPACITY];
    uint32_t gtc[GTC_NUM_FIELDS];
    arm_mode_t mode;
    uint32_t slide;
} jit_runner_t;

int jit_runner_init(jit_runner_t *runner, guest_arena_t *arena,
                    uint32_t entry_point, uint32_t sp, uint32_t slide);
void jit_runner_destroy(jit_runner_t *runner);

jit_block_fn jit_runner_get_or_compile(jit_runner_t *runner, uint32_t pc);
int jit_runner_step(jit_runner_t *runner);
int jit_runner_run(jit_runner_t *runner, size_t max_blocks);
int jit_runner_call(jit_runner_t *runner, uint32_t addr, const uint32_t *args, size_t nargs,
                    size_t max_steps, uint32_t *ret);

#endif /* IOS6_JIT_H */
