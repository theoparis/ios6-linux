#include "ios6/neon.h"
#include "ios6/context.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest memory is identity-mapped, so guest addresses are host pointers. */
#define VR(off) ((uint8_t *)(uintptr_t)(NEON_REG_BASE + (uint32_t)(off)))
#define GM(addr) ((uint8_t *)(uintptr_t)(uint32_t)(addr))

static uint32_t s_fpscr;

/* ---------------------------------------------------------------- data types */

typedef struct {
    char kind;  /* 'i' 'u' 's' 'f' 'p' or 0 when untyped (".8") */
    int bits;
} dtype_t;

static dtype_t parse_dt(const char *s) {
    dtype_t t = {0, 32};
    if (!s || !*s) return t;
    if (*s >= 'a' && *s <= 'z') t.kind = *s++;
    if (*s >= '0' && *s <= '9') t.bits = atoi(s);
    return t;
}

/* first and second data type of e.g. "f64.s32" */
static void parse_dt2(const char *s, dtype_t *a, dtype_t *b) {
    *a = parse_dt(s);
    const char *dot = strchr(s, '.');
    *b = dot ? parse_dt(dot + 1) : *a;
}

static uint64_t eget(const uint8_t *p, int bits, int i) {
    switch (bits) {
        case 8: return p[i];
        case 16: { uint16_t v; memcpy(&v, p + 2 * i, 2); return v; }
        case 32: { uint32_t v; memcpy(&v, p + 4 * i, 4); return v; }
        default: { uint64_t v; memcpy(&v, p + 8 * i, 8); return v; }
    }
}

static void eset(uint8_t *p, int bits, int i, uint64_t v) {
    switch (bits) {
        case 8: p[i] = (uint8_t)v; break;
        case 16: { uint16_t w = (uint16_t)v; memcpy(p + 2 * i, &w, 2); break; }
        case 32: { uint32_t w = (uint32_t)v; memcpy(p + 4 * i, &w, 4); break; }
        default: memcpy(p + 8 * i, &v, 8); break;
    }
}

static int64_t sext(uint64_t v, int bits) {
    if (bits >= 64) return (int64_t)v;
    uint64_t m = 1ull << (bits - 1);
    v &= (m << 1) - 1;
    return (int64_t)((v ^ m) - m);
}

static uint64_t mask_bits(int bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }

static double fget(const uint8_t *p, int bits, int i) {
    if (bits == 32) { float f; memcpy(&f, p + 4 * i, 4); return f; }
    double d; memcpy(&d, p + 8 * i, 8); return d;
}

static void fset(uint8_t *p, int bits, int i, double v) {
    if (bits == 32) { float f = (float)v; memcpy(p + 4 * i, &f, 4); }
    else memcpy(p + 8 * i, &v, 8);
}

static const neon_operand_t *OP(const neon_op_t *op, int i) { return &op->ops[i]; }

static bool is(const neon_op_t *op, const char *m) { return strcmp(op->mn, m) == 0; }

/* -------------------------------------------------------------- lane-wise ops */

