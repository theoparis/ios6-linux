# Apple ARMv7 (iOS 6) ABI — implementation notes

Scope: 32-bit Mach-O objects for the iOS 6 device ABI (`armv7` / `armv7s`, deployment target
`-miphoneos-version-min=6.0`), consumed by an ARMv7 lifter, an FFI bridge to native host code, and a
substituted libSystem. Statements marked `UNVERIFIED:` were not confirmed against a primary source or
a local measurement.

## 0. Sources and how claims here were checked

| Tag | What it is |
|---|---|
| `[AAPCS32:<line>]` | `github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst` (normative IHI 0042 text); line numbers refer to that file as of fetch 2026-10-16 |
| `[apple-av6]` / `[apple-av7]` | Apple, *Writing ARMv6 code for iOS* / *Writing ARMv7 code for iOS*, `developer.apple.com/documentation/xcode/...` |
| `[clang]` | clang/LLVM 23.1.3 **installed locally**; every "measured" claim is reproducible with `clang -target armv7-apple-ios6.0 -S/-emit-llvm` |
| `[clang31]` | `llvm-mirror/clang` branch `release_31` (the 2012 compiler that built iOS 6) |
| `[dsc]` | Real iOS 6.1.3 (10B329, iPad2,1 = armv7) decrypted `dyld_shared_cache_armv7` (569 images). Two copies exist: in-repo `work/rootfs-hfs.img` (cache begins at file offset `0x33C2A400`, magic `dyld_v1   armv7`) and `/mnt/ios6x/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7` (used for the measurements below). Image load commands, `LC_SYMTAB`, `LC_ID_DYLIB` and section tables were walked directly; `ipsw dyld` cannot parse this cache (it expects the newer `dyld_cache_header`) |
| `[objc4-532.2]` | `github.com/apple-oss-distributions/objc4` tag `objc4-532.2` (the iOS 6 runtime; `objc4-551.1` is *not* what iOS 6 shipped — see §7) |
| `[dyld-239.3]` | `github.com/apple-oss-distributions/dyld` tag `dyld-239.3` (iOS 6 / OS X 10.8 era) |
| `[libc-825.40.1]`, `[xnu]` | `apple-oss-distributions/Libc` tag `Libc-825.40.1`; `apple-oss-distributions/xnu` (iOS 6 = xnu-2107, unpublished; nearest published tree xnu-2050.x) |

Local ground truth special note: `[dsc]` + `clang` disagree with several widely repeated claims
(e.g. "iOS ARMv7 is hard-float", "Mach-O on ARM uses AAPCS-VFP HFAs", "`__thread` works on iOS 6",
"iOS has `__unwind_info`"), so those claims are corrected below with the measurement that refutes them.

## 1. Register and ABI baseline

### 1.1 Which ABI clang actually uses

* Target triple normalises `armv7-apple-ios6.0.0` → `thumbv7-apple-ios6.0.0`; code is Thumb-2
  (`.code 16`, `.thumb_func` on every function) [clang, measured].
* `clang -### -target armv7-apple-ios6.0` emits `-target-abi apcs-gnu`, `-mfloat-abi soft` and
  target features `+soft-float-abi`, `+read-tp-tpidruro`, `+vfp2`, `+vfp2sp`, `+vfp3` [clang, measured].
  The 2012 driver did the same: `AddARMTargetArgs()` defaults `ABIName = "apcs-gnu"` for any ARM
  triple whose environment is not `EABI`/`GNUEABI`/`ANDROIDEABI` [clang31 `lib/Driver/Tools.cpp`, ARM
  branch ending at the `default: ABIName = "apcs-gnu";`], and `armv7-apple-ios` has no EABI env.
* Consequence: **the Darwin ARMv7 ABI is the Apple/APCS-flavoured calling convention, not
  AAPCS-with-VFP.** clang selects `ARMABIInfo::APCS` (`getABIKind()==APCS`), which changes three
  things relative to AAPCS: no HFA/HVA co-processor classes at all, "integer-like" composites only
  ≤4 bytes returned in `r0`, and no 8-byte stack-alignment assumption. Evidence in clang source:
  `clang/lib/Basic/Targets/ARM.cpp:setABIAPCS()` sets `DoubleAlign = LongLongAlign =
  LongDoubleAlign = SuitableAlign = 32`, `UseBitFieldTypeAlignment = false`,
  `ZeroLengthBitfieldBoundary = 32`, `WCharType = SignedInt` [clang, `lib/Basic/Targets/ARM.cpp:47-66`];
  `setABIAAPCS()` is the variant that sets 64.
* Apple states the same rule in prose: *"Use the Base Standard variant of the procedure call
  standard for floating-point arguments. In this variant, you pass floating-point and vector
  arguments in general-purpose registers (GPRs) instead of in VFP registers."* [apple-av6 §"Pass
  arguments to functions and handle return values"]. The ARMv7 document states the ARMv7 and ARMv6
  calling conventions are "nearly identical" and lists VFP register preservation as the only
  deviation [apple-av7].
* **AAPCS-VFP (IHI 0055, "variant C") therefore does not apply here at all**: no HFA/HVA
  classification, no `s0-s15`/`d0-d7` argument registers, no `s0-s3` HFA returns. AAPCS-VFP's rules
  (`C.1.vfp`–`C.4.vfp` in the merged AAPCS32 text, `rst:1636-1670`) describe e.g. Linux
  `armv7-linux-gnueabihf` and are what one must *not* use when interpreting iOS 6 guest code.
  The only VFP-related rules that do apply on iOS are the register-preservation ones (`d8-d15`
  callee-saved) and the fact that VFP is used *for computation* (`vmul.f64`, `vldr`, `vmov`) even
  though arguments arrive in GPRs [clang, measured].

### 1.2 Core registers

| Reg | AAPCS32 role | iOS 6/Darwin role |
|---|---|---|
| `r0-r3` (`a1-a4`) | argument/result/scratch; never preserved [AAPCS32:806-820] | same |
| `r4-r8`, `r10` | callee-saved `v1-v5`,`v7` [AAPCS32:875-877] | same |
| `r9` (`v6`/`SB`/`TR`) | "platform specific": SB in a PIC data model, TR with TLS, or additional callee-saved `v6`; `r9` is preserved only "in PCS variants that designate r9 as v6" [AAPCS32:824-830, 866-877] | **`r9` is neither a platform register nor callee-saved: it is a volatile scratch register** — "R9 — No — A volatile scratch register in iOS 3 and later" [apple-av6 register table]. LLVM encodes this in the callee-saved mask (`CSR_iOS` = `CSR_AAPCS` minus r9) and `objc_msgSend` clobbers `r9` freely [objc4-532.2 `objc-msg-arm.s`, `CacheLookup` macro "Kills: a4, $1, r9, ip"]. A lifter/FFI **must not** treat r9 as callee-saved in guest code |
| `r11` (`v8`, sometimes named `FP`) | callee-saved `v8` [AAPCS32:822] | callee-saved GPR; "Some ARM environments refer to R11 with the mnemonic FP. In iOS, R11 is a general-purpose, nonvolatile register." [apple-av6] |
| `r7` (`v4`) | callee-saved `v4`, may be used as frame pointer "in some variants" [AAPCS32:857-859] | **frame pointer by convention**; "Register R7 serves as the frame pointer" [apple-av6]; this is what gives iOS 6 its only backtrace mechanism (§5). In Thumb-2, AAPCS allows either r7 or r11 to hold the frame pointer, but on Apple ARM the convention is fixed on r7 (a `mov r11, sp`-style prologue is not what iOS 6 code does) — `UNVERIFIED:` an explicit Apple statement that r11-FP is forbidden, the compiler behaviour is the evidence |
| `r12` (`IP`) | intra-procedure-call scratch: "may be used by a linker as a scratch register between a routine and any subroutine it calls" [AAPCS32:812-814] | same; always volatile across calls (linker veneers/stubs clobber it) [apple-av6] |
| `r13`/`r14`/`r15` | SP / LR / PC | same |
| `d0-d7` (`s0-s15`) | VFP scratch | volatile [apple-av7] |
| `d8-d15` (`s16-s31`) | VFP callee-saved | **callee-saved** [apple-av7] |
| `d16-d31` (`q8-q15`) | ARMv7 only | volatile [apple-av7] |
| `FPSCR` | — | condition bits and saturation bits not preserved; exception-control (8-12), rounding (22-23), flush-to-zero (24) bits may only be changed by routines that affect app state; short-vector length (16-18)/stride (20-21) must be zero at function entry/exit [apple-av6 FPSCR row] |

`r11` is *not* the frame pointer, and `r9` is *not* preserved: both are common bugs when porting
AAPCS-based tooling to guest iOS 6 code.

### 1.3 Stack alignment

| | AAPCS32 | iOS 6 ARMv7 |
|---|---|---|
| at a public interface | `SP mod 8 = 0` — "The stack must be double-word aligned" [AAPCS32:1079-1083] | **only 4 bytes guaranteed**: "The stack is 4-byte aligned at the point of function calls" [apple-av6 §"Configure stack frames…"]; the ARMv7 doc adds no change [apple-av7] |
| data alignment | 8 for `double`/`long long` (AAPCS classifiers apply Rule B.5/C.3 with 8-byte alignment) | 4 for `double`/`long long`/`long double` (§1.4), so no double-word register rounding happens |

Measured confirmation: `void h(void){int x; g(&x);}` compiles to `push {r7,lr}; sub sp,#4; mov r0,sp`
with no realignment; a function that needs an 8-aligned local emits an explicit realignment
(`mov r4,sp; bfc r4,#0,#3; mov sp,r4`), i.e. the callee cannot assume 8 [clang, measured]. clang only
emits `alignstack(8)` for interrupt handlers when the ABI is *not* APCS, with the comment "AAPCS
guarantees that sp will be 8-byte aligned on any public interface" — guarded by
`if (ABI == ARMABIKind::APCS) return;` [clang `lib/CodeGen/Targets/ARM.cpp:198-206`].

Practical rule for the FFI bridge/JIT: entering guest code with `SP % 8 == 0` (or 16) is safe and
recommended; *relying* on 8-alignment inside guest code the way AAPCS code does is not valid.

### 1.4 Type sizes, alignments, layout

Measured with `clang -target armv7-apple-ios6.0`; layout string reported by clang is
`e-m:o-p:32:32-Fi8-f64:32:64-v64:32:64-v128:32:128-a:0:32-n32-S32` [clang, measured].

| type | size | ABI align | notes |
|---|---|---|---|
| `char/short/int/long/unsigned*` | 1/2/4/4 | 1/2/4/4 | `long` = 4 bytes (LP32) |
| `pointer`, `size_t`, `ptrdiff_t` | 4 | 4 | |
| `wchar_t` | 4 | 4 | `SignedInt` under `apcs-gnu`; Apple's table lists it as 4 [apple-av6] |
| `long long`, `int64_t` | 8 | **4** | `i64:32:64` in the data layout |
| `double` | 8 | **4** | `f64:32:64`; Apple's own table claims 8 for `long long` [apple-av6] but the shipped compiler lays structs out with 4 |
| `long double` | **8** | **4** | **`long double` is just `double` on Apple ARM**, `__SIZEOF_LONG_DOUBLE__` = 8; Apple's table agrees ("long double 8 / natural alignment 4") [apple-av6]. There is no 80-bit or 128-bit ARM `long double` |
| `float` | 4 | 4 | |
| `struct{char;double}` | 12 | 4 | (AAPCS/Linux would give 16/8) |
| bitfield `struct{int a:3; int b:5;}` | 1 | 1 | `UseBitFieldTypeAlignment=false` (`apcs-gnu`) [clang `ARM.cpp:61-62`]; zero-length bitfields get 32-bit boundary (`ZeroLengthBitfieldBoundary = 32`) [ibid.]; APCS also reverts unsigned bitfields to signed [clang31 `lib/Basic/Targets.cpp:2687` "Revert to using SignedInt on apcs-gnu"] |
| `enum` | 4 | 4 | |

Consequences: `struct { double x; }` inside a guest struct packs at offset 4 boundaries; a struct
sized 12 may cross the 4-register/stack split differently than on Linux; `long double` arguments and
`printf("%Lf")` must be treated as `double` (8-byte, 4-aligned) in the shim.

### 1.5 Argument assignment

Base-standard algorithm, from AAPCS32 (all of Stage A/B/C apply, since iOS is a base-standard
platform):

* Rule A.1: NCRN = r0. Rule A.3: NSAA = SP. Rule A.4: "If the subroutine is a function that returns
  a result in memory, then the address for the result is placed in r0 and the NCRN is set to r1"
  [AAPCS32:1296-1300] — i.e. **sret pointer is argument 0 in r0**.
