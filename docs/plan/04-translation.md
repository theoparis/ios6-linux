# 04 — Translation (ARMv7 / Thumb-2 → LLVM IR)

Scope, decoding, block formation, the semantic table, the IR shape, and the specific traps that will
bite. Decided by [ADR-05](02-decisions.md) (two tiers, one table) and [ADR-07](02-decisions.md)
(LLVM MC authoritative, Capstone oracle).

## 1. Which ARM, exactly

| Feature | Status | Why |
|---|---|---|
| ARMv7-A integer + Thumb-2 | **required** | everything in iOS 6 |
| VFPv3-D32 | **required** | used for floating-point *arithmetic* only: the iOS 6 calling convention is soft-float (clang `apcs-gnu`), so float/double arguments and returns travel in `r0-r3`/stack ([research/apple-armv7-abi.md §1.1](../research/apple-armv7-abi.md)); iOS 6 devices are Cortex-A8/A9/Swift, VFPv3-D32 or better |
| NEON | **required, scheduled second** | present on every iOS 6 device; Apple's `armv7` dylibs are built without NEON, so frequency must be **measured** on the real cache (`IOS6RECOMP_LIFT_STATS`) and not assumed — measure before writing 200 NEON semantics |
| `SDIV`/`UDIV`, VFPv4 `VFMA`, FP16 convert | **required (armv7s)** | the `armv7s` (Swift, A6) slice inlines integer divide instead of calling `__divsi3` and may use fused multiply-add and half-float converts ([research/apple-armv7-abi.md §2,§3](../research/apple-armv7-abi.md)); Cortex-A8/A9 (armv7) never contain them |
| LDREX/STREX (global exclusive monitor) | required | `OSAtomic*` on ARMv7 uses them; used by libdispatch init |
| DMB/DSB/ISB | required | ordering in libdispatch, CF |
| ARMv6, ThumbEE, Jazelle, LPAE, VFPv2 | out of scope | no iOS 6 binary targets them |
| ARMv8/64-bit | out of scope | separate project |

Both `armv7` (CPU subtype 9) and `armv7s` (subtype 11, `CPU_SUBTYPE_ARM_V7S`; measured in
[research/ios6-platform.md §4](../research/ios6-platform.md)) are accepted; the ISA superset is decoded and
the required features are recorded per instruction, so an `armv7s`-only instruction found in an
`armv7` slice is a loader-level diagnostic, not a crash.

## 2. Decoding

```cpp
// lib/lift/decode.cpp — the only place instantiations happen
// Signatures checked against /usr/include/llvm (23.1.3); the whole snippet compiled, linked and
// decoded `udiv r2,r0,r1` in both modes (Measured: /tmp/aud/d.cpp, see below).
llvm::Triple triple("armv7s-apple-ios6.0");
std::string err;
const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);   // (const Triple&, std::string&)
// Initialize first: LLVMInitializeARMTargetInfo/TargetMC/Disassembler (llvm/Support/TargetSelect.h).
// CPU "swift" (+vfp4,+fp16,+hwdiv) is mandatory: plain armv7 rejects valid A6 code (sdiv, vfma, vcvtb.f16)
// Thumb needs "+thumb-mode" in the feature string; keep two STIs (ARM, Thumb), not one toggled.
std::unique_ptr<llvm::MCSubtargetInfo> STI(target->createMCSubtargetInfo(triple, "swift", thumb ? "+thumb-mode" : ""));
std::unique_ptr<llvm::MCRegisterInfo>  MRI(target->createMCRegInfo(triple));
std::unique_ptr<llvm::MCInstrInfo>     MII(target->createMCInstrInfo());
llvm::MCTargetOptions MTO;                                // MCAsmInfo keeps a REFERENCE to this: it must outlive Ctx
std::unique_ptr<llvm::MCAsmInfo>       MAI(target->createMCAsmInfo(*MRI, triple, MTO));
llvm::MCContext Ctx(triple, *MAI, *MRI, *STI);            // (Triple, MCAsmInfo&, MCRegisterInfo&, MCSubtargetInfo&, ...)
std::unique_ptr<llvm::MCDisassembler>  Dis(target->createMCDisassembler(*STI, Ctx));
llvm::MCInst inst; uint64_t size = 0; uint64_t pc = va;   // Bytes: llvm::ArrayRef<uint8_t>
// virtual DecodeStatus getInstruction(MCInst&, uint64_t& Size, ArrayRef<uint8_t>, uint64_t Address, raw_ostream& CStream) const
while (Dis->getInstruction(inst, size, Bytes.slice(off), pc, llvm::nulls()) == llvm::MCDisassembler::Success)
  { ...; pc += size; off += size; }
```

