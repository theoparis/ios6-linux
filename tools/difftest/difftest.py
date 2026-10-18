#!/usr/bin/env python3
"""
Differential tester: qemu-arm (oracle) vs the GNU Lightning JIT.

Each test is a short ARM or Thumb-2 instruction sequence. It is assembled with llvm-mc and run
with random register / NZCV inputs both (a) natively under qemu-arm and (b) through the JIT via
build/difftest_jit. Registers r0-r12, NZCV and a 256-byte scratch buffer must match.

Usage: difftest.py [-k substring] [-n inputs] [--seed N] [-v] [--mode arm|thumb|all]
"""
import argparse, os, random, re, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JIT = os.path.join(ROOT, "build", "difftest_jit")
SCRATCH = 0x40000000
NSCRATCH = 64  # words

INTERESTING = [0, 1, 2, 3, 7, 8, 0xf, 0x1f, 0x20, 0x21, 0x3f, 0x40, 0x7f, 0x80, 0xff, 0x100, 0x1ff,
               0x7fff, 0x8000, 0xffff, 0x10000, 0x12345678, 0x7fffffff, 0x80000000, 0x80000001,
               0xdeadbeef, 0xfffffffe, 0xffffffff, 0xffff0000, 0x0000ffff, 0xff00ff00, 0x55555555]

CONDS = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]


def sh_variants(op, rd="r0", rn="r1"):
    """Data-processing forms with every shifter operand kind."""
    out = [f"{op} {rd}, {rn}, #0xff", f"{op} {rd}, {rn}, r2", f"{op} {rd}, {rn}, #0x80000000"]
    for sh in ("lsl", "lsr", "asr", "ror"):
        out += [f"{op} {rd}, {rn}, r2, {sh} #1", f"{op} {rd}, {rn}, r2, {sh} #7",
                f"{op} {rd}, {rn}, r2, {sh} #31", f"{op} {rd}, {rn}, r2, {sh} r3"]
    out += [f"{op} {rd}, {rn}, r2, lsr #32", f"{op} {rd}, {rn}, r2, asr #32", f"{op} {rd}, {rn}, r2, rrx"]
    return out


def arm_tests():
    t = []
    for op in ["add", "adds", "sub", "subs", "rsb", "rsbs", "adc", "adcs", "sbc", "sbcs", "rsc", "rscs",
               "and", "ands", "orr", "orrs", "eor", "eors", "bic", "bics"]:
        t += sh_variants(op)
    for op in ["mov", "movs", "mvn", "mvns"]:
        t += [f"{op} r0, #0xff", f"{op} r0, r2", f"{op} r0, #0xf000000f"]
        for sh in ("lsl", "lsr", "asr", "ror"):
            t += [f"{op} r0, r2, {sh} #1", f"{op} r0, r2, {sh} #13", f"{op} r0, r2, {sh} r3"]
        t += [f"{op} r0, r2, rrx", f"{op} r0, r2, lsr #32"]
    for op in ["cmp", "cmn", "tst", "teq"]:
        t += [f"{op} r1, #0x80", f"{op} r1, r2", f"{op} r1, r2, lsl #4", f"{op} r1, r2, lsr r3",
              f"{op} r1, r2, asr #31", f"{op} r1, r2, ror #5"]
    # shift instructions (mnemonic form)
    for op in ["lsl", "lsls", "lsr", "lsrs", "asr", "asrs", "ror", "rors"]:
        t += [f"{op} r0, r1, #1", f"{op} r0, r1, #31", f"{op} r0, r1, r2"]
    t += ["rrx r0, r1", "rrxs r0, r1"]
    # multiplies
    t += ["mul r0, r1, r2", "muls r0, r1, r2", "mla r0, r1, r2, r3", "mls r0, r1, r2, r3",
          "umull r0, r4, r1, r2", "smull r0, r4, r1, r2", "umlal r0, r4, r1, r2", "smlal r0, r4, r1, r2",
          "smmul r0, r1, r2", "smmla r0, r1, r2, r3", "smmulr r0, r1, r2"]
    # misc
    t += ["clz r0, r1", "rbit r0, r1", "rev r0, r1", "rev16 r0, r1", "revsh r0, r1",
          "sxtb r0, r1", "sxth r0, r1", "uxtb r0, r1", "uxth r0, r1",
          "sxtb r0, r1, ror #8", "uxtb r0, r1, ror #16", "uxth r0, r1, ror #8",
          "sxtab r0, r2, r1", "uxtab r0, r2, r1", "uxtah r0, r2, r1", "sxtah r0, r2, r1",
          "ubfx r0, r1, #3, #7", "sbfx r0, r1, #3, #7", "ubfx r0, r1, #0, #32", "sbfx r0, r1, #31, #1",
          "bfi r0, r1, #4, #8", "bfi r0, r1, #0, #32", "bfc r0, #8, #8",
          "movw r0, #0xabcd", "movt r0, #0x1234"]
    # conditional execution of a few ops under every condition
    for c in CONDS:
        t += [f"mov{c} r0, #1", f"add{c} r0, r1, r2", f"sub{c}s r0, r1, r2", f"orr{c} r0, r1, r2, lsl #3"]
    return t