/* Returns true and stores result in *r for integer binary ops. */
static bool int_binop(const char *mn, char kind, int bits, uint64_t a, uint64_t b, uint64_t d, uint64_t *r) {
    uint64_t m = mask_bits(bits);
    int64_t sa = sext(a, bits), sb = sext(b, bits);
    bool sg = kind == 's';
    if (!strcmp(mn, "vadd")) *r = a + b;
    else if (!strcmp(mn, "vsub")) *r = a - b;
    else if (!strcmp(mn, "vmul")) *r = a * b;
    else if (!strcmp(mn, "vmla")) *r = d + a * b;
    else if (!strcmp(mn, "vmls")) *r = d - a * b;
    else if (!strcmp(mn, "vand")) *r = a & b;
    else if (!strcmp(mn, "vorr")) *r = a | b;
    else if (!strcmp(mn, "veor")) *r = a ^ b;
    else if (!strcmp(mn, "vbic")) *r = a & ~b;
    else if (!strcmp(mn, "vorn")) *r = a | ~b;
    else if (!strcmp(mn, "vbsl")) *r = (d & a) | (~d & b);
    else if (!strcmp(mn, "vbit")) *r = (a & b) | (d & ~b);
    else if (!strcmp(mn, "vbif")) *r = (d & b) | (a & ~b);
    else if (!strcmp(mn, "vceq")) *r = (a & m) == (b & m) ? m : 0;
    else if (!strcmp(mn, "vtst")) *r = (a & b & m) ? m : 0;
    else if (!strcmp(mn, "vcgt")) *r = (sg ? sa > sb : (a & m) > (b & m)) ? m : 0;
    else if (!strcmp(mn, "vcge")) *r = (sg ? sa >= sb : (a & m) >= (b & m)) ? m : 0;
    else if (!strcmp(mn, "vclt")) *r = (sg ? sa < sb : (a & m) < (b & m)) ? m : 0;
    else if (!strcmp(mn, "vcle")) *r = (sg ? sa <= sb : (a & m) <= (b & m)) ? m : 0;
    else if (!strcmp(mn, "vmax")) *r = sg ? (uint64_t)(sa > sb ? sa : sb) : ((a & m) > (b & m) ? a : b);
    else if (!strcmp(mn, "vmin")) *r = sg ? (uint64_t)(sa < sb ? sa : sb) : ((a & m) < (b & m) ? a : b);
    else if (!strcmp(mn, "vabd")) *r = sg ? (uint64_t)(sa > sb ? sa - sb : sb - sa) : ((a & m) > (b & m) ? a - b : b - a);
    else if (!strcmp(mn, "vhadd")) *r = sg ? (uint64_t)((sa + sb) >> 1) : (((a & m) + (b & m)) >> 1);
    else if (!strcmp(mn, "vrhadd")) *r = sg ? (uint64_t)((sa + sb + 1) >> 1) : (((a & m) + (b & m) + 1) >> 1);
    else if (!strcmp(mn, "vqadd")) {
        if (sg) { int64_t s = sa + sb, hi = (int64_t)(m >> 1), lo = -hi - 1; *r = (uint64_t)(s > hi ? hi : s < lo ? lo : s); }
        else { uint64_t s = (a & m) + (b & m); *r = s > m || s < (a & m) ? m : s; }
    } else if (!strcmp(mn, "vqsub")) {
        if (sg) { int64_t s = sa - sb, hi = (int64_t)(m >> 1), lo = -hi - 1; *r = (uint64_t)(s > hi ? hi : s < lo ? lo : s); }
        else *r = (a & m) > (b & m) ? a - b : 0;
    } else if (!strcmp(mn, "vshl")) { // register shift: signed shift amount in low byte of b
        int8_t sh = (int8_t)(b & 0xff);
        if (sh >= 0) *r = sh >= bits ? 0 : a << sh;
        else *r = sg ? (uint64_t)(sa >> (-sh >= bits ? bits - 1 : -sh)) : (-sh >= bits ? 0 : (a & m) >> -sh);
    } else return false;
    *r &= m;
    return true;
}

static bool flt_binop(const char *mn, double a, double b, double d, double *r, uint64_t *mask_out, bool *is_mask) {
    *is_mask = false;
    if (!strcmp(mn, "vadd")) *r = a + b;
    else if (!strcmp(mn, "vsub")) *r = a - b;
    else if (!strcmp(mn, "vmul")) *r = a * b;
    else if (!strcmp(mn, "vdiv")) *r = a / b;
    else if (!strcmp(mn, "vnmul")) *r = -(a * b);
    else if (!strcmp(mn, "vmla")) *r = d + a * b;
    else if (!strcmp(mn, "vmls")) *r = d - a * b;
    else if (!strcmp(mn, "vnmla")) *r = -d - a * b;
    else if (!strcmp(mn, "vnmls")) *r = -d + a * b;
    else if (!strcmp(mn, "vfma")) *r = fma(a, b, d);
    else if (!strcmp(mn, "vfms")) *r = fma(-a, b, d);
    else if (!strcmp(mn, "vmax")) *r = a > b ? a : b;
    else if (!strcmp(mn, "vmin")) *r = a < b ? a : b;
    else if (!strcmp(mn, "vabd")) *r = fabs(a - b);
    else if (!strcmp(mn, "vceq")) { *is_mask = true; *mask_out = a == b; }
    else if (!strcmp(mn, "vcgt")) { *is_mask = true; *mask_out = a > b; }
    else if (!strcmp(mn, "vcge")) { *is_mask = true; *mask_out = a >= b; }
    else if (!strcmp(mn, "vclt")) { *is_mask = true; *mask_out = a < b; }
    else if (!strcmp(mn, "vcle")) { *is_mask = true; *mask_out = a <= b; }
    else return false;
    return true;
}

