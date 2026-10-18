/*
 * difftest_jit: JIT side of the differential test against qemu-arm.
 *
 * Reads a spec file produced by tools/difftest/difftest.py:
 *   header:  u32 scratch_words, u32 scratch[scratch_words]   (initial scratch contents)
 *   repeat:  u32 mode(0=ARM,1=Thumb), u32 nbytes, u8 code[round4(nbytes)],
 *            u32 ninputs, then ninputs * { u32 r[13], u32 cpsr }
 * Writes, per input, { u32 r[13], u32 cpsr_nzcv, u32 scratch[scratch_words] } to the output file.
 */
#include "ios6/guestmem.h"
#include "ios6/jit.h"
#include "ios6/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH_BASE 0x40000000u
#define CODE_BASE    0x00200000u
#define CODE_STRIDE  0x100u

static uint32_t rd32(FILE *f) {
    uint32_t v;
    if (fread(&v, 4, 1, f) != 1) { fprintf(stderr, "difftest_jit: short read\n"); exit(2); }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s spec.bin out.bin\n", argv[0]); return 2; }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out) { perror("open"); return 2; }

    guest_arena_t arena;
    if (guest_arena_reserve(&arena) != 0) return 2;
    guest_arena_mprotect(&arena, CODE_BASE, 0x1000000, PROT_READ | PROT_WRITE | PROT_EXEC);
    guest_arena_mprotect(&arena, SCRATCH_BASE, 0x10000, PROT_READ | PROT_WRITE);

    uint32_t nscratch = rd32(in);
    uint32_t *init = malloc(nscratch * 4);
    if (fread(init, 4, nscratch, in) != nscratch) return 2;

    jit_runner_t *r = calloc(1, sizeof *r);
    if (jit_runner_init(r, &arena, CODE_BASE, 0x7ff00000, 0) != 0) return 2;

    uint32_t *scratch = (uint32_t *)guest_arena_to_host(&arena, SCRATCH_BASE);
    uint32_t idx = 0;
    for (;;) {
        uint32_t mode;
        if (fread(&mode, 4, 1, in) != 1) break;
        uint32_t nbytes = rd32(in);
        uint32_t padded = (nbytes + 3) & ~3u;
        uint8_t code[256];
        if (padded > 200) { fprintf(stderr, "code too long\n"); return 2; }
        if (fread(code, 1, padded, in) != padded) return 2;
        uint32_t addr = CODE_BASE + idx++ * CODE_STRIDE;
        uint8_t *host = (uint8_t *)guest_arena_to_host(&arena, addr);
        memcpy(host, code, nbytes);
        if (mode == 0) { uint32_t ret = 0xE1A0F00E; memcpy(host + nbytes, &ret, 4); }  /* mov pc, lr */
        else { uint16_t ret = 0x4770; memcpy(host + nbytes, &ret, 2); }                /* bx lr */

        uint32_t nin = rd32(in);
        for (uint32_t k = 0; k < nin; k++) {
            uint32_t regs[13], cpsr;
            if (fread(regs, 4, 13, in) != 13) return 2;
            cpsr = rd32(in);
            memcpy(scratch, init, nscratch * 4);
            memset(r->gtc, 0, sizeof r->gtc);
            for (int i = 0; i < 13; i++) r->gtc[i] = regs[i];
            r->gtc[GTC_SP] = 0x7ff00000;
            r->gtc[GTC_LR] = 0;
            r->gtc[GTC_FIELD_CPSR] = cpsr & 0xf0000000u;
            r->gtc[GTC_FIELD_NEXT_PC] = addr | (mode ? 1u : 0u);
            for (int steps = 0; r->gtc[GTC_FIELD_NEXT_PC] != 0 && steps < 1000; steps++) {
                if (jit_runner_step(r) < 0) break;
            }
            uint32_t o[14];
            for (int i = 0; i < 13; i++) o[i] = r->gtc[i];
            o[13] = r->gtc[GTC_FIELD_CPSR] & 0xf0000000u;
            fwrite(o, 4, 14, out);
            fwrite(scratch, 4, nscratch, out);
        }
    }
    fclose(out);
    return 0;
}