def thumb_tests():
    t = []
    for op in ["add", "adds", "sub", "subs", "adc", "adcs", "sbc", "sbcs", "rsb", "and", "ands", "orr", "orrs",
               "eor", "eors", "bic", "bics", "orn", "orns"]:
        t += [f"{op}.w r0, r1, r2", f"{op}.w r0, r1, #0xff", f"{op}.w r0, r1, r2, lsl #3",
              f"{op}.w r0, r1, r2, lsr #5", f"{op}.w r0, r1, r2, asr #9", f"{op}.w r0, r1, r2, ror #11"]
    for op in ["adds", "subs", "ands", "orrs", "eors", "adcs", "sbcs", "bics", "movs", "mvns", "lsls", "lsrs", "asrs", "rors"]:
        t += [f"{op} r0, r1" if op in ("movs", "mvns") else f"{op} r0, r1" ]
    t += ["adds r0, r1, #5", "subs r0, r1, #5", "adds r0, #200", "subs r0, #200", "rsbs r0, r1, #0",
          "cmp r1, #100", "cmp r1, r2", "cmn r1, r2", "tst r1, r2", "teq.w r1, r2", "cmp.w r1, r2, lsl #3",
          "lsls r0, r1, #3", "lsrs r0, r1, #3", "asrs r0, r1, #3", "lsl.w r0, r1, r2", "lsr.w r0, r1, r2",
          "asr.w r0, r1, r2", "ror.w r0, r1, r2", "lsls.w r0, r1, r2", "lsrs.w r0, r1, r2",
          "asrs.w r0, r1, r2", "rors.w r0, r1, r2", "rrx r0, r1", "rrxs r0, r1",
          "mul r0, r1, r0", "muls r0, r1, r0", "mul.w r0, r1, r2", "mla r0, r1, r2, r3", "mls r0, r1, r2, r3",
          "umull r0, r4, r1, r2", "smull r0, r4, r1, r2", "umlal r0, r4, r1, r2", "smlal r0, r4, r1, r2",
          "clz r0, r1", "rbit r0, r1", "rev r0, r1", "rev16 r0, r1", "revsh r0, r1",
          "sxtb r0, r1", "sxth r0, r1", "uxtb r0, r1", "uxth r0, r1", "uxtb.w r0, r1, ror #8",
          "uxtab r0, r2, r1", "sxtah r0, r2, r1", "ubfx r0, r1, #3, #7", "sbfx r0, r1, #3, #7",
          "bfi r0, r1, #4, #8", "bfc r0, #8, #8", "movw r0, #0xabcd", "movt r0, #0x1234",
          "mov.w r0, #0x00ff00ff", "mvn r0, #0xff", "add.w r0, r1, #0x1200", "addw r0, r1, #0xfff",
          "subw r0, r1, #0xfff"]
    # IT blocks
    for c in CONDS:
        t += [f"it {c}; mov{c} r0, #1", f"ite {c}; add{c} r0, r1, r2; subs r0, r1, r2".replace("subs", "sub" + {"eq":"ne","ne":"eq","hs":"lo","lo":"hs","mi":"pl","pl":"mi","vs":"vc","vc":"vs","hi":"ls","ls":"hi","ge":"lt","lt":"ge","gt":"le","le":"gt"}[c])]
        t += [f"itt {c}; mov{c} r0, #5; add{c} r3, r3, #1"]
    return t


