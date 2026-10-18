#ifndef IOS6_NEON_H
#define IOS6_NEON_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Interpreted VFP / NEON data-processing.
 *
 * The JIT emits a call to neon_exec() for vector / floating-point instructions it does not
 * translate inline. Operands are pre-decoded at compile time into a neon_op_t that lives as
 * long as the compiled block. The register file is the guest page at NEON_REG_BASE
 * (32 x 64-bit D registers; S/Q registers alias it as on real hardware).
 */

#define NEON_REG_BASE 0x2100a000u

enum { NOP_NONE, NOP_VREG, NOP_CORE, NOP_IMM, NOP_FP, NOP_FPSCR, NOP_APSR };

typedef struct {
    int kind;
    int off;      /* NOP_VREG: byte offset into the register file */
    int size;     /* NOP_VREG: 4 (S), 8 (D) or 16 (Q) */
    int lane;     /* scalar lane index or -1 */
    int core;     /* NOP_CORE: r0..r15 */
    int64_t imm;
    double fp;
} neon_operand_t;

typedef struct {
    char mn[16];      /* base mnemonic without data type, e.g. "vadd" */
    char dt[16];      /* data type suffix, e.g. "f32", "i16", "f64.s32" */
    int nops;
    neon_operand_t ops[6];
    uint32_t addr;
} neon_op_t;

/* Returns false if the instruction is not supported (caller warns at compile time). */
bool neon_supported(const neon_op_t *op);
void neon_exec(uint32_t *gtc, const neon_op_t *op);

#endif
