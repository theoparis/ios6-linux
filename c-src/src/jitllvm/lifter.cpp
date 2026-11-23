/*
 * LLVM IR lifter + TPDE-LLVM JIT engine.
 *
 * Translates a capstone-decoded ARM/Thumb basic block into an LLVM IR
 * function mirroring the guest translation contract (ios6/context.h): the
 * generated function receives the GTC (guest translation context:
 * r0..r12, sp, lr at words 0..14, CPSR at 15, next_pc at 16, trap_resume
 * at 17) and updates it in place. Guest memory is identity-mapped, so
 * guest addresses are used directly as host pointers.
 *
 * The IR is compiled by TPDE-LLVM (tpde_llvm::LLVMCompiler), a fast LLVM
 * back-end, and mapped into the process for execution.
 */
#include "ios6/llvmjit.h"
#include "ios6/neon.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <tpde-llvm/LLVMCompiler.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#define TLS_REGISTER_ADDR 0x21005000u

using namespace llvm;

struct BlockCtx {
    LLVMContext &ctx;
    IRBuilder<> b;
    Function *fn;
    Argument *gtc; // i32* param
    bool thumb;

    BlockCtx(LLVMContext &c, Module &m) : ctx(c), b(c) {
        auto *fty = FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false);
        fn = Function::Create(fty, Function::ExternalLinkage, "ios6_block", m);
        gtc = fn->getArg(0);
        gtc->setName("gtc");
        b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
    }

    Value *gtc_word(unsigned word) { return b.CreateGEP(b.getInt32Ty(), gtc, b.getInt32(word)); }
    Value *ld(unsigned reg) { return b.CreateLoad(b.getInt32Ty(), gtc_word(reg)); }
    void st(unsigned reg, Value *v) { b.CreateStore(b.CreateTrunc(v, b.getInt32Ty()), gtc_word(reg)); }
    Value *ld_next_pc() { return ld(GTC_FIELD_NEXT_PC); }
    void st_next_pc(Value *v) { st(GTC_FIELD_NEXT_PC, v); }
    void st_next_pc_imm(uint32_t imm) { b.CreateStore(b.getInt32(imm), gtc_word(GTC_FIELD_NEXT_PC)); }
    Value *ld_cpsr() { return ld(GTC_FIELD_CPSR); }
    void st_cpsr(Value *cpsr) { st(GTC_FIELD_CPSR, cpsr); }

    Value *z64(Value *v) { return b.CreateZExt(v, b.getInt64Ty()); }
    Value *s64(Value *v) { return b.CreateSExt(v, b.getInt64Ty()); }
    Value *t32(Value *v) { return b.CreateTrunc(v, b.getInt32Ty()); }
    Value *i32c(uint32_t c) { return b.getInt32(c); }
    Value *i64c(uint64_t c) { return b.getInt64(c); }
    Value *r32(Value *v32, unsigned n) { // rotate-right an i32 by n (n != 0)
        return b.CreateOr(b.CreateLShr(v32, n), b.CreateShl(v32, 32 - n));
    }
    // Identity-mapped guest address (i32) -> host pointer.
    Value *gptr(Value *addr32) { return b.CreateIntToPtr(z64(addr32), b.getPtrTy()); }
    Value *gptr_off(Value *addr32, int64_t off) {
        Value *p = b.CreateGEP(b.getInt8Ty(), gptr(addr32), b.getInt64(off));
        return p;
    }
    Value *hptr(uint64_t addr) { return b.CreateIntToPtr(b.getInt64(addr), b.getPtrTy()); }
    // Unaligned-safe guest loads/stores.
    Value *ld_p(Type *ty, Value *p) { return b.CreateAlignedLoad(ty, p, MaybeAlign(1)); }
    void st_p(Type *ty, Value *p, Value *v) { b.CreateAlignedStore(b.CreateTrunc(v, ty), p, MaybeAlign(1)); }
    Value *ld_mem(Value *addr32, unsigned bytes, bool sext) {
        Type *ty = bytes == 1 ? b.getInt8Ty() : bytes == 2 ? b.getInt16Ty() : b.getInt32Ty();
        Value *v = ld_p(ty, gptr(addr32));
        if (bytes == 4) return v;
        return sext ? b.CreateSExt(v, b.getInt32Ty()) : b.CreateZExt(v, b.getInt32Ty());
    }
    void st_mem(Value *addr32, Value *v32, unsigned bytes) {
        Type *ty = bytes == 1 ? b.getInt8Ty() : bytes == 2 ? b.getInt16Ty() : b.getInt32Ty();
        st_p(ty, gptr(addr32), v32);
    }
    Value *host_ld32(uint64_t addr) { return b.CreateLoad(b.getInt32Ty(), hptr(addr)); }
    void host_st32(uint64_t addr, Value *v) { b.CreateStore(v, hptr(addr)); }
};

// ---------------------------------------------------------------------------
// Condition codes
// ---------------------------------------------------------------------------

static arm_cc branch_mnemonic_cc(const char *m, arm_cc default_cc) {
    if (!strcmp(m, "beq")) return ARM_CC_EQ;
    if (!strcmp(m, "bne")) return ARM_CC_NE;
    if (!strcmp(m, "bgt")) return ARM_CC_GT;
    if (!strcmp(m, "blt")) return ARM_CC_LT;
    if (!strcmp(m, "bge")) return ARM_CC_GE;
    if (!strcmp(m, "ble")) return ARM_CC_LE;
    if (!strcmp(m, "bhs") || !strcmp(m, "bcs")) return ARM_CC_HS;
    if (!strcmp(m, "blo") || !strcmp(m, "bcc")) return ARM_CC_LO;
    if (!strcmp(m, "bmi")) return ARM_CC_MI;
    if (!strcmp(m, "bpl")) return ARM_CC_PL;
    if (!strcmp(m, "bvs")) return ARM_CC_VS;
    if (!strcmp(m, "bvc")) return ARM_CC_VC;
    if (!strcmp(m, "bhi")) return ARM_CC_HI;
    if (!strcmp(m, "bls")) return ARM_CC_LS;
    return default_cc;
}

static int arm_reg_to_num(int r) {
    if (r >= ARM_REG_R0 && r <= ARM_REG_R12) return r - ARM_REG_R0;
    switch (r) {
        case ARM_REG_SP: return 13;
        case ARM_REG_LR: return 14;
        case ARM_REG_PC: return 15;
        default: return -1;
    }
}

static int vreg_off(int r, int *size) {
    if (r >= ARM_REG_D0 && r <= ARM_REG_D31) { *size = 8; return (int)(r - ARM_REG_D0) * 8; }
    if (r >= ARM_REG_S0 && r <= ARM_REG_S31) { *size = 4; return (int)(r - ARM_REG_S0) * 4; }
    if (r >= ARM_REG_Q0 && r <= ARM_REG_Q15) { *size = 16; return (int)(r - ARM_REG_Q0) * 16; }
    return -1;
}

static Value *bit_set(BlockCtx &c, unsigned mask, Value *nzcv) {
    return c.b.CreateICmpNE(c.b.CreateAnd(nzcv, mask), c.i32c(0));
}

// i1: does condition `cc` hold given CPSR (NZCV in bits 31..28)?
static Value *lift_cond(BlockCtx &c, arm_cc cc) {
    if (cc == ARM_CC_AL || cc == ARM_CC_INVALID) return c.b.getTrue();
    Value *bits = c.b.CreateLShr(c.ld_cpsr(), 28u);
    Value *N = bit_set(c, 8, bits), *Z = bit_set(c, 4, bits),
          *C = bit_set(c, 2, bits), *V = bit_set(c, 1, bits);
    Value *nv = c.b.CreateXor(N, V); // N != V
    switch (cc) {
        case ARM_CC_EQ: return Z;
        case ARM_CC_NE: return c.b.CreateNot(Z);
        case ARM_CC_HS: return C;
        case ARM_CC_LO: return c.b.CreateNot(C);
        case ARM_CC_MI: return N;
        case ARM_CC_PL: return c.b.CreateNot(N);
        case ARM_CC_VS: return V;
        case ARM_CC_VC: return c.b.CreateNot(V);
        case ARM_CC_HI: return c.b.CreateAnd(C, c.b.CreateNot(Z));
        case ARM_CC_LS: return c.b.CreateOr(c.b.CreateNot(C), Z);
        case ARM_CC_GE: return c.b.CreateNot(nv);
        case ARM_CC_LT: return nv;
        case ARM_CC_GT: return c.b.CreateAnd(c.b.CreateNot(Z), c.b.CreateNot(nv));
        case ARM_CC_LE: return c.b.CreateOr(Z, nv);
        default: return c.b.getTrue();
    }
}