MEM_TESTS = {
    # (test, kind) : base reg r1 = SCRATCH+0x80, index r2 small
    "arm": [
        "ldr r0, [r1]", "ldr r0, [r1, #4]", "ldr r0, [r1, #-4]", "ldr r0, [r1, #8]!", "ldr r0, [r1, #-8]!",
        "ldr r0, [r1], #4", "ldr r0, [r1], #-4", "ldr r0, [r1, r2]", "ldr r0, [r1, -r2]", "ldr r0, [r1, r2]!",
        "ldr r0, [r1, r2, lsl #2]", "ldr r0, [r1], r2", "ldr r0, [r1], -r2",
        "ldrb r0, [r1, #3]", "ldrb r0, [r1, #1]!", "ldrb r0, [r1], #1", "ldrb r0, [r1, r2]",
        "ldrsb r0, [r1, #3]", "ldrsb r0, [r1, #2]!", "ldrsb r0, [r1], #1", "ldrsb r0, [r1, r2]",
        "ldrh r0, [r1, #2]", "ldrh r0, [r1, #2]!", "ldrh r0, [r1], #2", "ldrh r0, [r1, r2]",
        "ldrsh r0, [r1, #2]", "ldrsh r0, [r1, #2]!", "ldrsh r0, [r1], #2", "ldrsh r0, [r1, r2]",
        "ldrd r0, r3, [r1]", "ldrd r0, r3, [r1, #8]", "ldrd r0, r3, [r1, #8]!", "ldrd r0, r3, [r1], #8",
        "ldrd r0, r3, [r1, r2]",
        "str r3, [r1]", "str r3, [r1, #4]", "str r3, [r1, #-4]", "str r3, [r1, #8]!", "str r3, [r1], #4",
        "str r3, [r1, r2]", "str r3, [r1, r2, lsl #2]", "str r3, [r1], r2",
        "strb r3, [r1, #3]", "strb r3, [r1, #1]!", "strb r3, [r1], #1", "strb r3, [r1, r2]",
        "strh r3, [r1, #2]", "strh r3, [r1, #2]!", "strh r3, [r1], #2", "strh r3, [r1, r2]",
        "strd r4, r5, [r1]", "strd r4, r5, [r1, #8]", "strd r4, r5, [r1, #8]!", "strd r4, r5, [r1], #8",
        "ldm r1, {r0, r3, r4}", "ldm r1!, {r0, r3, r4}", "ldmib r1, {r0, r3, r4}", "ldmda r1, {r0, r3, r4}",
        "ldmdb r1!, {r0, r3, r4}", "stm r1, {r3, r4, r5}", "stm r1!, {r3, r4, r5}", "stmib r1, {r3, r4, r5}",
        "stmda r1, {r3, r4, r5}", "stmdb r1!, {r3, r4, r5}",
        "ldrex r0, [r1]", "strex r0, r3, [r1]", "ldrexb r0, [r1]", "ldrexh r0, [r1]",
        "ldrhi r0, [r1, #4]", "strne r3, [r1, #4]!", "ldrbeq r0, [r1], #1", "ldrblo r0, [r1, #1]!",
        "ldrbhi r0, [r1, #1]!", "ldrshne r0, [r1, #2]", "ldmhi r1!, {r0, r3}",
    ],
    "thumb": [
        "ldr r0, [r1]", "ldr r0, [r1, #4]", "ldr r0, [r1, #-4]", "ldr.w r0, [r1, #8]!", "ldr.w r0, [r1], #4",
        "ldr r0, [r1, r2]", "ldr.w r0, [r1, r2, lsl #2]", "ldrb r0, [r1, #3]", "ldrb.w r0, [r1, #1]!",
        "ldrb.w r0, [r1], #1", "ldrb r0, [r1, r2]", "ldrsb r0, [r1, r2]", "ldrsb.w r0, [r1, #2]!",
        "ldrh r0, [r1, #2]", "ldrh.w r0, [r1, #2]!", "ldrh.w r0, [r1], #2", "ldrh r0, [r1, r2]",
        "ldrsh r0, [r1, r2]", "ldrsh.w r0, [r1, #2]!", "ldrd r0, r3, [r1]", "ldrd r0, r3, [r1, #8]!",
        "ldrd r0, r3, [r1], #8", "str r3, [r1]", "str r3, [r1, #4]", "str.w r3, [r1, #-4]", "str.w r3, [r1, #8]!",
        "str.w r3, [r1], #4", "str r3, [r1, r2]", "str.w r3, [r1, r2, lsl #2]", "strb r3, [r1, #3]",
        "strb.w r3, [r1, #1]!", "strb.w r3, [r1], #1", "strh r3, [r1, #2]", "strh.w r3, [r1, #2]!",
        "strd r4, r5, [r1, #8]", "strd r4, r5, [r1, #8]!", "strd r4, r5, [r1], #8",
        "ldm r1, {r0, r3, r4}", "ldm r1!, {r0, r3, r4}", "ldmdb r1!, {r0, r3, r4}", "ldmdb r1, {r0, r3, r4}",
        "stm r1, {r3, r4, r5}", "stm r1!, {r3, r4, r5}", "stmdb r1!, {r3, r4, r5}", "stmdb r1, {r3, r4, r5}",
        "ldrex r0, [r1]", "strex r0, r3, [r1]", "ldrexb r0, [r1]", "strexb r0, r3, [r1]",
        "it hi; ldrbhi r0, [r1, #1]!", "it ne; strne r3, [r1, #4]", "ite eq; ldreq r0, [r1]; ldrne r0, [r1, #4]",
        "tbb [r1, r2]", "tbh [r1, r2, lsl #1]",
    ],
}


