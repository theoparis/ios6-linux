#include "ios6/codegen.h"
#include "ios6/neon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREDICATED_SCRATCH_ADDR 0x00010000u
#define TLS_REGISTER_ADDR       0x21005000u

/*
 * Memory offset helper:
 * In the generated block function `void block_fn(uint32_t *gtc)`,
 * JIT_V0 holds the base pointer `gtc`.
 * Register i (0..14) is at `gtc[i] * 4`.
 * CPSR is at `gtc[15] * 4`.
 * NEXT_PC is at `gtc[16] * 4`.
 * TRAP_RESUME is at `gtc[17] * 4`.
 */

#define GTC_BYTE_OFF(field) ((field) * 4)

static void emit_load_reg(jit_state_t *_jit, jit_gpr_t dst, int reg_num) {
    if (reg_num >= 0 && reg_num <= 14) {
        jit_ldxi_ui(dst, JIT_V0, GTC_BYTE_OFF(reg_num));
    } else {
        jit_movi(dst, 0);
    }
}

static void emit_store_reg(jit_state_t *_jit, int reg_num, jit_gpr_t src) {
    if (reg_num >= 0 && reg_num <= 14) {
        jit_stxi_i(GTC_BYTE_OFF(reg_num), JIT_V0, src);
    }
}

static void emit_load_next_pc(jit_state_t *_jit, jit_gpr_t dst) {
    jit_ldxi_ui(dst, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_NEXT_PC));
}

static void emit_store_next_pc(jit_state_t *_jit, jit_gpr_t src) {
    jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_NEXT_PC), JIT_V0, src);
}

static void emit_store_next_pc_imm(jit_state_t *_jit, uint32_t imm) {
    jit_movi(JIT_R0, imm);
    jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_NEXT_PC), JIT_V0, JIT_R0);
}

static arm_cc get_branch_condition(const char *m, arm_cc default_cc) {
    if (strcmp(m, "beq") == 0) return ARM_CC_EQ;
    if (strcmp(m, "bne") == 0) return ARM_CC_NE;
    if (strcmp(m, "bgt") == 0) return ARM_CC_GT;
    if (strcmp(m, "blt") == 0) return ARM_CC_LT;
    if (strcmp(m, "bge") == 0) return ARM_CC_GE;
    if (strcmp(m, "ble") == 0) return ARM_CC_LE;
    if (strcmp(m, "bhs") == 0 || strcmp(m, "bcs") == 0) return ARM_CC_HS;
    if (strcmp(m, "blo") == 0 || strcmp(m, "bcc") == 0) return ARM_CC_LO;
    if (strcmp(m, "bmi") == 0) return ARM_CC_MI;
    if (strcmp(m, "bpl") == 0) return ARM_CC_PL;
    if (strcmp(m, "bvs") == 0) return ARM_CC_VS;
    if (strcmp(m, "bvc") == 0) return ARM_CC_VC;
    if (strcmp(m, "bhi") == 0) return ARM_CC_HI;
    if (strcmp(m, "bls") == 0) return ARM_CC_LS;
    return default_cc;
}

// Maps Capstone register to 0..15 or -1
static int arm_reg_to_num(arm_reg r) {
    if (r >= ARM_REG_R0 && r <= ARM_REG_R12) {
        return r - ARM_REG_R0;
    }
    switch (r) {
        case ARM_REG_SP: return 13;
        case ARM_REG_LR: return 14;
        case ARM_REG_PC: return 15;
        default: return -1;
    }
}

/*
 * Evaluates condition code `cc` based on CPSR in GTC.
 * Returns a jit_node_t* branch (jump if condition is FALSE).
 * The caller must jit_patch() this label to jump past the predicated instruction(s).
 */
static jit_node_t *emit_branch_if_cc_false(jit_state_t *_jit, arm_cc cc) {
    if (cc == ARM_CC_AL || cc == ARM_CC_INVALID) {
        return NULL; // Always executes
    }
    // R1 = NZCV in bits 3..0;  R0 = 1 when the condition holds
    jit_ldxi_ui(JIT_R1, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
    jit_rshi_u(JIT_R1, JIT_R1, 28);
    switch (cc) {
        case ARM_CC_EQ: jit_rshi_u(JIT_R0, JIT_R1, 2); jit_andi(JIT_R0, JIT_R0, 1); break;          // Z
        case ARM_CC_NE: jit_rshi_u(JIT_R0, JIT_R1, 2); jit_andi(JIT_R0, JIT_R0, 1); jit_xori(JIT_R0, JIT_R0, 1); break;
        case ARM_CC_HS: jit_rshi_u(JIT_R0, JIT_R1, 1); jit_andi(JIT_R0, JIT_R0, 1); break;          // C
        case ARM_CC_LO: jit_rshi_u(JIT_R0, JIT_R1, 1); jit_andi(JIT_R0, JIT_R0, 1); jit_xori(JIT_R0, JIT_R0, 1); break;
        case ARM_CC_MI: jit_rshi_u(JIT_R0, JIT_R1, 3); jit_andi(JIT_R0, JIT_R0, 1); break;          // N
        case ARM_CC_PL: jit_rshi_u(JIT_R0, JIT_R1, 3); jit_andi(JIT_R0, JIT_R0, 1); jit_xori(JIT_R0, JIT_R0, 1); break;
        case ARM_CC_VS: jit_andi(JIT_R0, JIT_R1, 1); break;                                          // V
        case ARM_CC_VC: jit_andi(JIT_R0, JIT_R1, 1); jit_xori(JIT_R0, JIT_R0, 1); break;
        case ARM_CC_HI: // C && !Z  <=>  (bits Z,C) == 01
        case ARM_CC_LS:
            jit_andi(JIT_R0, JIT_R1, 6);
            jit_eqi(JIT_R0, JIT_R0, 2);
            if (cc == ARM_CC_LS) jit_xori(JIT_R0, JIT_R0, 1);
            break;
        case ARM_CC_GE: // N == V
        case ARM_CC_LT:
        case ARM_CC_GT: // !Z && N == V
        case ARM_CC_LE:
            jit_rshi_u(JIT_R0, JIT_R1, 3);
            jit_xorr(JIT_R0, JIT_R0, JIT_R1);
            jit_andi(JIT_R0, JIT_R0, 1);            // R0 = N ^ V
            jit_xori(JIT_R0, JIT_R0, 1);            // R0 = (N == V)
            if (cc == ARM_CC_GT || cc == ARM_CC_LE) {
                jit_rshi_u(JIT_R2, JIT_R1, 2);
                jit_andi(JIT_R2, JIT_R2, 1);
                jit_xori(JIT_R2, JIT_R2, 1);        // !Z
                jit_andr(JIT_R0, JIT_R0, JIT_R2);
            }
            if (cc == ARM_CC_LT || cc == ARM_CC_LE) jit_xori(JIT_R0, JIT_R0, 1);
            break;
        default:
            return NULL;
    }
    return jit_beqi(JIT_R0, 0);
}

// Resolves operand to a register (places into `dst`)
static void emit_resolve_operand(jit_state_t *_jit, const cs_arm_op *op,
                                 jit_gpr_t dst, uint32_t pc_val) {
    if (op->type == ARM_OP_IMM) {
        jit_movi(dst, (uint32_t)op->imm);
    } else if (op->type == ARM_OP_REG) {
        int r = arm_reg_to_num(op->reg);
        if (r == 15) {
            jit_movi(dst, pc_val);
        } else {
            emit_load_reg(_jit, dst, r);
        }
    } else {
        jit_movi(dst, 0);
    }

    // Handle shift (computed on the 32-bit value; result re-masked to 32 bits)
    arm_shifter st = op->shift.type;
    if (st == ARM_SFT_INVALID) return;
    bool by_reg = st == ARM_SFT_ASR_REG || st == ARM_SFT_LSL_REG || st == ARM_SFT_LSR_REG ||
                  st == ARM_SFT_ROR_REG || st == ARM_SFT_RRX_REG;
    if (by_reg) {
        // amount = Rs & 0xff, clamped to 63 so 64-bit host shifts saturate correctly
        emit_load_reg(_jit, JIT_V1, arm_reg_to_num((arm_reg)op->shift.value));
        jit_andi(JIT_V1, JIT_V1, 0xff);
        if (st == ARM_SFT_ROR_REG) {
            jit_andi(JIT_V1, JIT_V1, 31);
        } else {
            jit_node_t *ok = jit_blei_u(JIT_V1, 63);
            jit_movi(JIT_V1, 63);
            jit_patch(ok);
        }
        switch (st) {
            case ARM_SFT_LSL_REG: jit_lshr(dst, dst, JIT_V1); break;
            case ARM_SFT_LSR_REG: jit_rshr_u(dst, dst, JIT_V1); break;
            case ARM_SFT_ASR_REG: jit_extr_i(dst, dst); jit_rshr(dst, dst, JIT_V1); break;
            case ARM_SFT_ROR_REG:
                jit_rshr_u(JIT_V2, dst, JIT_V1);
                jit_rsbi(JIT_V1, JIT_V1, 32);
                jit_lshr(dst, dst, JIT_V1);
                jit_orr(dst, dst, JIT_V2);
                break;
            default: break;
        }
        jit_andi(dst, dst, 0xffffffff);
        return;
    }
    uint32_t amt = op->shift.value;
    switch (st) {
        case ARM_SFT_ASR:
            jit_extr_i(dst, dst);
            jit_rshi(dst, dst, amt >= 32 || amt == 0 ? 31 : amt); // ASR #32 encodes as 0/32
            break;
        case ARM_SFT_LSL:
            if (amt >= 32) jit_movi(dst, 0); else if (amt) jit_lshi(dst, dst, amt);
            break;
        case ARM_SFT_LSR:
            if (amt >= 32 || amt == 0) jit_movi(dst, 0); else jit_rshi_u(dst, dst, amt); // LSR #32
            break;
        case ARM_SFT_ROR: {
            uint32_t rot = amt & 31;
            if (rot) {
                jit_rshi_u(JIT_V2, dst, rot);
                jit_lshi(dst, dst, 32 - rot);
                jit_orr(dst, dst, JIT_V2);
            }
            break;
        }
        case ARM_SFT_RRX:
            jit_rshi_u(dst, dst, 1);
            jit_ldxi_ui(JIT_V2, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
            jit_andi(JIT_V2, JIT_V2, CPSR_C);
            jit_lshi(JIT_V2, JIT_V2, 31 - 29); // C is bit 29 -> bit 31
            jit_orr(dst, dst, JIT_V2);
            break;
        default: break;
    }
    jit_andi(dst, dst, 0xffffffff);
}

// Compute NZCV for ADD (dst = op1 + op2)
static void emit_compute_add_flags(jit_state_t *_jit, jit_gpr_t res, jit_gpr_t a, jit_gpr_t b) {
    // Truncate to 32 bits on 64-bit host
    jit_andi(res, res, 0xffffffff);
    jit_andi(a, a, 0xffffffff);
    jit_andi(b, b, 0xffffffff);

    // Save res, a, b across clobbers using JIT_V1..V3
    jit_movr(JIT_V1, res);
    jit_movr(JIT_V2, a);
    jit_movr(JIT_V3, b);

    // N: (res >> 31) & 1
    jit_rshi_u(JIT_R2, JIT_V1, 31);
    jit_lshi(JIT_R2, JIT_R2, 31); // N in bit 31

    // Z: res == 0
    jit_node_t *not_zero = jit_bnei(JIT_V1, 0);
    jit_ori(JIT_R2, JIT_R2, CPSR_Z); // set Z
    jit_patch(not_zero);

    // C: carry out for unsigned a + b: res < a (unsigned)
    jit_node_t *no_carry = jit_bger_u(JIT_V1, JIT_V2);
    jit_ori(JIT_R2, JIT_R2, CPSR_C);
    jit_patch(no_carry);

    // V: overflow for signed a + b: ((a ^ res) & (b ^ res)) < 0 in 32-bit
    jit_xorr(JIT_R0, JIT_V2, JIT_V1);
    jit_xorr(JIT_R1, JIT_V3, JIT_V1);
    jit_andr(JIT_R0, JIT_R0, JIT_R1);
    jit_rshi_u(JIT_R0, JIT_R0, 31);   /* NB: jit_andi(x, 0x80000000) is miscompiled by Lightning x86-64 */
    jit_andi(JIT_R0, JIT_R0, 1);
    jit_node_t *no_ovf = jit_beqi(JIT_R0, 0);
    jit_ori(JIT_R2, JIT_R2, CPSR_V);
    jit_patch(no_ovf);

    // Update CPSR in GTC
    jit_ldxi_ui(JIT_R0, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
    jit_andi(JIT_R0, JIT_R0, ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V));
    jit_orr(JIT_R0, JIT_R0, JIT_R2);
    jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_CPSR), JIT_V0, JIT_R0);
}