// ---------------------------------------------------------------------------
// Operand resolution (mirrors emit_resolve_operand)
// ---------------------------------------------------------------------------

static Value *resolve_operand(BlockCtx &c, const cs_arm_op *op, uint32_t pc_val) {
    Value *dst;
    if (op->type == ARM_OP_IMM) {
        dst = c.i32c((uint32_t)op->imm);
    } else if (op->type == ARM_OP_REG) {
        int r = arm_reg_to_num(op->reg);
        dst = r == 15 ? c.i32c(pc_val) : r >= 0 ? c.ld(r) : c.i32c(0);
    } else {
        dst = c.i32c(0);
    }

    arm_shifter stype = op->shift.type;
    if (stype == ARM_SFT_INVALID) return dst;

    bool by_reg = stype == ARM_SFT_ASR_REG || stype == ARM_SFT_LSL_REG ||
                  stype == ARM_SFT_LSR_REG || stype == ARM_SFT_ROR_REG ||
                  stype == ARM_SFT_RRX_REG;
    if (by_reg) {
        Value *amt32 = c.b.CreateAnd(c.ld(arm_reg_to_num((arm_reg)op->shift.value)), 0xff);
        if (stype != ARM_SFT_ROR_REG) // saturate: clamp to 63 so 64-bit host shifts stay defined
            amt32 = c.b.CreateSelect(c.b.CreateICmpULE(amt32, c.i32c(63)), amt32, c.i32c(63));
        switch (stype) {
            case ARM_SFT_LSL_REG:
                return c.t32(c.b.CreateShl(c.z64(dst), c.z64(amt32)));
            case ARM_SFT_LSR_REG:
                return c.t32(c.b.CreateLShr(c.z64(dst), c.z64(amt32)));
            case ARM_SFT_ASR_REG:
                return c.t32(c.b.CreateAShr(c.s64(dst), c.z64(amt32)));
            case ARM_SFT_ROR_REG: {
                Value *n = c.b.CreateAnd(amt32, 31);
                Value *r = c.b.CreateOr(c.b.CreateLShr(c.z64(dst), c.z64(n)),
                                        c.b.CreateShl(c.z64(dst),
                                                      c.b.CreateSub(c.i64c(32), c.z64(n))));
                return c.b.CreateSelect(c.b.CreateICmpEQ(n, c.i32c(0)), dst, c.t32(r));
            }
            default: return dst;
        }
    }

    uint32_t amt = op->shift.value;
    switch (stype) {
        case ARM_SFT_ASR:
            return c.t32(c.b.CreateAShr(c.s64(dst), c.i64c(amt >= 32 || amt == 0 ? 31 : amt)));
        case ARM_SFT_LSL:
            if (amt >= 32) return c.i32c(0);
            return amt ? c.t32(c.b.CreateShl(c.z64(dst), amt)) : dst;
        case ARM_SFT_LSR:
            if (amt >= 32 || amt == 0) return c.i32c(0);
            return c.t32(c.b.CreateLShr(c.z64(dst), amt));
        case ARM_SFT_ROR: {
            uint32_t rot = amt & 31;
            if (!rot) return dst;
            return c.r32(dst, rot);
        }
        case ARM_SFT_RRX: {
            Value *lo = c.b.CreateLShr(c.z64(dst), 1);
            Value *c31 = c.b.CreateShl(c.z64(c.b.CreateAnd(c.ld_cpsr(), CPSR_C)), 31u - 29u);
            return c.t32(c.b.CreateOr(lo, c31));
        }
        default: return dst;
    }
}

// ---------------------------------------------------------------------------
// Flags computation (mirrors emit_compute_add/sub/nz_flags)
// ---------------------------------------------------------------------------

static void update_cpsr_masked(BlockCtx &c, Value *new_bits_i64, uint64_t clear_mask) {
    Value *old = c.z64(c.ld_cpsr());
    Value *masked = c.b.CreateAnd(old, c.i64c(~clear_mask & 0xffffffff));
    c.st_cpsr(c.t32(c.b.CreateOr(masked, new_bits_i64)));
}
static void update_cpsr(BlockCtx &c, Value *new_bits_i64) {
    update_cpsr_masked(c, new_bits_i64, CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
}

static void add_flags(BlockCtx &c, Value *a32, Value *b32, Value *res32) {
    Value *a = c.z64(a32), *b = c.z64(b32), *res = c.z64(res32);
    Value *bits = c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(res, 31u), c.i64c(1)), 31u); // N
    bits = c.b.CreateOr(bits, c.b.CreateShl(
        c.b.CreateZExt(c.b.CreateICmpEQ(res, c.i64c(0)), c.b.getInt64Ty()), 30u)); // Z
    bits = c.b.CreateOr(bits, c.b.CreateShl(
        c.b.CreateZExt(c.b.CreateICmpULT(res, a), c.b.getInt64Ty()), 29u)); // C
    Value *v = c.b.CreateAnd(c.b.CreateXor(a, res), c.b.CreateXor(b, res));
    bits = c.b.CreateOr(bits, c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(v, 31u), c.i64c(1)), 28u)); // V
    update_cpsr(c, bits);
}

static void sub_flags(BlockCtx &c, Value *a32, Value *b32, Value *res32) {
    Value *a = c.z64(a32), *b = c.z64(b32), *res = c.z64(res32);
    Value *bits = c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(res, 31u), c.i64c(1)), 31u); // N
    bits = c.b.CreateOr(bits, c.b.CreateShl(
        c.b.CreateZExt(c.b.CreateICmpEQ(res, c.i64c(0)), c.b.getInt64Ty()), 30u)); // Z
    bits = c.b.CreateOr(bits, c.b.CreateShl(
        c.b.CreateZExt(c.b.CreateICmpUGE(a, b), c.b.getInt64Ty()), 29u)); // C
    Value *v = c.b.CreateAnd(c.b.CreateXor(a, b), c.b.CreateXor(a, res));
    bits = c.b.CreateOr(bits, c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(v, 31u), c.i64c(1)), 28u)); // V
    update_cpsr(c, bits);
}

static void nz_flags(BlockCtx &c, Value *res32) {
    Value *res = c.z64(res32);
    Value *bits = c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(res, 31u), c.i64c(1)), 31u); // N
    bits = c.b.CreateOr(bits, c.b.CreateShl(
        c.b.CreateZExt(c.b.CreateICmpEQ(res, c.i64c(0)), c.b.getInt64Ty()), 30u));
    update_cpsr_masked(c, bits, CPSR_N | CPSR_Z); // preserves C, V
}

// ADC-style core: res = a + b + Cin, stored to gtc[rd]; NZCV when `flags`.
static void adc_core(BlockCtx &c, Value *a32, Value *b32, int rd, bool flags) {
    Value *cin = c.b.CreateAnd(c.b.CreateLShr(c.ld_cpsr(), 29u), 1); // carry in
    Value *sum = c.b.CreateAdd(c.b.CreateAdd(c.z64(a32), c.z64(b32)), c.z64(cin));
    Value *res = c.t32(sum);
    c.st(rd, res);
    if (!flags) return;
    Value *bits = c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(sum, 32u), c.i64c(1)), 29u); // carry out
    Value *res64 = c.z64(res);
    bits = c.b.CreateOr(bits, c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(res64, 31u), c.i64c(1)), 31u)); // N
    bits = c.b.CreateOr(bits, c.b.CreateShl(
        c.b.CreateZExt(c.b.CreateICmpEQ(res64, c.i64c(0)), c.b.getInt64Ty()), 30u));
    Value *nv = c.b.CreateAnd(c.b.CreateNot(c.b.CreateXor(c.z64(a32), c.z64(b32))),
                              c.b.CreateXor(c.z64(a32), res64));
    bits = c.b.CreateOr(bits, c.b.CreateShl(c.b.CreateAnd(c.b.CreateLShr(nv, 31u), c.i64c(1)), 28u)); // V
    update_cpsr(c, bits);
}

// ---------------------------------------------------------------------------
// Memory-operand helpers
// ---------------------------------------------------------------------------

static Value *mem_index_shift(BlockCtx &c, Value *idx, const cs_arm_op *op) {
    uint32_t amt = op->mem.lshift;
    arm_shifter type = ARM_SFT_LSL;
    if (amt == 0 && op->shift.type != ARM_SFT_INVALID && op->shift.value > 0) {
        amt = op->shift.value;
        type = op->shift.type;
    }
    if (amt == 0) return idx;
    switch (type) {
        case ARM_SFT_LSR: return c.t32(c.b.CreateLShr(c.z64(idx), amt));
        case ARM_SFT_ASR: return c.t32(c.b.CreateAShr(c.s64(idx), amt));
        default: return c.t32(c.b.CreateShl(c.z64(idx), amt));
    }
}