static const char *s_binops[] = {
    "vadd", "vsub", "vmul", "vmla", "vmls", "vand", "vorr", "veor", "vbic", "vorn", "vbsl", "vbit",
    "vbif", "vceq", "vtst", "vcgt", "vcge", "vclt", "vcle", "vmax", "vmin", "vabd", "vhadd", "vrhadd",
    "vqadd", "vqsub", "vshl", "vdiv", "vnmul", "vnmla", "vnmls", "vfma", "vfms", NULL
};

static bool in_list(const char *mn, const char **l) {
    for (; *l; l++) if (!strcmp(mn, *l)) return true;
    return false;
}

/* ---------------------------------------------------------------- FP compare */

static void fp_compare(double a, double b) {
    uint32_t nzcv;
    if (isnan(a) || isnan(b)) nzcv = 0x3;      // C V
    else if (a == b) nzcv = 0x6;               // Z C
    else if (a < b) nzcv = 0x8;                // N
    else nzcv = 0x2;                           // C
    s_fpscr = (s_fpscr & 0x0fffffffu) | (nzcv << 28);
}

/* ------------------------------------------------------------ float <-> int */

static uint64_t f_to_int(double v, char kind, int bits) {
    if (isnan(v)) return 0;
    v = trunc(v);
    if (kind == 's') {
        double hi = (double)((1ll << (bits - 1)) - 1), lo = -(double)(1ll << (bits - 1));
        if (v > hi) v = hi;
        if (v < lo) v = lo;
        return (uint64_t)(int64_t)v & mask_bits(bits);
    }
    double hi = (double)mask_bits(bits);
    if (v < 0) v = 0;
    if (v > hi) v = hi;
    return (uint64_t)v;
}

/* ----------------------------------------------------- structured load/store */

static uint32_t reg(uint32_t *gtc, int r) { return r >= 0 && r < 15 ? gtc[r] : 0; }

static void ldst_multi(uint32_t *gtc, const neon_op_t *op, bool load) {
    // vldN/vstN {list}, [rn] {!} {, rm}
    int n = op->mn[3] - '0';                 // structure count 1..4
    dtype_t t = parse_dt(op->dt);
    int esz = t.bits / 8 ? t.bits / 8 : 1;
    int nregs = 0, mi = -1;
    for (int i = 0; i < op->nops; i++) {
        if (op->ops[i].kind == NOP_VREG) nregs++;
        else if (op->ops[i].kind == NOP_CORE && mi < 0 && i > 0 && op->ops[i - 1].kind == NOP_VREG) { mi = i; break; }
    }
    const neon_operand_t *base = &op->ops[mi];
    uint32_t addr = reg(gtc, base->core);
    bool lane = op->ops[0].lane >= 0;
    bool dup = op->ops[0].lane == -2; // all-lanes form "{d0[]}"
    uint32_t total;
    if (lane || dup) {
        int L = op->ops[0].lane;
        for (int r = 0; r < nregs; r++) {
            uint8_t *v = VR(op->ops[r].off);
            if (dup) {
                uint64_t e = eget(GM(addr) + r * esz, t.bits, 0);
                for (int k = 0; k < 8 / esz; k++) eset(v, t.bits, k, e);
            } else if (load) eset(v, t.bits, L, eget(GM(addr) + r * esz, t.bits, 0));
            else eset(GM(addr) + r * esz, t.bits, 0, eget(v, t.bits, L));
        }
        total = (uint32_t)(nregs * esz);
    } else {
        int per = 8 / esz;                      // elements per D register
        int groups = nregs / n;                 // D registers per structure member
        for (int g = 0; g < groups; g++)
            for (int k = 0; k < per; k++)
                for (int s = 0; s < n; s++) {
                    uint8_t *v = VR(op->ops[s * groups + g].off);
                    uint8_t *m = GM(addr) + ((g * per + k) * n + s) * esz;
                    if (n == 1) m = GM(addr) + (op->ops[s * groups + g].off - op->ops[0].off) + k * esz;
                    if (load) eset(v, t.bits, k, eget(m, t.bits, 0));
                    else eset(m, t.bits, 0, eget(v, t.bits, k));
                }
        total = (uint32_t)(nregs * 8);
    }
    if (mi + 1 < op->nops && op->ops[mi + 1].kind == NOP_CORE) gtc[base->core] = addr + reg(gtc, op->ops[mi + 1].core);
    else if (base->imm) gtc[base->core] = addr + total; // writeback "!"
}