Measured: `clang++ d.cpp $(llvm-config --cxxflags --ldflags --libs ARM MC MCDisassembler Support TargetParser --system-libs)`
then `./d` → `NumOpcodes=4531`; ARM bytes `10 f1 32 e7` → `UDIV` and Thumb bytes `b0 fb f1 f2` → `t2UDIV`, both
`DecodeStatus` 3 (`Success`; `SoftFail`=1, `Fail`=0 — treat `SoftFail` as a diagnostic, not as success).
`createMCSubtargetInfo`, `createMCAsmInfo`, `createMCRegInfo` and `lookupTarget` take `const llvm::Triple&`,
not a string; `MCContext`'s second to fourth arguments are references.

Notes that matter:

- **Decode with the superset subtarget.** A `MCSubtargetInfo` built for plain `armv7` rejects NEON
  and armv7s encodings, which would turn a legal instruction into a discovery failure. Decode with
  CPU `swift` and record what each instruction requires ([research/llvm-tooling.md §5](../research/llvm-tooling.md)).
- **Mode is block state.** `MCInst` for a given byte stream differs between ARM and Thumb, so the
  decoder is parameterised by `(va, mode)`. Mode changes are computed, never guessed: `bx`/`blx`
  register forms take the mode from bit 0 of the target; `blx <imm>` switches ARM→Thumb
  unconditionally; `mov pc, lr` (ARM mode only — ARMv7 `ALUWritePC` interworks in ARM state, plain branch in Thumb state) and
  `ldm`/`pop {..., pc}`/`ldr pc` take it from bit 0 of the value. Measured: `mov pc,r0` in ARM mode with `r0 = th|1`
  entered Thumb code and exited 0 under `qemu-arm -cpu max` (/tmp/aud/t6.s).
- **Capstone** is built for the same two modes and is used (a) in the decode-differential test
  (`12-testing.md`) and (b) by `ios6recomp disasm`. Its `cs_arm` detail supplies `cc`, IT-block
  fields, `writeback` and `svc` immediates, which makes it a genuinely independent second opinion
  rather than a wrapper over the same tables.
- A decode failure inside a block that was reached by *execution* is a hard error (ADR-13): print
  the bytes, the mode, the address, and the caller's block. Silent truncation of a block is the
  classic way to burn a week.

## 3. Block discovery

**Discovery is a heuristic, never a correctness requirement.** The runtime can execute a block at any
guest address on demand ([ADR-04](02-decisions.md) fault-driven, or table lookup). Static discovery
exists only to (a) pre-populate, (b) give the AOT path a closed set, and (c) inform optimisation.
This removes the single largest risk in recompiler projects (perfect CFG recovery for computed jumps)
by construction.

Seed addresses, in priority order:

1. `LC_FUNCTION_STARTS` — Apple emits it for every armv7 image; it enumerates function starts exactly.
2. Export trie (`__LINKEDIT`) and the symbol table — externals and (with `LC_DYSYMTAB`) locals.
3. `__TEXT,__unwind_info` — **absent on iOS 6** (no image in the 569-image cache has it; exceptions are
   SjLj, [research/apple-armv7-abi.md §5](../research/apple-armv7-abi.md)). Function ranges therefore come from
   `LC_FUNCTION_STARTS` deltas and the symbol table, not from unwind info.
4. ObjC metadata: `__objc_classlist` → class/`class_ro_t` → method lists → `IMP` pointers in
   `__TEXT`; `__objc_selrefs`, `__objc_protolist` for completeness.
5. Direct branch targets found by a recursive sweep from all of the above.
6. `__DATA`/`__DATA_CONST` pointer-sized values that land inside an executable segment — a heuristic
   that catches C++ vtables and callback tables; verified by decoding at the target, and (because it
   is a heuristic) recorded as a hint, not a fact.

`LC_DATA_IN_CODE` is authoritative for "this range in `__TEXT` is data, not code" (jump tables,
absolute references) and must be honoured before any linear sweep, or the sweep will decode table
data as instructions and produce plausible garbage.

## 4. Block formation