// Compute NZCV for SUB (dst = op1 - op2)
static void emit_compute_sub_flags(jit_state_t *_jit, jit_gpr_t res, jit_gpr_t a, jit_gpr_t b) {
    // Truncate to 32 bits on 64-bit host
    jit_andi(res, res, 0xffffffff);
    jit_andi(a, a, 0xffffffff);
    jit_andi(b, b, 0xffffffff);

    jit_movr(JIT_V1, res);
    jit_movr(JIT_V2, a);
    jit_movr(JIT_V3, b);

    // N: (res >> 31) & 1
    jit_rshi_u(JIT_R2, JIT_V1, 31);
    jit_lshi(JIT_R2, JIT_R2, 31);

    // Z: res == 0
    jit_node_t *not_zero = jit_bnei(JIT_V1, 0);
    jit_ori(JIT_R2, JIT_R2, CPSR_Z);
    jit_patch(not_zero);

    // C: not borrow: a >= b (unsigned)
    jit_node_t *borrow = jit_bltr_u(JIT_V2, JIT_V3);
    jit_ori(JIT_R2, JIT_R2, CPSR_C);
    jit_patch(borrow);

    // V: signed overflow: ((a ^ b) & (a ^ res)) < 0 in 32-bit
    jit_xorr(JIT_R0, JIT_V2, JIT_V3);
    jit_xorr(JIT_R1, JIT_V2, JIT_V1);
    jit_andr(JIT_R0, JIT_R0, JIT_R1);
    jit_rshi_u(JIT_R0, JIT_R0, 31);   /* NB: jit_andi(x, 0x80000000) is miscompiled by Lightning x86-64 */
    jit_andi(JIT_R0, JIT_R0, 1);
    jit_node_t *no_ovf = jit_beqi(JIT_R0, 0);
    jit_ori(JIT_R2, JIT_R2, CPSR_V);
    jit_patch(no_ovf);

    // Update CPSR
    jit_ldxi_ui(JIT_R0, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
    jit_andi(JIT_R0, JIT_R0, ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V));
    jit_orr(JIT_R0, JIT_R0, JIT_R2);
    jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_CPSR), JIT_V0, JIT_R0);
}

// Compute NZ for logic instructions
static void emit_compute_nz_flags(jit_state_t *_jit, jit_gpr_t res) {
    jit_andi(res, res, 0xffffffff);
    jit_rshi_u(JIT_R2, res, 31);
    jit_lshi(JIT_R2, JIT_R2, 31); // N

    jit_node_t *not_zero = jit_bnei(res, 0);
    jit_ori(JIT_R2, JIT_R2, CPSR_Z); // Z
    jit_patch(not_zero);

    jit_ldxi_ui(JIT_R0, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
    jit_andi(JIT_R0, JIT_R0, ~(CPSR_N | CPSR_Z));
    jit_orr(JIT_R0, JIT_R0, JIT_R2);
    jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_CPSR), JIT_V0, JIT_R0);
}

/*
 * Compiles a block into Lightning machine code.
 */
/* Apply the shift of a register-offset memory operand (e.g. [r1, r0, lsl #3]) to JIT_R1. */
static void emit_mem_index_shift(jit_state_t *_jit, const cs_arm_op *op) {
    uint32_t amt = op->mem.lshift;
    arm_shifter type = ARM_SFT_LSL;
    if (amt == 0 && op->shift.type != ARM_SFT_INVALID && op->shift.value > 0) {
        amt = op->shift.value;
        type = op->shift.type;
    }
    if (amt == 0) return;
    switch (type) {
        case ARM_SFT_LSR: jit_rshi_u(JIT_R1, JIT_R1, amt); break;
        case ARM_SFT_ASR: jit_extr_i(JIT_R1, JIT_R1); jit_rshi(JIT_R1, JIT_R1, amt); break;
        default: jit_lshi(JIT_R1, JIT_R1, amt); break;
    }
}

/* For post-indexed ldr/str ([rn], #imm / [rn], rm) JIT_R0 holds the unmodified base: apply the offset. */
static void emit_post_index_offset(jit_state_t *_jit, const disassembled_insn_t *insn) {
    uint32_t op_count = insn->arm_detail.op_count;
    if (op_count == 0) return;
    const cs_arm_op *last = &insn->arm_detail.operands[op_count - 1];
    if (last->type == ARM_OP_MEM) return; /* pre-indexed: JIT_R0 already holds the new base */
    if (last->type == ARM_OP_IMM) {
        if (last->subtracted) jit_subi(JIT_R0, JIT_R0, (int32_t)last->imm);
        else jit_addi(JIT_R0, JIT_R0, (int32_t)last->imm);
    } else if (last->type == ARM_OP_REG) {
        int r = arm_reg_to_num(last->reg);
        emit_load_reg(_jit, JIT_R1, r);
        emit_mem_index_shift(_jit, last);
        if (last->subtracted) jit_subr(JIT_R0, JIT_R0, JIT_R1);
        else jit_addr(JIT_R0, JIT_R0, JIT_R1);
    }
    jit_extr_ui(JIT_R0, JIT_R0);
}

/* VFP/NEON register file offset for a capstone register, or -1. Q=16B, D=8B, S=4B. */
static int vreg_off(arm_reg r, int *size) {
    if (r >= ARM_REG_D0 && r <= ARM_REG_D31) { *size = 8; return (int)(r - ARM_REG_D0) * 8; }
    if (r >= ARM_REG_S0 && r <= ARM_REG_S31) { *size = 4; return (int)(r - ARM_REG_S0) * 4; }
    if (r >= ARM_REG_Q0 && r <= ARM_REG_Q15) { *size = 16; return (int)(r - ARM_REG_Q0) * 16; }
    return -1;
}

/* Copy `size` bytes between guest memory at JIT_R2+moff and the vreg file. Uses R0/R1. */
static void emit_vreg_xfer(jit_state_t *_jit, bool load, int voff, int size, int32_t moff) {
    jit_movi(JIT_R0, NEON_REG_BASE + (uint32_t)voff);
    for (int w = 0; w < size; w += 4) {
        if (load) { jit_ldxi_ui(JIT_R1, JIT_R2, moff + w); jit_stxi_i(w, JIT_R0, JIT_R1); }
        else      { jit_ldxi_ui(JIT_R1, JIT_R0, w); jit_stxi_i(moff + w, JIT_R2, JIT_R1); }
    }
}

