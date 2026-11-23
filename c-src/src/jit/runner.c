#include "ios6/jit.h"
#include "ios6/codegen.h"
#include "ios6/runtime.h"
#include "ios6/hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static uint32_t hash_pc(uint32_t pc, arm_mode_t mode) {
    uint32_t h = pc ^ ((uint32_t)mode << 31);
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h & (BLOCK_CACHE_CAPACITY - 1);
}

/* Host SIGSEGV/SIGBUS while running guest code: report where the guest was, then let the
 * default action terminate the process. Only async-signal-safe calls. */
static jit_runner_t *s_fault_runner;

static void fault_write(const char *s) { (void)!write(2, s, strlen(s)); }

static void fault_hex(uint64_t v) {
    char buf[24];
    char *p = buf + sizeof buf - 1;
    *p = '\0';
    do {
        unsigned d = (unsigned)(v & 0xf);
        *--p = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    } while (v);
    *--p = 'x';
    *--p = '0';
    (void)!write(2, p, strlen(p));
}

static void fault_reg(const char *name, uint32_t v) {
    fault_write(" ");
    fault_write(name);
    fault_write("=");
    fault_hex(v);
}

static void fault_handler(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    jit_runner_t *r = s_fault_runner;
    uintptr_t addr = (uintptr_t)si->si_addr;
    fault_write(sig == SIGBUS ? "\nios6: SIGBUS" : "\nios6: SIGSEGV");
    fault_write(" while executing guest block at ");
    fault_hex(r ? r->block_pc : 0);
    fault_write("\n  host address: ");
    fault_hex(addr);
    if (r && addr >= (uintptr_t)r->arena->base &&
        addr - (uintptr_t)r->arena->base < GUEST_SPACE_LIMIT) {
        fault_write("\n  guest address: ");
        fault_hex(addr - (uintptr_t)r->arena->base);
    }
    if (r) {
        fault_write("\n  regs:");
        fault_reg("r0", r->gtc[0]);
        fault_reg("r1", r->gtc[1]);
        fault_reg("r2", r->gtc[2]);
        fault_reg("r3", r->gtc[3]);
        fault_reg("sp", r->gtc[GTC_SP]);
        fault_reg("lr", r->gtc[GTC_LR]);
    }
    fault_write("\n");
}

static void install_fault_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

int jit_runner_init(jit_runner_t *runner, guest_arena_t *arena,
                    uint32_t entry_point, uint32_t sp, uint32_t slide) {
    memset(runner, 0, sizeof(*runner));
    runner->arena = arena;
    runner->slide = slide;
    runner->mode = (entry_point & 1) ? ARM_MODE_THUMB : ARM_MODE_ARM;

    runner->gtc[GTC_SP] = sp;
    runner->gtc[GTC_LR] = 0;
    runner->gtc[GTC_FIELD_NEXT_PC] = entry_point;

    if (disasm_init(&runner->dis) != 0) {
        return -1;
    }

    runner->engine = codegen_engine_create();
    if (!runner->engine) {
        disasm_destroy(&runner->dis);
        return -1;
    }

    s_fault_runner = runner;
    install_fault_handlers();

    return 0;
}

void jit_runner_destroy(jit_runner_t *runner) {
    if (s_fault_runner == runner) s_fault_runner = NULL;
    if (runner->engine) {
        codegen_engine_destroy(runner->engine);
        runner->engine = NULL;
    }
    disasm_destroy(&runner->dis);
}

jit_block_fn jit_runner_get_or_compile(jit_runner_t *runner, uint32_t pc) {
    uint32_t idx = hash_pc(pc, runner->mode);
    for (size_t i = 0; i < 16; i++) {
        size_t slot = (idx + i) & (BLOCK_CACHE_CAPACITY - 1);
        if (runner->cache[slot].occupied &&
            runner->cache[slot].pc == pc &&
            runner->cache[slot].mode == runner->mode) {
            return runner->cache[slot].fn;
        }
        if (!runner->cache[slot].occupied) {
            break;
        }
    }

    // Decode basic block from guest arena
    uint8_t *code = (uint8_t *)guest_arena_to_host(runner->arena, pc);
    if (!code) {
        fprintf(stderr, "jit_runner: address 0x%08x outside guest arena\n", pc);
        return NULL;
    }

    basic_block_t bb;
    if (disasm_decode_block(&runner->dis, code, 4096, pc, runner->mode, &bb) != 0) {
        fprintf(stderr, "jit_runner: failed to disassemble block at 0x%08x\n", pc);
        return NULL;
    }

    jit_block_fn fn = codegen_compile_block(runner->engine, &bb, runner->slide);
    if (!fn) {
        fprintf(stderr, "jit_runner: codegen failed for block at 0x%08x\n", pc);
        return NULL;
    }

    // Insert into cache
    for (size_t i = 0; i < 16; i++) {
        size_t slot = (idx + i) & (BLOCK_CACHE_CAPACITY - 1);
        if (!runner->cache[slot].occupied ||
            (runner->cache[slot].pc == pc && runner->cache[slot].mode == runner->mode)) {
            runner->cache[slot].occupied = true;
            runner->cache[slot].pc = pc;
            runner->cache[slot].mode = runner->mode;
            runner->cache[slot].fn = fn;
            break;
        }
    }

    return fn;
}