A **block** is a maximal instruction sequence that starts at a block start and ends at the first of:

- any `B`/`BL`/`BX`/`BLX`/`CBZ`/`CBNZ`/`TBB`/`TBH`,
- any instruction writing `pc` (`mov pc, …`, `pop {…, pc}`, `ldr pc, […]`, `ldm {…, pc}`),
- any `SVC`.

Rules:

- **IT blocks are atomic.** An `IT` plus its up to four following instructions form one indivisible
  unit; the predicate applies per-instruction. A conditional `BL` inside an IT block terminates the
  block (with the predicate attached to the call). Getting IT grouping wrong is invisible until a
  conditional `bl`.
- Call instructions terminate a block but do **not** end guest control flow; the return address is a
  call site, not a new function — the block after a `bl` is simply the next block.
- Block key is `(va, mode)`. The same address in ARM and Thumb is two different blocks; this is
  legal and does occur (Thumb-interworking stubs).
- Size cap (default 4 KiB of guest code) purely for compile latency; a capped block is split at a
  non-control-flow boundary with an appended fallthrough jump, so semantics are unchanged.
- An ARM-mode block start with `va % 4 != 0`, or a Thumb start with `va` odd after masking bit 0 of the interworking address, is an error signal, not something to guess. (Interworking addresses carry the mode in bit 0; the block key `va` is the masked address.)

## 5. The semantic table

One row per LLVM ARM opcode. The skeleton is generated at build time from `MCInstrInfo` (all 4531
ARM opcode names (Measured: `MCInstrInfo::getNumOpcodes()` = 4531 for the ARM target in the local 23.1.3 libs and `MCInstrDesc` flags are available at runtime from the linked LLVM), so a small
generator tool emits `opcode_list.inc` — no `llvm-tblgen` include path needed, and there is no
`-gen-instr-semantics` backend in LLVM 23.1.3, [research/llvm-tooling.md §5](../research/llvm-tooling.md)) so
that the compiler refuses to build when a row is missing.

```cpp
struct Semantics {
  unsigned        op;         // MCInst::getOpcode(); ARMGenInstrInfo enum is private to LLVM, so index by number, name via MCInstrInfo::getName
  FeatureMask     requires;   // armv7 | vfp | neon | hwdiv | ...
  uint8_t         flags_read;    // N Z C V Q GE
  uint8_t         flags_written;
  uint8_t         gprs_read, gprs_written;   // bitmasks
  MemoryEffect    mem;                        // none | read | write | barriers...
  InterpFn        interpret;                  // tier 0
  EmitFn          emit;                       // tier 1
  const char*     notes;                      // why this one is subtle, if it is
};
```

Coverage is deliberately tiered, and the tier is a field in the table so the reporting is automatic:

| Tier | Contents | Implementation |
|---|---|---|
| T1 | all integer ALU + flags, barrel shifter, all load/store addressing modes, `ldm/stm`, all branches incl. `tbb/tbh`, IT, `mrs/msr` (limited), all VFP arithmetic/compare/convert/load-store, `clz/rbit/rev`, `ldrex/strex`, barriers | hand-written, both backends |
| T2 | NEON integer + float, structure loads/stores, `vtbl/vtbx`, `vext`, pairwise | hand-written by shape (one emitter parameterised by lane width/count covers most of the ISA) |
| T3 | `setend`, `cps`, `srs/rfe`, `mcr/mrc` to unmodelled system registers, `pld/pldw`, coprocessor corners | **interpret-only**; counted in `lift.missing`; strict mode aborts |

The `lift.missing` table is the compatibility burn-down chart; every real binary we run tightens it.

## 6. IR shape

First version (M3), chosen for correctness and debuggability, not speed:

- One `llvm::Function` per block: `void @ios6.<image>.<va>_<arm|thumb>(ptr %gtc)`.
- Guest registers are **loaded from and stored to the GTC around every operation** in the naive
  version, and the block's SSA values are internal. This makes `longjmp`, guest unwinding and
  signal delivery correct for free ([ADR-06](02-decisions.md)), and makes the per-instruction
  differential test exact.
- Then, still in M3, add the standard structure that makes this fast without breaking ADR-06:
  load the guest registers into SSA values at block entry, run the block, store them back at exit,
  and let `mem2reg`/`SROA` clean up. Boundaries (calls, `svc`, any point a signal can be delivered)
  are where loads/stores reappear.