/* Handles VFP/NEON memory + core<->vreg moves. Returns true if the insn was consumed. */
static bool emit_vfp_insn(jit_state_t *_jit, const disassembled_insn_t *insn, const char *mn, uint32_t pc_val) {
    const cs_arm *d = &insn->arm_detail;
    int sz;
    if (strstr(insn->op_str, "[]")) return false;
    for (int i = 0; i < d->op_count; i++) if (d->operands[i].vector_index >= 0) return false;
    bool is_vld1 = !strncmp(mn, "vld1", 4), is_vst1 = !strncmp(mn, "vst1", 4);
    bool is_push = !strcmp(mn, "vpush"), is_pop = !strcmp(mn, "vpop");
    bool is_vldm = !strncmp(mn, "vldm", 4), is_vstm = !strncmp(mn, "vstm", 4);
    if (is_vld1 || is_vst1) {
        // {dN, ...}, [rn(:align)] {!} {, rm}
        int mi = -1;
        for (int i = 0; i < d->op_count; i++) if (d->operands[i].type == ARM_OP_MEM) { mi = i; break; }
        if (mi < 0) return false;
        int rn = arm_reg_to_num(d->operands[mi].mem.base);
        emit_load_reg(_jit, JIT_R2, rn);
        int32_t total = 0;
        for (int i = 0; i < mi; i++) {
            int off = vreg_off(d->operands[i].reg, &sz);
            if (off < 0) return false;
            emit_vreg_xfer(_jit, is_vld1, off, sz, total);
            total += sz;
        }
        if (mi + 1 < d->op_count && d->operands[mi + 1].type == ARM_OP_REG) {
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(d->operands[mi + 1].reg));
            jit_addr(JIT_R2, JIT_R2, JIT_R1);
            emit_store_reg(_jit, rn, JIT_R2);
        } else if (insn->writeback) {
            jit_addi(JIT_R2, JIT_R2, total);
            emit_store_reg(_jit, rn, JIT_R2);
        }
        return true;
    }
    if (is_push || is_pop || is_vldm || is_vstm) {
        int first = (is_push || is_pop) ? 0 : 1;
        int rn = (is_push || is_pop) ? 13 : arm_reg_to_num(d->operands[0].reg);
        bool load = is_pop || is_vldm;
        bool db = is_push || strstr(mn, "db") != NULL;
        int32_t total = 0;
        for (int i = first; i < d->op_count; i++) { if (vreg_off(d->operands[i].reg, &sz) < 0) return false; total += sz; }
        emit_load_reg(_jit, JIT_R2, rn);
        if (db) jit_subi(JIT_R2, JIT_R2, total);
        int32_t o = 0;
        for (int i = first; i < d->op_count; i++) {
            int off = vreg_off(d->operands[i].reg, &sz);
            emit_vreg_xfer(_jit, load, off, sz, o);
            o += sz;
        }
        if (is_push || is_pop || insn->writeback) {
            if (!db) jit_addi(JIT_R2, JIT_R2, total);
            emit_store_reg(_jit, rn, JIT_R2);
        }
        return true;
    }
    if (!strcmp(mn, "vdup") && d->op_count == 2 && d->operands[1].type == ARM_OP_REG &&
        arm_reg_to_num(d->operands[1].reg) >= 0) {
        // vdup.<8|16|32> dN/qN, rt : replicate the low element of rt across the register
        int off = vreg_off(d->operands[0].reg, &sz);
        if (off < 0) return false;
        const char *dot = strchr(insn->mnemonic, '.');
        int esz = dot ? atoi(dot + 1) : 32;
        emit_load_reg(_jit, JIT_R1, arm_reg_to_num(d->operands[1].reg));
        if (esz == 8) { jit_andi(JIT_R1, JIT_R1, 0xff); jit_muli(JIT_R1, JIT_R1, 0x01010101); }
        else if (esz == 16) { jit_andi(JIT_R1, JIT_R1, 0xffff); jit_muli(JIT_R1, JIT_R1, 0x00010001); }
        jit_movi(JIT_R0, NEON_REG_BASE + (uint32_t)off);
        for (int w = 0; w < sz; w += 4) jit_stxi_i(w, JIT_R0, JIT_R1);
        return true;
    }
    if (!strcmp(mn, "vldr") || !strcmp(mn, "vstr")) {
        int off = vreg_off(d->operands[0].reg, &sz);
        if (off < 0) return false;
        const cs_arm_op *m = &d->operands[1];
        int rn = arm_reg_to_num(m->mem.base);
        if (rn == 15) jit_movi(JIT_R2, pc_val & ~3u); else emit_load_reg(_jit, JIT_R2, rn);
        emit_vreg_xfer(_jit, mn[1] == 'l', off, sz, m->mem.disp);
        return true;
    }
    if (!strcmp(mn, "vmov") && d->op_count == 2 && d->operands[1].type == ARM_OP_IMM) {
        // vmov.i8/i16/i32 dN/qN, #imm : replicate immediate per element
        int off = vreg_off(d->operands[0].reg, &sz);
        if (off < 0 || sz < 8) return false;
        const char *dot = strchr(insn->mnemonic, '.');
        if (!dot || dot[1] != 'i') return false;
        int esz = atoi(dot + 2);
        uint32_t v = (uint32_t)d->operands[1].imm, word;
        if (esz == 8) word = (v & 0xff) * 0x01010101u;
        else if (esz == 16) word = (v & 0xffff) * 0x00010001u;
        else if (esz == 32) word = v;
        else return false;
        jit_movi(JIT_R0, NEON_REG_BASE + (uint32_t)off);
        jit_movi(JIT_R1, word);
        for (int w = 0; w < sz; w += 4) jit_stxi_i(w, JIT_R0, JIT_R1);
        return true;
    }
    if (!strcmp(mn, "vmov")) {
        // vmov rt, sN | vmov sN, rt | vmov rt, rt2, dN | vmov dN, rt, rt2 | vmov dN, dM | vmov qN, qM
        const cs_arm_op *o = d->operands;
        int n = d->op_count;
        for (int i = 0; i < n; i++) if (o[i].type != ARM_OP_REG) return false; // immediate forms: unsupported
        int a, b;
        if (n == 2 && (a = vreg_off(o[0].reg, &sz)) >= 0 && (b = vreg_off(o[1].reg, &sz)) >= 0) {
            jit_movi(JIT_R0, NEON_REG_BASE);
            for (int w = 0; w < sz; w += 4) { jit_ldxi_ui(JIT_R1, JIT_R0, b + w); jit_stxi_i(a + w, JIT_R0, JIT_R1); }
            return true;
        }
        if (n == 2 && (a = vreg_off(o[1].reg, &sz)) >= 0) { // core <- vreg
            jit_movi(JIT_R0, NEON_REG_BASE + (uint32_t)a);
            jit_ldr_ui(JIT_R1, JIT_R0);
            emit_store_reg(_jit, arm_reg_to_num(o[0].reg), JIT_R1);
            return true;
        }
        if (n == 2 && (a = vreg_off(o[0].reg, &sz)) >= 0) { // vreg <- core
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[1].reg));
            jit_movi(JIT_R0, NEON_REG_BASE + (uint32_t)a);
            jit_str_i(JIT_R0, JIT_R1);
            return true;
        }
        if (n == 3 && (a = vreg_off(o[2].reg, &sz)) >= 0) { // rt, rt2 <- dN
            jit_movi(JIT_R0, NEON_REG_BASE + (uint32_t)a);
            jit_extr_ui(JIT_R0, JIT_R0);
            jit_ldxi_ui(JIT_R1, JIT_R0, 0); emit_store_reg(_jit, arm_reg_to_num(o[0].reg), JIT_R1);
            jit_ldxi_ui(JIT_R1, JIT_R0, 4); emit_store_reg(_jit, arm_reg_to_num(o[1].reg), JIT_R1);
            return true;
        }
        if (n == 3 && (a = vreg_off(o[0].reg, &sz)) >= 0) { // dN <- rt, rt2
            jit_movi(JIT_R0, NEON_REG_BASE + (uint32_t)a);
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[1].reg)); jit_stxi_i(0, JIT_R0, JIT_R1);
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[2].reg)); jit_stxi_i(4, JIT_R0, JIT_R1);
            return true;
        }
    }
    return false;
}

/* ADC-style op: res = a + b + C (a, b in R0/R1, zero-extended). Result in R2; sets NZCV if `flags`. */
static void emit_adc_core(jit_state_t *_jit, bool flags) {
    jit_andi(JIT_R0, JIT_R0, 0xffffffff);
    jit_andi(JIT_R1, JIT_R1, 0xffffffff);
    jit_ldxi_ui(JIT_V3, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
    jit_rshi_u(JIT_V3, JIT_V3, 29);
    jit_andi(JIT_V3, JIT_V3, 1);                 // carry in
    jit_addr(JIT_V1, JIT_R0, JIT_R1);
    jit_addr(JIT_V1, JIT_V1, JIT_V3);            // 33-bit sum
    jit_andi(JIT_R2, JIT_V1, 0xffffffff);
    if (!flags) return;
    jit_rshi_u(JIT_V2, JIT_V1, 32);
    jit_lshi(JIT_V2, JIT_V2, 29);                // C
    jit_rshi_u(JIT_V3, JIT_R2, 31);
    jit_lshi(JIT_V3, JIT_V3, 31);                // N
    jit_orr(JIT_V2, JIT_V2, JIT_V3);
    jit_node_t *nz = jit_bnei(JIT_R2, 0);
    jit_ori(JIT_V2, JIT_V2, CPSR_Z);
    jit_patch(nz);
    // V = (~(a ^ b) & (a ^ res)) bit 31
    jit_xorr(JIT_V1, JIT_R0, JIT_R1);
    jit_xori(JIT_V1, JIT_V1, 0xffffffff);
    jit_xorr(JIT_V3, JIT_R0, JIT_R2);
    jit_andr(JIT_V1, JIT_V1, JIT_V3);
    jit_rshi_u(JIT_V1, JIT_V1, 31);
    jit_andi(JIT_V1, JIT_V1, 1);
    jit_lshi(JIT_V1, JIT_V1, 28);                // V
    jit_orr(JIT_V2, JIT_V2, JIT_V1);
    jit_ldxi_ui(JIT_V1, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
    jit_andi(JIT_V1, JIT_V1, ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V));
    jit_orr(JIT_V1, JIT_V1, JIT_V2);
    jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_CPSR), JIT_V0, JIT_V1);
}