// For post-indexed accesses: apply the trailing offset to `base`.
static Value *post_index_offset(BlockCtx &c, const disassembled_insn_t *insn, Value *base) {
    uint32_t n = insn->arm_detail.op_count;
    if (n == 0) return base;
    const cs_arm_op *last = &insn->arm_detail.operands[n - 1];
    if (last->type == ARM_OP_MEM) return base; // pre-indexed: base already updated
    if (last->type == ARM_OP_IMM) {
        int64_t off = last->subtracted ? -(int64_t)last->imm : (int64_t)last->imm;
        return c.t32(c.b.CreateAdd(c.z64(base), c.i64c((uint64_t)off)));
    }
    if (last->type == ARM_OP_REG) {
        Value *v = mem_index_shift(c, c.ld(arm_reg_to_num(last->reg)), last);
        return last->subtracted ? c.b.CreateSub(base, v) : c.b.CreateAdd(base, v);
    }
    return base;
}

static Value *gep_bytes(BlockCtx &c, Value *base_ptr, int64_t byte_off) {
    return c.b.CreateGEP(c.b.getInt8Ty(), base_ptr, c.i64c((uint64_t)byte_off));
}

// VFP/NEON register file transfer: `size` bytes between guest memory at
// memaddr + moff and NEON_REG_BASE + voff.
static void vreg_xfer(BlockCtx &c, bool load, int voff, int size, int32_t moff, Value *memaddr) {
    for (int w = 0; w < size; w += 4) {
        Value *mp = gep_bytes(c, memaddr, moff + w);
        Value *rp = c.hptr(NEON_REG_BASE + (uint32_t)voff + w);
        if (load) {
            Value *v = c.ld_p(c.b.getInt32Ty(), mp);
            c.b.CreateStore(v, rp);
        } else {
            Value *v = c.b.CreateLoad(c.b.getInt32Ty(), rp);
            c.st_p(c.b.getInt32Ty(), mp, v);
        }
    }
}

// Handles VFP/NEON memory + core<->vreg moves. Returns true if consumed.
static bool lift_vfp_insn(BlockCtx &c, const disassembled_insn_t *insn, const char *mn,
                          uint32_t pc_val) {
    const cs_arm *d = &insn->arm_detail;
    int sz;
    if (strstr(insn->op_str, "[]")) return false;
    for (int i = 0; i < d->op_count; i++)
        if (d->operands[i].vector_index >= 0) return false;
    bool is_vld1 = !strncmp(mn, "vld1", 4), is_vst1 = !strncmp(mn, "vst1", 4);
    bool is_push = !strcmp(mn, "vpush"), is_pop = !strcmp(mn, "vpop");
    bool is_vldm = !strncmp(mn, "vldm", 4), is_vstm = !strncmp(mn, "vstm", 4);
    if (is_vld1 || is_vst1) {
        int mi = -1;
        for (int i = 0; i < d->op_count; i++)
            if (d->operands[i].type == ARM_OP_MEM) { mi = i; break; }
        if (mi < 0) return false;
        int rn = arm_reg_to_num(d->operands[mi].mem.base);
        Value *base = rn == 15 ? c.i32c(pc_val) : c.ld(rn);
        int32_t total = 0;
        for (int i = 0; i < mi; i++) {
            int off = vreg_off(d->operands[i].reg, &sz);
            if (off < 0) return false;
            vreg_xfer(c, is_vld1, off, sz, total, c.gptr(base));
            total += sz;
        }
        if (mi + 1 < d->op_count && d->operands[mi + 1].type == ARM_OP_REG) {
            Value *v = c.ld(arm_reg_to_num(d->operands[mi + 1].reg));
            c.st(rn, c.b.CreateAdd(base, v));
        } else if (insn->writeback) {
            c.st(rn, c.b.CreateAdd(base, c.i32c(total)));
        }
        return true;
    }
    if (is_push || is_pop || is_vldm || is_vstm) {
        int first = (is_push || is_pop) ? 0 : 1;
        int rn = (is_push || is_pop) ? 13 : arm_reg_to_num(d->operands[0].reg);
        bool load = is_pop || is_vldm;
        bool db = is_push || strstr(mn, "db") != NULL;
        int32_t total = 0;
        for (int i = first; i < d->op_count; i++) {
            if (vreg_off(d->operands[i].reg, &sz) < 0) return false;
            total += sz;
        }
        Value *base = c.ld(rn);
        if (db) base = c.b.CreateSub(base, c.i32c(total));
        for (int i = first, o = 0; i < d->op_count; i++) {
            int off = vreg_off(d->operands[i].reg, &sz);
            vreg_xfer(c, load, off, sz, o, c.gptr(base));
            o += sz;
        }
        if (is_push || is_pop || insn->writeback) {
            Value *end = base;
            if (!db) end = c.b.CreateAdd(base, c.i32c(total));
            c.st(rn, end);
        }
        return true;
    }
    if (!strcmp(mn, "vdup") && d->op_count == 2 && d->operands[1].type == ARM_OP_REG &&
        arm_reg_to_num(d->operands[1].reg) >= 0) {
        int off = vreg_off(d->operands[0].reg, &sz);
        if (off < 0) return false;
        const char *dot = strchr(insn->mnemonic, '.');
        int esz = dot ? atoi(dot + 1) : 32;
        Value *v = c.ld(arm_reg_to_num(d->operands[1].reg));
        if (esz == 8) v = c.b.CreateMul(c.b.CreateAnd(v, 0xff), c.i32c(0x01010101));
        else if (esz == 16) v = c.b.CreateMul(c.b.CreateAnd(v, 0xffff), c.i32c(0x00010001));
        for (int w = 0; w < sz; w += 4) c.host_st32(NEON_REG_BASE + (uint32_t)off + w, v);
        return true;
    }
    if (!strcmp(mn, "vldr") || !strcmp(mn, "vstr")) {
        int off = vreg_off(d->operands[0].reg, &sz);
        if (off < 0) return false;
        const cs_arm_op *m = &d->operands[1];
        int rn = arm_reg_to_num(m->mem.base);
        Value *base = rn == 15 ? c.i32c(pc_val & ~3u) : c.ld(rn);
        Value *addr = c.gptr_off(base, m->mem.disp);
        bool load = mn[1] == 'l';
        for (int w = 0; w < sz; w += 4) {
            Value *mp = gep_bytes(c, addr, w);
            Value *rp = c.hptr(NEON_REG_BASE + (uint32_t)off + w);
            if (load) c.b.CreateStore(c.ld_p(c.b.getInt32Ty(), mp), rp);
            else c.st_p(c.b.getInt32Ty(), mp, c.b.CreateLoad(c.b.getInt32Ty(), rp));
        }
        return true;
    }
    if (!strcmp(mn, "vmov") && d->op_count == 2 && d->operands[1].type == ARM_OP_IMM) {
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
        Value *wv = c.i32c(word);
        for (int w = 0; w < sz; w += 4) c.host_st32(NEON_REG_BASE + (uint32_t)off + w, wv);
        return true;
    }
    if (!strcmp(mn, "vmov")) {
        const cs_arm_op *o = d->operands;
        int n = d->op_count;
        for (int i = 0; i < n; i++)
            if (o[i].type != ARM_OP_REG) return false;
        int a, b;
        if (n == 2 && (a = vreg_off(o[0].reg, &sz)) >= 0 && (b = vreg_off(o[1].reg, &sz)) >= 0) {
            for (int w = 0; w < sz; w += 4)
                c.host_st32(NEON_REG_BASE + (uint32_t)a + w,
                            c.host_ld32(NEON_REG_BASE + (uint32_t)b + w));
            return true;
        }
        if (n == 2 && (a = vreg_off(o[1].reg, &sz)) >= 0) { // core <- vreg
            int rt = arm_reg_to_num(o[0].reg);
            if (rt < 0) return false;
            c.st(rt, c.host_ld32(NEON_REG_BASE + (uint32_t)a));
            return true;
        }
        if (n == 2 && (a = vreg_off(o[0].reg, &sz)) >= 0) { // vreg <- core
            int rt = arm_reg_to_num(o[1].reg);
            if (rt < 0) return false;
            c.host_st32(NEON_REG_BASE + (uint32_t)a, c.ld(rt));
            return true;
        }
        if (n == 3 && (a = vreg_off(o[2].reg, &sz)) >= 0) { // rt, rt2 <- dN
            int r0 = arm_reg_to_num(o[0].reg), r1 = arm_reg_to_num(o[1].reg);
            if (r0 < 0 || r1 < 0) return false;
            c.st(r0, c.host_ld32(NEON_REG_BASE + (uint32_t)a));
            c.st(r1, c.host_ld32(NEON_REG_BASE + (uint32_t)a + 4));
            return true;
        }
        if (n == 3 && (a = vreg_off(o[0].reg, &sz)) >= 0) { // dN <- rt, rt2
            int r1 = arm_reg_to_num(o[1].reg), r2 = arm_reg_to_num(o[2].reg);
            if (r1 < 0 || r2 < 0) return false;
            c.host_st32(NEON_REG_BASE + (uint32_t)a, c.ld(r1));
            c.host_st32(NEON_REG_BASE + (uint32_t)a + 4, c.ld(r2));
            return true;
        }
    }
    return false;
}