- Flags: NZCV as four `i1` SSA values inside a block, spilled to the GTC at exits. M7 replaces this
  with **lazy flags**: record `(op, lhs, rhs, shifter_amount)` and materialise only on a read. UNVERIFIED: the expected IR reduction (guess 30–50 %); close by counting, over the lifted libSystem image, flag-writing instructions whose flags are dead before the next write (`IOS6RECOMP_LIFT_STATS`). The table's `flags_read`/`flags_written` columns are what make
  this mechanical.
- Conditional execution: with flags in SSA and a known condition code, emit `select`/`br` per
  instruction. Do not attempt to re-form conditionals into basic blocks in the lifter; that is a
  later IR-level optimisation with a much better reward/risk ratio.
- Guest memory: one `Value*` base pointer for the arena; every access is
  `load/store (i32|i16|i8|float|double|<N x i8>) , ptr base + guest_va`. `!tbaa` is **not** attached
  to guest memory (it is an untyped bag of bytes that guest code reinterprets at will); host-side
  metadata (block tables, GTC) gets its own alias scope.
- Alignment: emit `align 1` for guest loads/stores. ARMv7-A permits unaligned normal memory accesses,
  x86-64 handles them natively, and LLVM must not be allowed to assume otherwise. `ldrd/strd`,
  `ldm/stm` and exclusive accesses are the aligned cases and are handled explicitly (§7).
- Endianness: **guest LE, host LE — raw mapping, no byte swapping anywhere.** Worth stating because
  it removes an entire class of bug that ARM-on-other-hosts work has to fight; do not let a
  "portable" helper reintroduce byteswaps.
- `pc` reads are constant-folded per instruction (`va + 8` ARM, `va + 4` Thumb); a write to `pc`
  becomes a branch, never a store into the GTC.

## 7. Known traps (each one has a test in `12-testing.md`)