/* Build an interpreted NEON/VFP op; returns NULL if the helper cannot execute it. */
static neon_op_t *build_neon_op(const disassembled_insn_t *insn, const char *base_mn) {
    neon_op_t *op = calloc(1, sizeof *op);
    if (!op) return NULL;
    strncpy(op->mn, base_mn, sizeof op->mn - 1);
    const char *dot = strchr(insn->mnemonic, '.');
    if (dot) strncpy(op->dt, dot + 1, sizeof op->dt - 1);
    op->addr = insn->address;
    const cs_arm *d = &insn->arm_detail;
    bool dup_lanes = strstr(insn->op_str, "[]") != NULL;
    for (int i = 0; i < d->op_count && op->nops < 6; i++) {
        const cs_arm_op *o = &d->operands[i];
        neon_operand_t *n = &op->ops[op->nops++];
        n->lane = o->vector_index;
        int sz;
        if (o->type == ARM_OP_REG) {
            int off = vreg_off(o->reg, &sz);
            if (off >= 0) {
                n->kind = NOP_VREG; n->off = off; n->size = sz;
                if (dup_lanes && n->lane < 0) n->lane = -2;
            }
            else if (o->reg == ARM_REG_FPSCR) n->kind = NOP_FPSCR;
            else if (o->reg == ARM_REG_APSR_NZCV) n->kind = NOP_APSR;
            else { n->kind = NOP_CORE; n->core = arm_reg_to_num(o->reg); }
        } else if (o->type == ARM_OP_IMM) {
            n->kind = NOP_IMM; n->imm = o->imm;
        } else if (o->type == ARM_OP_FP) {
            n->kind = NOP_FP; n->fp = o->fp;
        } else if (o->type == ARM_OP_MEM) {
            n->kind = NOP_CORE; n->core = arm_reg_to_num(o->mem.base);
            n->imm = insn->writeback ? 1 : 0; // writeback "!" (post-index reg handled separately)
        } else if (o->type == ARM_OP_SYSREG) {
            n->kind = NOP_FPSCR;
        } else {
            op->nops--;
        }
    }
    // For "[rn], rm" post-index the writeback flag is also set; the helper prefers rm when present.
    if (!neon_supported(op)) { free(op); return NULL; }
    return op;
}