// Builds an interpreted NEON op (mirrors build_neon_op); NULL if unsupported.
static neon_op_t *build_neon_op(const disassembled_insn_t *insn, const char *base_mn) {
    neon_op_t *op = (neon_op_t *)calloc(1, sizeof *op);
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
            } else if (o->reg == ARM_REG_FPSCR) n->kind = NOP_FPSCR;
            else if (o->reg == ARM_REG_APSR_NZCV) n->kind = NOP_APSR;
            else { n->kind = NOP_CORE; n->core = arm_reg_to_num(o->reg); }
        } else if (o->type == ARM_OP_IMM) {
            n->kind = NOP_IMM; n->imm = o->imm;
        } else if (o->type == ARM_OP_FP) {
            n->kind = NOP_FP; n->fp = o->fp;
        } else if (o->type == ARM_OP_MEM) {
            n->kind = NOP_CORE; n->core = arm_reg_to_num(o->mem.base);
            n->imm = insn->writeback ? 1 : 0;
        } else if (o->type == ARM_OP_SYSREG) {
            n->kind = NOP_FPSCR;
        } else {
            op->nops--;
        }
    }
    if (!neon_supported(op)) { free(op); return NULL; }
    return op;
}

// ---------------------------------------------------------------------------
// Bit-reversal helpers (rbit / rev / rev16 / revsh)
// ---------------------------------------------------------------------------

static Value *bswap32(BlockCtx &c, Value *v) {
    v = c.b.CreateOr(c.b.CreateAnd(c.b.CreateLShr(v, 8u), c.i32c(0x00ff00ff)),
                     c.b.CreateShl(c.b.CreateAnd(v, c.i32c(0x00ff00ff)), 8u));
    return c.b.CreateOr(c.b.CreateLShr(v, 16u), c.b.CreateShl(v, 16u));
}

static Value *rev32_bits(BlockCtx &c, Value *v) {
    v = c.b.CreateOr(c.b.CreateAnd(c.b.CreateLShr(v, 1u), c.i32c(0x55555555)),
                     c.b.CreateShl(c.b.CreateAnd(v, c.i32c(0x55555555)), 1u));
    v = c.b.CreateOr(c.b.CreateAnd(c.b.CreateLShr(v, 2u), c.i32c(0x33333333)),
                     c.b.CreateShl(c.b.CreateAnd(v, c.i32c(0x33333333)), 2u));
    v = c.b.CreateOr(c.b.CreateAnd(c.b.CreateLShr(v, 4u), c.i32c(0x0f0f0f0f)),
                     c.b.CreateShl(c.b.CreateAnd(v, c.i32c(0x0f0f0f0f)), 4u));
    return bswap32(c, v);
}

// ---------------------------------------------------------------------------
// The per-insn lift switch (mirrors codegen_compile_block)
// ---------------------------------------------------------------------------

// Host side of an unsupported guest instruction; reached only when that instruction executes.
static void ios6_unhandled_insn(const char *text, uint32_t pc) {
    fprintf(stderr, "ios6: unsupported instruction at 0x%08x: %s\n", pc, text);
    exit(1);
}

