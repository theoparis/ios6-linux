# 12 — Testing

Testing strategy for ios6-recomp. Every test here is deterministic, isolated, and runnable without
Apple binaries ([ADR-12](02-decisions.md)); real-cache results are recorded as hashes and never
committed. The translator and JIT are tested standalone, per the dependency rule in
`01-architecture.md` §Components (`lift`/`jit`/`aot` do not depend on `kernel`/`builtins`).

## 1. Test pyramid

| Layer | Location | What it checks | Gates |
|---|---|---|---|
| Unit | `tests/unit/` | Component invariants: Mach-O parser, arena, block table, GTC layout, semantic-table row completeness | M0–M1 |
| ABI/layout | `tests/abi/` | Every guest-visible struct's size/offset under ARMv7 rules (ADR-03) | M1 |
| Decode-differential | `tests/decode/` | LLVM MC ↔ Capstone on `__TEXT` of real cache images (ADR-07) | M2 |
| Per-instruction differential | `tests/fuzz/insn/` | Random instruction + state → tier 0 vs tier 1 vs oracle | M2, M3 |
| Program-level differential | `tests/fixtures/` | Fixture stdout/exit under tier 0, tier 1, and `qemu-arm` ELF twin | M2, M3 |
| Syscall-trace replay | `tests/trace/` | Recorded `IOS6RECOMP_SYSCALL_TRACE` replays byte-identically | M4+ |
| Real-cache fixture corpus | `tests/cache/` (hashes only) | Stub/missing-opcode burn-down on real images | M4, M6 |
| Benchmarks | `tests/bench/` | CoreMark/Dhrystone/GLES three-number table | M3, M7 |
| Sanitizers | CI matrix | ASan/UBSan/TSan builds | M0+ |
| Fuzzing | `tests/fuzz/` | libFuzzer targets for parsers/decoder | M2+ |

**CI budget.** M0 exit requires the whole unit + ABI suite green in **< 10 minutes** on one job.
Per-instruction and decode-differential suites run nightly (they are large); program-level fixtures
run on every PR.

## 2. Fixture toolchain

Verified on this host (clang 23.1.3, no Apple SDK present).

### 2.1 Mach-O (`armv7`/`armv7s`)

No Apple SDK/sysroot exists on this machine, and the installed `ld64.lld` (LLD 23.1.3) **does
not support 32-bit ARM Mach-O**: `ld64.lld -arch armv7` → `missing or unsupported -arch armv7`,
same for `armv7s`/`i386`. (`ld64.lld -arch arm64` works.) Therefore on this host we can compile
to Mach-O objects but not link a Mach-O executable:

```
clang -target armv7-apple-ios6.0  -mcpu=cortex-a9 -mfloat-abi=soft -nostdlib -c foo.c -o foo.o
clang -target armv7s-apple-ios6.0 -mcpu=swift       -mfloat-abi=soft -nostdlib -c foo.c -o foo.o
```

produces a 32-bit Mach-O relocatable (`file`: `Mach-O … arm`). Linking requires an Apple
toolchain (Xcode `ld64`) or a user-supplied SDK; the plan's M1 "fixtures built with
`-fuse-ld=lld`" must be revised to either (a) ship pre-linked fixtures from a macOS build host,
or (b) keep fixtures as objects + a tiny in-tree Mach-O linker. Full .tbd generation from the
cache's export trie is a ~15-line Python script reading the trie; deferred to M1.

**Soft-float constraint.** `clang -target armv7-apple-ios6.0` already selects `-mfloat-abi soft`
and `apcs-gnu` (research/apple-armv7-abi.md §1.1). Never pass `-mfloat-abi=hard`; AAPCS-VFP does
not apply to iOS 6.

### 2.2 ELF twin (oracle input)

```
clang -target armv7-linux-gnueabihf -mfloat-abi=soft -c foo.c -o foo.elf.o
ld.lld -m armelf_linux_eabi -o foo.elf foo.elf.o
qemu-arm ./foo.elf
```