def assemble(mode, seq):
    """Assemble 'a; b; c' in the given mode; returns code bytes."""
    src = (".syntax unified\n.thumb\n" if mode == "thumb" else ".syntax unified\n.arm\n") + \
          "\n".join(x.strip() for x in seq.split(";")) + "\n"
    r = subprocess.run(["llvm-mc", "-triple=armv7-linux-gnueabi", "-mattr=+thumb2,+v7,+dsp,+neon,+vfp3", "-show-encoding"],
                       input=src, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    out = bytearray()
    for m in re.finditer(r"encoding: \[([^\]]*)\]", r.stdout):
        out += bytes(int(x, 16) for x in m.group(1).split(","))
    return bytes(out)


def rand_inputs(rng, n, mem):
    ins = []
    for _ in range(n):
        regs = [rng.choice(INTERESTING) if rng.random() < 0.8 else rng.getrandbits(32) for _ in range(13)]
        if mem:
            regs[1] = SCRATCH + 0x80
            regs[2] = rng.choice([0, 1, 2, 4, 8, 12, 16, 0xfffffff8, 0xfffffffc])
        cpsr = rng.getrandbits(4) << 28
        ins.append(regs + [cpsr])
    return ins


def build_qemu_program(tests, scratch_init):
    """tests: list of (mode, code_bytes, inputs). Returns assembly source."""
    L = [".syntax unified", ".arm", ".globl _start", ".text", "_start:"]
    for i in range(len(tests)):
        L.append(f"  blx t{i}")
    L += ["  mov r0, #1", "  ldr r1, =results", "  ldr r2, =results_end", "  sub r2, r2, r1", "  mov r7, #4", "  svc #0",
          "  mov r0, #0", "  mov r7, #1", "  svc #0", ".ltorg"]
    for i, (mode, code, inputs) in enumerate(tests):
        thumb = mode == "thumb"
        L.append(".thumb_func" if thumb else ".arm")
        if thumb:
            L.append(".thumb")
        else:
            L.append(".arm")
        L.append(f".type t{i}, %function")
        L.append(f"t{i}:")
        # save lr/sp, restore scratch, loop over inputs
        L += ["  ldr r12, =savearea", "  str lr, [r12]", "  str sp, [r12, #4]"]
        L += [f"  ldr r4, =n_{i}", "  ldr r11, =in_%d" % i, "  ldr r10, =out_%d" % i, "  str r4, [r12, #8]",
              "  str r11, [r12, #12]", "  str r10, [r12, #16]", f"loop_{i}:"]
        # restore scratch from pristine copy: 16 x (4 words)
        L += ["  ldr r0, =scratch_init", "  ldr r1, =scratch", f"  mov r2, #{NSCRATCH // 4}", f"cp_{i}:",
              "  ldm r0!, {r3, r4, r5, r6}", "  stm r1!, {r3, r4, r5, r6}", "  subs r2, r2, #1", f"  bne cp_{i}"]
        L += ["  ldr r12, =savearea", "  ldr lr, [r12, #12]", "  ldr sp, [r12, #16]",
              "  ldr r12, [lr, #52]", "  msr apsr_nzcvq, r12", "  ldm lr, {r0-r12}"]
        # instruction bytes inlined as raw data to avoid assembler re-encoding differences
        if thumb:
            for k in range(0, len(code), 2):
                L.append("  .hword 0x%04x" % struct.unpack_from("<H", code, k)[0])
        else:
            for k in range(0, len(code), 4):
                L.append("  .word 0x%08x" % struct.unpack_from("<I", code, k)[0])
        L += ["  stm sp, {r0-r12}", "  mrs r0, apsr", "  str r0, [sp, #52]", "  mov r3, sp", "  add r3, r3, #56",
              "  ldr r1, =scratch", f"  mov r2, #{NSCRATCH // 4}", f"cq_{i}:", "  ldm r1!, {r4, r5, r6, r7}",
              "  stm r3!, {r4, r5, r6, r7}", "  subs r2, r2, #1", f"  bne cq_{i}"]
        L += ["  ldr r12, =savearea", "  ldr lr, [r12, #12]", "  add lr, lr, #56", "  str lr, [r12, #12]",
              "  ldr r3, [r12, #16]", f"  add r3, r3, #{56 + NSCRATCH * 4}", "  str r3, [r12, #16]",
              "  ldr r4, [r12, #8]", "  subs r4, r4, #1", "  str r4, [r12, #8]", f"  bne loop_{i}",
              "  ldr sp, [r12, #4]", "  ldr lr, [r12]", "  bx lr", ".ltorg"]
    L += [".data", ".align 4", "savearea: .space 64", "scratch_init:"]
    for w in scratch_init:
        L.append(f"  .word 0x{w:08x}")
    L += [".section .scratch, \"aw\", %progbits", ".align 4", "scratch: .space %d" % (NSCRATCH * 4)]
    for i, (mode, code, inputs) in enumerate(tests):
        L.append(f".equ n_{i}, {len(inputs)}")
        L.append(f"in_{i}:")
        for inp in inputs:
            L.append("  .word " + ",".join(f"0x{v:08x}" for v in inp))
    L.append(".bss")
    L.append(".align 4")
    L.append("results:")
    for i, (mode, code, inputs) in enumerate(tests):
        L.append(f"out_{i}: .space {len(inputs) * (56 + NSCRATCH * 4)}")
    L.append("results_end:")
    return "\n".join(L) + "\n"


def run_qemu(tests, scratch_init, tmp):
    asm = build_qemu_program(tests, scratch_init)
    sfile = os.path.join(tmp, "t.s")
    open(sfile, "w").write(asm)
    obj = os.path.join(tmp, "t.o")
    exe = os.path.join(tmp, "t")
    r = subprocess.run(["llvm-mc", "-triple=armv7-linux-gnueabi", "-mattr=+thumb2,+v7,+dsp,+neon,+vfp3",
                        "-filetype=obj", sfile, "-o", obj], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:2000]); sys.exit(2)
    r = subprocess.run(["ld.lld", "--section-start=.scratch=0x40000000", "-Ttext=0x10000",
                        obj, "-o", exe], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:2000]); sys.exit(2)
    # .data holds savearea first (32 bytes) then scratch_init then scratch: patch SCRATCH to be at a known address
    r = subprocess.run(["qemu-arm", exe], capture_output=True)
    return r.stdout