static bool lift_insn(BlockCtx &c, Function *neon_exec,
                      std::vector<std::unique_ptr<neon_op_t>> &ops,
                      const disassembled_insn_t *insn, uint32_t pc_val) {
    const cs_arm *d = &insn->arm_detail;

    char base_mn[32];
    strncpy(base_mn, insn->mnemonic, sizeof(base_mn) - 1);
    base_mn[sizeof(base_mn) - 1] = '\0';
    if (char *dot = strchr(base_mn, '.')) *dot = '\0';
    strip_cc_suffix(base_mn, insn->cc);
    bool is_b = !strcmp(base_mn, "b") ||
                (base_mn[0] == 'b' && strlen(base_mn) <= 4 &&
                 branch_mnemonic_cc(base_mn, ARM_CC_INVALID) != ARM_CC_INVALID);

    // ---- Branch / terminator instructions ----
    if (is_b) {
        if (d->op_count > 0 && d->operands[0].type == ARM_OP_IMM) {
            uint32_t target = (uint32_t)d->operands[0].imm;
            if (c.thumb) target |= 1;
            c.st_next_pc_imm(target);
        }
        return true;
    }
    if (!strncmp(base_mn, "bx", 2) && strlen(base_mn) <= 4) {
        c.st_next_pc(resolve_operand(c, &d->operands[0], pc_val));
        return true;
    }
    if (!strcmp(base_mn, "bl") ||
        (strncmp(base_mn, "bl", 2) == 0 && strlen(base_mn) <= 4 && base_mn[2] != 'x')) {
        uint32_t ret_addr = insn->address + insn->size;
        if (c.thumb) ret_addr |= 1;
        c.st(14, c.i32c(ret_addr));
        if (d->operands[0].type == ARM_OP_IMM) {
            uint32_t target = (uint32_t)d->operands[0].imm;
            if (c.thumb) target |= 1;
            c.st_next_pc_imm(target);
        }
        return true;
    }
    if (!strncmp(base_mn, "blx", 3) && strlen(base_mn) <= 5) {
        uint32_t ret_addr = insn->address + insn->size;
        if (c.thumb) ret_addr |= 1;
        c.st(14, c.i32c(ret_addr));
        if (d->operands[0].type == ARM_OP_IMM) {
            uint32_t other_bit = c.thumb ? 0 : 1; // blx imm toggles mode
            c.st_next_pc_imm((uint32_t)d->operands[0].imm | other_bit);
        } else {
            c.st_next_pc(resolve_operand(c, &d->operands[0], pc_val));
        }
        return true;
    }
    if (!strcmp(base_mn, "cbz") || !strcmp(base_mn, "cbnz")) {
        Value *v = resolve_operand(c, &d->operands[0], pc_val);
        uint32_t target = (uint32_t)d->operands[1].imm | 1;
        Value *hit = !strcmp(base_mn, "cbz") ? c.b.CreateICmpEQ(v, c.i32c(0))
                                             : c.b.CreateICmpNE(v, c.i32c(0));
        Value *cur = c.ld_next_pc();
        c.st_next_pc(c.b.CreateSelect(hit, c.i32c(target), cur));
        return true;
    }
    if (!strcmp(base_mn, "svc")) {
        uint32_t resume = insn->address + insn->size;
        if (c.thumb) resume |= 1;
        c.b.CreateStore(c.i32c(resume), c.gtc_word(GTC_FIELD_TRAP_RESUME));
        c.st_next_pc_imm(SYSCALL_TRAP_PC);
        return true;
    }
    // ---- PUSH / POP ----
    if (!strcmp(base_mn, "push")) {
        uint32_t count = d->op_count;
        Value *sp = c.b.CreateSub(c.ld(13), c.i32c(count * 4));
        c.st(13, sp);
        for (uint32_t i = 0; i < count; i++) {
            Value *v = resolve_operand(c, &d->operands[i], pc_val);
            c.st_p(c.b.getInt32Ty(), c.gptr_off(sp, (int64_t)(i * 4)), v);
        }
        return true;
    }
    if (!strcmp(base_mn, "pop")) {
        uint32_t count = d->op_count;
        Value *sp = c.ld(13);
        for (uint32_t i = 0; i < count; i++) {
            int r = arm_reg_to_num(d->operands[i].reg);
            Value *v = c.ld_p(c.b.getInt32Ty(), c.gptr_off(sp, (int64_t)(i * 4)));
            if (r == 15) c.st_next_pc(v);
            else if (r >= 0) c.st(r, v);
        }
        c.st(13, c.b.CreateAdd(sp, c.i32c(count * 4)));
        return true;
    }
    // ---- Exclusive access (single guest thread: STREX always succeeds) ----
    if (!strncmp(base_mn, "ldrex", 5) || !strncmp(base_mn, "strex", 5)) {
        bool is_load = base_mn[0] == 'l';
        char w = base_mn[5];
        int mi = -1;
        for (int k = 0; k < d->op_count; k++)
            if (d->operands[k].type == ARM_OP_MEM) { mi = k; break; }
        if (mi < 0) return true;
        const cs_arm_op *m = &d->operands[mi];
        Value *addr = c.ld(arm_reg_to_num(m->mem.base));
        if (m->mem.disp) addr = c.b.CreateAdd(addr, c.i32c((uint32_t)m->mem.disp));
        if (is_load) {
            int rt = arm_reg_to_num(d->operands[0].reg);
            Value *v;
            if (w == 'b') v = c.ld_mem(addr, 1, false);
            else if (w == 'h') v = c.ld_mem(addr, 2, false);
            else v = c.ld_mem(addr, 4, false);
            c.st(rt, v);
            if (w == 'd')
                c.st(arm_reg_to_num(d->operands[1].reg),
                     c.ld_mem(c.b.CreateAdd(addr, c.i32c(4)), 4, false));
        } else {
            int rs = arm_reg_to_num(d->operands[0].reg);
            Value *v = c.ld(arm_reg_to_num(d->operands[1].reg));
            if (w == 'b') c.st_mem(addr, v, 1);
            else if (w == 'h') c.st_mem(addr, v, 2);
            else c.st_mem(addr, v, 4);
            if (w == 'd')
                c.st_mem(c.b.CreateAdd(addr, c.i32c(4)),
                         c.ld(arm_reg_to_num(d->operands[2].reg)), 4);
            c.st(rs, c.i32c(0));
        }
        return true;
    }
    // ---- LDRD / STRD ----
    if (!strcmp(base_mn, "ldrd") || !strcmp(base_mn, "strd")) {
        bool is_load = base_mn[0] == 'l';
        int rt = arm_reg_to_num(d->operands[0].reg);
        int rt2 = arm_reg_to_num(d->operands[1].reg);
        const cs_arm_op *mem_op = &d->operands[2];
        int base_reg = arm_reg_to_num(mem_op->mem.base);
        Value *addr = base_reg == 15 ? c.i32c(pc_val) : c.ld(base_reg);
        if (mem_op->mem.index != ARM_REG_INVALID) {
            Value *idx = c.ld(arm_reg_to_num(mem_op->mem.index));
            if (mem_op->subtracted || mem_op->mem.scale == -1) addr = c.b.CreateSub(addr, idx);
            else addr = c.b.CreateAdd(addr, idx);
        } else if (mem_op->mem.disp != 0) {
            addr = c.b.CreateAdd(addr, c.i32c((uint32_t)mem_op->mem.disp));
        }
        if (is_load) {
            Value *lo = c.ld_mem(addr, 4, false);
            Value *hi = c.ld_mem(c.b.CreateAdd(addr, c.i32c(4)), 4, false);
            if (rt >= 0) c.st(rt, lo);
            if (rt2 >= 0) c.st(rt2, hi);
        } else {
            if (rt >= 0) c.st_mem(addr, c.ld(rt), 4);
            if (rt2 >= 0) c.st_mem(c.b.CreateAdd(addr, c.i32c(4)), c.ld(rt2), 4);
        }
        if (insn->writeback && base_reg >= 0) c.st(base_reg, post_index_offset(c, insn, addr));
        return true;
    }
    // ---- LDR / STR family ----
    if (!strncmp(base_mn, "ldr", 3) && strlen(base_mn) <= 5) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        const cs_arm_op *mem_op = &d->operands[1];
        int base_reg = arm_reg_to_num(mem_op->mem.base);
        Value *addr = base_reg == 15 ? c.i32c(pc_val) : c.ld(base_reg);
        if (mem_op->mem.index != ARM_REG_INVALID) {
            Value *idx = mem_index_shift(c, c.ld(arm_reg_to_num(mem_op->mem.index)), mem_op);
            if (mem_op->subtracted || mem_op->mem.scale == -1) addr = c.b.CreateSub(addr, idx);
            else addr = c.b.CreateAdd(addr, idx);
        } else if (mem_op->mem.disp != 0) {
            addr = c.b.CreateAdd(addr, c.i32c((uint32_t)mem_op->mem.disp));
        }
        Value *v;
        if (!strcmp(base_mn, "ldrb")) v = c.ld_mem(addr, 1, false);
        else if (!strcmp(base_mn, "ldrsb")) v = c.ld_mem(addr, 1, true);
        else if (!strcmp(base_mn, "ldrh")) v = c.ld_mem(addr, 2, false);
        else if (!strcmp(base_mn, "ldrsh")) v = c.ld_mem(addr, 2, true);
        else v = c.ld_mem(addr, 4, false);
        if (rd == 15) c.st_next_pc(v);
        else if (rd >= 0) c.st(rd, v);
        if (insn->writeback && base_reg >= 0) c.st(base_reg, post_index_offset(c, insn, addr));
        return true;
    }
    if (!strncmp(base_mn, "str", 3) && strlen(base_mn) <= 5) {
        int rt = arm_reg_to_num(d->operands[0].reg);
        const cs_arm_op *mem_op = &d->operands[1];
        int base_reg = arm_reg_to_num(mem_op->mem.base);
        Value *addr = base_reg == 15 ? c.i32c(pc_val) : c.ld(base_reg);
        if (mem_op->mem.index != ARM_REG_INVALID) {
            Value *idx = mem_index_shift(c, c.ld(arm_reg_to_num(mem_op->mem.index)), mem_op);
            if (mem_op->subtracted || mem_op->mem.scale == -1) addr = c.b.CreateSub(addr, idx);
            else addr = c.b.CreateAdd(addr, idx);
        } else if (mem_op->mem.disp != 0) {
            addr = c.b.CreateAdd(addr, c.i32c((uint32_t)mem_op->mem.disp));
        }
        Value *v = rt >= 0 ? c.ld(rt) : c.i32c(0);
        if (!strcmp(base_mn, "strb")) c.st_mem(addr, v, 1);
        else if (!strcmp(base_mn, "strh")) c.st_mem(addr, v, 2);
        else c.st_mem(addr, v, 4);
        if (insn->writeback && base_reg >= 0) c.st(base_reg, post_index_offset(c, insn, addr));
        return true;
    }
    // ---- MOV / MOVW / MOVT / MVN ----
    if (!strcmp(base_mn, "mov") || !strcmp(base_mn, "movs") || !strcmp(base_mn, "movw")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *v = resolve_operand(c, &d->operands[1], pc_val);
        if (rd == 15) c.st_next_pc(v);
        else if (rd >= 0) c.st(rd, v);
        if (insn->update_flags || !strcmp(base_mn, "movs")) nz_flags(c, v);
        return true;
    }
    if (!strcmp(base_mn, "movt")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        uint32_t imm16 = (uint32_t)d->operands[1].imm << 16;
        c.st(rd, c.b.CreateOr(c.b.CreateAnd(c.ld(rd), 0x0000ffff), c.i32c(imm16)));
        return true;
    }
    if (!strcmp(base_mn, "mvn") || !strcmp(base_mn, "mvns")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *v = c.b.CreateXor(resolve_operand(c, &d->operands[1], pc_val), 0xffffffffu);
        c.st(rd, v);
        if (insn->update_flags) nz_flags(c, v);
        return true;
    }
    // ---- ADD / SUB / RSB / ADC / SBC / RSC / CMP / CMN / MUL ----
    unsigned op_rn = 1, op_rm = 2;
    if (d->op_count == 2) { op_rn = 0; op_rm = 1; }
    auto one_of = [&](auto... names) {
        return ((std::string_view(names) == std::string_view(base_mn)) || ...);
    };
    if (one_of("add", "adds", "addw") || one_of("sub", "subs", "subw")) {
        Value *x = resolve_operand(c, &d->operands[op_rn], pc_val);
        Value *y = resolve_operand(c, &d->operands[op_rm], pc_val);
        bool flags = insn->update_flags || base_mn[strlen(base_mn) - 1] == 's';
        Value *r = base_mn[0] == 'a' ? c.b.CreateAdd(x, y) : c.b.CreateSub(x, y);
        int rd = arm_reg_to_num(d->operands[0].reg);
        if (rd == 15) c.st_next_pc(r);
        else if (rd >= 0) c.st(rd, r);
        if (flags) {
            if (base_mn[0] == 'a') add_flags(c, x, y, r);
            else sub_flags(c, x, y, r);
        }
        return true;
    }
    if (one_of("rsb", "rsbs")) {
        Value *x = resolve_operand(c, &d->operands[op_rn], pc_val);
        Value *y = resolve_operand(c, &d->operands[op_rm], pc_val);
        Value *r = c.b.CreateSub(y, x);
        c.st(arm_reg_to_num(d->operands[0].reg), r);
        if (insn->update_flags || base_mn[3] == 's') sub_flags(c, y, x, r);
        return true;
    }
    if (one_of("adc", "adcs", "sbc", "sbcs", "rsc", "rscs")) {
        Value *x = resolve_operand(c, &d->operands[op_rn], pc_val);
        Value *y = resolve_operand(c, &d->operands[op_rm], pc_val);
        if (base_mn[0] == 'r') std::swap(x, y);      // rsc: rm - rn
        if (strncmp(base_mn, "adc", 3) != 0)
            y = c.b.CreateXor(y, 0xffffffffu);       // a - b - !C == a + ~b + C
        bool sfx = base_mn[strlen(base_mn) - 1] == 's';
        adc_core(c, x, y, arm_reg_to_num(d->operands[0].reg), sfx);
        return true;
    }
    if (one_of("cmp", "cmn")) {
        Value *x = resolve_operand(c, &d->operands[0], pc_val);
        Value *y = resolve_operand(c, &d->operands[1], pc_val);
        if (base_mn[2] == 'n') { // cmn (cmp is [2]=='p')
            Value *r = c.b.CreateAdd(x, y);
            add_flags(c, x, y, r);
        } else {
            Value *r = c.b.CreateSub(x, y);
            sub_flags(c, x, y, r);
        }
        return true;
    }
    if (one_of("mul", "muls")) {
        Value *x = resolve_operand(c, &d->operands[op_rn], pc_val);
        Value *y = resolve_operand(c, &d->operands[op_rm], pc_val);
        Value *r = c.b.CreateMul(x, y);
        c.st(arm_reg_to_num(d->operands[0].reg), r);
        if (insn->update_flags) nz_flags(c, r);
        return true;
    }
    // ---- Logical ----
    if (one_of("and", "ands") || one_of("orr", "orrs") || one_of("eor", "eors") ||
        one_of("bic", "bics") || one_of("orn", "orns") || one_of("tst") || one_of("teq")) {
        Value *x = resolve_operand(c, &d->operands[op_rn], pc_val);
        Value *y = resolve_operand(c, &d->operands[op_rm], pc_val);
        Value *r;
        switch (base_mn[0]) {
            case 'a': r = c.b.CreateAnd(x, y); break;
            case 'o': r = base_mn[1] == 'r' && base_mn[2] == 'r'
                          ? c.b.CreateOr(x, y)
                          : c.b.CreateOr(x, c.b.CreateXor(y, 0xffffffffu)); break; // orn
            case 'e': r = c.b.CreateXor(x, y); break;
            case 'b': r = c.b.CreateAnd(x, c.b.CreateXor(y, 0xffffffffu)); break;
            case 't': r = base_mn[1] == 's' ? c.b.CreateAnd(x, y) : c.b.CreateXor(x, y); break;
            default: return true;
        }
        bool flags_only = base_mn[0] == 't';
        if (!flags_only) c.st(arm_reg_to_num(d->operands[0].reg), r);
        if (insn->update_flags || flags_only) nz_flags(c, r);
        return true;
    }
    // ---- Shifts ----
    if (!strncmp(base_mn, "lsl", 3) || !strncmp(base_mn, "lsr", 3) ||
        !strncmp(base_mn, "asr", 3) || !strncmp(base_mn, "ror", 3)) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *x, *amt;
        if (d->op_count == 2) {
            if (d->operands[1].shift.type != ARM_SFT_INVALID) {
                // ARM mode alias: <shift> Rd, Rm, #imm (MOV Rd, Rm, <shift> #imm)
                int rm = arm_reg_to_num(d->operands[1].reg);
                x = rm == 15 ? c.i32c(pc_val) : c.ld(rm);
                uint32_t a = d->operands[1].shift.value;
                if (a == 0 && (!strncmp(base_mn, "lsr", 3) || !strncmp(base_mn, "asr", 3))) a = 32;
                amt = c.i32c(a);
            } else {
                // Thumb-1: <shift>s Rd, Rm (Rd = Rd <shift> Rm)
                x = c.ld(rd);
                amt = resolve_operand(c, &d->operands[1], pc_val);
            }
        } else {
            x = resolve_operand(c, &d->operands[1], pc_val);
            amt = resolve_operand(c, &d->operands[2], pc_val);
        }
        amt = c.b.CreateAnd(amt, 0xff);
        bool is_ror = !strncmp(base_mn, "ror", 3);
        if (!is_ror) // saturate: x86 masks shift counts to 6 bits
            amt = c.b.CreateSelect(c.b.CreateICmpULE(c.z64(amt), c.i64c(63)), amt, c.i32c(63));
        Value *x64 = c.z64(x);
        Value *r;
        if (!strncmp(base_mn, "lsl", 3)) r = c.t32(c.b.CreateShl(x64, c.z64(amt)));
        else if (!strncmp(base_mn, "lsr", 3)) r = c.t32(c.b.CreateLShr(x64, c.z64(amt)));
        else if (!strncmp(base_mn, "asr", 3)) r = c.t32(c.b.CreateAShr(c.s64(x), c.z64(amt)));
        else {
            Value *n = c.b.CreateAnd(amt, 31);
            Value *ro = c.b.CreateOr(c.b.CreateLShr(x64, c.z64(n)),
                                     c.b.CreateShl(x64, c.b.CreateSub(c.i64c(32), c.z64(n))));
            r = c.b.CreateSelect(c.b.CreateICmpEQ(n, c.i32c(0)), x, c.t32(ro));
        }
        if (insn->update_flags) {
            nz_flags(c, r);
            // Shifter carry-out (C unchanged when the amount is 0)
            Value *carry;
            if (!strncmp(base_mn, "lsl", 3))
                carry = c.b.CreateAnd(c.b.CreateLShr(c.b.CreateShl(x64, c.z64(amt)), 32u),
                                      c.i64c(1));
            else if (!strncmp(base_mn, "lsr", 3))
                carry = c.b.CreateAnd(c.b.CreateLShr(x64, c.b.CreateSub(c.z64(amt), c.i64c(1))),
                                      c.i64c(1));
            else if (!strncmp(base_mn, "asr", 3))
                carry = c.b.CreateAnd(c.b.CreateAShr(c.s64(x), c.b.CreateSub(c.z64(amt), c.i64c(1))),
                                      c.i64c(1));
            else
                carry = c.b.CreateLShr(c.z64(r), 31u);
            Value *cbits = c.b.CreateShl(carry, 29u); // -> CPSR_C position
            Value *masked = c.b.CreateAnd(c.z64(c.ld_cpsr()),
                                          c.i64c((uint64_t)~CPSR_C & 0xffffffff));
            Value *newcpsr = c.b.CreateSelect(c.b.CreateICmpEQ(c.z64(amt), c.i64c(0)),
                                              c.z64(c.ld_cpsr()), c.b.CreateOr(masked, cbits));
            c.st_cpsr(c.t32(newcpsr));
        }
        c.st(rd, r);
        return true;
    }
    // ---- MLA / MLS / long multiplies ----
    if (one_of("mla", "mls")) {
        const cs_arm_op *o = d->operands;
        Value *r = c.b.CreateMul(c.ld(arm_reg_to_num(o[1].reg)), c.ld(arm_reg_to_num(o[2].reg)));
        Value *acc = c.ld(arm_reg_to_num(o[3].reg));
        r = base_mn[2] == 'a' ? c.b.CreateAdd(acc, r) : c.b.CreateSub(acc, r);
        c.st(arm_reg_to_num(o[0].reg), r);
        if (insn->update_flags) nz_flags(c, r);
        return true;
    }
    if (one_of("umull", "smull", "umlal", "smlal")) {
        const cs_arm_op *o = d->operands;
        bool sgn = base_mn[0] == 's', acc = base_mn[3] == 'a';
        int lo = arm_reg_to_num(o[0].reg), hi = arm_reg_to_num(o[1].reg);
        Value *x = c.ld(arm_reg_to_num(o[2].reg));
        Value *y = c.ld(arm_reg_to_num(o[3].reg));
        Value *p = sgn ? c.b.CreateMul(c.s64(x), c.s64(y))
                       : c.b.CreateMul(c.z64(x), c.z64(y));
        if (acc) {
            Value *lo64 = c.z64(c.ld(lo));
            Value *hi64 = c.b.CreateShl(c.z64(c.ld(hi)), 32u);
            p = c.b.CreateAdd(p, c.b.CreateOr(lo64, hi64));
        }
        c.st(lo, c.t32(p));
        c.st(hi, c.t32(c.b.CreateLShr(p, 32u)));
        return true;
    }
    if (one_of("smmul", "smmla", "smmulr")) {
        const cs_arm_op *o = d->operands;
        Value *p = c.b.CreateMul(c.s64(c.ld(arm_reg_to_num(o[1].reg))),
                                 c.s64(c.ld(arm_reg_to_num(o[2].reg))));
        if (!strcmp(base_mn, "smmla"))
            p = c.b.CreateAdd(p, c.b.CreateShl(c.z64(c.ld(arm_reg_to_num(o[3].reg))), 32u));
        if (!strcmp(base_mn, "smmulr")) p = c.b.CreateAdd(p, c.i64c(0x80000000));
        c.st(arm_reg_to_num(o[0].reg), c.t32(c.b.CreateAShr(p, 32u)));
        return true;
    }
    // ---- Sign / zero extension ----
    if (one_of("sxtb", "sxth", "uxtb", "uxth")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *v = resolve_operand(c, &d->operands[1], pc_val);
        if (!strcmp(base_mn, "sxtb"))
            v = c.b.CreateAShr(c.b.CreateShl(v, 24u), 24u);
        else if (!strcmp(base_mn, "sxth"))
            v = c.b.CreateAShr(c.b.CreateShl(v, 16u), 16u);
        else if (!strcmp(base_mn, "uxtb")) v = c.b.CreateAnd(v, 0xff);
        else v = c.b.CreateAnd(v, 0xffff);
        c.st(rd, v);
        return true;
    }
    // ---- Pack halfword: PKHBT Rd,Rn,Rm{,LSL #n} / PKHTB Rd,Rn,Rm{,ASR #n} ----
    if (one_of("pkhbt", "pkhtb")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *rn = resolve_operand(c, &d->operands[1], pc_val);
        Value *rm = resolve_operand(c, &d->operands[2], pc_val);
        Value *v;
        if (base_mn[3] == 'b') // pkhbt: bottom half from Rn, top half from shifted Rm
            v = c.b.CreateOr(c.b.CreateAnd(rn, c.i32c(0x0000ffff)), c.b.CreateAnd(rm, c.i32c(0xffff0000)));
        else                   // pkhtb: top half from Rn, bottom half from shifted Rm
            v = c.b.CreateOr(c.b.CreateAnd(rn, c.i32c(0xffff0000)), c.b.CreateAnd(rm, c.i32c(0x0000ffff)));
        c.st(rd, v);
        return true;
    }

    // ---- Bitfield: BFI / BFC / UBFX / SBFX ----
    if (one_of("bfi", "bfc")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        bool is_bfc = base_mn[2] == 'c';
        uint32_t lsb = (uint32_t)d->operands[is_bfc ? 1 : 2].imm;
        uint32_t width = (uint32_t)d->operands[is_bfc ? 2 : 3].imm;
        uint32_t fmask = (width >= 32) ? 0xffffffffu : ((1u << width) - 1);
        Value *v = c.b.CreateAnd(c.ld(rd), c.i32c(~(fmask << lsb)));
        if (!is_bfc) {
            Value *src = c.b.CreateAnd(c.ld(arm_reg_to_num(d->operands[1].reg)), c.i32c(fmask));
            if (lsb) src = c.t32(c.b.CreateShl(c.z64(src), lsb));
            v = c.b.CreateOr(v, src);
        }
        c.st(rd, v);
        return true;
    }
    if (one_of("ubfx", "sbfx")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *v = resolve_operand(c, &d->operands[1], pc_val);
        uint32_t lsb = (uint32_t)d->operands[2].imm;
        uint32_t width = (uint32_t)d->operands[3].imm;
        uint32_t mask = (width >= 32) ? 0xffffffffu : ((1u << width) - 1);
        if (base_mn[0] == 'u') {
            v = c.b.CreateAnd(c.t32(c.b.CreateLShr(c.z64(v), lsb)), c.i32c(mask));
        } else {
            v = c.b.CreateAnd(c.t32(c.b.CreateAShr(c.s64(v), lsb)), c.i32c(mask));
            if (width < 32)
                v = c.t32(c.b.CreateAShr(c.b.CreateShl(c.s64(v), 64u - width), 64u - width));
        }
        c.st(rd, v);
        return true;
    }
    // ---- Thread local storage (TPIDRURW / mrc p15, 0, ...) ----
    if (one_of("mrc")) {
        c.st(arm_reg_to_num(d->operands[2].reg), c.host_ld32(TLS_REGISTER_ADDR));
        return true;
    }
    if (one_of("mcr")) {
        c.host_st32(TLS_REGISTER_ADDR, c.ld(arm_reg_to_num(d->operands[2].reg)));
        return true;
    }
    // ---- VFP / NEON memory and transfer ops ----
    if (base_mn[0] == 'v') {
        if (lift_vfp_insn(c, insn, base_mn, pc_val)) {
            if (getenv("IOS6_TRACE_VFP"))
                fprintf(stderr, "[vfp-inline] %08x %s %s\n", insn->address, insn->mnemonic,
                        insn->op_str);
            return true;
        }
        if (neon_op_t *op = build_neon_op(insn, base_mn)) {
            ops.emplace_back(op);
            Value *opptr = c.b.CreateIntToPtr(c.i64c((uintptr_t)op), c.b.getPtrTy());
            auto *fty = FunctionType::get(c.b.getVoidTy(), {c.b.getPtrTy(), c.b.getPtrTy()}, false);
            c.b.CreateCall(FunctionCallee(fty, neon_exec), {c.gtc, opptr});
            return true;
        }
    }
    // ---- Extend-and-add: UXTAB / UXTAH / SXTAB / SXTAH ----
    if (one_of("uxtab", "uxtah", "sxtab", "sxtah")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *rn = resolve_operand(c, &d->operands[1], pc_val);
        Value *rm = resolve_operand(c, &d->operands[2], pc_val);
        bool half = base_mn[4] == 'h';
        if (base_mn[0] == 'u') rm = c.b.CreateAnd(rm, half ? 0xffff : 0xff);
        else if (half) rm = c.b.CreateAShr(c.b.CreateShl(rm, 16u), 16u);
        else rm = c.b.CreateAShr(c.b.CreateShl(rm, 24u), 24u);
        c.st(rd, c.b.CreateAdd(rm, rn));
        return true;
    }
    if (one_of("clz")) {
        Value *v = c.z64(resolve_operand(c, &d->operands[1], pc_val));
        Value *lz = c.b.CreateIntrinsic(Intrinsic::ctlz, {c.b.getInt64Ty()},
                                        {v, c.b.getFalse()});
        c.st(arm_reg_to_num(d->operands[0].reg),
             c.t32(c.b.CreateSub(lz, c.i64c(32))));
        return true;
    }
    if (one_of("rbit")) {
        Value *v = resolve_operand(c, &d->operands[1], pc_val);
        c.st(arm_reg_to_num(d->operands[0].reg), rev32_bits(c, v));
        return true;
    }
    if (one_of("rev", "rev16", "revsh")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        Value *v = resolve_operand(c, &d->operands[1], pc_val);
        if (!strcmp(base_mn, "rev")) {
            v = bswap32(c, v);
        } else if (!strcmp(base_mn, "rev16")) {
            v = c.b.CreateOr(c.b.CreateAnd(c.b.CreateLShr(v, 8u), c.i32c(0x00ff00ff)),
                             c.b.CreateAnd(c.b.CreateShl(v, 8u), c.i32c(0xff00ff00)));
        } else { // revsh: byteswap 16 bits then sign-extend
            Value *lo = c.b.CreateAnd(v, 0xff);
            Value *hi = c.b.CreateAnd(c.b.CreateLShr(v, 8u), 0xff);
            Value *sw = c.b.CreateOr(c.b.CreateShl(lo, 8u), hi);      // byteswap low halfword
            v = c.b.CreateAShr(c.b.CreateShl(sw, 16u), 16u);          // sign-extend 16 bits
        }
        c.st(rd, v);
        return true;
    }
    if (one_of("adr")) {
        int rd = arm_reg_to_num(d->operands[0].reg);
        c.st(rd, c.i32c((pc_val & ~3u) + (uint32_t)d->operands[1].imm));
        return true;
    }
    // ---- TBB / TBH: table branch (Thumb-2) ----
    if (one_of("tbb", "tbh")) {
        const cs_arm_op *m = &d->operands[0];
        int rn = arm_reg_to_num(m->mem.base);
        int rm = arm_reg_to_num(m->mem.index);
        Value *base = rn == 15 ? c.i32c(pc_val) : c.ld(rn);
        Value *idx = c.ld(rm);
        Value *entry;
        if (base_mn[2] == 'h') {
            entry = c.ld_mem(c.b.CreateAdd(base, c.b.CreateShl(idx, 1u)), 2, false);
        } else {
            entry = c.ld_mem(c.b.CreateAdd(base, idx), 1, false);
        }
        Value *t = c.b.CreateAdd(c.b.CreateShl(c.z64(entry), 1u), c.i64c(pc_val | 1));
        c.st_next_pc(c.t32(t));
        return true;
    }
    // ---- LDM / STM family ----
    if (!strncmp(base_mn, "ldm", 3) || !strncmp(base_mn, "stm", 3)) {
        bool is_load = base_mn[0] == 'l';
        const char *mode = base_mn + 3;
        bool inc = true, before = false;
        if (!strcmp(mode, "ib") || (is_load && !strcmp(mode, "ed")) ||
            (!is_load && !strcmp(mode, "fa"))) { inc = true; before = true; }
        else if (!strcmp(mode, "da") || (is_load && !strcmp(mode, "fa")) ||
                 (!is_load && !strcmp(mode, "ed"))) { inc = false; before = false; }
        else if (!strcmp(mode, "db") || (is_load && !strcmp(mode, "ea")) ||
                 (!is_load && !strcmp(mode, "fd"))) { inc = false; before = true; }
        int base_reg = arm_reg_to_num(d->operands[0].reg);
        uint32_t nregs = d->op_count - 1;
        int32_t start = inc ? (before ? 4 : 0) : -(int32_t)(nregs * 4) + (before ? 0 : 4);
        Value *base = c.ld(base_reg);
        bool base_in_list = false;
        for (uint32_t k = 0; k < nregs; k++) {
            int r = arm_reg_to_num(d->operands[1 + k].reg);
            int32_t off = start + (int32_t)k * 4;
            Value *p = c.gptr_off(base, off);
            if (is_load) {
                Value *v = c.ld_p(c.b.getInt32Ty(), p);
                if (r == 15) c.st_next_pc(v);
                else if (r >= 0) c.st(r, v);
                if (r == base_reg) base_in_list = true;
            } else {
                Value *v = r == 15 ? c.i32c(pc_val) : c.ld(r);
                c.st_p(c.b.getInt32Ty(), p, v);
            }
        }
        if (insn->writeback && !(is_load && base_in_list)) {
            base = c.b.CreateAdd(base, c.i32c(inc ? (int32_t)(nregs * 4) : -(int32_t)(nregs * 4)));
            c.st(base_reg, base);
        }
        return true;
    }
    // ---- NOP family ----
    if (one_of("nop", "dmb", "dsb", "isb", "clrex", "yield", "pld", "pli")) return true;
    if (strcmp(base_mn, "it") && strncmp(base_mn, "it", 2)) {
        // Unsupported instruction: fail when it executes, instead of compiling nothing and
        // running off the end of the block.
        auto *fty = FunctionType::get(c.b.getVoidTy(), {c.b.getPtrTy(), c.b.getInt32Ty()}, false);
        FunctionCallee unhandled = c.fn->getParent()->getOrInsertFunction("ios6_unhandled_insn", fty);
        std::string text = std::string(insn->mnemonic) + " " + insn->op_str;
        c.b.CreateCall(unhandled, {c.b.CreateGlobalString(text), c.i32c(insn->address)});
    }
    return true;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

