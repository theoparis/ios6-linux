#include "ios6/guestmem.h"
#include "ios6/jit.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main() {
    guest_arena_t arena;
    if (guest_arena_reserve(&arena) != 0) {
        fprintf(stderr, "Failed to reserve guest arena\n");
        return 1;
    }

    uint32_t test_addr = 0x20000;
    if (guest_arena_mprotect(&arena, test_addr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "Failed to mprotect test memory\n");
        return 1;
    }

    /*
     * ARM instructions:
     *   mov r0, #42        (0xe3a0002a)
     *   add r0, r0, #58    (0xe280003a)
     *   bx lr              (0xe12fff1e)
     */
    uint8_t arm_code[] = {
        0x2a, 0x00, 0xa0, 0xe3,
        0x3a, 0x00, 0x80, 0xe2,
        0x1e, 0xff, 0x2f, 0xe1
    };
    void *ptr = guest_arena_to_host(&arena, test_addr);
    memcpy(ptr, arm_code, sizeof(arm_code));

    jit_runner_t runner;
    if (jit_runner_init(&runner, &arena, test_addr, 0x30000, 0) != 0) {
        fprintf(stderr, "Failed to initialize JIT runner\n");
        return 1;
    }

    runner.gtc[GTC_LR] = 0x4000;

    int step_res = jit_runner_step(&runner);
    printf("Step result: %d\n", step_res);
    printf("GTC: r0 = %u (expected 100), next_pc = 0x%x (expected 0x4000)\n",
           runner.gtc[GTC_R0], runner.gtc[GTC_FIELD_NEXT_PC]);

    assert(step_res == 1);
    assert(runner.gtc[GTC_R0] == 100);
    assert(runner.gtc[GTC_FIELD_NEXT_PC] == 0x4000);

    jit_runner_destroy(&runner);
    printf("C GNU Lightning JIT basic block execution PASSED!\n");
    return 0;
}