jit_block_fn codegen_compile_block(jit_state_t *_jit_unused, const basic_block_t *bb, uint32_t slide) {
    (void)_jit_unused;
    (void)slide;
    jit_state_t *_jit = jit_new_state();
    jit_prolog();
    // Entry signature: void block_fn(uint32_t *gtc)
    jit_node_t *arg_gtc = jit_arg();
    jit_getarg(JIT_V0, arg_gtc); // JIT_V0 holds gtc pointer throughout execution

    // Default next_pc: address of instruction after last insn (fallthrough)
    uint32_t default_next_pc = bb->start_address;
    if (bb->insn_count > 0) {
        const disassembled_insn_t *last = &bb->insns[bb->insn_count - 1];
        default_next_pc = last->address + last->size;
        if (bb->mode == ARM_MODE_THUMB) default_next_pc |= 1;
    }
    emit_store_next_pc_imm(_jit, default_next_pc);

    for (size_t i = 0; i < bb->insn_count; i++) {
        const disassembled_insn_t *insn = &bb->insns[i];
        uint32_t pc_val = get_pc_value(insn->address, bb->mode);

        char base_mn[32];
        strncpy(base_mn, insn->mnemonic, sizeof(base_mn) - 1);
        base_mn[sizeof(base_mn) - 1] = '\0';
        char *dot = strchr(base_mn, '.');
        if (dot) *dot = '\0';

        arm_cc cond = insn->cc;
        neon_op_t *neon = NULL;
        strip_cc_suffix(base_mn, insn->cc);
        if (base_mn[0] == 'b' && strlen(base_mn) <= 4) {
            cond = get_branch_condition(base_mn, insn->cc);
        }
        jit_node_t *skip_label = emit_branch_if_cc_false(_jit, cond);

        // 1. Branch / Terminator instructions
        if (strcmp(base_mn, "b") == 0 ||
            (base_mn[0] == 'b' && strlen(base_mn) <= 4 &&
             (strcmp(base_mn, "beq") == 0 || strcmp(base_mn, "bne") == 0 ||
              strcmp(base_mn, "bgt") == 0 || strcmp(base_mn, "blt") == 0 ||
              strcmp(base_mn, "bge") == 0 || strcmp(base_mn, "ble") == 0 ||
              strcmp(base_mn, "bhs") == 0 || strcmp(base_mn, "blo") == 0 ||
              strcmp(base_mn, "bmi") == 0 || strcmp(base_mn, "bpl") == 0 ||
              strcmp(base_mn, "bvs") == 0 || strcmp(base_mn, "bvc") == 0 ||
              strcmp(base_mn, "bhi") == 0 || strcmp(base_mn, "bls") == 0))) {
            // Target is operand 0 immediate
            if (insn->arm_detail.op_count > 0 && insn->arm_detail.operands[0].type == ARM_OP_IMM) {
                uint32_t target = (uint32_t)insn->arm_detail.operands[0].imm;
                if (bb->mode == ARM_MODE_THUMB) target |= 1;
                emit_store_next_pc_imm(_jit, target);
            }
        }
        else if (strcmp(base_mn, "bx") == 0 ||
                 (strncmp(base_mn, "bx", 2) == 0 && strlen(base_mn) <= 4)) {
            // Target is operand 0 register
            emit_resolve_operand(_jit, &insn->arm_detail.operands[0], JIT_R0, pc_val);
            emit_store_next_pc(_jit, JIT_R0);
        }
        else if (strcmp(base_mn, "bl") == 0 || (strncmp(base_mn, "bl", 2) == 0 && strlen(base_mn) <= 4 && base_mn[2] != 'x')) {
            uint32_t ret_addr = insn->address + insn->size;
            if (bb->mode == ARM_MODE_THUMB) ret_addr |= 1;
            jit_movi(JIT_R0, ret_addr);
            emit_store_reg(_jit, 14, JIT_R0); // LR

            if (insn->arm_detail.operands[0].type == ARM_OP_IMM) {
                uint32_t target = (uint32_t)insn->arm_detail.operands[0].imm;
                if (bb->mode == ARM_MODE_THUMB) target |= 1;
                emit_store_next_pc_imm(_jit, target);
            }
        }
        else if (strcmp(base_mn, "blx") == 0 || (strncmp(base_mn, "blx", 3) == 0 && strlen(base_mn) <= 5)) {
            uint32_t ret_addr = insn->address + insn->size;
            if (bb->mode == ARM_MODE_THUMB) ret_addr |= 1;
            jit_movi(JIT_R0, ret_addr);
            emit_store_reg(_jit, 14, JIT_R0); // LR

            if (insn->arm_detail.operands[0].type == ARM_OP_IMM) {
                uint32_t target = (uint32_t)insn->arm_detail.operands[0].imm;
                // blx imm toggles mode!
                uint32_t other_bit = (bb->mode == ARM_MODE_ARM) ? 1 : 0;
                target |= other_bit;
                emit_store_next_pc_imm(_jit, target);
            } else {
                emit_resolve_operand(_jit, &insn->arm_detail.operands[0], JIT_R0, pc_val);
                emit_store_next_pc(_jit, JIT_R0);
            }
        }
        else if (strcmp(base_mn, "cbz") == 0 || strcmp(base_mn, "cbnz") == 0) {
            emit_resolve_operand(_jit, &insn->arm_detail.operands[0], JIT_R0, pc_val);
            uint32_t target = (uint32_t)insn->arm_detail.operands[1].imm | 1;
            jit_node_t *br;
            if (strcmp(base_mn, "cbz") == 0) {
                br = jit_bnei(JIT_R0, 0); // skip if not zero
            } else {
                br = jit_beqi(JIT_R0, 0); // skip if zero
            }
            emit_store_next_pc_imm(_jit, target);
            jit_patch(br);
        }
        else if (strcmp(base_mn, "svc") == 0) {
            uint32_t resume = insn->address + insn->size;
            if (bb->mode == ARM_MODE_THUMB) resume |= 1;
            jit_movi(JIT_R0, resume);
            jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_TRAP_RESUME), JIT_V0, JIT_R0);
            emit_store_next_pc_imm(_jit, SYSCALL_TRAP_PC);
        }
        // 2. Memory instructions: PUSH / POP
        else if (strcmp(base_mn, "push") == 0) {
            uint32_t count = insn->arm_detail.op_count;
            emit_load_reg(_jit, JIT_R0, 13); // SP
            jit_subi(JIT_R0, JIT_R0, count * 4);
            emit_store_reg(_jit, 13, JIT_R0); // Update SP

            for (uint32_t idx = 0; idx < count; idx++) {
                emit_resolve_operand(_jit, &insn->arm_detail.operands[idx], JIT_R1, pc_val);
                // Guest memory is identity-mapped, so host addr == guest addr!
                jit_stxi_i(idx * 4, JIT_R0, JIT_R1);
            }
        }
        else if (strcmp(base_mn, "pop") == 0) {
            uint32_t count = insn->arm_detail.op_count;
            emit_load_reg(_jit, JIT_R0, 13); // SP

            for (uint32_t idx = 0; idx < count; idx++) {
                int r = arm_reg_to_num(insn->arm_detail.operands[idx].reg);
                jit_ldxi_ui(JIT_R1, JIT_R0, idx * 4);
                if (r == 15) {
                    emit_store_next_pc(_jit, JIT_R1);
                } else if (r >= 0) {
                    emit_store_reg(_jit, r, JIT_R1);
                }
            }
            jit_addi(JIT_R0, JIT_R0, count * 4);
            emit_store_reg(_jit, 13, JIT_R0);
        }
        // Exclusive access (single guest thread: STREX always succeeds)
        else if (!strncmp(base_mn, "ldrex", 5) || !strncmp(base_mn, "strex", 5)) {
            const cs_arm *d = &insn->arm_detail;
            bool is_load = base_mn[0] == 'l';
            char w = base_mn[5]; // '\0' word, 'b', 'h', 'd'
            int mi = -1;
            for (int k = 0; k < d->op_count; k++) if (d->operands[k].type == ARM_OP_MEM) { mi = k; break; }
            const cs_arm_op *m = &d->operands[mi];
            emit_load_reg(_jit, JIT_R2, arm_reg_to_num(m->mem.base));
            if (m->mem.disp) jit_addi(JIT_R2, JIT_R2, m->mem.disp);
            jit_extr_ui(JIT_R2, JIT_R2);
            if (is_load) {
                int rt = arm_reg_to_num(d->operands[0].reg);
                if (w == 'b') jit_ldr_uc(JIT_R1, JIT_R2);
                else if (w == 'h') jit_ldr_us(JIT_R1, JIT_R2);
                else jit_ldr_ui(JIT_R1, JIT_R2);
                emit_store_reg(_jit, rt, JIT_R1);
                if (w == 'd') { jit_ldxi_ui(JIT_R1, JIT_R2, 4); emit_store_reg(_jit, arm_reg_to_num(d->operands[1].reg), JIT_R1); }
            } else {
                int rs = arm_reg_to_num(d->operands[0].reg);
                emit_load_reg(_jit, JIT_R1, arm_reg_to_num(d->operands[1].reg));
                if (w == 'b') jit_str_c(JIT_R2, JIT_R1);
                else if (w == 'h') jit_str_s(JIT_R2, JIT_R1);
                else jit_str_i(JIT_R2, JIT_R1);
                if (w == 'd') { emit_load_reg(_jit, JIT_R1, arm_reg_to_num(d->operands[2].reg)); jit_stxi_i(4, JIT_R2, JIT_R1); }
                jit_movi(JIT_R1, 0);
                emit_store_reg(_jit, rs, JIT_R1);
            }
        }
        // 3. Memory: LDRD / STRD
        else if (strcmp(base_mn, "ldrd") == 0) {
            int rt = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int rt2 = arm_reg_to_num(insn->arm_detail.operands[1].reg);
            const cs_arm_op *mem_op = &insn->arm_detail.operands[2];
            int base_reg = arm_reg_to_num(mem_op->mem.base);
            if (base_reg == 15) {
                jit_movi(JIT_R0, pc_val);
            } else {
                emit_load_reg(_jit, JIT_R0, base_reg);
            }
            if (mem_op->mem.index != ARM_REG_INVALID) {
                emit_load_reg(_jit, JIT_R1, arm_reg_to_num(mem_op->mem.index));
                if (mem_op->subtracted || mem_op->mem.scale == -1) jit_subr(JIT_R0, JIT_R0, JIT_R1);
                else jit_addr(JIT_R0, JIT_R0, JIT_R1);
            } else if (mem_op->mem.disp != 0) {
                jit_addi(JIT_R0, JIT_R0, mem_op->mem.disp);
            }
            jit_extr_ui(JIT_R0, JIT_R0);
            jit_ldxi_ui(JIT_R1, JIT_R0, 0);
            jit_ldxi_ui(JIT_R2, JIT_R0, 4);
            if (rt >= 0) emit_store_reg(_jit, rt, JIT_R1);
            if (rt2 >= 0) emit_store_reg(_jit, rt2, JIT_R2);
            if (insn->writeback && base_reg >= 0) {
                emit_post_index_offset(_jit, insn);
                emit_store_reg(_jit, base_reg, JIT_R0);
            }
        }
        else if (strcmp(base_mn, "strd") == 0) {
            int rt = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int rt2 = arm_reg_to_num(insn->arm_detail.operands[1].reg);
            const cs_arm_op *mem_op = &insn->arm_detail.operands[2];
            int base_reg = arm_reg_to_num(mem_op->mem.base);
            if (base_reg == 15) {
                jit_movi(JIT_R0, pc_val);
            } else {
                emit_load_reg(_jit, JIT_R0, base_reg);
            }
            if (mem_op->mem.index != ARM_REG_INVALID) {
                emit_load_reg(_jit, JIT_R1, arm_reg_to_num(mem_op->mem.index));
                if (mem_op->subtracted || mem_op->mem.scale == -1) jit_subr(JIT_R0, JIT_R0, JIT_R1);
                else jit_addr(JIT_R0, JIT_R0, JIT_R1);
            } else if (mem_op->mem.disp != 0) {
                jit_addi(JIT_R0, JIT_R0, mem_op->mem.disp);
            }
            jit_extr_ui(JIT_R0, JIT_R0);
            if (rt >= 0) {
                emit_load_reg(_jit, JIT_R1, rt);
                jit_stxi_i(0, JIT_R0, JIT_R1);
            }
            if (rt2 >= 0) {
                emit_load_reg(_jit, JIT_R1, rt2);
                jit_stxi_i(4, JIT_R0, JIT_R1);
            }
            if (insn->writeback && base_reg >= 0) {
                emit_post_index_offset(_jit, insn);
                emit_store_reg(_jit, base_reg, JIT_R0);
            }
        }
        // 3. Memory: LDR / STR
        else if (strncmp(base_mn, "ldr", 3) == 0 && strlen(base_mn) <= 5) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            const cs_arm_op *mem_op = &insn->arm_detail.operands[1];
            int base_reg = arm_reg_to_num(mem_op->mem.base);
            if (base_reg == 15) {
                jit_movi(JIT_R0, pc_val);
            } else {
                emit_load_reg(_jit, JIT_R0, base_reg);
            }

            if (mem_op->mem.index != ARM_REG_INVALID) {
                int idx_reg = arm_reg_to_num(mem_op->mem.index);
                emit_load_reg(_jit, JIT_R1, idx_reg);
                emit_mem_index_shift(_jit, mem_op);
                if (mem_op->subtracted || mem_op->mem.scale == -1) {
                    jit_subr(JIT_R0, JIT_R0, JIT_R1);
                } else {
                    jit_addr(JIT_R0, JIT_R0, JIT_R1);
                }
            } else if (mem_op->mem.disp != 0) {
                jit_addi(JIT_R0, JIT_R0, mem_op->mem.disp);
            }

            jit_extr_ui(JIT_R0, JIT_R0);
            if (strcmp(base_mn, "ldrb") == 0) {
                jit_ldr_c(JIT_R1, JIT_R0);
                jit_andi(JIT_R1, JIT_R1, 0xff);
            } else if (strcmp(base_mn, "ldrsb") == 0) {
                jit_ldr_c(JIT_R1, JIT_R0);
            } else if (strcmp(base_mn, "ldrh") == 0) {
                jit_ldr_s(JIT_R1, JIT_R0);
                jit_andi(JIT_R1, JIT_R1, 0xffff);
            } else if (strcmp(base_mn, "ldrsh") == 0) {
                jit_ldr_s(JIT_R1, JIT_R0);
            } else {
                jit_ldr_ui(JIT_R1, JIT_R0);
            }

            if (rd == 15) {
                emit_store_next_pc(_jit, JIT_R1);
            } else if (rd >= 0) {
                emit_store_reg(_jit, rd, JIT_R1);
            }

            if (insn->writeback && base_reg >= 0) {
                emit_post_index_offset(_jit, insn);
                emit_store_reg(_jit, base_reg, JIT_R0);
            }
        }
        else if (strncmp(base_mn, "str", 3) == 0 && strlen(base_mn) <= 5) {
            int rt = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            const cs_arm_op *mem_op = &insn->arm_detail.operands[1];
            int base_reg = arm_reg_to_num(mem_op->mem.base);
            if (base_reg == 15) {
                jit_movi(JIT_R0, pc_val);
            } else {
                emit_load_reg(_jit, JIT_R0, base_reg);
            }

            if (mem_op->mem.index != ARM_REG_INVALID) {
                int idx_reg = arm_reg_to_num(mem_op->mem.index);
                emit_load_reg(_jit, JIT_R1, idx_reg);
                emit_mem_index_shift(_jit, mem_op);
                if (mem_op->subtracted || mem_op->mem.scale == -1) {
                    jit_subr(JIT_R0, JIT_R0, JIT_R1);
                } else {
                    jit_addr(JIT_R0, JIT_R0, JIT_R1);
                }
            } else if (mem_op->mem.disp != 0) {
                jit_addi(JIT_R0, JIT_R0, mem_op->mem.disp);
            }

            jit_extr_ui(JIT_R0, JIT_R0);
            emit_load_reg(_jit, JIT_R1, rt);
            if (strcmp(base_mn, "strb") == 0) {
                jit_str_c(JIT_R0, JIT_R1);
            } else if (strcmp(base_mn, "strh") == 0) {
                jit_str_s(JIT_R0, JIT_R1);
            } else {
                jit_str_i(JIT_R0, JIT_R1);
            }

            if (insn->writeback && base_reg >= 0) {
                emit_post_index_offset(_jit, insn);
                emit_store_reg(_jit, base_reg, JIT_R0);
            }
        }
        // 4. Data Processing: MOV / MOVW / MOVT / MVN
        else if (strcmp(base_mn, "mov") == 0 || strcmp(base_mn, "movs") == 0 ||
                 strcmp(base_mn, "movw") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
            if (rd == 15) {
                emit_store_next_pc(_jit, JIT_R0);
            } else if (rd >= 0) {
                emit_store_reg(_jit, rd, JIT_R0);
            }
            if (insn->update_flags || strcmp(base_mn, "movs") == 0) {
                emit_compute_nz_flags(_jit, JIT_R0);
            }
        }
        else if (strcmp(base_mn, "movt") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_load_reg(_jit, JIT_R0, rd);
            jit_andi(JIT_R0, JIT_R0, 0x0000ffff);
            uint32_t imm16 = (uint32_t)insn->arm_detail.operands[1].imm << 16;
            jit_ori(JIT_R0, JIT_R0, imm16);
            emit_store_reg(_jit, rd, JIT_R0);
        }
        else if (strcmp(base_mn, "mvn") == 0 || strcmp(base_mn, "mvns") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
            jit_xori(JIT_R0, JIT_R0, 0xffffffff);
            emit_store_reg(_jit, rd, JIT_R0);
            if (insn->update_flags) {
                emit_compute_nz_flags(_jit, JIT_R0);
            }
        }
        // 5. Arithmetic: ADD / SUB / RSB / CMP / CMN
        else if (strcmp(base_mn, "add") == 0 || strcmp(base_mn, "adds") == 0 ||
                 strcmp(base_mn, "addw") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) {
                op_rn = 0; op_rm = 1;
            }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_addr(JIT_R2, JIT_R0, JIT_R1);
            if (rd == 15) {
                emit_store_next_pc(_jit, JIT_R2);
            } else if (rd >= 0) {
                emit_store_reg(_jit, rd, JIT_R2);
            }
            if (insn->update_flags || strcmp(base_mn, "adds") == 0) {
                emit_compute_add_flags(_jit, JIT_R2, JIT_R0, JIT_R1);
            }
        }
        else if (!strcmp(base_mn, "adc") || !strcmp(base_mn, "adcs") ||
                 !strcmp(base_mn, "sbc") || !strcmp(base_mn, "sbcs") ||
                 !strcmp(base_mn, "rsc") || !strcmp(base_mn, "rscs")) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) { op_rn = 0; op_rm = 1; }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            if (base_mn[0] == 'r') { jit_movr(JIT_R2, JIT_R0); jit_movr(JIT_R0, JIT_R1); jit_movr(JIT_R1, JIT_R2); }
            if (base_mn[0] != 'a') jit_xori(JIT_R1, JIT_R1, 0xffffffff); // a - b - !C == a + ~b + C
            /* capstone reports update_flags=true for plain adc/sbc/rsc: trust the 's' suffix only */
            emit_adc_core(_jit, base_mn[strlen(base_mn) - 1] == 's');
            emit_store_reg(_jit, rd, JIT_R2);
        }
        else if (strcmp(base_mn, "sub") == 0 || strcmp(base_mn, "subs") == 0 ||
                 strcmp(base_mn, "subw") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) {
                op_rn = 0; op_rm = 1;
            }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_subr(JIT_R2, JIT_R0, JIT_R1);
            if (rd == 15) {
                emit_store_next_pc(_jit, JIT_R2);
            } else if (rd >= 0) {
                emit_store_reg(_jit, rd, JIT_R2);
            }
            if (insn->update_flags || strcmp(base_mn, "subs") == 0) {
                emit_compute_sub_flags(_jit, JIT_R2, JIT_R0, JIT_R1);
            }
        }
        else if (strcmp(base_mn, "rsb") == 0 || strcmp(base_mn, "rsbs") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) {
                op_rn = 0; op_rm = 1;
            }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_subr(JIT_R2, JIT_R1, JIT_R0); // R2 = R1 - R0
            emit_store_reg(_jit, rd, JIT_R2);
            if (insn->update_flags || strcmp(base_mn, "rsbs") == 0) {
                emit_compute_sub_flags(_jit, JIT_R2, JIT_R1, JIT_R0);
            }
        }
        else if (strcmp(base_mn, "cmp") == 0) {
            emit_resolve_operand(_jit, &insn->arm_detail.operands[0], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R1, pc_val);
            jit_subr(JIT_R2, JIT_R0, JIT_R1);
            emit_compute_sub_flags(_jit, JIT_R2, JIT_R0, JIT_R1);
        }
        else if (strcmp(base_mn, "cmn") == 0) {
            emit_resolve_operand(_jit, &insn->arm_detail.operands[0], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R1, pc_val);
            jit_addr(JIT_R2, JIT_R0, JIT_R1);
            emit_compute_add_flags(_jit, JIT_R2, JIT_R0, JIT_R1);
        }
        // 6. Logical: AND / ORR / EOR / BIC / TST / TEQ
        else if (strcmp(base_mn, "and") == 0 || strcmp(base_mn, "ands") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) { op_rn = 0; op_rm = 1; }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_andr(JIT_R0, JIT_R0, JIT_R1);
            emit_store_reg(_jit, rd, JIT_R0);
            if (insn->update_flags) emit_compute_nz_flags(_jit, JIT_R0);
        }
        else if (strcmp(base_mn, "orr") == 0 || strcmp(base_mn, "orrs") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) { op_rn = 0; op_rm = 1; }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_orr(JIT_R0, JIT_R0, JIT_R1);
            emit_store_reg(_jit, rd, JIT_R0);
            if (insn->update_flags) emit_compute_nz_flags(_jit, JIT_R0);
        }
        else if (strcmp(base_mn, "eor") == 0 || strcmp(base_mn, "eors") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) { op_rn = 0; op_rm = 1; }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_xorr(JIT_R0, JIT_R0, JIT_R1);
            emit_store_reg(_jit, rd, JIT_R0);
            if (insn->update_flags) emit_compute_nz_flags(_jit, JIT_R0);
        }
        else if (strcmp(base_mn, "bic") == 0 || strcmp(base_mn, "bics") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) { op_rn = 0; op_rm = 1; }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_xori(JIT_R1, JIT_R1, 0xffffffff);
            jit_andr(JIT_R0, JIT_R0, JIT_R1);
            emit_store_reg(_jit, rd, JIT_R0);
            if (insn->update_flags) emit_compute_nz_flags(_jit, JIT_R0);
        }
        else if (strcmp(base_mn, "tst") == 0) {
            emit_resolve_operand(_jit, &insn->arm_detail.operands[0], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R1, pc_val);
            jit_andr(JIT_R0, JIT_R0, JIT_R1);
            emit_compute_nz_flags(_jit, JIT_R0);
        }
        else if (strcmp(base_mn, "teq") == 0) {
            emit_resolve_operand(_jit, &insn->arm_detail.operands[0], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R1, pc_val);
            jit_xorr(JIT_R0, JIT_R0, JIT_R1);
            emit_compute_nz_flags(_jit, JIT_R0);
        }
        // 7. Shifts: LSL / LSR / ASR / ROR
        else if (strncmp(base_mn, "lsl", 3) == 0 || strncmp(base_mn, "lsr", 3) == 0 ||
                 strncmp(base_mn, "asr", 3) == 0 || strncmp(base_mn, "ror", 3) == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            if (insn->arm_detail.op_count == 2) {
                if (insn->arm_detail.operands[1].shift.type != ARM_SFT_INVALID) {
                    // ARM mode alias: <shift> Rd, Rm, #imm (MOV Rd, Rm, <shift> #imm)
                    int rm = arm_reg_to_num(insn->arm_detail.operands[1].reg);
                    if (rm == 15) jit_movi(JIT_R0, pc_val);
                    else emit_load_reg(_jit, JIT_R0, rm);
                    uint32_t amt = insn->arm_detail.operands[1].shift.value;
                    if (amt == 0 && (strncmp(base_mn, "lsr", 3) == 0 || strncmp(base_mn, "asr", 3) == 0)) amt = 32;
                    jit_movi(JIT_R1, amt);
                } else {
                    // Thumb-1: <shift>s Rd, Rm (Rd = Rd <shift> Rm)
                    emit_load_reg(_jit, JIT_R0, arm_reg_to_num(insn->arm_detail.operands[0].reg));
                    emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R1, pc_val);
                }
            } else {
                emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
                emit_resolve_operand(_jit, &insn->arm_detail.operands[2], JIT_R1, pc_val);
            }
            jit_andi(JIT_R1, JIT_R1, 0xff);
            bool is_ror = strncmp(base_mn, "ror", 3) == 0;
            if (!is_ror) { // saturate: x86 masks shift counts to 6 bits
                jit_node_t *ok = jit_blei_u(JIT_R1, 63);
                jit_movi(JIT_R1, 63);
                jit_patch(ok);
            }
            jit_movr(JIT_V3, JIT_R1);   // amount (pre-mask for ror)
            jit_movr(JIT_V2, JIT_R0);   // original operand

            if (strncmp(base_mn, "lsl", 3) == 0) {
                jit_lshr(JIT_R0, JIT_R0, JIT_R1);
            } else if (strncmp(base_mn, "lsr", 3) == 0) {
                jit_rshr_u(JIT_R0, JIT_R0, JIT_R1);
            } else if (strncmp(base_mn, "asr", 3) == 0) {
                jit_extr_i(JIT_R0, JIT_R0);
                jit_rshr(JIT_R0, JIT_R0, JIT_R1);
            } else { // ROR
                jit_andi(JIT_R1, JIT_R1, 31);
                jit_rshr_u(JIT_R2, JIT_R0, JIT_R1);
                jit_movi(JIT_V1, 32);
                jit_subr(JIT_V1, JIT_V1, JIT_R1);
                jit_lshr(JIT_R0, JIT_R0, JIT_V1);
                jit_orr(JIT_R0, JIT_R0, JIT_R2);
            }
            jit_andi(JIT_R0, JIT_R0, 0xffffffff);
            if (insn->update_flags) {
                jit_movr(JIT_V1, JIT_R0);
                emit_compute_nz_flags(_jit, JIT_R0);
                // Shifter carry-out (C unchanged when the amount is 0)
                jit_node_t *zero_amt = jit_beqi(JIT_V3, 0);
                if (strncmp(base_mn, "lsl", 3) == 0) {
                    jit_lshr(JIT_R1, JIT_V2, JIT_V3);
                    jit_rshi_u(JIT_R1, JIT_R1, 32);
                } else if (strncmp(base_mn, "lsr", 3) == 0) {
                    jit_subi(JIT_V3, JIT_V3, 1);
                    jit_rshr_u(JIT_R1, JIT_V2, JIT_V3);
                } else if (strncmp(base_mn, "asr", 3) == 0) {
                    jit_subi(JIT_V3, JIT_V3, 1);
                    jit_extr_i(JIT_V2, JIT_V2);
                    jit_rshr(JIT_R1, JIT_V2, JIT_V3);
                } else {
                    jit_rshi_u(JIT_R1, JIT_V1, 31);
                }
                jit_andi(JIT_R1, JIT_R1, 1);
                jit_lshi(JIT_R1, JIT_R1, 29);
                jit_ldxi_ui(JIT_R2, JIT_V0, GTC_BYTE_OFF(GTC_FIELD_CPSR));
                jit_andi(JIT_R2, JIT_R2, ~CPSR_C);
                jit_orr(JIT_R2, JIT_R2, JIT_R1);
                jit_stxi_i(GTC_BYTE_OFF(GTC_FIELD_CPSR), JIT_V0, JIT_R2);
                jit_patch(zero_amt);
                jit_movr(JIT_R0, JIT_V1);
            }
            emit_store_reg(_jit, rd, JIT_R0);
        }
        // 8. Multiply: MUL / MLA / MLS
        else if (!strcmp(base_mn, "mla") || !strcmp(base_mn, "mls")) {
            const cs_arm_op *o = insn->arm_detail.operands;
            emit_load_reg(_jit, JIT_R0, arm_reg_to_num(o[1].reg));
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[2].reg));
            jit_mulr(JIT_R0, JIT_R0, JIT_R1);
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[3].reg));
            if (base_mn[2] == 'a') jit_addr(JIT_R0, JIT_R1, JIT_R0); else jit_subr(JIT_R0, JIT_R1, JIT_R0);
            emit_store_reg(_jit, arm_reg_to_num(o[0].reg), JIT_R0);
            if (insn->update_flags) emit_compute_nz_flags(_jit, JIT_R0);
        }
        else if (!strcmp(base_mn, "umull") || !strcmp(base_mn, "smull") ||
                 !strcmp(base_mn, "umlal") || !strcmp(base_mn, "smlal")) {
            // {u,s}mul{l,al} rdlo, rdhi, rn, rm   (64-bit host arithmetic)
            const cs_arm_op *o = insn->arm_detail.operands;
            bool sgn = base_mn[0] == 's', acc = base_mn[3] == 'a';
            int lo = arm_reg_to_num(o[0].reg), hi = arm_reg_to_num(o[1].reg);
            emit_load_reg(_jit, JIT_R0, arm_reg_to_num(o[2].reg));
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[3].reg));
            if (sgn) { jit_extr_i(JIT_R0, JIT_R0); jit_extr_i(JIT_R1, JIT_R1); }
            else { jit_extr_ui(JIT_R0, JIT_R0); jit_extr_ui(JIT_R1, JIT_R1); }
            jit_mulr(JIT_R0, JIT_R0, JIT_R1);
            if (acc) {
                emit_load_reg(_jit, JIT_R1, hi);
                jit_extr_ui(JIT_R1, JIT_R1);
                jit_lshi(JIT_R1, JIT_R1, 32);
                emit_load_reg(_jit, JIT_R2, lo);
                jit_extr_ui(JIT_R2, JIT_R2);
                jit_orr(JIT_R1, JIT_R1, JIT_R2);
                jit_addr(JIT_R0, JIT_R0, JIT_R1);
            }
            emit_store_reg(_jit, lo, JIT_R0);
            jit_rshi_u(JIT_R1, JIT_R0, 32);
            emit_store_reg(_jit, hi, JIT_R1);
        }
        else if (!strcmp(base_mn, "smmul") || !strcmp(base_mn, "smmla") || !strcmp(base_mn, "smmulr")) {
            const cs_arm_op *o = insn->arm_detail.operands;
            emit_load_reg(_jit, JIT_R0, arm_reg_to_num(o[1].reg));
            emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[2].reg));
            jit_extr_i(JIT_R0, JIT_R0); jit_extr_i(JIT_R1, JIT_R1);
            jit_mulr(JIT_R0, JIT_R0, JIT_R1);
            if (!strcmp(base_mn, "smmla")) {
                emit_load_reg(_jit, JIT_R1, arm_reg_to_num(o[3].reg));
                jit_lshi(JIT_R1, JIT_R1, 32);
                jit_addr(JIT_R0, JIT_R0, JIT_R1);
            }
            if (!strcmp(base_mn, "smmulr")) { jit_movi(JIT_R1, 0x80000000); jit_addr(JIT_R0, JIT_R0, JIT_R1); }
            jit_rshi(JIT_R0, JIT_R0, 32);
            emit_store_reg(_jit, arm_reg_to_num(o[0].reg), JIT_R0);
        }
        else if (strcmp(base_mn, "mul") == 0 || strcmp(base_mn, "muls") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) { op_rn = 0; op_rm = 1; }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_mulr(JIT_R0, JIT_R0, JIT_R1);
            emit_store_reg(_jit, rd, JIT_R0);
            if (insn->update_flags) emit_compute_nz_flags(_jit, JIT_R0);
        }
        // 9. Sign / zero extension: SXTB / SXTH / UXTB / UXTH
        else if (strcmp(base_mn, "sxtb") == 0 || strcmp(base_mn, "sxth") == 0 ||
                 strcmp(base_mn, "uxtb") == 0 || strcmp(base_mn, "uxth") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
            if (strcmp(base_mn, "sxtb") == 0) {
                jit_extr_c(JIT_R0, JIT_R0);
            } else if (strcmp(base_mn, "sxth") == 0) {
                jit_extr_s(JIT_R0, JIT_R0);
            } else if (strcmp(base_mn, "uxtb") == 0) {
                jit_andi(JIT_R0, JIT_R0, 0xff);
            } else if (strcmp(base_mn, "uxth") == 0) {
                jit_andi(JIT_R0, JIT_R0, 0xffff);
            }
            emit_store_reg(_jit, rd, JIT_R0);
        }
        // 10. Bitfield: UBFX / SBFX / BFC / BFI
        else if (strcmp(base_mn, "bfi") == 0 || strcmp(base_mn, "bfc") == 0) {
            // bfi rd, rn, #lsb, #width / bfc rd, #lsb, #width
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            bool is_bfc = base_mn[2] == 'c';
            uint32_t lsb = (uint32_t)insn->arm_detail.operands[is_bfc ? 1 : 2].imm;
            uint32_t width = (uint32_t)insn->arm_detail.operands[is_bfc ? 2 : 3].imm;
            uint32_t fmask = (width >= 32) ? 0xffffffffu : ((1u << width) - 1);
            emit_load_reg(_jit, JIT_R0, rd);
            jit_andi(JIT_R0, JIT_R0, ~(fmask << lsb));
            if (!is_bfc) {
                emit_load_reg(_jit, JIT_R1, arm_reg_to_num(insn->arm_detail.operands[1].reg));
                jit_andi(JIT_R1, JIT_R1, fmask);
                if (lsb) jit_lshi(JIT_R1, JIT_R1, lsb);
                jit_orr(JIT_R0, JIT_R0, JIT_R1);
            }
            emit_store_reg(_jit, rd, JIT_R0);
        }
        else if (strcmp(base_mn, "ubfx") == 0 || strcmp(base_mn, "sbfx") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
            uint32_t lsb = (uint32_t)insn->arm_detail.operands[2].imm;
            uint32_t width = (uint32_t)insn->arm_detail.operands[3].imm;
            jit_rshi_u(JIT_R0, JIT_R0, lsb);
            uint32_t mask = (width >= 32) ? 0xffffffff : ((1u << width) - 1);
            jit_andi(JIT_R0, JIT_R0, mask);
            if (strcmp(base_mn, "sbfx") == 0 && width < 32) {
                jit_lshi(JIT_R0, JIT_R0, 64 - width);
                jit_rshi(JIT_R0, JIT_R0, 64 - width);
            }
            emit_store_reg(_jit, rd, JIT_R0);
        }
        // 11. Thread local storage (TPIDRURW / mrc p15, 0, ...)
        else if (strcmp(base_mn, "mrc") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[2].reg);
            jit_movi(JIT_R0, TLS_REGISTER_ADDR);
            jit_ldr_i(JIT_R1, JIT_R0);
            emit_store_reg(_jit, rd, JIT_R1);
        }
        else if (strcmp(base_mn, "mcr") == 0) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[2].reg);
            emit_load_reg(_jit, JIT_R1, rd);
            jit_movi(JIT_R0, TLS_REGISTER_ADDR);
            jit_str_i(JIT_R0, JIT_R1);
        }
        // VFP / NEON memory and transfer ops
        else if (base_mn[0] == 'v' && emit_vfp_insn(_jit, insn, base_mn, pc_val)) {
            if (getenv("IOS6_TRACE_VFP")) fprintf(stderr, "[vfp-inline] %08x %s %s\n", insn->address, insn->mnemonic, insn->op_str);
        }
        else if (base_mn[0] == 'v' && (neon = build_neon_op(insn, base_mn)) != NULL) {
            jit_prepare();
            jit_pushargr(JIT_V0);
            jit_pushargi((jit_word_t)neon);
            jit_finishi((jit_pointer_t)neon_exec);
        }
        // Extend-and-add: UXTAB / UXTAH / SXTAB / SXTAH  (rd = rn + ext(ror(rm)))
        else if (!strcmp(base_mn, "uxtab") || !strcmp(base_mn, "uxtah") ||
                 !strcmp(base_mn, "sxtab") || !strcmp(base_mn, "sxtah")) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R2, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[2], JIT_R0, pc_val);
            bool half = base_mn[4] == 'h';
            if (base_mn[0] == 'u') jit_andi(JIT_R0, JIT_R0, half ? 0xffff : 0xff);
            else if (half) jit_extr_s(JIT_R0, JIT_R0); else jit_extr_c(JIT_R0, JIT_R0);
            jit_addr(JIT_R0, JIT_R0, JIT_R2);
            emit_store_reg(_jit, rd, JIT_R0);
        }
        else if (!strcmp(base_mn, "orn") || !strcmp(base_mn, "orns")) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            int op_rn = 1, op_rm = 2;
            if (insn->arm_detail.op_count == 2) { op_rn = 0; op_rm = 1; }
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rn], JIT_R0, pc_val);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[op_rm], JIT_R1, pc_val);
            jit_xori(JIT_R1, JIT_R1, 0xffffffff);
            jit_orr(JIT_R0, JIT_R0, JIT_R1);
            emit_store_reg(_jit, rd, JIT_R0);
            if (insn->update_flags) emit_compute_nz_flags(_jit, JIT_R0);
        }
        else if (!strcmp(base_mn, "clz")) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
            jit_andi(JIT_R0, JIT_R0, 0xffffffff);
            jit_clzr(JIT_R0, JIT_R0);        // 64-bit count on the zero-extended value
            jit_subi(JIT_R0, JIT_R0, 32);
            emit_store_reg(_jit, rd, JIT_R0);
        }
        else if (!strcmp(base_mn, "rbit")) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
            jit_movi(JIT_R2, 0);
            for (int k = 0; k < 32; k++) {
                jit_rshi_u(JIT_R1, JIT_R0, k);
                jit_andi(JIT_R1, JIT_R1, 1);
                if (31 - k) jit_lshi(JIT_R1, JIT_R1, 31 - k);
                jit_orr(JIT_R2, JIT_R2, JIT_R1);
            }
            emit_store_reg(_jit, rd, JIT_R2);
        }
        else if (!strcmp(base_mn, "rev") || !strcmp(base_mn, "rev16") || !strcmp(base_mn, "revsh")) {
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            emit_resolve_operand(_jit, &insn->arm_detail.operands[1], JIT_R0, pc_val);
            if (!strcmp(base_mn, "rev")) { jit_bswapr_ui(JIT_R0, JIT_R0); }
            else if (!strcmp(base_mn, "rev16")) {
                jit_rshi_u(JIT_R1, JIT_R0, 8); jit_andi(JIT_R1, JIT_R1, 0x00ff00ff);
                jit_lshi(JIT_R0, JIT_R0, 8);   jit_andi(JIT_R0, JIT_R0, 0xff00ff00);
                jit_orr(JIT_R0, JIT_R0, JIT_R1);
            } else { jit_bswapr_us(JIT_R0, JIT_R0); jit_extr_s(JIT_R0, JIT_R0); }
            emit_store_reg(_jit, rd, JIT_R0);
        }
        else if (!strcmp(base_mn, "adr")) {
            // adr rd, #off : rd = Align(PC, 4) + off
            int rd = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            jit_movi(JIT_R0, (pc_val & ~3u) + (uint32_t)insn->arm_detail.operands[1].imm);
            emit_store_reg(_jit, rd, JIT_R0);
        }
        // TBB / TBH: table branch (Thumb-2). next_pc = PC + 2 * table[Rm]
        else if (!strcmp(base_mn, "tbb") || !strcmp(base_mn, "tbh")) {
            const cs_arm_op *m = &insn->arm_detail.operands[0];
            int rn = arm_reg_to_num(m->mem.base);
            int rm = arm_reg_to_num(m->mem.index);
            if (rn == 15) jit_movi(JIT_R0, pc_val); else emit_load_reg(_jit, JIT_R0, rn);
            emit_load_reg(_jit, JIT_R1, rm);
            if (base_mn[2] == 'h') {
                jit_lshi(JIT_R1, JIT_R1, 1);
                jit_addr(JIT_R0, JIT_R0, JIT_R1);
                jit_ldr_us(JIT_R1, JIT_R0);
            } else {
                jit_addr(JIT_R0, JIT_R0, JIT_R1);
                jit_ldr_uc(JIT_R1, JIT_R0);
            }
            jit_lshi(JIT_R1, JIT_R1, 1);
            jit_addi(JIT_R1, JIT_R1, pc_val | 1);
            emit_store_next_pc(_jit, JIT_R1);
        }
        // LDM / STM family: ldm{ia,ib,da,db}, stm{...}, plus fd/ea/fa/ed aliases
        else if (!strncmp(base_mn, "ldm", 3) || !strncmp(base_mn, "stm", 3)) {
            bool is_load = base_mn[0] == 'l';
            const char *mode = base_mn + 3;
            // before: address offset of first reg; after: whether the base increments
            bool inc = true, before = false;
            if (!strcmp(mode, "ib") || (is_load && !strcmp(mode, "ed")) || (!is_load && !strcmp(mode, "fa"))) { inc = true; before = true; }
            else if (!strcmp(mode, "da") || (is_load && !strcmp(mode, "fa")) || (!is_load && !strcmp(mode, "ed"))) { inc = false; before = false; }
            else if (!strcmp(mode, "db") || (is_load && !strcmp(mode, "ea")) || (!is_load && !strcmp(mode, "fd"))) { inc = false; before = true; }
            int base_reg = arm_reg_to_num(insn->arm_detail.operands[0].reg);
            uint32_t nregs = insn->arm_detail.op_count - 1;
            int32_t start = inc ? (before ? 4 : 0) : -(int32_t)(nregs * 4) + (before ? 0 : 4);
            emit_load_reg(_jit, JIT_R2, base_reg);
            bool base_in_list = false;
            for (uint32_t k = 0; k < nregs; k++) {
                int r = arm_reg_to_num(insn->arm_detail.operands[1 + k].reg);
                int32_t off = start + (int32_t)k * 4;
                if (is_load) {
                    jit_ldxi_ui(JIT_R1, JIT_R2, off);
                    if (r == 15) emit_store_next_pc(_jit, JIT_R1);
                    else if (r >= 0) emit_store_reg(_jit, r, JIT_R1);
                    if (r == base_reg) base_in_list = true;
                } else {
                    if (r == 15) jit_movi(JIT_R1, pc_val);
                    else emit_load_reg(_jit, JIT_R1, r);
                    jit_stxi_i(off, JIT_R2, JIT_R1);
                }
            }
            if (insn->writeback && !(is_load && base_in_list)) {
                jit_addi(JIT_R2, JIT_R2, inc ? (int32_t)(nregs * 4) : -(int32_t)(nregs * 4));
                emit_store_reg(_jit, base_reg, JIT_R2);
            }
        }
        // NOP, DMB, DSB, ISB, CLREX
        else if (strcmp(base_mn, "nop") == 0 || strcmp(base_mn, "dmb") == 0 ||
                 strcmp(base_mn, "dsb") == 0 || strcmp(base_mn, "isb") == 0 ||
                 strcmp(base_mn, "clrex") == 0 || strcmp(base_mn, "yield") == 0 ||
                 strcmp(base_mn, "pld") == 0 || strcmp(base_mn, "pli") == 0) {
            // No-op
        }
        else if (strcmp(base_mn, "it") && strncmp(base_mn, "it", 2)) {
            static int warned;
            if (warned++ < 64)
                fprintf(stderr, "[codegen] unhandled insn at 0x%08x: %s %s\n",
                        insn->address, insn->mnemonic, insn->op_str);
        }

        if (skip_label) {
            jit_patch(skip_label);
        }
    }

    jit_ret();
    jit_epilog();

    jit_block_fn fn = (jit_block_fn)jit_emit();
    jit_clear_state();
    return fn;
}