struct llvmjit_engine {
    LLVMContext ctx;
    std::unique_ptr<tpde_llvm::LLVMCompiler> compiler;
    // Mappers keep the mapped machine code (and unwind info) alive.
    std::vector<std::unique_ptr<tpde_llvm::JITMapper>> mappers;
    // Kept-alive neon_op_t allocations referenced by compiled code.
    std::vector<std::unique_ptr<neon_op_t>> neon_ops;
};

llvmjit_engine_t *llvmjit_create(void) {
    auto *e = new (std::nothrow) llvmjit_engine();
    if (!e) return NULL;
    Triple triple(Triple::normalize(sys::getProcessTriple()));
    e->compiler = tpde_llvm::LLVMCompiler::create(triple);
    if (!e->compiler) {
        fprintf(stderr, "llvmjit: TPDE unsupported triple %s\n", triple.str().c_str());
        delete e;
        return NULL;
    }
    return e;
}

void llvmjit_destroy(llvmjit_engine_t *engine) { delete engine; }

extern "C" {
llvmjit_engine_t *codegen_engine_create(void) { return llvmjit_create(); }
void codegen_engine_destroy(llvmjit_engine_t *engine) { llvmjit_destroy(engine); }
jit_block_fn codegen_compile_block(llvmjit_engine_t *engine, const basic_block_t *bb,
                                   uint32_t slide) {
    (void)slide; // slide is unused: guest memory is identity-mapped
    return llvmjit_compile_block(engine, bb);
}
}