The ELF twin **must** use `-mfloat-abi=soft` (or `softfp`) to match iOS arg passing in r0–r3/stack
(research/apple-armv7-abi.md §1.1, §1.5). Tests that depend on the iOS-only rules cannot be
byte-compared across ABIs: struct returns where `struct{float}` is sret on iOS but an HFA in
r0–r3 under AAPCS-VFP; `long double` (= `double`, 4-aligned) layout; 4-byte stack alignment;
r9 volatility; r7 frame pointer. Those are covered by the Mach-O/interpreter differential instead.

## 3. Per-instruction differential harness

Design per research/llvm-tooling.md §7 and apple-armv7-abi.md.

**Oracle choice.** `qemu-arm -one-insn-per-tb -d in_asm,cpu,nochain` (verified parseable, QEMU 11.1.2).
Unicorn and dynarmic are rejected: Unicorn's ARMv7 model defaults to AAPCS-VFP-style VFP and lacks
the iOS-specific `apcs-gnu` soft-float (irrelevant for instruction execution but its memory/flag
semantics need re-validation), and dynarmic is AArch64/ARMv8-oriented. QEMU is already in the
toolchain, is the same oracle used for program-level differential, and its text dump is stable.

**State-dump format.** One block per instruction:

```
----------------
IN: <disasm>
<addr>:  <bytes>
R00=... R01=... ... R15=...
PSR=xxxxxxxx <flags> <mode>
```

Parser extracts R00–R15, PSR (NZCVQ + mode), and (when `-d fpu`) D0–D31/FPSCR. Memory is compared
via a separate sandboxed 4 KiB region whose pre/post bytes are hashed.

**Generator constraint rules.**
- Allow-list built from LLVM MC + Capstone agreement (research/llvm-tooling.md §1.7: 4531 ARM
  opcodes in `MCInstrInfo`): an encoding is eligible only if both decoders agree on mnemonic and
  operand set (§4). UNPREDICTABLE/CONSTRAINED UNPREDICTABLE encodings (e.g. `str r0,[r1],#4` with
  r0==r1, `ldm` base in list) are excluded by construction: the generator never emits overlapping
  base/dest or base-in-list forms unless that exact defined case is under test.
- IT-block generation: emit `IT` + up to 4 predicated instructions as one atomic unit; condition
  codes drawn from the 16 codes; the `BL`-inside-IT case is included.
- Memory operands sandboxed to a fixed 4 KiB region with known initial bytes; exclusive monitors
  (LDREX/STREX) modelled per-thread.
- Coverage accounting: `lift.missing`/stats report implemented opcodes vs the 4531-opcode table;
  M2 exit requires 100 % register/flag/memory agreement on all implemented opcodes over ≥ 10⁷
  random streams.

**Prototype (verified).** Command:

```
qemu-arm -one-insn-per-tb -d in_asm,cpu,nochain ./five.elf
```

produces the block format above (excerpt):

```
----------------
IN: 
0x000200b4:  21072005  mrshs    r2, apsr
R00=00000000 R01=00000000 R02=00000000 ... R15=000200b4
PSR=00000010 ---- A usr32
```

Parser regex: `^IN:\s*$` … `^R(\d{2})=([0-9a-f]{8})` … `^PSR=([0-9a-f]{8})`. Thumb entries require
the Thumb bit in R15/PSR; the generator marks Thumb blocks via bit 0.

## 4. Decode-differential (ADR-07)

Procedure per research/llvm-tooling.md §1.7 and ios6-platform.md §4/§5.

1. For each real cache image (≥ 50 for M2), walk `LC_FUNCTION_STARTS` + exports; for each function
   range, decode with LLVM MC (`armv7s-apple-ios` superset subtarget, CPU `swift`, features
   `+vfp4,+fp16,+hwdiv,+hwdiv-arm` so armv7s code is accepted) and Capstone (mode ARM/Thumb).
2. Compare mnemonic + operand set (registers, immediates, memory base/offset/writeback, condition
   code, IT block). Disagreements recorded in `tests/decode/allowlist.txt` with `(image, va, bytes,
   llvm, capstone, reason)`.