* Rule B.2: integral types smaller than a word are extended to 4 bytes [AAPCS32:1322-1331].
* Rule B.4: composite sizes are rounded up to a multiple of 4.
* Rule C.3: an argument requiring double-word (8-byte) alignment rounds NCRN up to an even register.
  **Never triggers on iOS 6** because nothing has 8-byte alignment (§1.4) — measured:
  `gm(int a, long long b, double c)` passes `b` in `r1,r2` (odd start) and `c` **split across `r3`
  and the stack**, high word at `[sp]` [clang, measured].
* Rule C.4: if the argument fits in the remaining core registers, it is copied into consecutive
  registers. Rule C.5: if it does not fit and NSAA == SP, it is **split** between `r3..` and the
  stack. C.6-C.8: otherwise all of it goes to the stack.
* AAPCS's "no arguments are candidates for co-processor register classes" in the base standard
  [AAPCS32:1285-1287] is why floats/doubles occupy GPRs on iOS.

Measured classification (`clang -target armv7-apple-ios6.0 -emit-llvm -S`, argument side; "IR" is
what the front end emits *before* LLVM's ARM lowering, and it matches the emitted asm):

| C type | LLVM argument IR | machine effect |
|---|---|---|
| `struct{char}`, `struct{short}`, `struct{int}` (≤4 B) | `[1 x i32]` | r0 |
| `struct{int,char}`, `struct{int,short}`, `struct{int,int}`, `struct{char[8]}` (5-8 B) | `[2 x i32]` | r0,r1 |
| 9-12 B (`struct{int,int,int}`, `struct{int,double}`, 3 floats) | `[3 x i32]` | r0,r1,r2 |
| 13-16 B (`struct{char[16]}`, `struct{float[4]}`, `struct{double,double}`) | `[4 x i32]` | r0-r3 |
| 17-64 B (`struct{int[4];char}`, `struct{int[5]}`, `struct{int[16]}`) | `[N x i32]`, N = ceil(size/4) | r0-r3 then stack (split) |
| >64 B (`struct{char[65]}`, `struct{int[20]}`) | `ptr byval(%struct) align 4` | copied to the stack argument area by the caller and passed **by value** there (not as a pointer in a register) |

* Every composite whose size is a multiple of 4 up to 64 bytes is passed as N × i32 in
  registers/stack; there is no HFA path and no ">4 bytes ⇒ pointer" shortcut. `void f(struct F4)`
  compiles to a *tail branch* `b.w _gF4` because the callee's 4 words are already in r0-r3
  [clang, measured].
* The 64-byte/16-word byval threshold is clang's own implementation detail
  (`getContext().getTypeSizeInChars(Ty) > CharUnits::fromQuantity(64)` in
  `classifyArgumentType`) [clang `lib/CodeGen/Targets/ARM.cpp:457-464`]; it is stable across the
  compilers that built iOS 6 to the extent that the same clang code path was used, but it is the
  single most fragile item in this table. `UNVERIFIED:` a 2012-era Apple clang object demonstrating
  the identical threshold.
* A single `double`/`long long` is a 2-word scalar: it takes two consecutive GPRs, may start on an
  odd register, and may be split with the remainder on the stack (§ above).

**AAPCS-VFP (IHI 0055 / variant C) rules, listed only for contrast and for interop with
non-Apple ARMv7 objects** — Apple's own compiler never uses them for iOS ARMv7 (§1.1), but a
third-party static library built for `armv7-linux-gnueabihf`, or hand-written assembly compiled for
the VFP PCS, will follow them, and mixing the two silently corrupts arguments:

* VFP CPRCs are: a single-precision float (including a promoted half), a double, a 64/128-bit
  containerized vector, and a homogeneous aggregate (HFA/HVA) with a base type of float/double/64-bit
  or 128-bit vector and **1 to 4 members** [AAPCS32:1591-1611; clang
  `lib/CodeGen/Targets/ARM.cpp:363-370`].
* Rule C.1.vfp: if there are enough free co-processor registers of the right class, the argument is
  allocated to them, oldest first: floats to `s0-s15` and doubles to `d0-d7` (which alias as
  `s0-s15`) [AAPCS32:1636-1670]. Rule C.2.vfp: once VFP registers are exhausted, the remaining
  registers of that class are marked unavailable and the argument falls through to the core-register
  rules (so after any spill, *no* further float argument goes in VFP registers).
* HFA arguments are passed as the corresponding sequence of VFP registers; HFA results are returned
  in `s0-s3`/`d0-d3`; a non-HFA composite still uses `sret`.
* Variadic functions never use VFP registers: "no VFP CPRCs in a variadic procedure" — all variadic
  arguments are marshalled per the base standard [AAPCS32:1611].
* Consequence for this project: if a guest object contains an HFA-passing function (e.g. built with a
  GCC/Linux toolchain and linked into the app), the shim's FFI must classify per the *object's* ABI,
  which cannot be inferred from the Mach-O alone. The compiler's `pcs("aapcs-vfp")` attribute and
  clang's `-mfloat-abi=hard` are the markers to look for; a Mach-O slice that requires
  `CPU_SUBTYPE_ARM_V7` and was built by Apple clang is always the base standard.

### 1.6 Return values

Base-standard rules [AAPCS32:1229-1250]:

* `float` → `r0`; `double`/`long long`/`long double`( = `double`) → `r0,r1`; ≤4-byte fundamental → `r0`
  (extended).
* Composite ≤ 4 bytes: AAPCS would return it in `r0`, but clang's **APCS** classifier returns only
  "integer-like" composites that way and sends everything else to memory
  [clang `lib/CodeGen/Targets/ARM.cpp:606-640`, the `ABIKind::APCS` branch of
  `classifyReturnType`]. Measured: `struct{int}` → `i32` in `r0`; `struct{char[3]}`,
  `struct{float}`, `struct{int,char}`, `struct{double}` (8 B), `struct{int[3]}` → **all `sret`**.
  Note `struct{float}` is 4 bytes yet is returned indirectly: it is not "integer-like".
* Everything else: memory at an address passed as an extra argument in `r0` ("sret"). clang renders
  it as `ptr dead_on_unwind writable sret(%struct) align N` and LLVM passes it in `r0` first
  [clang, measured].

### 1.7 Variadic functions and `va_list`

* "A variadic function is always marshaled as for the base standard" [AAPCS32:1290]; "Variadic
  functions should always marshal to the base standard" [clang `lib/CodeGen/Targets/ARM.cpp:371-374`].
  Nothing changes relative to the fixed-arity rules; in particular there are no co-processor
  registers to reserve.
* Measured caller: `gv(1, 2, 3, 4, 5, 6.0f, 7.0)` with `gv(int, ...)` → named `1` in `r0`, unnamed
  `2,3,4` in `r1,r2,r3`, then `5` at `[sp+0]`, `6.0f` (promoted to `double`) at `[sp+4]`, `7.0` at
  `[sp+12]`. So unnamed arguments **continue filling r0-r3** after the named ones and then spill to a
  stack area whose slots are only 4-byte aligned [clang, measured].
* Callee: prologue stores `r1,r2,r3` into the incoming-argument home area just above the frame and
  sets `ap = &saved_r1`; `va_arg(double)` is therefore a plain aligned pointer bump
  (`add r1,#8`) with no register-file state [clang, measured].
* `va_list` is a bare pointer: clang 23 reports `__builtin_va_list 'void *'` for this triple
  [clang, measured]; clang 3.2 reported the `CharPtrBuiltinVaList` (`char *`) variant
  [clang31 `lib/Basic/Targets.cpp:3495`], and Apple's headers define `va_list` as
  `__darwin_va_list`, itself a pointer type [xnu `bsd/sys/_types/_va_list.h`]. It is **not** the
  AAPCS `struct __va_list { void *__ap; }` [AAPCS32:1936-1941].
* Implication: a `va_list` passed to a guest function from the FFI is just an address; a shim
  function that consumes a guest `va_list` must implement the same "pointer into the register save
  area / stack argument area" model.

### 1.8 C++ specifics

* `this` in `r0`; constructors/destructors return `this` in `r0`
  (`constructorsAndDestructorsReturnThis() == true` for ARM) [ARM C++ ABI / IHI 0031 §5; measured:
  `define ptr @_ZN1AC1Ev`].
* Itanium vtable layout, 2-word header `[offset-to-top, &_ZTI]` + address point; member-function
  pointers are 2 words / 8 bytes [Concrete ABI, `cppabi32.rst`; measured IR for a class with a
  virtual dtor].
* Exception model: **SjLj**, not DWARF/table-driven (§5).
* Objective-C++ and ObjC exceptions go through the ObjC runtime personalities `__objc_personality_v0`
  (present in `libobjc.A.dylib`) and `__gxx_personality_sj0`; `@try/@catch` compiles to
  `objc_begin_catch`/`objc_end_catch` plus the `__Unwind_SjLj_*` state machine [clang, measured].

## 2. armv7 vs armv7s

| | `armv7` | `armv7s` |
|---|---|---|
| Mach-O `cpusubtype` | `CPU_SUBTYPE_ARM_V7` = **9** [llvm `BinaryFormat/MachO.h:1690`] | `CPU_SUBTYPE_ARM_V7S` = **11** [llvm `BinaryFormat/MachO.h:1692`] |
| Devices | A4/A5 (`iPhone3GS`…`iPad2,4`, `iPhone4S`) | A6/A6X (`iPhone5,1`, `iPad3,4`+ …) — `UNVERIFIED:` exact device table |
| clang sub-target | `-target-cpu generic`, features `+vfp2 +vfp2sp +vfp3 +vfp3d16 +vfp3d16sp +vfp3sp +fp64 +d32 +neon` (+`+soft-float-abi +read-tp-tpidruro` on both) | `-target-cpu swift`, features = armv7's **plus `+fp16 +vfp4 +vfp4d16 +vfp4d16sp +vfp4sp`** [clang, measured via `clang -### -target armv7s-apple-ios6.0`] |
| ISA delta | ARMv7-A + VFPv3-D32 + NEON | ARMv7-A + **VFPv4** (true fused multiply-add `vfma`/`vfms`) + **FP16 conversions** (`vcvtb.f32.f16`/`vcvtt`) + **integer divide** (`sdiv`/`udiv`, "hwdiv") + NEON. It is *not* "VFPv3 with a divider": code compiled for A6 may contain VFPv4/FP16 instructions that fault or mis-decode on a VFPv3 model |
| ABI delta | **none** — same register roles, same argument/return rules, same stack alignment, same `long double` | none |
| Codegen delta: integer division | `a/b` on `int` calls `__divsi3`/`__udivsi3` | inlined `sdiv r0,r0,r1` / `udiv` [clang, measured at `-O0 -fno-inline`] |
| Codegen delta: fused multiply-add | no `vfma` in the architecture; `a*b+c` contracts to VFP `vmla.f32/f64` (`-ffp-contract=on`), `__builtin_fmaf` lowers without `vfma` | `__builtin_fmaf` → **`vfma.f32`** [clang, measured]; but plain `a*b+c` at `-O2` emits **NEON `vmul`+`vadd`, uncontracted**, because the Swift sub-target prefers NEON for FP (NEON has no FMA) — so the same C expression yields *different rounding* on the two slices, and a lifter must emulate the actual instructions rather than the source expression |
| 64-bit division | always calls `__divdi3`/`__udivdi3`/`__moddi3`/`__umoddi3` | idem [clang, measured] |
| Tooling gotcha | — | LLVM does **not** infer the armv7s feature set from the bare triple: `llvm-mc -triple=armv7s-apple-ios6.0` rejects `sdiv`/`udiv` ("instruction requires: divide in ARM"), `vfma.f32` ("requires: VFP4") and `vcvtb.f32.f16` ("requires: half-float conversions"). Reproducing it needs the *correct* feature names, all measured with llvm-mc 23.1.3: divide is **mode-specific** — `-mattr=+hwdiv` enables only Thumb-mode `sdiv`/`udiv`, `-mattr=+hwdiv-arm` only ARM-mode `sdiv`/`udiv` (using the wrong one gives "invalid instruction, any one of the following would fix this"); `-mattr=+vfp4` alone accepts both `vfma.f32` and `vcvtb.f32.f16` (so `+fp16` is redundant in 23.1.3); `+v7s` is *not* a subtarget feature (warning `'+v7s' is not a recognized feature for this target (ignoring feature)`) even though `v7s` is a valid arch-extension string in `TargetParser/ARMTargetParser.def`. `-mcpu=swift`, or `-mattr=+vfp4,+hwdiv-arm` for ARM mode, assembles all three. clang is consistent because `armv7s` implies `-target-cpu swift`, which carries both divide features plus `vfp4`/`fp16` (`ProcSwift`, LLVM `release_32 ARM.td`) [measured; LlvmTooling] |

Lifter/runtime requirements: dispatch on the slice's `cpusubtype` (`9` vs `11`) — do not silently
accept an `armv7s` slice if the lifter cannot decode `sdiv`/`udiv`/`vfma`/`vcvtb`; a recompiler that
decodes the *instructions actually present* needs no ABI distinction. `[dsc]` shows iOS 6.1.3 armv7
images with `cpusubtype 9` (`libobjc.A.dylib`, `libcompiler_rt.dylib`, all 569 images checked), i.e.
the iPad 2 cache is the `v7` variant; the same OS build shipped `armv7s` slices for A6 devices.
Do **not** model `armv7s` as VFPv3 + hwdiv only — the 2012 toolchain that introduced armv7s defines
the sub-target the same way: LLVM `release_32` `lib/Target/ARM/ARM.td`:
`def ProcSwift : SubtargetFeature<"swift", …>;` with features
`FeatureNEONForFP, FeatureT2XtPk, FeatureVFP4, FeatureMP, FeatureHWDiv, FeatureHWDivARM,
FeatureAvoidPartialCPSR, FeatureHasSlowFPVMLx`, selected by
`def : ProcessorModel<"swift", SwiftModel, [ProcSwift, HasV7Ops, FeatureNEON, …]>`, and clang
`release_32 lib/Driver/Tools.cpp` maps `armv7s|armv7-s|swift` → `swift`. `FeatureNEONForFP` plus
`FeatureHasSlowFPVMLx` are the reason the same C expression uses VFP `vmla` on `armv7` but
uncontracted NEON `vmul`+`vadd` on `armv7s` (measured above).

Consequence for our own tooling: if the lifter's disassembler/decoder is configured through LLVM's
`MCSubtargetInfo`, a mis-specified armv7s feature set makes valid A6 instructions decode as failures
(`MCDisassembler::getInstruction()` returns `Fail`/`SoftFail` rather than raising), i.e. guest code
silently becomes "illegal instruction" — configure armv7s with `+vfp4` **and** both divide features
(assembler and disassembler behave identically), or decode the handful of armv7s-only encodings
(`sdiv`/`udiv` in both modes, `vfma`/`vfms`, `vcvtb`/`vcvtt` f16) directly [LlvmTooling].

## 3. Compiler runtime helpers the guest calls

**Naming rule:** a Mach-O symbol is `_` + the C identifier, and the assembler prints that symbol, so
`bl ___divsi3` in clang output is Mach-O symbol `___divsi3` = C name `__divsi3`. Triple-check names
when writing the shim — one leading underscore off and the guest silently binds nothing.

**There are no `__aeabi_*` symbols in the iOS 6 world.** clang never emits AEABI helper names for this
target (measured: division/memcpy/TP access all use Darwin names), and a full walk of the 569 images
in `[dsc]` finds zero `__aeabi_idiv`/`__aeabi_memcpy` definitions or references. `__aeabi_read_tp`,
`__aeabi_*divmod`, `__aeabi_dadd` … belong to bare-metal/AEABI/Linux toolchains; if a guest binary
contains them (hand-written asm or a third-party static library built with a non-Apple toolchain),
they are app-local, not libSystem.

Measured emission (clang 23.1.3, `armv7-apple-ios6.0`, symbols as Mach-O names):

| guest construct | emitted symbol | where it resolves on iOS 6 | evidence |
|---|---|---|---|
| `int`/`unsigned` `/` `%` (armv7) | `___divsi3`, `___udivsi3`, `___modsi3`, `___umodsi3` | `libcompiler_rt.dylib` (`__divsi3` @ +0x1100 ARM mode, `__udivsi3` @ +0x2850, `__modsi3` @ +0x1a00, `__umodsi3` @ +0x28c0) | [clang] + [dsc] export trie |
| `int`/`unsigned` `/` `%` (armv7s) | none (inline `sdiv`/`udiv`) | — | [clang, measured] |
| `long long` `/` `%` | `___divdi3`, `___udivdi3`, `___moddi3`, `___umoddi3` | `libcompiler_rt.dylib` (`__divdi3` @ +0xdb5, Thumb) | [clang] + [dsc] |
| 64-bit multiply, shifts | `___muldi3` (only when not inlined) | `libcompiler_rt.dylib` (`__muldi3` @ +0x1cb5) | [dsc] |
| `(float|double) ← long long/unsigned long long` | `___floatdisf`, `___floatundidf` | `libcompiler_rt.dylib` (`__floatundidf` @ +0x15a1) | [clang] + [dsc] |
| `long long ← double` | `___fixdfdi`; unsigned form `___fixunsdfdi`; float form `___fixsfdi` | `libcompiler_rt.dylib` | [clang] + [dsc] |
| `__builtin_powi` | `___powidf2` | `libcompiler_rt.dylib` | [clang] + [dsc] |
| `__builtin_memcpy/memmove/memset` (not inlined) | `_memcpy`, `_memmove`, `_memset` (**not** `__aeabi_memcpy`) | `libsystem_c.dylib` | [clang, measured] + [dsc] |
| large zero-initialised buffers | `___bzero` | `libsystem_c.dylib` | [dsc] |
| `__builtin___clear_cache` | `___clear_cache` | `libsystem_c.dylib` | [clang, measured] |
| `-fstack-protector*` | `___stack_chk_fail` (call) + `___stack_chk_guard` (data, via a non-lazy pointer) | `libsystem_c.dylib` | [clang, measured] |
| `__atomic_*` on 64-bit objects, `-O0` | `___atomic_load`, `___atomic_compare_exchange` | `libcompiler_rt.dylib` | [clang, measured] |
| 32-bit atomics | inlined `ldrex`/`strex` (+`dmb ish`) | — | [clang, measured] |
| soft-float compare/convert helpers (`__eqdf2vfp`, `__floatsidfvfp`, `__truncdfsf2vfp`) | only in `-msoft-float` builds | `libcompiler_rt.dylib` exports them | [dsc] export trie |
| C++ EH scaffolding | `__Unwind_SjLj_Register`, `__Unwind_SjLj_Unregister`, `__Unwind_SjLj_Resume`, `__cxa_allocate_exception`, `__cxa_throw`, `__cxa_begin_catch`, `__cxa_end_catch`, personality `__gxx_personality_sj0` | `libunwind.dylib` (SjLj API), `libc++abi.dylib` (`__cxa_*`, `_Unwind_SjLj_RaiseException`), `libcompiler_rt.dylib` (`__gcc_personality_sj0` @ +0x16c1) | [clang, measured] + [dsc] |
| ObjC EH | `objc_exception_throw`, `objc_begin_catch`, `objc_end_catch`, `__objc_personality_v0`, `OBJC_EHTYPE_id` | `libobjc.A.dylib` (`_objc_exception_throw` @ 0x38fc7960) | [clang, measured] + [dsc] |
| TLS bootstrap | `__tlv_bootstrap` (private extern, dyld-resolved) | `dyld` (`threadLocalVariables.c`), never exported by a cache dylib | [dsc] + [dyld-239.3 §4] |

Resolution mechanics and what the shim must provide:

* The iOS link line is `ld -dynamic -arch armv7 -iphoneos_version_min 6.0.0 … -lSystem` — no
  `-lclang_rt`, no `-lgcc_s.1` [clang, measured via `clang -### -target armv7-apple-ios6.0 m.c`].
  `usr/lib/libgcc_s.1.dylib` in an Apple SDK is a **symlink to `libSystem.B.dylib`** (SDK content is
  the literal string `libSystem.B.dylib`) [MacOSX10.8.sdk stub, measured].
* `libSystem.B.dylib` is a re-export umbrella; on iOS 6 it re-exports `libcompiler_rt.dylib`
  (compiler-rt builtins), `libunwind.dylib`, `libc++abi.dylib`, `libsystem_c.dylib`,
  `libsystem_kernel.dylib`, `libdispatch.dylib`, … [dsc, `LC_REEXPORT_DYLIB` list of
  `/usr/lib/libSystem.B.dylib`; the same list appears in the macOS 10.8 SDK stub].
* Therefore the shim must export, at minimum, every symbol in the left column above (plus everything
  the guest's undefined-symbol set names) with the Mach-O name, and the lifter must be able to
  resolve both lazy (`__la_symbol_ptr`/`__stub_helper`) and non-lazy (`__nl_symbol_ptr`) bindings,
  since clang binds some helpers non-lazily (e.g. `___stack_chk_guard`, `__gxx_personality_sj0`,
  `OBJC_EHTYPE_id`, `objc_msgSend` itself) and others lazily [clang, measured].
* `__divsi3` etc. are *plain functions* (no special calling convention); the 32-bit ones are ARM-mode
  (`LSB=0`) and the 64-bit ones Thumb in the shipped library [dsc].

Explicitly, the AEABI names an implementer is likely to expect — `__aeabi_idiv`, `__aeabi_uidiv`,
`__aeabi_idivmod`, `__aeabi_uidivmod`, `__aeabi_ldivmod`, `__aeabi_uldivmod`, `__aeabi_lmul`,
`__aeabi_memcpy`/`memcpy4`/`memcpy8`, `__aeabi_memmove`, `__aeabi_memset`/`memclr`,
`__aeabi_read_tp`, `__aeabi_dadd`…`__aeabi_f2d` — are **absent from the iOS 6 toolchain output and
from the shipped runtime** [clang measured; dsc: zero `__aeabi_*` definitions or references in 569
images]. Do not pre-define them in the shim as an "ARM compatibility layer": they would only ever be
hit by objects built for a different platform.

Not everything is a call. Measured inlining on this target: 32-bit `__atomic_*`/`__sync_*` become
`ldrex`/`strex` + `dmb ish` sequences (only 64-bit/`__atomic_load`/`__atomic_compare_exchange`
produce calls) [clang measured]; `__builtin_popcount`/`__builtin_clz` become `usad8`/`clz`;
`__builtin_memcpy`/`memset` with constant small/medium sizes are expanded inline (a `memcpy` call only
appears for non-constant sizes or very large ones) [clang measured]; and on `armv7s` the 32-bit
division helpers disappear into `sdiv`/`udiv` (§2). A shim therefore cannot assume "if the binary
divides, it calls a helper", but must still export every name that appears in the guest's undefined
symbol table.

## 4. Thread-local storage

### 4.1 The uncomfortable fact first: iOS 6 guest code has no `__thread` variables

* clang refuses `__thread` for 32-bit iOS device targets below iOS 9:
  `error: thread-local storage is not supported for the current target` — measured for
  `armv7-apple-ios6.0`, `x86_64-apple-ios6.0`, `arm64-apple-ios7.0`; accepted for
  `armv7-apple-macosx10.8` [clang, measured]. Rule: `DarwinTargetInfo` ctor starts with
  `TLSSupported = false`, then `isMacOSX() → !isMacOSXVersionLT(10,7)`, `isiOS() → 64-bit:
  !isOSVersionLT(8), 32-bit device: !isOSVersionLT(9), 32-bit simulator: !isOSVersionLT(10)`
  [clang `lib/Basic/Targets/OSTargets.h:85-110`].
* **A full walk of all 569 images in the iOS 6.1.3 armv7 shared cache finds zero
  `__thread_vars`/`__thread_data`/`__thread_bss`/`__thread_ptrs` sections** (section census:
  `__TEXT,__text` 569, `__DATA,__data` 445, `__gcc_except_tab` 232, … no TLS section at all) [dsc,
  measured]. `_tlv_bootstrap`, `_tlv_get_addr`, `_tlv_atexit` are not exported by any cache image
  either [dsc].
* So on an iOS 6 guest, "TLS" means `pthread_getspecific`/`pthread_key_create`, `errno`, and the
  compiler's `__thread` path is unreachable. Implement §4.4 anyway (it is what guest code uses), and
  implement §4.2/4.3 if you intend to run iOS 8/9-era armv7 binaries or hand-written asm that uses
  TLV, or if the guest is a later-OS binary back-ported.

### 4.2 Mach-O TLV descriptor format (what a runtime must implement if it ever sees one)

| item | value | source |
|---|---|---|
| descriptor sections | `__DATA,__thread_vars` (`S_THREAD_LOCAL_VARIABLES` = 0x13); payload `__DATA,__thread_data` (`S_THREAD_LOCAL_REGULAR` = 0x11), `__DATA,__thread_bss` (`S_THREAD_LOCAL_ZEROFILL` = 0x12); cross-image pointer slots `__DATA,__thread_ptrs` = 0x14; `__thread_init` = 0x15 | `/usr/include/llvm/BinaryFormat/MachO.h:166-178` (local LLVM 23 headers, measured) |
| descriptor | `struct TLVDescriptor { void *(*thunk)(TLVDescriptor *); unsigned long key; unsigned long offset; }` — **12 bytes on armv7, fields in that order** | [dyld-239.3 `src/threadLocalVariables.c:93`] |
| field 0 (`func`) | `__tlv_bootstrap` in the object file; a loader rewrites it to point at `_tlv_get_addr` | [dyld-239.3 `src/threadLocalVariables.c`], dyld `libdyld/ThreadLocalVariables.h` |
| field 1 (`key`) | 0 in the object file; loader writes the pthread key/index used to find the per-thread block | ibid. |
| field 2 (`offset`) | offset of the variable inside the image's per-thread template block (linker converts the `__thread_data`/`__thread_bss` reference) | ibid. |
| compiler output | per TU: data in `__thread_data`/`__thread_bss` with symbol `_x$tlv$init`; descriptor in `__thread_vars`; every access is `ldr r0,=descriptor; ldr r1,[r0]; blx r1; ldr r0,[r0]` (the `blx` result is the variable address) | [clang, measured: `clang -target armv7-apple-ios9.0 -S` / `llc -mtriple=armv7-apple-ios6.0`] |
| thunk ABI | `void *_tlv_get_addr(TLVDescriptor *)` — descriptor in `r0`, result in `r0`, **all other registers preserved** (`push {r1,r2,r3,r7,lr}` … `pop {r1,r2,r3,r7,pc}`) | [dyld-239.3 `src/threadLocalHelpers.s`, `#if __arm__` branch] |
| other entry points | `_tlv_bootstrap` (aborts: "thread locals not initialized"), `_tlv_atexit(termFunc, objAddr)`, `_tlv_exit` | [dyld-239.3 `threadLocalVariables.c:387,461`; dyld `include/mach-o/dyld.h:107-115`] |
| per-thread allocation | first touch mallocs the whole image template, copies `__thread_data`, zeroes `__thread_bss`, stores the block under the key | [dyld-239.3 `src/threadLocalVariables.c:180-260` (`tlv_allocate_and_initialize_for_key`)] |

Minimum shim implementation: parse `__thread_vars` at guest load; for each descriptor set
`thunk = &our_tlv_get_addr`; keep a per-image template (size = `__thread_data` + `__thread_bss`),
allocate per thread on first use, copy/zero the image, publish it in a table indexed by the
descriptor (do **not** rely on pthread key values), and run `$tlv$init` destructors registered via
`_tlv_atexit` at thread exit. `_tlv_get_addr` must return `block + descriptor->offset`.

### 4.3 Thread pointer register on ARMv7 Darwin

* The thread pointer is `TPIDRURO`, read as `MRC p15, 0, rX, c13, c0, 3` (PL0 read-only; the kernel
  writes it on context switch). clang models exactly this for the target: the cc1 feature list
  contains `+read-tp-tpidruro` [clang, measured via `clang -### -target armv7-apple-ios6.0`].
  (`__builtin_thread_pointer()` is nevertheless rejected for this triple with "cannot compile this
  __builtin_thread_pointer yet" [clang, measured].)
* Its value is `&pthread->tsd[0]`, i.e. the base of the per-thread TSD array, with CPU-number bits in
  the low bits: `_os_tsd_get_base()` returns `TPIDRURO & ~3` (xnu `libsyscall/os/tsd.h`).
* `r9` is *not* used as the thread register on ARMv6/v7 — that was the pre-ARMv6 scheme
  (`Libc-763.13 arm/pthreads/pthread_self.s` `#ifndef _ARM_ARCH_6`; whole-OS relevant copy:
  `Libc-825.40.1`).

### 4.4 How libSystem implements per-thread data

* TSD lives directly off the TP: `pthread_getspecific(slot)` `= ((void **)_os_tsd_get_base())[slot]`
  [xnu `libsyscall/os/tsd.h`, `Libc-763.13 arm/pthreads/pthread_getspecific.s`: `mrc p15,0,r1,c13,c0,3;
  bic r1,r1,#3; ldr r1,[r1, slot*4]`].
* Reserved slots (iOS 6): 0 `__TSD_THREAD_SELF` (the `pthread_t`), 1 `ERRNO`, 2 `MIG_REPLY`,
  3 `MACH_THREAD_SELF`, 4 `QOS_CLASS`, … [xnu `libsyscall/os/tsd.h:36-45`].
* `pthread_self()` as shipped is exactly `*(void **)(TPIDRURO & ~3)` — `mrc p15,0,r0,c13,c0,3;
  bic r0,r0,#3; ldr r0,[r0]` [Libc-825.40.1 `arm/pthreads/pthread_self.s`; identical bytes at
  0x3942d964 in the iOS 6.1.3 cache]. `pthread_getspecific(key)` fast path is `tsd[key]` at
  `(TPIDRURO & ~3) + 4*key` [Libc-825.40.1 `arm/pthreads/pthread_getspecific.s`, cache 0x3942d950].
* Key numbering (matters if you emulate keys): dyld owns 1-10 (`__pthread_tsd_first`), static/
  libSystem keys 10-255, dynamic keys from `__pthread_tsd_start` = 256 to `__pthread_tsd_end` = 512;
  the TSD array is `void *tsd[512]` inside `struct _pthread`; key 0 is never returned (it is
  `pthread_self`); `pthread_key_create` returns `EAGAIN` when exhausted and `pthread_key_delete`
  `EINVAL` for bad keys [Libc-825.40.1 `pthreads/pthread_internals.h:91-99`,
  `pthread_tsd.c`]. `pthread_setspecific` returns `0`/`EINVAL` (errno-style `int`, never `-1`).
* Destructors: `_pthread_tsd_cleanup` runs destructors `PTHREAD_DESTRUCTOR_ITERATIONS` times, dynamic
  keys first then static keys [Libc-825.40.1 `pthreads/pthread_tsd.c:191-228`]; the numeric value of
  `PTHREAD_DESTRUCTOR_ITERATIONS` (4 in Darwin headers) is not present in the shipped libraries —
  `UNVERIFIED:`.
  The installing call is `_pthread_set_self(&p->tsd[0])` after `p->tsd[0] = p`
  [Libc-763.13 `pthreads/pthread.c:1928`].
* Runtime requirements, in order of how often guest code depends on them:
  1. `pthread_self()` must return a stable per-thread handle whose `[0]` slot is itself and whose
     following slots are the reserved TSD slots (guest code and libobjc's autorelease pools index
     the TP directly in optimized paths, so a glibc-style TCB with a different layout is *not*
     interchangeable).
  2. `pthread_getspecific/setspecific` with destructor support and re-entering destructors up to
     `PTHREAD_DESTRUCTOR_ITERATIONS` (4) times.
  3. `errno` must be at slot 1 of the TP block.
  4. If the guest uses `__thread` at all (iOS 8/9 binaries): §4.2 + `TPIDRURO` visible to the guest
     code that reads it with `MRC`. Because the guest runs on a host thread, either the guest's TP
     value is reconstructed on every entry (cheap: a host→guest TP field in the runtime's thread
     descriptor, loaded by the lifter's `MRC c13,c0,3` implementation) or guest TP-relative
     accesses are rewritten to a runtime call. The former preserves guest code with no analysis.

### 4.5 ELF TLS contrast (why "just use ELF TLS" fails)

| | Mach-O TLV (Darwin) | ELF (ARM32) | ELF (arm64) |
|---|---|---|---|
| model | per-variable descriptor + indirect call through a thunk (inherently "general dynamic" + lazy) | link-time variants: local-exec / initial-exec / general-dynamic / local-dynamic | same as ELF ARM32 |
| accessor | `thunk(descriptor)` — an ordinary function call, ABI above | GD/LD: `__tls_get_addr(tls_index *)`; IE/LE: TP-relative offsets, no call | same |
| sections | `__thread_vars` + `__thread_data`/`__thread_bss` (+`__thread_ptrs`); **no per-image PT_TLS** | `.tdata`/`.tbss` inside the `PT_TLS` segment | same |
| allocation | template block malloc'd per thread by the loader on first touch | loader/thread-create allocates `PT_TLS` block, `tcbhead_t` before it | same |
| TP | `TPIDRURO` (PL0 RO) on armv7, `TPIDRRO_EL0` on arm64; value is the TSD array base, low bits = CPU | `TPIDRURO` (via `__aeabi_read_tp`/`libc`), glibc x86 uses `%fs` | `TPIDR_EL0` (read/write), no CPU bits |
| specs | [dyld-239.3] + Mach-O section types | Drepper, *ELF Handling for Thread-Local Storage*; `R_ARM_TLS_*` in aaelf32 | `R_AARCH64_TLSLE_*`, AAPCS64 §9.3 |

Never map ELF TLS relocation semantics onto Darwin TLV: the Darwin side has no TP-relative static
form and no ELF-style TLS relocations at all; a `TPIDR_EL0`-relative offset from an ELF object is
simply a different address space.

## 5. Unwinding

### 5.1 What iOS 6 armv7 binaries actually contain (measured)

Section census over all 569 images of the iOS 6.1.3 armv7 shared cache:

| section | images |
|---|---|
| `__TEXT,__text` | 569 |
| `__TEXT,__gcc_except_tab` (LSDA for SjLj EH) | 232 |
| `__DATA,__mod_init_func` / `__mod_term_func` | 47 / 5 |
| `__TEXT,__unwind_info` | **0** |
| `__TEXT,__eh_frame` | **0** |
| `.ARM.exidx` / `.ARM.extab` (ARM EHABI) | **0** |

Consequences, and these are the load-bearing facts of this section:

1. **There is no compact unwind and no DWARF CFI in iOS 6 ARMv7 binaries.** The only cross-frame
   unwinding mechanism available in-process is the **r7 frame-pointer chain** (plus the `lr`-in-leaf
   case), which is exactly why Apple's documentation insists on r7
   ("Failure to use R7 as a frame pointer prevents debugging and performance tools from generating
   valid backtraces" [apple-av6]).
2. This is a deliberate result of the compiler's exception model: the Darwin driver selects the
   **SjLj** exception model for 32-bit ARM (`Darwin::GetExceptionModel()` → `SjLj` for
   `arm`/`thumb` unless watchOS) and `MachO::getDefaultUnwindTableLevel()` returns `None` under
   SjLj, so neither `__compact_unwind`/`__unwind_info` nor `__eh_frame` is emitted at all
   [clang `lib/Driver/ToolChains/Darwin.cpp` ~3786-3820; CompactUnwind research note]. `UNVERIFIED:`
   an Apple clang-4.x binary inspected directly (the cache census above is the strongest available
   evidence and agrees).
3. ARM EHABI (`.ARM.exidx`/`.ARM.extab`, `SHT_ARM_EXIDX`) is an **ELF-only** format, emitted by
   `llvm/lib/Target/ARM/MCTargetDesc/ARMELFStreamer.cpp:1210-1215`; Mach-O has no such section and
   no EHABI opcode tables. Tools must not look for it.
4. Frame-pointer walking on this ABI is workable because r7 is callee-saved and standardized:
   `push {r7, lr}` / `mov r7, sp` prologue, `[r7]` = caller's r7, `[r7+4]` = return address,
   `mov sp, r7; pop {r7, pc}` epilogue [clang, measured; apple-av6 prolog/epilog description].

### 5.2 `__unwind_info` (compact unwind) format reference

Not present in iOS 6 armv7 files, but required to read arm64/macOS binaries, to interoperate with
crash reporters that parse it, and if you build guest code with `-femit-dwarf-unwind=...`/CU on later
targets. Canonical struct definitions: `mach-o/compact_unwind_encoding.h`
(byte-identical copy at `/home/theo/src/xnu/EXTERNAL_HEADERS/mach-o/compact_unwind_encoding.h`;
upstream `llvm-project/libunwind/include/mach-o/compact_unwind_encoding.h`).

| struct | size | fields (offset) |
|---|---|---|
| `unwind_info_section_header` | 28 B | `version`(0) = 1 (`UNWIND_SECTION_VERSION`), `commonEncodingsArraySectionOffset`(4), `commonEncodingsArrayCount`(8), `personalityArraySectionOffset`(12), `personalityArrayCount`(16), `indexSectionOffset`(20), `indexCount`(24) |
| `unwind_info_section_header_index_entry` | 12 B | `functionOffset`(0) — function start, image-relative to the Mach-O header; `secondLevelPagesSectionOffset`(4) — section-relative; `lsdaIndexArraySectionOffset`(8) |
| `unwind_info_section_header_lsda_index_entry` | 8 B | `functionOffset`, `lsdaOffset` (image-relative) |
| regular second-level page | header 8 B | `kind`(0)=`UNWIND_SECOND_LEVEL_REGULAR`(2), `entryPageOffset`(u16@4), `entryCount`(u16@6); entries are `{uint32 functionOffset; uint32 encoding;}` pairs |
| compressed second-level page | header 12 B | `kind`(0)=`UNWIND_SECOND_LEVEL_COMPRESSED`(3), `entryPageOffset`(u16@4), `entryCount`(u16@6), `encodingsPageOffset`(u16@8), `encodingsCount`(u16@10) |

Compressed entries: `u32 entry = functionOffset (low 24 bits: `UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET` = `& 0x00FFFFFF`) | encodingIndex (high 8 bits: `UNWIND_INFO_COMPRESSED_ENTRY_ENCODING_INDEX` = `>> 24`).
If `encodingIndex < header.commonEncodingsArrayCount` the encoding comes from the common encodings
array at `commonEncodingsArraySectionOffset`; otherwise from the page's own array at
`page + encodingsPageOffset + (encodingIndex - commonEncodingsCount) * 4`
[CompactUnwind research note; Apple libunwind-35.1 `src/UnwindCursor.hpp`
`getInfoFromCompactEncodingSection`].

Decode procedure (`find_unwind_info(pc)`), correcting two widespread errors:

1. The index is **not** indexed by `pc >> 12`. Binary-search `index[]` on `functionOffset` for the
   greatest entry ≤ (pc - image_header) [Apple libunwind `UnwindCursor.hpp`; LLDB
   `CompactUnwindInfo.cpp:245 ScanIndex`, `:451 BinarySearchCompressedSecondPage`,
   `:491 GetCompactUnwindInfoForFunction`].
2. 4096 is only the *alignment* ld64 gives each second-level page (and a descriptive comment);
   nothing about the lookup is page-based [ld64 `src/ld/passes/compact_unwind.cpp`].
3. Inside the page, binary-search/skip over the entries (regular: `functionOffset`; compressed:
   `functionOffset` + `entryCount`; note compressed indices are `entryCount >> 1` for encodings),
   then decode the encoding into a register-restore program.

Decoding an ARM32 encoding to a DWARF-ish restore program
(`encoding` → which registers to pop, from where, and the CFA):

| field | mask | meaning |
|---|---|---|
| mode | `UNWIND_ARM_MODE_MASK` = `0x0F000000` | `UNWIND_ARM_MODE_FRAME` = `0x01000000`, `UNWIND_ARM_MODE_FRAME_D` = `0x02000000`, `UNWIND_ARM_MODE_DWARF` = `0x04000000` |
| stack adjustment | `UNWIND_ARM_FRAME_STACK_ADJUST_MASK` = `0x00C00000` | multiple of 4 words **added on top of** the pushed registers |
| first push set | bits 0-2 = `UNWIND_ARM_FRAME_FIRST_PUSH_R4/R5/R6` | saved in the first push (low bits) |
| second push set | bits 3-7 = `SECOND_PUSH_R8..R12` | saved in the second push |
| saved D registers | `0x00000700` (LLDB/ld64) — clang writes `(floatRegCount-1) << 8` with ≤ 4 D regs, i.e. bits 8-11 | D8-D15 saved, count encoded |
| DWARF escape | low 24 bits = FDE offset into `__TEXT,__eh_frame` when mode == `UNWIND_ARM_MODE_DWARF` | |

`r7` and `lr` are implicit in `UNWIND_ARM_MODE_FRAME`; `fp`/`lr` handling and the "r14 in the low
nibble with a register count in the high nibble" description that circulates in some writeups does
not exist in any of clang's `ARMAsmBackend.cpp` (`namespace CU`), LLDB's `CompactUnwindInfo.cpp:116`
or ld64's `unwinddump.cpp:763` — treat it as wrong. `UNVERIFIED:` the exact `0x700` vs `0xF00` D-reg
count mask disagreement between LLDB/ld64 and clang's writer for >1 D register.

### 5.3 When `__eh_frame` is used

`-femit-dwarf-unwind={default,always,no-compact-unwind,dwarf-only}`
[clang `docs/UsersManual.md`, `Options.td`]: functions the compact encoder cannot represent get an
`UNWIND_*_MODE_DWARF` encoding whose low 24 bits are an FDE offset in `__TEXT,__eh_frame`; `always`
emits both. On iOS 6 ARMv7 the mechanism is moot (no EH data is emitted at all, §5.1); it matters for
arm64 slices and for tools.

### 5.4 The exception runtime iOS 6 actually uses (SjLj)

Symbol presence in `[dsc]` (569 images) and the call graph. Shipped versions (measured from
`LC_ID_DYLIB` compatibility versions): `libobjc.A.dylib` 228.0.0, `libSystem.B.dylib` 173.8.0,
`libunwind.dylib` 35.1.0, `libcompiler_rt.dylib` 33.0.0, `libc++abi.dylib` 24.1.0, `libc++.1.dylib`
65.0.0, all `cpusubtype 9` (armv7):

| symbol | definer on iOS 6 armv7 | role |
|---|---|---|
| `_Unwind_SjLj_Register` / `_Unwind_SjLj_Unregister` / `_Unwind_SjLj_Resume` | `libunwind.dylib` (+0xc15/0xc2d/0xc91), re-exported by `libSystem.B.dylib` | push/pop a `_Unwind_FunctionContext` onto the thread's SjLj chain; resume into a frame |
| `_Unwind_SjLj_RaiseException` | `libc++abi.dylib` | the "raise" entry: walks the per-thread context chain, calls personalities |
| `__gxx_personality_sj0` | `libcompiler_rt.dylib` (+0x16c1) | C++ personality for the SjLj model |
| `__objc_personality_v0` | `libobjc.A.dylib` | ObjC personality (`@try/@catch`, cleanup) |
| `__cxa_throw`, `__cxa_allocate_exception`, `__cxa_begin_catch`, `__cxa_end_catch`, `__cxa_rethrow` | `libc++abi.dylib` | Itanium C++ ABI entry points; `__cxa_throw` ends in `_Unwind_SjLj_RaiseException` when built with `__USING_SJLJ_EXCEPTIONS__` [libc++abi `src/cxa_exception.cpp:277-279`] |
| `_Unwind_RaiseException`, `_Unwind_Resume`, `__gcc_personality_v0` | **not exported by any of the 569 images** (no DWARF-EH consumer exists on iOS 6 armv7) | — |

Mechanism: each function with a `try`/`catch` (or with ObjC `@try` and non-trivial cleanups) calls
`_Unwind_SjLj_Register(&context)` in its prologue-ish setup and `_Unwind_SjLj_Unregister` on all exits;
the context records the saved register set (the same callee-saved set as `setjmp`: r4-r8, r10, r11,
sp, lr and d8-d15) plus a `resumeLocation`. Throwing calls `__cxa_throw` →
`_Unwind_SjLj_RaiseException`, which walks the thread's chain (head kept in a TSD slot,
`__PTK_LIBC_DYLD_Unwind_SjLj_Key`) invoking `__gxx_personality_sj0`/`__objc_personality_v0` until one
returns `_URC_INSTALL_CONTEXT`, then "resumes" by restoring that recorded state and jumping to the
landing pad (a longjmp-like transfer, not a frame-by-frame unwinder)
[CompactUnwind research note; libunwind-35.1 `src/Unwind-sjlj.c:93-107,278,308`;
libc++abi `src/cxa_personality.cpp:1015-1023`].

### 5.5 Constraints this places on the JIT/lifter

* **There is nothing to register for guest unwinding.** The guest never looks at FDEs, compact
  unwind, or `.ARM.exidx`, so the JIT does not need to synthesize unwind tables for JIT'd guest
  functions to make guest exceptions work. What it must not do is *break the chain* and *break the
  saved state*:
  1. Never clobber the per-thread SjLj chain head (TS D slot) or the guest's
     `_Unwind_FunctionContext` records. If the JIT replaces the TSD slot layout, it must keep the
     same slot indices (§4.4).
  2. Materialize guest register state in the guest's own stack/registers before any guest call that
     may throw or `longjmp`: the SjLj resume path restores **only** what `_Unwind_SjLj_Register`
     recorded (r4-r8, r10, r11, sp, lr, d8-d15) plus the landing pad's own expectations. Anything the
     lifter kept in host registers (or in a host-side "guest register cache") is not restored — see
     §6.4.
  3. Host frames must be transparent. If a guest frame A calls JIT-compiled guest frame B which calls
     guest frame C that throws and is caught in A, the unwind walk only visits frames that registered
     contexts; the JIT's own host frames are skipped. That is fine, *provided* the JIT does not need
     host destructors/cleanup on the skipped path and does not let host state (e.g. a host
     `catch`) intercept. Corollary: never implement guest `try/catch` by catching a host exception
     in a host frame between two guest frames, because the host unwinder will be handed a guest
     control transfer it cannot represent.
  4. If the JIT *itself* throws (host C++ exception escaping into guest code), the guest has no
     mechanism to handle it; the runtime must catch at the guest boundary and convert
     (`objc_exception_throw`/`__cxa_throw` into guest code) or abort.
  5. If JIT'd code needs to *catch* guest C++/ObjC exceptions, it must emit the same SjLj
     registration sequence: a `_Unwind_SjLj_Register`-compatible context, an LSDA in a
     `__gcc_except_tab`-like table, and a call to the guest personality — i.e. a lifter must either
     recompile the guest's existing LSDA/personality references verbatim (§3) or re-implement the
     personality. Re-raising through a JIT frame that has no LSDA is legal (frame is skipped).
* For the host side of the runtime (our own C++ exceptions across JIT'd guest trampolines), LLVM's
  ORC provides EH-frame registration (`llvm::orc::RegisterEHFrames` →
  `__register_frame` per-FDE on Mach-O hosts) and, on newer hosts, dynamic unwind-section lookup
  (`__unw_add_find_dynamic_unwind_sections`/`unw_dynamic_unwind_sections`,
  `Orc/TargetProcess/UnwindInfoManager.cpp`). JITLink's `CompactUnwindSupport` emits
  `__TEXT,__unwind_info` only for x86_64 and arm64 guests (`MachO_x86_64.cpp`, `MachO_arm64.cpp`);
  there is **no armv7 Mach-O backend in lld or JITLink** (measured: `ld64.lld -arch armv7` →
  `error: missing or unsupported -arch armv7`; `ld64.lld` 23.1.3 accepts only `arm64`/`x86_64`), so
  any guest-side unwind/link data for armv7 must be produced by the project's own writer.
* Guest crash reporters call `_dyld_find_unwind_sections`/`dyld_unwind_sections` (dyld private API,
  `dyld-239.3 src/dyldExceptions.c:116-123`, `include/mach-o/dyld_priv.h:151-166`) to discover
  `{dwarf_section, compact_unwind_section}`. On iOS 6 these return empty sections; the shim should
  provide the call and return zeroed pointers rather than leaving the symbol undefined.

## 6. `setjmp` / `longjmp`

Source of truth for iOS 6: `Libc-825.40.1` (tag in `apple-oss-distributions/Libc`), files
`arm/sys/_setjmp.s`, `arm/sys/_setjmp.h`, `arm/sys/setjmp.s`, `arm/sys/longjmp.s`, `arm/sys/_longjmp.s`,
`include/setjmp.h`. Contents below are from that tag (each claim file+symbol-cited there), and match
the jmp_buf *closure* set used by the SjLj EH implementation (§5.4), which is not a coincidence: both
record the callee-saved register set.

### 6.1 `jmp_buf` layout on armv7 (Apple)

| offset | contents |
|---|---|
| `0x00`-`0x0C` | r4, r5, r6, r7 (`stmia r0!, {r4-r8,r10-r11,sp,lr}`) |
| `0x10`-`0x14` | r8, r10 |
| `0x18`-`0x1C` | r11 (`fp`), r13 (`sp`) |
| `0x20` | r14 (`lr`) |
| `0x24`-`0x63` | `d8-d15` (64 bytes, = `s16-s31`) via `vstmia r0, {d8-d15}` |
| `0x64` | unused (`FSTMX` slot, legacy) |
| `0x68` | `sigset_t` (`JMP_sig`) |
| `0x70` | `sigflag` — only in `sigjmp_buf` |

* **Saved:** r4-r8, r10, r11, sp, lr, d8-d15. **Not saved:** r0-r3, r9, r12, d0-d7, d16-d31, FPSCR
  (no `fmrx`/`fstmx`/`vstm` for d0-d7 anywhere in `arm/sys/`). r9 is skipped because it is caller-saved
  on iOS (§1.2), not because it is a "platform register"; r12 is IP; d0-d7/d16-d31 are volatile.
* Sizes: `#define _JBLEN (10+16+2)` → `jmp_buf` = `int[28]` = 112 bytes; `sigjmp_buf` = `int[29]` =
  116 bytes (`include/setjmp.h:52-66`). There is **no** `_JBLEN_ARM`/`_JBLEN_ARM64` macro in Apple's
  headers (those names are a common myth; arm64 uses `((14+8+2)*2)`).

### 6.2 Which entry point saves the signal mask

Mach-O names are the C names with one extra leading underscore, so the mapping is off by one and is
the classic source of "my handler's `longjmp` restored the wrong mask" bugs:

| Mach-O symbol | C name | behaviour on iOS 6 |
|---|---|---|
| `__setjmp` | `_setjmp` | registers only, returns 0 |
| `_setjmp` | `setjmp` | `sigprocmask(SIG_BLOCK, NULL, &env->JMP_sig)` then `__setjmp` — **saves the mask** |
| `_sigsetjmp` | `sigsetjmp` | stores `savemask` at 0x70; 0 ⇒ `__setjmp`, else `_setjmp` path |
| `__longjmp` | `_longjmp` | registers only; `val==0 → 1` |
| `_longjmp` | `longjmp` | `sigsetmask(env->JMP_sig)` then `__longjmp` — **restores the mask** |
| `_siglongjmp` | `siglongjmp` | 0 ⇒ `__longjmp`, else `_longjmp` |

So on Darwin `setjmp`/`longjmp` **do** save/restore the signal mask (BSD behaviour) and `_setjmp` /
`_longjmp` do not; `sigsetjmp(env,0)` ≡ `_setjmp`. POSIX leaves this unspecified, which is why other
Unixes differ. `longjmp`/`siglongjmp` are `__dead2` (`noreturn`), `setjmp`/`_setjmp`/`sigsetjmp`
carry `returns_twice`.

### 6.3 Semantics the JIT must not break

* `longjmp` transfers to the `setjmp` site; the value returned there is `val`, or 1 if `val == 0`.
  Non-`volatile` locals modified between `setjmp` and `longjmp` are indeterminate — a lifter that
  recompiles guest code must not "improve" the guest's stack slots into registers in a way that
  breaks the documented contract, and must not treat the `setjmp` call as a normal
  call-returns-once site (it is `returns_twice`: downstream code must be re-loaded from memory at the
  branch target).
* `setjmp` may only be called in normal (non-signal-handler) code; `longjmp` is async-signal-safe.
  The canonical iOS pattern is a handler installed with `sigaction(SA_ONSTACK)` +
  `sigaltstack`, whose body calls `siglongjmp`/`longjmp` out of a fault.

### 6.4 Why keeping guest state in host caller-saved registers across a guest call is unsafe

Concretely, for a lifter that maps guest r0-r12/s0-s31 onto host registers (or into a host-side
"guest register cache" that is only flushed lazily):

1. `longjmp` does not call the unwinder and does not return through any intermediate frame. It
   restores exactly the register set saved at `setjmp` time (r4-r8, r10, r11, sp, lr, d8-d15) and
   jumps. Every other register (r0-r3, r9, r12, d0-d7, d16-d31, FPSCR) and **any host-side state
   associated with the frames being skipped** is simply abandoned — including any pending writes of
   guest registers the lifter had batched in host registers but not yet stored to the guest stack.
   The guest's post-`longjmp` code then observes stale values that no guest-visible write produced.
2. Signal handlers run *on the interrupted thread* with the interrupted thread's stack replaced by
   the alternate signal stack, and they are the usual trigger for `longjmp`/`siglongjmp` in guest
   code. So the transfer can happen at any instruction boundary, including inside a lifter-generated
   helper where the lifter's own invariants (guest state only "logically" in host registers) hold —
   there is no safe point to rely on.
3. `longjmp` also restores the signal mask (`longjmp`) or not (`_longjmp`) depending on which symbol
   the guest called; a runtime that implements guest signals (or emulates them with host signals)
   must therefore implement both variants faithfully, including the fact that a guest `_setjmp`-made
   env carries a *garbage* mask word that `longjmp` (not `_longjmp`) will try to restore.
4. Even the happy path forces the rule: because the restore set is a fixed register set rather than a
   call-site ABI, `sp` and `lr` are restored from memory, so all live guest state that must survive
   the jump has to be in the guest's own stack frame at the time the signal/exception is taken.
   Practical implementation: flush guest register state to the guest stack/thread-struct (a)
   before any guest call that can transfer control non-locally (which is any call, since any
   instruction can fault and any handler may longjmp), and (b) at every host-callback boundary.
   Cheap version: keep the guest register file entirely in memory (thread descriptor) with
   load/store at basic-block boundaries, and treat guest r0-r12/s0-s31 as "always spilled".

## 7. `objc_msgSend` and the iOS 6 Objective-C runtime

### 7.1 Which runtime iOS 6 shipped (measured, not assumed)

`objc4-551.1` is often quoted for iOS 6, but the shipped armv7 code is the **objc4-532.x lineage**
(the shipped `libobjc.A.dylib` advertises compatibility version **228.0.0** [dsc, `LC_ID_DYLIB`], and
its behaviour below matches the 532.x source, not 551.1):

* `[dsc]` `_objc_msgSend` (Thumb, `0x38fc25a0`) indexes the cache as `buckets[(sel >> 2) & cache->mask]`
  with a 4-byte `Method *` bucket array and reads `method->name` at +0 and `method->imp` at +8 — this
  is `struct objc_cache { uintptr_t mask; uintptr_t occupied; Method *buckets[]; }` plus
  `METHOD_NAME 0 / METHOD_IMP 8` exactly as in `objc4-532.2 objc-msg-arm.s` (`.set MASK,0`,
  `.set OCCUPIED,4`, `.set BUCKETS,8`) [objc4-532.2 `runtime/Messengers.subproj/objc-msg-arm.s`].
  `objc4-551.1` replaced the cache with inline 8-byte buckets and `shiftmask` in the class
  (`ldrh r12,[r9,#12]`), which is *not* what the shipped code does.
* Statement-level markers also match 532.2: nil check is `teq.w r0,#0; beq`, not `cbz`; the miss path
  re-loads the class from the receiver (`ldr r2,[r0]`) in the uncached helper.
* Shipped exports relevant to a runtime: `_objc_msgSend` 0x38fc25a0, `_objc_msgSend_stret` 0x38fc2680,
  `_objc_msgSendSuper2` 0x38fc27c0, `_objc_msgSendSuper2_stret` 0x38fc2900, `_objc_retain` 0x38fc4520,
  `_objc_release` 0x38fc4f20, `_objc_autoreleaseReturnValue` 0x38fc5058,
  `_objc_retainAutoreleasedReturnValue` 0x38fd2b54, `_objc_storeStrong` 0x38fd2b70,
  `_objc_storeWeak` 0x38fd2660, `_objc_loadWeak` 0x38fd2738, `_objc_loadWeakRetained` 0x38fd26f4,
  `_objc_moveWeak` 0x38fd2780, `_objc_initWeak` 0x38fd2748, `_objc_destroyWeak` 0x38fd2750,
  `_objc_retainAutorelease` 0x38fd2b94, `_objc_retainBlock` 0x38fd2610, `_objc_autorelease`,
  `_objc_exception_throw` 0x38fc7960, `__objc_personality_v0` [dsc, `LC_SYMTAB` of
  `/usr/lib/libobjc.A.dylib`].
* **`_objc_msgSend_fpret` / `_objc_msgSend_fp2ret` do not exist on ARM**: they are `#if defined(__i386__)`
  / `#elif defined(__x86_64__)` only in `objc4-551.1 runtime/message.h:139-151` ("arm:
  objc_msgSend_fpret not used") and are absent from the shipped dylib's symbol table [dsc].
  A float/double-returning message on armv7 uses plain `objc_msgSend` and the result arrives in
  `r0`/`r0,r1` (§1.6) [clang, measured: `float` return → `bl _objc_msgSend` + `vmov s0,r0`;
  `double` return → `bl _objc_msgSend` + `vmov d16,r0,r1`].
* **No tagged pointers on iOS 6.** `SUPPORT_TAGGED_POINTERS` requires `__OBJC2__ && __LP64__`
  (`objc4-532.2 runtime/objc-config.h`), so 32-bit Apple platforms never had runtime tagged pointers;
  `isa` is a plain 32-bit `Class` pointer. Tagged pointers arrived with 64-bit (arm64 iOS 7++ /
  x86_64 macOS); 32-bit *MSB*-tagged pointers only ever existed on arm64/arm64_32
  (`OBJC_MSB_TAGGED_POINTERS`). A guest `isa` must therefore be dereferenced as a raw pointer, and a
  shim must not "helpfully" untag values.

### 7.2 Register/stack contract of each messenger (armv7)

From `objc4-532.2 runtime/Messengers.subproj/objc-msg-arm.s` (constants verified against the shipped
Thumb code in `[dsc]`):

```
.set RECEIVER, 0     /* objc_super */
.set CLASS,    4
.set ISA,       0    /* class */
.set SUPERCLASS,4
.set CACHE,     8
.set METHOD_NAME, 0  /* bucket (Method) */
.set METHOD_IMP,  8
.set MASK, 0         /* objc_cache */
.set NEGMASK, -8     /* mask is addressed as [buckets - 8] */
.set OCCUPIED, 4
.set BUCKETS, 8      /* &cache->buckets[0] */
```

| entry point | r0 | r1 | r2 | r3 | further args | nil receiver |
|---|---|---|---|---|---|---|
| `objc_msgSend` | `self` | `_cmd` | arg1 | arg2 | r4+ / stack, untouched | returns 0 (clears `r0` and `r1`) |
| `objc_msgSend_stret` | address of result struct | `self` | `_cmd` | arg1 | r1/r2/r3 are the ones the IMP will see as `self`/`_cmd`/arg1, so the IMP's view is shifted back by the runtime | returns without touching the buffer |
| `objc_msgSendSuper` | `struct objc_super *` | `_cmd` | arg1 | arg2 | — | n/a (no nil check) |
| `objc_msgSendSuper2` | `struct objc_super *` | `_cmd` | arg1 | arg2 | — | n/a |
| `objc_msgSendSuper_stret` / `objc_msgSendSuper2_stret` | address of result struct | `objc_super *` | `_cmd` | arg1 | — | n/a |

* The runtime must not disturb anything the caller put in `r2`/`r3`/stack (including float/double
  argument words, which on this ABI live in GPRs and on the stack, not in VFP registers) and must tail
  into the IMP (`bx ip`) with `lr` untouched: the shipped `objc_msgSend` uses `push {r3,r4}` and
  `pop {r3,r4}; bx ip`, so the IMP returns straight to the original caller and stack arguments stay
  where the caller put them [dsc disassembly; objc4-532.2 asm `CacheLookup` macro]. The shipped code
  touches only `r0,r1,r3,r4,r9,r12` and performs **no VFP access at all**, so `s0-s15`/`d0-d7`/
  `d8-d15`/`FPSCR` state (e.g. from a hard-float-calibrated IMP, §1.5) is forwarded verbatim.
* Debug/optimized variants shipped in the same file: `_objc_msgSend_debug`,
  `objc_msgSendSuper2_debug` (a branch to the non-debug entry), `_objc_msgSend_noarg`,
  `_cache_getImp`, `_cache_getMethod`; the address points tables `_objc_entryPoints`/`_objc_exitPoints`
  delimit the cache-lookup regions so the runtime can detect threads inside a lookup
  [objc4-532.2 `objc-msg-arm.s`].
* Scratch/clobber set: `r3`, `r4`, `r9`, `r12` (`Kills: a4, $1, r9, ip` — "a4" = r3, "$1" = the class
  register) [objc4-532.2 asm]. `r9` being clobberable is only valid because iOS does not make r9
  callee-saved (§1.2).
* Compiler-side messenger selection: `CGM.ReturnTypeUsesSRet(FI)` → `objc_msgSend[_Super2]_stret`,
  else plain messenger [clang `lib/CodeGen/CGObjCMac.cpp`, `GenerateMessageSend`]. Measured on
  `armv7-apple-ios6.0`: `- (id)`, `- (float)`, `- (double)` and `- (struct{int})` use `objc_msgSend`;
  `- (struct{float})`, `- (struct{float[4]})`, `- (struct{double,double})`, `- (struct{int[3]})` use
  `objc_msgSend_stret`. Note the 4-byte `struct{float}` goes **stret** — the "HFA returns in s0-s3"
  rule is the AAPCS-VFP rule and does not apply here (§1.1). `[super …]` compiles to
  `objc_msgSendSuper2[_stret]` with `r0` pointing at a static `__DATA,__objc_superrefs` entry
  [clang, measured].
* Binding: clang emits a *non-lazy* direct reference for `objc_msgSend` but a *lazy* stub reference
  for `objc_msgSend_stret` [clang `lib/CodeGen/CGObjCMac.cpp`, `GenerateMessageSend` list]. The
  lifter must therefore support both binding styles for these two symbols. `UNVERIFIED:` claims about
  a "lazily-registered `__objc_msgSend_stret` weak symbol": the shipped symbol is a strong definition
  (`n_type=0x0f`, `n_desc=0x0008`) [dsc].

### 7.3 `objc_super` / `objc_super2` and the class object

| struct | layout (32-bit) |
|---|---|
| `struct objc_super` | `{ id receiver; /* +0 */ Class super_class; /* +4 */ }` — 8 bytes; there is no cache field on ARM |
| semantics | `objc_msgSendSuper`'s `+4` field is the class to *search* (= the superclass); `objc_msgSendSuper2`'s `+4` field is the class in whose implementation the `[super …]` appears (the metaclass for `+` methods), and the runtime searches `class->superclass` [objc4-532.2 asm; `objc4-532.2 runtime/objc-abi.h`] |
| private `struct objc_super2` | `{ id receiver; Class current_class; }` [objc4-532.2 `runtime/objc-runtime-new.h:353-356`] |
| `class_t` (objc4-532.2, 32-bit) | `isa` +0, `superclass` +4, `cache` +8 (a 4-byte pointer), `vtable` +12, `data_NEVER_USE` +16 → **20 bytes**; `data()` masks the low 2 bits (`data_NEVER_USE & ~3`) |
| `class_ro_t` (32-bit) | `flags` +0, `instanceStart` +4, `instanceSize` +8, `ivarLayout` +12, `name` +16, `baseMethods` +20, `baseProtocols` +24, `ivars` +28, `weakIvarLayout` +32, `baseProperties` +36 → **40 bytes (0x28)** (`__LP64__` adds `reserved` at +12) |
| `class_rw_t` | `flags` +0, `version` +4, `ro` +8, `method_lists`/`method_list` +12, `properties` +16, `protocols` +20, `firstSubclass` +24, `nextSiblingClass` +28 |
| munged `data` field | On iOS 6 the low 2 bits are RW-data flags only (`CLASS_FAST_FLAGS_VIA_RW_DATA` etc. are false on 32-bit; `RW_HAS_CUSTOM_RR`/`RW_HAS_CUSTOM_AWZ` live in `class_rw_t.flags`). The name `FAST_DATA_MASK` (0xfffffffc) belongs to much later runtimes (objc4-646+) — do not cite it for iOS 6. |

All of the above are the `objc4-532.2` definitions with the exact field order shown; the shipped
`objc_msgSend` confirms `CACHE = 8`, `SUPERCLASS = 4`, `ISA = 0`, `METHOD_NAME = 0`, `METHOD_IMP = 8`,
`MASK = 0`, `BUCKETS = 8` [dsc].

### 7.4 ARM/Thumb interworking

* Function pointers and IMPs carry the Thumb state in bit 0: `1` = Thumb. Every function in the
  shipped armv7 `libobjc.A.dylib` is Thumb (`n_desc = 0x0008`, `N_ARM_THUMB_DEF`), and
  `__objc_empty_vtable` holds `0x38fc25a1` = `objc_msgSend | 1` [dsc] — a cast/marshalling of an
  IMP to a function pointer must preserve bit 0.
* Calls to IMPs must be indirect branches that do not discard the state bit: `bx Rd` (tail) or
  `blx Rd` (call). AAPCS32: "function-pointer calls `blx Rm`", "tail calls `bx Rm`", and "`mov pc, Rm`
  must not be used if inter-working might be required" [AAPCS32:1464-1495]. The runtime asm uses
  `bx ip`/`blx r12` [objc4-532.2 asm].
* A lifter must therefore model the *state bit of every code pointer*, and when the guest branches to
  an address whose bit 0 is set it must switch to Thumb decoding at `addr & ~1`. Conversely, a
  recompiler that emits host code for a Thumb function must keep the guest's pointer value intact so
  that guest code doing `ptr & ~1` / `ptr | 1` arithmetic still works.

## 8. Mach/BSD threading and synchronization relevant to a Linux shim

Symbol presence below is measured from `[dsc]` (which image defines what), because "which library"
matters for shim completeness. Shipped versions of the relevant libraries in iOS 6.1.3 (10B329,
iPad2,1): `libsystem_kernel` 2107.7.55, `libsystem_c` 849.29.0, `libdispatch` 244.33.0,
`libSystem.B.dylib` 173.8.0; `libsystem_platform.dylib` and `libsystem_pthread.dylib` do not exist;
`/usr/lib/libpthread.dylib` is an alias image of `libSystem.B.dylib`.

### 8.1 `pthread_t` and TSD

* `pthread_t` is an opaque pointer to the runtime's `_pthread` structure; `pthread_self()` is
  `TP→[0]` (a `mrc`-based read, §4.4) [Libc-763.13 `arm/pthreads/pthread_self.s`]. Guest code may and
  does index the thread pointer directly (`_pthread_getspecific_direct`), so a shim cannot substitute
  a glibc `pthread_t`. `pthread_equal` compares handles; `pthread_t` values are never recycled while
  the thread object lives.
* `pthread_key_create`/`_delete`, `pthread_setspecific`/`getspecific`, destructors and
  `PTHREAD_DESTRUCTOR_ITERATIONS` (4) live in `libsystem_c.dylib` (`[dsc]`: definer of
  `_pthread_key_create` etc.). Semantics that differ from glibc and bite: Darwin's
  `pthread_setspecific` returns the error code (0 or `EINVAL`/`ENOMEM`), never -1; keys are opaque
  small integers whose values are *not* the TSD slot index (the TSD array uses fixed reserved slots
  plus a key table, §4.4).
* `pthread_exit` exists (`libsystem_c.dylib`) and must run TSD destructors; it is not a `longjmp`, so
  a thread that calls it from inside a guest frame must unwind/return through the runtime, not by
  abandoning host frames that hold locks.
* `pthread_cancel` does exist in Darwin's pthread (it is not POSIX-deferred), but iOS 6's libobjc and
  libSystem do not use it; `UNVERIFIED:` its exact iOS 6 restrictions.

### 8.2 Spinlocks and gates

| API | iOS 6 status | implementation basis |
|---|---|---|
| `OSSpinLockLock`/`Try`/`Unlock` | present, defined in `libsystem_c.dylib` (`[dsc]`; also referenced by `libobjc.A.dylib`); `OS_SPINLOCK_INIT` = 0, `OSSpinLock` is a 32-bit word, locked value **1** on embedded targets (`_OSSpinLockLocked = TARGET_OS_EMBEDDED ? 1 : -1`) | cmpxchg spin (`OSAtomicCompareAndSwap32Barrier` → `ldrex`/`strex` + `dmb` on armv7), then a yield path: `_OSSpinLockLockYield` → `thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, timeout)` and after N tries `SWITCH_OPTION_WAIT` [libplatform-126.1.2 `src/os/lock.c`]. It is **not** a `pthread_mutex`, has no ownership tracking and no priority inheritance — exactly the defect `os_unfair_lock` was later created to fix. Internal aliases `_spin_lock`/`spin_lock` |
| `os_unfair_lock*` | **absent on iOS 6** (`[dsc]`: `_os_unfair_lock_lock` not exported by any of the 569 images) | introduced with macOS 10.12 / iOS 10 |
| `os_once`/`os_once_t` | **absent on iOS 6** (`[dsc]`: `_os_once` not exported) | `os/once.h`'s `os_once` is a later libplatform API (macOS 10.12+ / iOS 10+) |
| `pthread_once` | present (`libsystem_c.dylib`) | ordinary once-gate |
| `dispatch_once` / `dispatch_once_f` | present, defined in `libdispatch.dylib` (shipped libdispatch 244.33.0, `[dsc]`) | `typedef long dispatch_once_t`; done sentinel is `~0l` (`DISPATCH_ONCE_DONE`); the fast path is an inline load/compare, the slow path `cmpxchg`s `NULL` into the predicate, queues waiters and uses per-thread Mach semaphores (`semaphore_create(mach_task_self(), &s, SYNC_POLICY_FIFO, 0)`) [libdispatch-228.18 `src/once.c`, `src/semaphore.c:555-640`]. The `DLOCK_ONCE_*`/`dispatch_once_gate_t` design is a **later** libdispatch (≈339+) and is not iOS 6. Predicate must be static/global, never stack memory |

Practical shim note: libobjc's autorelease pools and `+initialize` use `OSSpinLock`/dispatch gates, so
a correct `OSSpinLock` (including the blocking fallback) is on the critical path for `objc_msgSend`.

### 8.3 Mach time and semaphores

| API | iOS 6 status / semantics |
|---|---|
| `mach_absolute_time()` | defined in `libsystem_c.dylib` (`[dsc]`); shipped as a **trap stub**: `mvn ip,#2; svc #0x80; bx lr` (Mach trap 2) at 0x3940e240; the whole cache contains zero `CNTVCT`/`CNTPCT`/`CNTFRQ` reads, so A5 does not expose a user-readable counter | monotonic, **excludes** time asleep (the asleep accumulator is `mach_absolutetime_asleep`); convert with `mach_timebase_info(&numer,&denom)` (also a trap): ns = ticks × numer / denom, timebase is per-device (never assume 1:1); `mach_continuous_time` (includes sleep) is later — `UNVERIFIED:` exact introduction |
| `mach_wait_until(deadline)` | present; Mach trap **90** (`mvn ip,#0x59`) | absolute deadline in `mach_absolute_time` units; kernel `assert_wait_deadline`; a shim must map to the host clock and preserve the "does not count suspend" property if guest-visible monotonicity matters |
| `semaphore_create(task, &sem, policy, value)` | present (`libsystem_kernel.dylib`) — `sem` is a **Mach port name** (port object of type `IKOT_SEMAPHORE`), not a pointer; rejects `value < 0` and `policy > SYNC_POLICY_MAX` (7) | policies: `SYNC_POLICY_FIFO 0x0`, `FIXED_PRIORITY 0x1`, `REVERSED 0x2`, `LIFO 0x3`, `PREPOST 0x4` (kernel-private) [xnu `osfmk/mach/sync_policy.h`] |
| `semaphore_wait` / `_wait_signal` / `_signal` / `_signal_all` / `_signal_thread` / `_timedwait` / `_timedwait_signal` | present: Mach traps **33-39** (`_semaphore_wait_trap` = `mvn ip,#0x23` = 36 in the shipped stub) | `semaphore_timedwait` takes *relative* seconds/nanoseconds (not a deadline); `{0,0}` means non-blocking; a shim must therefore emulate Mach port names and `KERN_*` return codes, not just `sem_t` |
| `thread_switch(thread_name, option, time_out)` | present; Mach trap **61** (`mvn ip,#0x3c`); `thread_swtch`/`swtch_pri` are traps 60/59 | `option` ∈ `SWITCH_OPTION_NONE 0`, `DEPRESS 1`, `WAIT 2` [xnu `osfmk/mach/thread_switch.h`]; used by `OSSpinLock`'s yield path and by old YIELD code |
| `__psynch_*` | present as **`___psynch_mutexwait` / `___psynch_cvwait`** in `libsystem_c.dylib` (`[dsc]`; Mach-O name = `_` + C `__psynch_mutexwait`). XNU syscalls **297-312** (`bsd/kern/syscalls.master`), also `__semwait_signal` 334 and its nocancel variant 423 | shipped pthread mutex/condvar use them: `__psynch_mutexwait(mutex,mgen,ugen,tid,flags)`, `__psynch_mutexdrop`, `__psynch_cvbroad/cvsignal/cvwait/cvclrprepost`, `__psynch_rw_*` [Libc-825.40.1 `pthreads/pthread_mutex.c:719,961`], plus a commpage fast path `_commpage_pthread_mutex_lock` |
| `__ulock_*` | **absent on iOS 6** (`[dsc]`: `___ulock_wait` not exported) — `ulock` arrived with XNU 3xxx / iOS 11 | |
| `libsystem_platform.dylib` | does **not exist** on iOS 6 (`[dsc]`: no such image); `_platform_memmove`/`_platform_strlen` and `os_unfair_lock` therefore have no iOS 6 analogue. `libsystem_pthread.dylib` is also absent — `/usr/lib/libpthread.dylib` is an alias image of `libSystem.B.dylib` and the pthread code lives in `libsystem_c.dylib` | |

### 8.4 What a Linux shim must provide (minimum)

`pthread_self/equal/key_create/key_delete/getspecific/setspecific/exit/join/detach/once/mutex_*/cond_*/rwlock_*/sigmask/kill`,
`pthread_key` destructors run to iteration 4, a guest-visible thread pointer block (§4.4),
`OSSpinLock*` on top of host atomics with a blocking fallback, `dispatch_once*` with the libdispatch
gate semantics, `mach_absolute_time`+`mach_timebase_info`, `semaphore_create/wait/signal` with Mach
port names, `thread_switch`, `mach_wait_until`, and the `__psynch_*` names for guest code that
resolves them. Mach ports, `mach_msg`, and exception ports are discussed in §9.

### 8.5 Thread lifecycle details that differ from glibc

* Returning from the start routine of a pthread *is* `pthread_exit(retval)`
  (`_pthread_exit(self, (self->fun)(self->arg))`) [Libc-825.40.1 `pthreads/pthread.c:891,954`];
  `_pthread_exit` runs `__cleanup_stack`, `_pthread_tsd_cleanup`, then signals the joiner's death
  semaphore, otherwise terminates via `__bsdthread_terminate` and returns the port to a recycle list
  (`thread_recycle_port`, `_pthread_reap_threads`, `_pthread_become_available`) — i.e. **handles are
  recycled**, so a shim cannot use a guest `pthread_t` as a permanent identity key without also
  participating in that recycle/join protocol.
* `pthread_cancel` exists and is deferred by default (sets `_PTHREAD_CANCEL_PENDING`,
  `__pthread_markcancel(kernel_thread)`; returns `ESRCH` for unknown threads, `ENOTSUP` for workqueue
  threads); cancelable cancellation points live in the `*_nocancel`-wrapped variants
  (`pthread_cancelable.c`, `__semwait_signal_nocancel`) [Libc-825.40.1]. `pthread_kill`,
  `pthread_sigmask` (wrapper over `__pthread_sigmask`) are present.
* `UNVERIFIED:` the often-repeated claim that Darwin's TSD destructor frames must be able to
  `longjmp`: no such guard exists in `Libc-825.40.1` (the only `setjmp` reference is
  workqueue-specific, `pthread.c:3165`).

## 9. Crash and exception path

### 9.1 Mach exception ports (what the runtime must implement to not hang)

| item | iOS 6 fact |
|---|---|
| API | `task_set_exception_ports(task, exception_mask, new_port, behavior, flavor)` and `thread_set_exception_ports(...)` [xnu `osfmk/kern/ipc_tt.c`; nearest published tree xnu-2050.x, iOS 6 = xnu-2107 (unpublished)]. Both reject `exception_mask & ~EXC_MASK_VALID` with `KERN_INVALID_ARGUMENT` |
| masks that exist | `EXC_BAD_ACCESS` 0x2, `EXC_BAD_INSTRUCTION` 0x4, `EXC_ARITHMETIC` 0x8, `EXC_SOFTWARE` 0x20, `EXC_BREAKPOINT` 0x40, `EXC_CRASH` 0x400, `EXC_RESOURCE` 0x800 [xnu `osfmk/mach/exception_types.h`] |
| masks that do **not** exist | `EXC_GUARD` (1<<12, macOS 10.9/xnu-2422+) and `EXC_CORPSE_NOTIFY` (13, xnu-3248+) — absent in the iOS 6-era headers, so shim code must not accept them |
| behavior/flavors | `EXCEPTION_DEFAULT` (1), `EXCEPTION_STATE` (2), `EXCEPTION_STATE_IDENTITY` (3); flavor `THREAD_STATE`/`ARM_THREAD_STATE`; the `MACH_EXCEPTION_CODES` bit (`0x80000000`, in the private `mach_exc.h`, not in the public SDK headers on iOS 6) selects 64-bit `code`/`subcode` |
| triage order | `exception_triage` → thread port → task port → host port → `task_terminate`. **A Mach handler that replies `KERN_SUCCESS` means the thread is resumed and no BSD signal (`SIGSEGV`/`SIGBUS`) is generated at all**; the reverse bridge (kernel signal → Mach) is `bsd/kern/kern_sig.c: do_bsdexception` → `bsd_exception()` (task port only) |
| `EXC_BAD_ACCESS` payload | `code` = `KERN_INVALID_ADDRESS` (1) or `KERN_PROTECTION_FAILURE` (2), plus ARM-specific `EXC_ARM_DA_ALIGN` = 0x101 and `EXC_ARM_DA_DEBUG` = 0x102 [xnu `osfmk/mach/arm/exception.h`]; `subcode` = faulting virtual address |
| per-thread state flavors (`thread_get_state`/`thread_set_state`) | `ARM_THREAD_STATE` = 1 (`r[13]`, `sp`, `lr`, `pc`, `cpsr` = 17 words), `ARM_VFP_STATE` = 2 (`r[64]` + `fpscr`, covers `d0-d31`), `ARM_EXCEPTION_STATE` = 3 (`__exception`, `__fsr`, `__far`), `ARM_DEBUG_STATE` = 4 [cctools `include/mach/arm/thread_status.h`] |

### 9.2 How third-party crash reporters actually work on iOS 6

All of the common ones (PLCrashReporter, KSCrash; also Apple's own in-process `NSExceptionHandler`
era code) run the same three-layer scheme:

1. **Mach exception handler first**: they call `task_set_exception_ports(mach_task_self(), mask,
   server_port, EXCEPTION_DEFAULT|EXCEPTION_STATE(|_IDENTITY), THREAD_STATE)` and run a `mach_msg`
   server loop on a dedicated thread, usually with two or three receive contexts (the message can
   arrive on the exception port or on a reply port). Masks registered are typically
   `EXC_BAD_ACCESS|EXC_BAD_INSTRUCTION|EXC_ARITHMETIC|EXC_SOFTWARE|EXC_BREAKPOINT`
   [KSCrash `KSCrashMonitor_MachException.c`]. The handler replies `KERN_SUCCESS` (killing the
   process after recording) or `KERN_FAILURE` (to chain to the previous/saved ports) and restores
   the previous exception ports. PLCrashReporter's server replies by `msgh_id`,
   `EXCEPTION_DEFAULT(|MACH_EXCEPTION_CODES)` with an explicit note that `sigaltstack()` "is broken in
   later iOS releases" [PLCrashReporter `PLCrashMachExceptionServer.m`].
2. **BSD signal fallback**: `sigaltstack()` + `sigaction(SA_SIGINFO|SA_ONSTACK)` for
   `SIGSEGV`/`SIGBUS`/`SIGABRT`/`SIGILL`/`SIGFPE`/`SIGTRAP` [KSCrash `KSCrashMonitor_Signal.c:179-197`].
   This is the layer that fires if the Mach layer is not installed or forwards.
3. **ObjC uncaught exception handler**: `NSSetUncaughtExceptionHandler`
   (`NSUncaughtExceptionHandler`); it does not chain — only one handler exists process-wide
   [PLCrashReporter `PLCrashReporter.m:558-562`].

Backtrace collection is a **frame-pointer walk** on armv7: each thread's register state is fetched
with `thread_get_state` (after `task_threads` + `thread_suspend`), then
`FrameEntry{uintptr_t previous; uintptr_t return_address;}` records are followed down the `r7` chain
with monotonic/stack-bounds validation [PLCrashReporter `PLCrashAsyncThread_arm.c:231-307`,
`PLCrashFrameStackUnwind.c`; KSCrash `KSStackCursor_MachineContext.c`]. Both libraries *also* contain
a compact-unwind/`__eh_frame` unwinder (they run on macOS/arm64 too), but on iOS 6 armv7 that path has
no data to read (§5.1) and every real backtrace comes from r7 — which is exactly why Apple mandates
r7 as the frame pointer.

### 9.3 Minimum runtime surface so crash-reporter paths do not hang

Provide, with real semantics (not stubs that return success):
`task_threads`, `thread_get_state`, `thread_set_state`, `thread_suspend`, `thread_resume`,
`task_get_exception_ports`/`task_set_exception_ports`, `thread_get_exception_ports`/
`thread_set_exception_ports`, `mach_port_allocate`/`mach_port_insert_right`/`mach_port_deallocate`,
`mach_msg`/`mach_msg_trap` (receive with timeout, `msgh_id` dispatch, correct complex/make-send
rights), `mach_vm_read_overwrite`/`vm_read`/`vm_write`, `mach_task_self`,
`vm_region`/`mach_vm_region`, `sigaltstack`, `sigaction`, `pthread_create` (from §8),
`mach_absolute_time`/`mach_wait_until` (from §8.3, timeouts are used everywhere), and
`_dyld_find_unwind_sections` (§5.5).

Failure modes to design against, all of which appear as *hangs* rather than clean errors:

* A `mach_msg` receive that never returns because the runtime swallowed the exception event instead
  of delivering a real message to the registered port. Crash reporters block forever on that thread.
* A handler that replies `KERN_SUCCESS` but whose `thread_set_state` cannot restore a coherent
  context (missing `ARM_VFP_STATE`, wrong `cpsr`, or a `pc` without the Thumb bit) — the thread
  resumes into garbage instead of the reporter's recovery path.
* Guests that expect `thread_get_state` for *other* threads to work while those threads are
  suspended: this only works if the runtime keeps a per-guest-thread saved register set, i.e.
  lifter state must be synchronizable at arbitrary points (§6.4).
* Missing `sigaltstack` support: the fault handler then runs on the faulting stack (often overflowed),
  and the reporter's trampoline dies instead of reporting.

## 10. The ten ABI details most likely to be gotten wrong first

1. **Floating-point argument passing.** iOS 6 ARMv7 is **base-standard/softfp**: `float`/`double`
   arrive in `r0-r3`/stack, *not* in `s0-s15`/`d0-d7`; `long double` is `double` (8 bytes, 4-aligned)
   [apple-av6; clang measured; AAPCS32:1265-1290]. Building the FFI on AAPCS-VFP (the Linux
   hard-float model) produces code that works on synthetic tests and dies on real guests.
2. **`r9` is volatile scratch on iOS, not a platform register and not callee-saved** [apple-av6];
   assuming otherwise lets guest callees clobber a value you cached there. `r12` (IP) likewise.
3. **`r7` is the frame pointer, `r11` is not** — and on iOS 6 armv7 there is **no `__unwind_info` and
   no `__eh_frame`** anywhere in the system (0/569 images), so an r7 frame-pointer walk is the only
   unwinding mechanism that exists [dsc; clang `Darwin.cpp` SjLj/`None` unwind tables].
4. **Stack alignment is 4, not 8** [apple-av6]; nothing in the ABI has 8-byte alignment,
   so a 2-word argument can start in an odd register (`r3`) and be *split* across `r3` and the stack
   [clang measured]. `SP % 8 == 0` on entry is safe to assume but not required — never *require* it.
5. **Composite classification**: ≤4-byte *integer-like* composites return in `r0`; every other
   composite returns via `sret` in `r0` (including the 4-byte `struct{float}`); composites up to
   64 bytes are passed as N × i32 in registers + stack, larger ones by value on the stack
   [clang measured; clang `lib/CodeGen/Targets/ARM.cpp` APCS classifier].
6. **`objc_msgSend` details**: `objc_msgSend_stret` takes the destination in `r0` and `self`/`_cmd`
   in `r1`/`r2` (so the IMP sees `self` in `r0`); `objc_msgSend_fpret` does **not** exist on ARM;
   nil-receiver returns 0 (and 0/0 for `stret`-shaped returns) with a *tail* `bx` that keeps stack
   arguments in place; IMP pointers carry the Thumb bit in bit 0 and must be reached with
   `bx`/`blx`; `objc_msgSendSuper2`'s class field is the *current* class (the runtime uses its
   `superclass`); the class cache is `{mask@0, occupied@4, buckets@8}` with `Method{name@0, imp@8}`
   and index `(sel>>2) & mask` for the shipped objc4-532.x runtime [dsc; objc4-532.2 asm].
7. **Mach-O symbol naming is off by one.** `___divsi3` = C `__divsi3`; Mach-O `_setjmp` is C
   `setjmp` (the mask-saving one!) and Mach-O `__setjmp` is C `_setjmp`; `__aeabi_*` names do not
   exist here at all [dsc; clang measured; Libc `arm/sys/setjmp.s`].
8. **TLS.** `__thread` was rejected by the compiler on iOS 6 (32-bit device support starts at iOS 9),
   and no shipped image has a TLS section; per-thread data is `pthread_getspecific`, which is
   `((void**)(TPIDRURO & ~3))[key]` with reserved slots at 0-9 and dynamic keys from 256
   [clang `OSTargets.h`; dsc; Libc-825.40.1].
9. **Exception handling is SjLj, not DWARF.** `__cxa_throw` ends in `_Unwind_SjLj_RaiseException`;
   the API is `_Unwind_SjLj_Register/Unregister/Resume` (libunwind.dylib) + `__gxx_personality_sj0`
   (libcompiler_rt) / `__objc_personality_v0` (libobjc); the LSDA lives in `__TEXT,__gcc_except_tab`;
   `_Unwind_RaiseException`, `_Unwind_Resume` and `__gcc_personality_v0` are not exported by any
   iOS 6 image [dsc; libc++abi `cxa_exception.cpp:277-279`; libunwind-35.1 `Unwind-sjlj.c`].
10. **`setjmp`/`longjmp` register set and mask semantics.** Saved: r4-r8, r10, r11, sp, lr, d8-d15
    (offsets 0x00-0x63 in `jmp_buf`), plus a signal mask word at 0x68 that `setjmp`/`longjmp` (but
    not `_setjmp`/`_longjmp`) touch; *not* saved: r9, r12, d0-d7, d16-d31, FPSCR. Because a signal
    handler can `longjmp` at any instruction and the restore set is fixed, **all live guest state
    must be in the guest's own stack/thread block before any guest call** — never only in host
    registers or a lazily-flushed register cache [Libc-825.40.1 `arm/sys/_setjmp.h`, `_setjmp.s`,
    `_longjmp.s`; POSIX `longjmp`/`sigaltstack`].