jit_block_fn llvmjit_compile_block(llvmjit_engine_t *e, const basic_block_t *bb) {
    Module mod("ios6_block", e->ctx);
    mod.setTargetTriple(Triple(sys::getProcessTriple()));
    mod.setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128");

    BlockCtx c(e->ctx, mod);
    c.thumb = bb->mode == ARM_MODE_THUMB;

    uint32_t default_next_pc = bb->start_address;
    if (bb->insn_count > 0) {
        const disassembled_insn_t *last = &bb->insns[bb->insn_count - 1];
        default_next_pc = last->address + last->size;
        if (c.thumb) default_next_pc |= 1;
    }
    c.st_next_pc_imm(default_next_pc);

    // extern void neon_exec(uint32_t *gtc, const neon_op_t *op);
    auto *neon_fty = FunctionType::get(c.b.getVoidTy(), {c.b.getPtrTy(), c.b.getPtrTy()}, false);
    Function *neon_fn = Function::Create(neon_fty, Function::ExternalLinkage,
                                         "neon_exec", &mod);

    for (size_t i = 0; i < bb->insn_count; i++) {
        const disassembled_insn_t *insn = &bb->insns[i];
        uint32_t pc_val = get_pc_value(insn->address, bb->mode);
        arm_cc cond = insn->cc;
        char bm[32];
        strncpy(bm, insn->mnemonic, sizeof bm - 1);
        bm[sizeof bm - 1] = 0;
        if (char *dot = strchr(bm, '.')) *dot = 0;
        strip_cc_suffix(bm, insn->cc);
        if (bm[0] == 'b' && strlen(bm) <= 4) cond = branch_mnemonic_cc(bm, insn->cc);

        BasicBlock *exec = BasicBlock::Create(e->ctx, "exec", c.fn);
        BasicBlock *skip = BasicBlock::Create(e->ctx, "skip", c.fn);
        c.b.CreateCondBr(lift_cond(c, cond), exec, skip);

        c.b.SetInsertPoint(exec);
        lift_insn(c, neon_fn, e->neon_ops, insn, pc_val);
        c.b.CreateBr(skip);
        c.b.SetInsertPoint(skip);
    }

    c.b.CreateRetVoid();

    if (getenv("IOS6_DUMP_LLVM")) {
        std::string s;
        llvm::raw_string_ostream os(s);
        mod.print(os, nullptr);
        fprintf(stderr, "%s\n", s.c_str());
    }
#ifndef NDEBUG
    if (verifyModule(mod, &llvm::errs())) {
        fprintf(stderr, "llvmjit: IR verification failed\n");
        return NULL;
    }
#endif

    auto mapper = e->compiler->compile_and_map(
        mod, +[](std::string_view name) -> void * {
            if (name == "ios6_unhandled_insn") return (void *)&ios6_unhandled_insn;
            if (name == "neon_exec") return (void *)&::neon_exec;
            return dlsym(RTLD_DEFAULT, std::string(name).c_str());
        });
    if (!mapper) {
        fprintf(stderr, "llvmjit: TPDE compile failed for block at 0x%08x\n", bb->start_address);
        return NULL;
    }
    jit_block_fn fn = (jit_block_fn)mapper.lookup_global(mod.getFunction("ios6_block"));
    if (!fn) {
        fprintf(stderr, "llvmjit: lookup failed for block at 0x%08x\n", bb->start_address);
        return NULL;
    }
    e->mappers.push_back(std::make_unique<tpde_llvm::JITMapper>(std::move(mapper)));
    return fn;
}