| Case | ARM behaviour | Host/x86 behaviour | Handling |
|---|---|---|---|
| `sdiv`/`udiv` by zero | result 0, no trap (Measured: `udiv r2,r0,r1`, `sdiv r5,r0,r1` with r1=0 → 0, `qemu-arm -cpu max`, /tmp/aud/t3.s; `INT_MIN/-1` → `0x80000000`) | `#DE` fault | guard before the divide (LLVM must not fold the guard away — use a branch, not `select`, because `sdiv`/`udiv` by 0 is immediate UB in LLVM IR, so the guard must dominate the divide) |
| `INT_MIN / -1` | `INT_MIN` (measured, same run) | `#DE` fault | same guard |
| V flag on add/sub | signed overflow from operands | `of` flag | compute manually (`llvm.sadd.with.overflow`) |
| Carry from barrel-shifted operand | shifter carry-out | n/a | compute shifter carry and feed the ALU carry input |
| `clz(0)` | 32 (measured: `0x20`) | `bsr` undefined | `llvm.ctlz(x, /*is_zero_undef=*/false)` |
| `str rN,[rN],#4` (Rt == Rn with write-back) | UNPREDICTABLE in the ARM ARM; QEMU stores the *old* base value and then updates the base (measured, Thumb-2 `str.w r11,[r11],#4`, /tmp/aud/t2.s) | — | store the pre-update value, write the base after the store; no iOS 6 code should rely on it, so log it as a lift warning |
| ARM `ldm rN!,{...,rN,...}` (write-back, base in list) | UNPREDICTABLE; QEMU raises SIGILL (measured, `0xe8b90600`, /tmp/aud/t.s) | — | lift as a hard `lift.unpredictable` diagnostic, not as silent semantics |
| 16-bit Thumb `ldm rN,{rN,...}` (base in list, no `!`) | defined: no write-back, loaded value wins (measured: `ldm r0,{r0,r1}` over `{0x11,0x22}` → r0=0x11, r1=0x22) | — | model write-back as `wb = base ∉ list` for the T1 encoding |
| unaligned `ldm/stm`, `ldrd/strd`, `ldrex`, `vldr/vstr`, `swp` | alignment fault (measured under `qemu-arm`: SIGBUS for `ldm`, `ldrd`, `ldrex`, `vldr`, `swp` at addr+1; plain `ldr`/`ldrh` succeed) | works fine | emulate the fault (deliver a guest `SIGBUS`) to match observable behaviour (UNVERIFIED that iOS 6's kernel leaves `SCTLR.A`=0 as Linux does; close by reading the `cpu_data` setup in `~/src/xnu` `osfmk/arm/` for xnu-2107 or measuring with the unaligned test on a device); keep `align 1` only for `ldr/str/ldrh/strh` |
| `IT` + `BL` | predicated call | — | predicate the call, keep the return path |
| FPSCR `FZ`/`DN`, rounding mode | FPSCR-controlled | MXCSR-controlled | mirror FPSCR into MXCSR (`FZ`→`FTZ|DAZ`, `RMode`→`RC`), and mask all MXCSR exception masks (ARM's default FPSCR does not trap). UNVERIFIED: ARM `FZ` flushes denormal inputs and outputs, MXCSR `FTZ`+`DAZ` do the same; confirm with a denormal `vmul.f32` vs `mulss` test under `qemu-arm` |
| NaN propagation in `VADD.F32`/`VMUL`/`VCMP` | ARM operand-order rules | SSE rules | known deviation (UNVERIFIED which cases differ; close with a differential run of `vadd/vmul/vcmp` over NaN payload pairs against `qemu-arm`); `IOS6RECOMP_STRICT_FP=1` switches to `llvm.experimental.constrained.*` with a soft-float reference for tests |
| VFPv3 has no fused multiply-add | `VFMA` exists on VFPv4 (armv7s) only | `fma` | for `armv7` code never fuse (`fmul`+`fadd`, contraction disabled); for `VFMA` use `llvm.fma` |
| LDREX/STREX | global exclusive monitor per thread | `cmpxchg` per address | per-thread monitor armed with `(address, loaded value)`; `STREX` succeeds only if armed for that exact address and the value is unchanged — a bare address check gives false success under ABA ([06-runtime-core.md](06-runtime-core.md)) |
| APSR.GE (SIMD "greater or equal" flags) | set by `sadd16`-family | n/a | compute on demand; almost never read, but `pkhbt`-style code does read it |
| `nop`/`yield`/`wfe`/`sev` | hints | nothing / `pause` | map `yield`→nothing or `pause`, `wfe`→yield the guest thread |

## 8. Interposition — hot paths are *guest* code

`objc_msgSend` is called millions of times per second and must forward `r0-r3` and the stack
untouched (floats travel in GPRs/stack on this soft-float ABI, so there is no `s0-s15` to preserve).
Routing it through a host-side FFI stub (which must spill and reload everything, per
ADR-06) is both slow and a permanent ABI-fidelity hazard.

**Decision:** performance-critical runtime hot paths are written by us in **ARMv7 assembly/C,
compiled by clang for `armv7-apple-ios6`, and loaded as guest code**. They touch guest data
structures directly (via the guest base pointer) so no marshalling happens at all:

- `objc_msgSend` / `objc_msgSendSuper2` / `objc_msgSend_stret` — class cache lookup in guest
  assembly, tail-calling `K + (imp << E)` on hit, and calling a host-side resolver (through one
  tiny FFI trampoline) only on miss.
- `objc_retain`/`objc_release`/`objc_autoreleaseReturnValue`, `OSAtomic*` fast paths,
  `pthread_getspecific`, and leaf libc routines (`memcpy`, `memset`, `strlen`, ...). There is no
  `_tlv_bootstrap` and no `__aeabi_*` on iOS 6 ([research/apple-armv7-abi.md §3,§4](../research/apple-armv7-abi.md)).

Mechanism: `lib/lift/hooks.td` maps `(image, symbol) → { host_builtin | guest_object }`. At load
time the loader binds the symbol to the guest object's entry point, and the lifter's block cache
treats that entry as pre-translated. This is a *loader* mechanism, not a lifter special case, so it
applies uniformly to calls, `dlsym` results, ObjC `IMP`s and C function pointers.

The same mechanism subsumes `stub` mode (ADR-09): a stub is a `host_builtin` that logs and returns.

## 9. What this document deliberately does not do

No cycle-accurate timing, no cache modelling, no instruction-accurate memory ordering beyond
`DMB/DSB` lowering to `fence seq_cst`, no `SWI`-handler recompilation, no precise self-modifying-code
handling (page granularity, see [ADR-04](02-decisions.md) and `05-jit.md` §Invalidation), no
JIT-support for guests that generate code at runtime beyond the normal `mprotect` → invalidate path.