def run_jit(tests, scratch_init, tmp):
    spec = os.path.join(tmp, "spec.bin")
    outp = os.path.join(tmp, "jit.bin")
    with open(spec, "wb") as f:
        f.write(struct.pack("<I", NSCRATCH))
        f.write(struct.pack("<%dI" % NSCRATCH, *scratch_init))
        for mode, code, inputs in tests:
            f.write(struct.pack("<II", 1 if mode == "thumb" else 0, len(code)))
            f.write(code + b"\0" * ((4 - len(code) % 4) % 4))
            f.write(struct.pack("<I", len(inputs)))
            for inp in inputs:
                f.write(struct.pack("<14I", *inp))
    subprocess.run([JIT, spec, outp], check=True, timeout=600)
    return open(outp, "rb").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", default="")
    ap.add_argument("-n", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--mode", default="all", choices=["arm", "thumb", "all"])
    ap.add_argument("--mem", action="store_true", help="only memory tests")
    ap.add_argument("--all", action="store_true", help="print every failing input")
    ap.add_argument("--alu", action="store_true", help="only non-memory tests")
    a = ap.parse_args()
    rng = random.Random(a.seed)
    scratch_init = [(0x01010101 * (k + 1) ^ 0x9e3779b9 ^ (k << 8)) & 0xffffffff for k in range(NSCRATCH)]

    cands = []
    if a.mode in ("arm", "all"):
        if not a.mem:
            cands += [("arm", s, False) for s in arm_tests()]
        if not a.alu:
            cands += [("arm", s, True) for s in MEM_TESTS["arm"]]
    if a.mode in ("thumb", "all"):
        if not a.mem:
            cands += [("thumb", s, False) for s in thumb_tests()]
        if not a.alu:
            cands += [("thumb", s, True) for s in MEM_TESTS["thumb"]]
    seen = set()
    tests, labels = [], []
    for mode, seq, mem in cands:
        if a.k and a.k not in seq:
            continue
        if (mode, seq) in seen:
            continue
        seen.add((mode, seq))
        code = assemble(mode, seq)
        if not code:
            if a.v:
                print(f"skip (does not assemble): {mode}: {seq}")
            continue
        tests.append((mode, code, rand_inputs(rng, a.n, mem)))
        labels.append(f"{mode}: {seq}")
    print(f"{len(tests)} tests x {a.n} inputs")

    with tempfile.TemporaryDirectory() as tmp:
        q = run_qemu(tests, scratch_init, tmp)
        j = run_jit(tests, scratch_init, tmp)
    rec = 56 + NSCRATCH * 4
    exp_len = sum(len(t[2]) for t in tests) * rec
    if len(q) != exp_len:
        print(f"qemu output size {len(q)} != expected {exp_len}"); sys.exit(2)
    if len(j) != exp_len:
        print(f"jit output size {len(j)} != expected {exp_len}"); sys.exit(2)

    names = ["r%d" % i for i in range(13)] + ["nzcv"]
    pos = 0
    failed = 0
    for lab, (mode, code, inputs) in zip(labels, tests):
        bad = []
        for k, inp in enumerate(inputs):
            qa = struct.unpack_from("<14I", q, pos)
            ja = struct.unpack_from("<14I", j, pos)
            qs = q[pos + 56: pos + rec]
            js = j[pos + 56: pos + rec]
            qa = list(qa); qa[13] &= 0xf0000000
            diffs = [(names[i], qa[i], ja[i]) for i in range(14) if qa[i] != ja[i]]
            if qs != js:
                diffs.append(("scratch", 0, 0))
            if diffs:
                bad.append((inp, diffs))
            pos += rec
        if bad:
            failed += 1
            inp, diffs = bad[0]
            print(f"FAIL {lab}   ({len(bad)}/{len(inputs)} inputs differ)")
            for inp, diffs in (bad if a.all else bad[:1]):
                print("   in : " + " ".join(f"r{i}={inp[i]:x}" for i in range(6)) + f" nzcv={inp[13] >> 28:x}")
                for n, qv, jv in diffs[:4]:
                    print(f"   {n}: qemu={qv:#x} jit={jv:#x}")
    print(f"\n{len(tests) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