/* --------------------------------------------------------------- dispatcher */

static bool exec(uint32_t *gtc, const neon_op_t *op, bool dry) {
    const char *mn = op->mn;
    dtype_t t, t2;
    parse_dt2(op->dt, &t, &t2);
    const neon_operand_t *d = OP(op, 0);

    if (!strncmp(mn, "vld", 3) || !strncmp(mn, "vst", 3)) {
        if (mn[3] < '1' || mn[3] > '4' || mn[4]) return false;
        if (!dry) ldst_multi(gtc, op, mn[1] == 'l');
        return true;
    }
    if (is(op, "vmrs")) {
        if (!dry) {
            if (d->kind == NOP_APSR) gtc[GTC_FIELD_CPSR] = (gtc[GTC_FIELD_CPSR] & 0x0fffffffu) | (s_fpscr & 0xf0000000u);
            else gtc[d->core] = s_fpscr;
        }
        return true;
    }
    if (is(op, "vmsr")) { if (!dry) s_fpscr = reg(gtc, OP(op, 1)->core); return true; }

    // vmov / vdup with lanes and core registers
    if (is(op, "vmov") || is(op, "vdup")) {
        const neon_operand_t *s = OP(op, 1);
        int bits = t.bits ? t.bits : 32;
        if (op->nops == 2 && d->kind == NOP_CORE && s->kind == NOP_VREG && s->lane >= 0) {
            if (!dry) { uint64_t e = eget(VR(s->off), bits, s->lane); gtc[d->core] = (uint32_t)(t.kind == 's' ? (uint64_t)sext(e, bits) : e); }
            return true;
        }
        if (op->nops == 2 && d->kind == NOP_VREG && d->lane >= 0 && s->kind == NOP_CORE) {
            if (!dry) eset(VR(d->off), bits, d->lane, reg(gtc, s->core));
            return true;
        }
        if (op->nops == 2 && d->kind == NOP_VREG && s->kind == NOP_VREG && s->lane >= 0 && is(op, "vdup")) {
            if (!dry) { uint64_t e = eget(VR(s->off), bits, s->lane); for (int k = 0; k < d->size * 8 / bits; k++) eset(VR(d->off), bits, k, e); }
            return true;
        }
        if (op->nops == 2 && d->kind == NOP_VREG && s->kind == NOP_CORE && is(op, "vdup")) {
            if (!dry) { uint64_t e = reg(gtc, s->core); for (int k = 0; k < d->size * 8 / bits; k++) eset(VR(d->off), bits, k, e); }
            return true;
        }
        if (op->nops == 2 && d->kind == NOP_VREG && s->kind == NOP_FP) {
            if (!dry) { int fb = d->size == 4 ? 32 : t.bits == 64 ? 64 : 32; for (int k = 0; k < d->size * 8 / fb; k++) fset(VR(d->off), fb, k, s->fp); }
            return true;
        }
        if (op->nops == 2 && d->kind == NOP_VREG && s->kind == NOP_IMM) {
            if (!dry) {
                if (t.bits == 64) { // byte mask immediate
                    uint64_t v = 0;
                    for (int b = 0; b < 8; b++) if ((s->imm >> b) & 1) v |= 0xffull << (8 * b);
                    for (int k = 0; k < d->size / 8; k++) eset(VR(d->off), 64, k, v);
                } else for (int k = 0; k < d->size * 8 / t.bits; k++) eset(VR(d->off), t.bits, k, (uint64_t)s->imm);
            }
            return true;
        }
        if (op->nops == 2 && d->kind == NOP_VREG && s->kind == NOP_VREG) {
            if (!dry) memmove(VR(d->off), VR(s->off), d->size < s->size ? d->size : s->size);
            return true;
        }
        return false;
    }
    if (is(op, "vmvn") && op->nops == 2) {
        const neon_operand_t *s = OP(op, 1);
        if (!dry) {
            if (s->kind == NOP_IMM) for (int k = 0; k < d->size / 4; k++) eset(VR(d->off), 32, k, ~(uint32_t)s->imm);
            else for (int k = 0; k < d->size; k++) VR(d->off)[k] = (uint8_t)~VR(s->off)[k];
        }
        return true;
    }
    if (is(op, "vswp")) {
        if (!dry) { uint8_t tmp[16]; memcpy(tmp, VR(d->off), d->size); memcpy(VR(d->off), VR(OP(op, 1)->off), d->size); memcpy(VR(OP(op, 1)->off), tmp, d->size); }
        return true;
    }

    // VFP compare
    if (is(op, "vcmp") || is(op, "vcmpe")) {
        if (!dry) {
            int fb = t.bits;
            double a = fget(VR(d->off), fb, 0);
            double b = OP(op, 1)->kind == NOP_VREG ? fget(VR(OP(op, 1)->off), fb, 0) : OP(op, 1)->kind == NOP_FP ? OP(op, 1)->fp : 0.0;
            fp_compare(a, b);
        }
        return true;
    }

    // conversions
    if (is(op, "vcvt") || is(op, "vcvtr")) {
        const neon_operand_t *s = OP(op, 1);
        int fbits = op->nops == 3 && OP(op, 2)->kind == NOP_IMM ? (int)OP(op, 2)->imm : 0;
        double scale = ldexp(1.0, fbits);
        int lanes = (d->size >= 8 && s->size >= 8 && t.bits == 32 && t2.bits == 32) ? d->size / 4 : 1;
        if (!dry) for (int k = 0; k < lanes; k++) {
            if (t.kind == 'f' && t2.kind == 'f') fset(VR(d->off), t.bits, k, fget(VR(s->off), t2.bits, k));
            else if (t.kind == 'f') { // int -> float
                uint64_t e = eget(VR(s->off), t2.bits, k);
                double v = t2.kind == 's' ? (double)sext(e, t2.bits) : (double)e;
                fset(VR(d->off), t.bits, k, v / scale);
            } else { // float -> int
                double v = fget(VR(s->off), t2.bits, k) * scale;
                if (is(op, "vcvtr")) v = nearbyint(v);
                eset(VR(d->off), t.bits, k, f_to_int(v, t.kind, t.bits));
            }
        }
        return true;
    }

    // unary float / int
    if ((is(op, "vneg") || is(op, "vabs") || is(op, "vsqrt")) && op->nops == 2) {
        const neon_operand_t *s = OP(op, 1);
        if (!dry) {
            if (t.kind == 'f') {
                for (int k = 0; k < d->size * 8 / t.bits; k++) {
                    double v = fget(VR(s->off), t.bits, k);
                    fset(VR(d->off), t.bits, k, is(op, "vneg") ? -v : is(op, "vabs") ? fabs(v) : sqrt(v));
                }
            } else {
                for (int k = 0; k < d->size * 8 / t.bits; k++) {
                    int64_t v = sext(eget(VR(s->off), t.bits, k), t.bits);
                    eset(VR(d->off), t.bits, k, (uint64_t)(is(op, "vneg") ? -v : v < 0 ? -v : v));
                }
            }
        }
        return true;
    }
    if ((is(op, "vcnt") || is(op, "vclz")) && op->nops == 2) {
        if (!dry) for (int k = 0; k < d->size * 8 / t.bits; k++) {
            uint64_t v = eget(VR(OP(op, 1)->off), t.bits, k);
            int c = 0;
            if (is(op, "vcnt")) c = __builtin_popcountll(v);
            else { for (int b = t.bits - 1; b >= 0 && !((v >> b) & 1); b--) c++; }
            eset(VR(d->off), t.bits, k, (uint64_t)c);
        }
        return true;
    }
    if (!strncmp(mn, "vrev", 4)) {
        int cont = atoi(mn + 4);  // container bits
        if (!dry) {
            uint8_t tmp[16];
            const uint8_t *src = VR(OP(op, 1)->off);
            int per = cont / t.bits;
            for (int k = 0; k < d->size * 8 / t.bits; k++) {
                int base = k - k % per;
                eset(tmp, t.bits, base + (per - 1 - k % per), eget(src, t.bits, k));
            }
            memcpy(VR(d->off), tmp, d->size);
        }
        return true;
    }
    if (is(op, "vext")) {
        if (!dry) {
            uint8_t tmp[32];
            int sz = d->size, k = (int)OP(op, 3)->imm * (t.bits / 8 ? t.bits / 8 : 1);
            memcpy(tmp, VR(OP(op, 1)->off), sz);
            memcpy(tmp + sz, VR(OP(op, 2)->off), sz);
            memcpy(VR(d->off), tmp + k, sz);
        }
        return true;
    }
    if (is(op, "vtbl") || is(op, "vtbx")) {
        if (!dry) {
            int nt = op->nops - 2;
            uint8_t table[32], idx[8], out[8];
            for (int i = 0; i < nt; i++) memcpy(table + 8 * i, VR(OP(op, 1 + i)->off), 8);
            memcpy(idx, VR(OP(op, op->nops - 1)->off), 8);
            memcpy(out, VR(d->off), 8);
            for (int i = 0; i < 8; i++) {
                if (idx[i] < nt * 8) out[i] = table[idx[i]];
                else if (is(op, "vtbl")) out[i] = 0;
            }
            memcpy(VR(d->off), out, 8);
        }
        return true;
    }
    if (is(op, "vzip") || is(op, "vuzp") || is(op, "vtrn")) {
        if (!dry) {
            uint8_t a[16], b[16], ra[16], rb[16];
            int sz = d->size, n = sz * 8 / t.bits;
            memcpy(a, VR(d->off), sz); memcpy(b, VR(OP(op, 1)->off), sz);
            for (int k = 0; k < n; k++) {
                if (is(op, "vzip")) {
                    uint64_t src = (k % 2 == 0 ? eget(a, t.bits, k / 2) : eget(b, t.bits, k / 2));
                    eset(ra, t.bits, k, src);
                    uint64_t src2 = (k % 2 == 0 ? eget(a, t.bits, n / 2 + k / 2) : eget(b, t.bits, n / 2 + k / 2));
                    eset(rb, t.bits, k, src2);
                } else if (is(op, "vuzp")) {
                    uint64_t e = k < n / 2 ? eget(a, t.bits, 2 * k) : eget(b, t.bits, 2 * (k - n / 2));
                    uint64_t o = k < n / 2 ? eget(a, t.bits, 2 * k + 1) : eget(b, t.bits, 2 * (k - n / 2) + 1);
                    eset(ra, t.bits, k, e); eset(rb, t.bits, k, o);
                } else {
                    if (k % 2 == 0) { eset(ra, t.bits, k, eget(a, t.bits, k)); eset(rb, t.bits, k, eget(a, t.bits, k + 1)); }
                    else { eset(ra, t.bits, k, eget(b, t.bits, k - 1)); eset(rb, t.bits, k, eget(b, t.bits, k)); }
                }
            }
            memcpy(VR(d->off), ra, sz); memcpy(VR(OP(op, 1)->off), rb, sz);
        }
        return true;
    }

    // widening / narrowing moves
    if (is(op, "vmovl") || is(op, "vmovn") || is(op, "vqmovn") || is(op, "vqmovun")) {
        if (!dry) {
            const neon_operand_t *s = OP(op, 1);
            uint8_t tmp[16];
            if (is(op, "vmovl")) {
                for (int k = 0; k < 8 * 8 / t.bits; k++) {
                    uint64_t e = eget(VR(s->off), t.bits, k);
                    eset(tmp, t.bits * 2, k, t.kind == 's' ? (uint64_t)sext(e, t.bits) : e);
                }
                memcpy(VR(d->off), tmp, 16);
            } else {
                int nb = t.bits / 2; // dt is the source width (e.g. vmovn.i32 -> 16-bit lanes)
                for (int k = 0; k < 64 / nb; k++) {
                    uint64_t e = eget(VR(s->off), t.bits, k);
                    if (!is(op, "vmovn")) {
                        uint64_t hi = mask_bits(nb);
                        if (t.kind == 's' && is(op, "vqmovn")) {
                            int64_t v = sext(e, t.bits), mx = (int64_t)(hi >> 1), mnv = -mx - 1;
                            e = (uint64_t)(v > mx ? mx : v < mnv ? mnv : v);
                        } else if (is(op, "vqmovun")) {
                            int64_t v = sext(e, t.bits);
                            e = v < 0 ? 0 : (uint64_t)v > hi ? hi : (uint64_t)v;
                        } else if (e > hi) e = hi;
                    }
                    eset(tmp, nb, k, e);
                }
                memcpy(VR(d->off), tmp, 8);
            }
        }
        return true;
    }

    // shifts by immediate
    if ((is(op, "vshl") || is(op, "vshr") || is(op, "vsra") || is(op, "vrshr") || is(op, "vsri") ||
         is(op, "vsli") || is(op, "vshrn") || is(op, "vshll") || is(op, "vrshrn")) &&
        OP(op, op->nops - 1)->kind == NOP_IMM) {
        if (!dry) {
            const neon_operand_t *s = OP(op, 1);
            int sh = (int)OP(op, op->nops - 1)->imm;
            uint8_t tmp[16];
            int bits = t.bits;
            if (is(op, "vshrn") || is(op, "vrshrn")) {
                for (int k = 0; k < 64 / (bits / 2); k++) {
                    uint64_t e = eget(VR(s->off), bits, k);
                    if (is(op, "vrshrn")) e += 1ull << (sh - 1);
                    eset(tmp, bits / 2, k, e >> sh);
                }
                memcpy(VR(d->off), tmp, 8);
            } else if (is(op, "vshll")) {
                for (int k = 0; k < 64 / bits; k++) {
                    uint64_t e = eget(VR(s->off), bits, k);
                    if (t.kind == 's') e = (uint64_t)sext(e, bits);
                    eset(tmp, bits * 2, k, e << sh);
                }
                memcpy(VR(d->off), tmp, 16);
            } else {
                uint64_t m = mask_bits(bits);
                for (int k = 0; k < d->size * 8 / bits; k++) {
                    uint64_t e = eget(VR(s->off), bits, k), dv = eget(VR(d->off), bits, k), r;
                    if (is(op, "vshl")) r = sh >= bits ? 0 : e << sh;
                    else if (is(op, "vsli")) r = ((e << sh) & m) | (dv & ((1ull << sh) - 1));
                    else if (is(op, "vsri")) { uint64_t keep = sh >= bits ? m : ~(m >> sh) & m; r = ((e & m) >> sh) | (dv & keep); }
                    else {
                        if (is(op, "vrshr")) e = t.kind == 's' ? (uint64_t)(sext(e, bits) + (1ll << (sh - 1))) : e + (1ull << (sh - 1));
                        uint64_t v = t.kind == 's' ? (uint64_t)(sext(e, bits) >> (sh >= bits ? bits - 1 : sh))
                                                   : (sh >= bits ? 0 : (e & m) >> sh);
                        r = is(op, "vsra") ? dv + v : v;
                    }
                    eset(VR(d->off), bits, k, r & m);
                }
            }
        }
        return true;
    }

    // pairwise add (integer and float)
    if (is(op, "vpadd") && op->nops == 3) {
        if (!dry) {
            uint8_t tmp[8];
            int n = 64 / t.bits;
            for (int k = 0; k < n; k++) {
                const uint8_t *src = VR((k < n / 2 ? OP(op, 1) : OP(op, 2))->off);
                int j = (k % (n / 2)) * 2;
                if (t.kind == 'f') fset(tmp, t.bits, k, fget(src, t.bits, j) + fget(src, t.bits, j + 1));
                else eset(tmp, t.bits, k, eget(src, t.bits, j) + eget(src, t.bits, j + 1));
            }
            memcpy(VR(d->off), tmp, 8);
        }
        return true;
    }
    if (is(op, "vpaddl") || is(op, "vpadal")) {
        if (!dry) {
            uint8_t tmp[16];
            const uint8_t *src = VR(OP(op, 1)->off);
            int n = d->size * 8 / (t.bits * 2);
            for (int k = 0; k < n; k++) {
                uint64_t a = eget(src, t.bits, 2 * k), b = eget(src, t.bits, 2 * k + 1);
                uint64_t s = t.kind == 's' ? (uint64_t)(sext(a, t.bits) + sext(b, t.bits)) : a + b;
                if (is(op, "vpadal")) s += eget(VR(d->off), t.bits * 2, k);
                eset(tmp, t.bits * 2, k, s);
            }
            memcpy(VR(d->off), tmp, d->size);
        }
        return true;
    }

    // widening multiply / add (vmull, vaddl, vsubl, vaddw, vsubw, vmlal, vmlsl)
    if (is(op, "vmull") || is(op, "vaddl") || is(op, "vsubl") || is(op, "vaddw") || is(op, "vsubw") ||
        is(op, "vmlal") || is(op, "vmlsl")) {
        if (!dry) {
            uint8_t tmp[16];
            const neon_operand_t *a = OP(op, 1), *b = OP(op, 2);
            int wb = t.bits * 2;
            bool wide_a = is(op, "vaddw") || is(op, "vsubw");
            for (int k = 0; k < 64 / t.bits; k++) {
                uint64_t x = wide_a ? eget(VR(a->off), wb, k) : eget(VR(a->off), t.bits, k);
                uint64_t y = b->lane >= 0 ? eget(VR(b->off), t.bits, b->lane) : eget(VR(b->off), t.bits, k);
                if (t.kind == 's') { if (!wide_a) x = (uint64_t)sext(x, t.bits); y = (uint64_t)sext(y, t.bits); }
                uint64_t acc = eget(VR(d->off), wb, k), r;
                if (t.kind == 'p' && is(op, "vmull")) { r = 0; for (int bi = 0; bi < t.bits; bi++) if ((y >> bi) & 1) r ^= x << bi; }
                else if (is(op, "vmull")) r = x * y;
                else if (is(op, "vmlal")) r = acc + x * y;
                else if (is(op, "vmlsl")) r = acc - x * y;
                else if (is(op, "vaddl") || is(op, "vaddw")) r = x + y;
                else r = x - y;
                eset(tmp, wb, k, r);
            }
            memcpy(VR(d->off), tmp, 16);
        }
        return true;
    }

    // generic binary lane-wise (int or float), incl. 2-operand "vadd d0, d1" and immediate forms
    if (in_list(mn, s_binops)) {
        const neon_operand_t *a, *b;
        if (op->nops == 2) { a = d; b = OP(op, 1); }
        else { a = OP(op, 1); b = OP(op, 2); }
        if (a->kind != NOP_VREG) return false;
        if (dry) return true;
        int bits = t.bits ? t.bits : 64;
        if (t.kind == 0 || t.kind == 'i' || t.kind == 'u' || t.kind == 's' || t.kind == 'p') {
            int width = d->size;
            for (int k = 0; k < width * 8 / bits; k++) {
                uint64_t x = eget(VR(a->off), bits, k);
                uint64_t y = b->kind == NOP_VREG ? (b->lane >= 0 ? eget(VR(b->off), bits, b->lane) : eget(VR(b->off), bits, k))
                                                 : (uint64_t)b->imm;
                uint64_t dv = eget(VR(d->off), bits, k), r = 0;
                int_binop(mn, t.kind, bits, x, y, dv, &r);
                eset(VR(d->off), bits, k, r);
            }
        } else {
            for (int k = 0; k < d->size * 8 / bits; k++) {
                double x = fget(VR(a->off), bits, k);
                double y = b->kind == NOP_VREG ? fget(VR(b->off), bits, b->lane >= 0 ? b->lane : k)
                                               : b->kind == NOP_FP ? b->fp : 0.0;
                double dv = fget(VR(d->off), bits, k), r = 0;
                uint64_t mk = 0; bool is_mask;
                flt_binop(mn, x, y, dv, &r, &mk, &is_mask);
                if (is_mask) eset(VR(d->off), bits, k, mk ? mask_bits(bits) : 0);
                else fset(VR(d->off), bits, k, r);
            }
        }
        return true;
    }
    // compare against #0 (vceq.i8 d0, d1, #0)
    return false;
}

bool neon_supported(const neon_op_t *op) {
    return exec(NULL, op, true);
}

void neon_exec(uint32_t *gtc, const neon_op_t *op) {
    static int trace = -1;
    if (trace < 0) trace = getenv("IOS6_TRACE_NEON") != NULL;
    exec(gtc, op, false);
    if (trace) {
        fprintf(stderr, "[neon] %08x %s.%s", op->addr, op->mn, op->dt);
        for (int i = 0; i < op->nops; i++) {
            const neon_operand_t *o = &op->ops[i];
            if (o->kind == NOP_VREG) {
                uint64_t v; memcpy(&v, VR(o->off & ~7), 8);
                fprintf(stderr, " v%d/%d=%016llx", o->off, o->size, (unsigned long long)v);
            } else if (o->kind == NOP_CORE) fprintf(stderr, " r%d=%x", o->core, o->core >= 0 && o->core < 15 ? gtc[o->core] : 0);
            else if (o->kind == NOP_IMM) fprintf(stderr, " #%lld", (long long)o->imm);
            else if (o->kind == NOP_FP) fprintf(stderr, " #%g", o->fp);
        }
        fprintf(stderr, " fpscr=%08x\n", s_fpscr);
    }
}