int jit_runner_step(jit_runner_t *runner) {
    uint32_t next_pc = runner->gtc[GTC_FIELD_NEXT_PC];
    if (next_pc == 0) {
        // A jump to NULL is an unset (weak) function pointer: treat it as a no-op
        // that returns through LR, as if the callee were an empty stub.
        uint32_t lr = runner->gtc[GTC_LR];
        if (lr == 0) return 0; // Finished / return to host
        if (getenv("IOS6_TRACE")) fprintf(stderr, "[jit] call through NULL pointer, returning to 0x%08x\n", lr);
        runner->gtc[GTC_R0] = 0;
        next_pc = lr;
        runner->gtc[GTC_FIELD_NEXT_PC] = next_pc;
    }
    if (next_pc == SYSCALL_TRAP_PC) {
        runtime_handle_syscall(runner->arena, runner->gtc);
        return 1;
    }
    if ((next_pc & ~1u) >= HOST_TRAMPOLINE_BASE &&
        (next_pc & ~1u) < HOST_TRAMPOLINE_BASE + 0x10000) {
        runtime_handle_trampoline(runner->arena, runner->gtc);
        return 1;
    }

    uint32_t pc = next_pc & ~1u;
    if (hooks_dispatch(runner->arena, runner->gtc, pc)) {
        return 1;
    }
    runner->mode = (next_pc & 1) ? ARM_MODE_THUMB : ARM_MODE_ARM;

    jit_block_fn fn = jit_runner_get_or_compile(runner, pc);
    if (!fn) {
        return -1;
    }

    // Execute compiled block
    runner->block_pc = pc;
    fn(runner->gtc);
    if (getenv("IOS6_TRACE") && getenv("IOS6_TRACE")[0] == '2') {
        printf("JIT Step: pc=0x%08x -> next_pc=0x%08x cpsr=%x", pc, runner->gtc[GTC_FIELD_NEXT_PC], runner->gtc[GTC_FIELD_CPSR]);
        for (int r = 0; r < 15; r++) printf(" r%d=%x", r, runner->gtc[r]);
        printf("\n");
    } else if (getenv("IOS6_TRACE")) {
        printf("JIT Step: pc=0x%08x -> next_pc=0x%08x, r0=0x%x, r1=0x%x, r2=0x%x, r3=0x%x, sp=0x%x, lr=0x%x\n",
               pc, runner->gtc[GTC_FIELD_NEXT_PC], runner->gtc[GTC_R0], runner->gtc[GTC_R1],
               runner->gtc[2], runner->gtc[3],
               runner->gtc[GTC_SP], runner->gtc[GTC_LR]);
    }
    return 1;
}

int jit_runner_run(jit_runner_t *runner, size_t max_blocks) {
    for (size_t i = 0; i < max_blocks; i++) {
        int res = jit_runner_step(runner);
        if (res <= 0) {
            return res;
        }
    }
    return 2; // Max blocks reached
}

/*
 * Run a guest function to completion. The callee returns to LR=0, which the step loop
 * treats as "finished". Returns R0 of the callee, or -1 on failure / step limit.
 */
int jit_runner_call(jit_runner_t *runner, uint32_t addr, const uint32_t *args, size_t nargs,
                    size_t max_steps, uint32_t *ret) {
    uint32_t saved_lr = runner->gtc[GTC_LR];
    uint32_t saved_next = runner->gtc[GTC_FIELD_NEXT_PC];
    for (size_t i = 0; i < 4; i++) {
        runner->gtc[i] = i < nargs ? args[i] : 0;
    }
    runner->gtc[GTC_LR] = 0;
    runner->gtc[GTC_FIELD_NEXT_PC] = addr;

    int rc = -1;
    for (size_t step = 0; step < max_steps; step++) {
        if (runner->gtc[GTC_FIELD_NEXT_PC] == 0) { rc = 0; break; }
        if (jit_runner_step(runner) < 0) break;
    }
    if (rc == 0 && ret) *ret = runner->gtc[GTC_R0];
    runner->gtc[GTC_LR] = saved_lr;
    runner->gtc[GTC_FIELD_NEXT_PC] = saved_next;
    return rc;
}