3. Corpus = `__TEXT` of all 569 cache images (ios6-platform.md §5). Metrics: decode errors (must be
   0) and disagreement rate (M2: < 0.01 %). Measured baseline 1293/1500 agreement; Thumb halfword
   resync artefacts in Capstone are a known caveat and must not be auto-classified as LLVM bugs.

Allow-list format (TSV):

```
# image va mode bytes llvm_mnemonic llvm_operands capstone_mnemonic capstone_operands reason
libobjc.A.dylib 0x39... thumb 0x... mov r0,r1 mov r0, r1 capstone-thumb-resync UNVERIFIED
```

## 5. Program-level testing

- **Syscall-trace record/replay.** `IOS6RECOMP_SYSCALL_TRACE=path` writes a replayable trace of
  BSD syscalls, Mach traps, and `mach_msg` sends/receives (opcode, args, return, errno). Replay
  mode feeds recorded returns so a run is deterministic. Format: newline-delimited JSON, one record
  per trap, with `seq`, `kind`, `nr`, `args[]`, `ret`, `errno`, `thread`.
- **Golden outputs.** Fixtures commit expected stdout + exit status; `ctest` compares byte-for-byte.
- **ObjC/CF fixture list (M4).** `NSMutableArray` add/enumerate, `NSString` formatting (`%@`),
  category method, subclass with `super`, `NSInvocation`, `objc_msgSend_stret`, ARC retain/release,
  autorelease pool, `NSDictionary` JSON round-trip via `NSJSONSerialization`, `CFRunLoop` timer.
- **Determinism rules.** Time uses a virtual clock driven by the trace (no `CLOCK_REALTIME` in
  tests); ASLR disabled (fixed arena, ADR-02); thread scheduling made deterministic by running
  single-threaded fixtures under a fixed seed; `mach_absolute_time` fast path (r12 == -3) returns
  a monotonic counter in tests.

## 6. Real-app policy (ADR-12)

- The corpus is user-supplied and never committed; `.gitignore` excludes `work/`, `*.ipsw`,
  `*.dsc`, `artifacts/`.
- Compat results recorded as hashes (app name → SHA-256 of the binary) in `docs/compat.md`,
  generated, never the binary.
- Stub/missing-opcode burn-down reports are test artefacts: `IOS6RECOMP_LIFT_STATS=1` output
  captured per app and stored under `artifacts/burndown/<hash>.json`. M6 exit: zero unknown-opcode
  hits across 5 apps of different genres.

## 7. Performance testing

- **Benchmark set.** CoreMark and Dhrystone built as fixtures (both Mach-O and ELF twin), plus a
  GLES fixture (M5). Measured on pinned CPU, warm and cold runs, 3 trials.
- **Three-number table (M3).** native x86-64 / `qemu-arm` / ours, recorded per benchmark.
- **M7 exit.** ≥ 3× `qemu-arm` and ≥ 30 % of native x86-64 on at least one workload; cold start
  (first 200 ms) ≤ 2× warm. Regression threshold: any commit that drops ours below 0.9× the
  recorded baseline fails CI.

## 8. Sanitizers & fuzzing

- **libFuzzer targets.** `fuzz_macho` (Mach-O header/load commands), `fuzz_cache` (dyld cache
  header/mappings/image array), `fuzz_bind` (bind/rebase opcodes), `fuzz_decode` (decoder, must
  never crash on arbitrary bytes).
- **TSan.** Guest-code/jit concurrency (`tests/guestcode/` property + concurrency tests, 05-jit.md
  §8) run under TSan in CI nightly.
- **ASan/UBSan.** Default in the `Debug` preset; required green for merge.

## 9. Rules

- **MUST NOT be a permanent test:** wiring/mocks, source-text pins, tests that assert on log
  strings, tests that re-implement the production code. Tests assert on observable behaviour
  (return values, stdout, exit status, struct layout, decoded mnemonics).
- Tests are deterministic and isolated: no network, no real Apple binaries, no shared mutable state
  between tests, no reliance on wall-clock time.
- A test that can only pass on one machine is not a test; it belongs in a manual runbook.
