# Decisions (ADR log)

Status: **proposed**. Every ADR here is load-bearing for the rest of the plan; a doc that
contradicts one of these is wrong, not this file. Entries marked **SPIKE** are decisions that
MUST be validated by a measurement before they are locked (see `11-milestones.md`).

Superseding an ADR: append a new numbered ADR that says `Supersedes ADR-NN` and edit ADR-NN's
status to `superseded by ADR-MM`. Do not rewrite history in place.

| # | Decision | Confidence |
|---|---|---|
| 01 | Recompile guest userland; emulate only below the syscall/trap boundary; per-dylib `builtin`/`guest`/`stub` substitution | high |
| 02 | Guest VA == host VA inside a reserved, exclusive arena `[vm.mmap_min_addr, 4 GiB)` (64 KiB on this host) | high |
| 03 | Pointer-width firewall: everything crossing into guest-visible memory is explicit ILP32 | high |
| 04 | Translated code lives in a fixed-expansion shadow region: `host = K + (guest_va << E)` | **SPIKE** |
| 05 | Two execution tiers (interpreter + LLVM ORC JIT) driven by one shared semantic table | high |
| 06 | Guest register state is materialised in the thread context at every boundary (SjLj landing pads, `longjmp`, signals) | high |
| 07 | LLVM MC (CPU `swift`) is the authoritative decoder; Capstone is an independent oracle and tooling decoder | medium |
| 08 | Mach IPC is emulated in-process; system daemons are native services, never recompiled | high |
| 09 | Framework cut: `guest` libsystem_*/objc/CF/Foundation/libdispatch, `builtin` only libdyld + hardware boundary, `stub` for bring-up | medium |
| 10 | JIT and AOT are the same pipeline; translated objects are real ELF objects linked by lld | high |
| 11 | Graphics/audio/compositor are native; `OpenGLES.framework`/CoreAudio are substituted at the framework boundary (Mesa GLES, PipeWire) | medium |
| 12 | Only decrypted, user-supplied binaries; the loader refuses `cryptid != 0` | high |
| 13 | Fail loudly: unknown opcode/symbol/selector never silently mis-executes | high |
| 14 | Test fixtures are armv7 Mach-O objects from clang linked by an in-tree minimal Mach-O linker (`ld64.lld` cannot target 32-bit ARM) | high |

---

## ADR-01 — Recompile guest userland; emulate below the syscall/trap boundary

**Context.** Two ways to run an iOS 6 app: (a) reimplement the frameworks (Wine's approach: Wine
ships its own `kernel32`/`user32`/`ntdll`), or (b) run Apple's own framework binaries and emulate
what is underneath them. Wine can do (a) partly because Windows DLLs are x86 PE files and Wine can
also `LoadLibrary` the real ones; on iOS every framework is ARMv7 Mach-O, so "run the real one"
means "translate it", not "load it".

**Decision.** We do (b) with a per-dylib substitution switch:

- `guest` — parse the dylib from the dyld shared cache and execute its translated ARMv7 code.
- `builtin` — our own native implementation exports the same symbol names; the guest never sees
  Apple's bytes.
- `stub` — auto-generated from the image's export trie: logs `(symbol, caller, args)`, returns a
  benign value. Bring-up only; a stub hit in a release preset is a loud warning.

The boundary where we stop recompiling is the **kernel trap** (`svc #0x80`) plus Mach IPC. Above it
(the BSD/Mach userland, CoreFoundation, Foundation, UIKit, GLES) we prefer `guest`; below it we are
the kernel.

**Consequences.** The hard work moves from "reimplement UIKit" to "make the translator and the
kernel shim right", and framework behaviour (including undocumented quirks) is inherited for free.
Cost: we depend on the shared cache being available and on our translator handling *all* ARMv7 that
Apple's own frameworks use — a much larger code corpus than one app. This is why ADR-05's
interpreter tier exists.

**Rejected.** Reimplementing CF/Foundation natively: multi-year ABI-fidelity treadmill (CFString
layout, `objc_class` internals, `NSInvocation` marshalling). Not runnable except for toy apps.

---

## ADR-02 — Guest VA == host VA inside a reserved low arena `[mmap_min_addr, 4 GiB)`

**Context.** Guest is ARMv7: pointers are 32-bit. Host is x86-64: pointers are 64-bit. The naive
design keeps a translation function `host = f(guest)`, which forces a truncate/shift at every load,
store, pointer compare, and struct field holding a pointer.

**Decision.** At startup read `/proc/sys/vm/mmap_min_addr` (`M`; 65536 here) and reserve the low
address space from `M` up to 4 GiB with a single
`mmap((void*)M, 4 GiB - M, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE, -1, 0)`
and keep it exclusively for guest memory. The arena is **`[M, 4 GiB)`, not `[0, 4 GiB)`**: a mapping
that includes address 0 is refused by the kernel. Measured (`/tmp/aud/m.c`, clang, this host):
`mmap(0, 4 GiB, …|MAP_FIXED_NOREPLACE|…)` → `Operation not permitted` (`EPERM`);
`mmap(65536, 4 GiB-65536, …)` → `0x10000` ok; `/proc/sys/vm/mmap_min_addr` = `65536`,
`vm.overcommit_memory` = 0. Guest page 0 up to `M` is therefore *guaranteed unmapped* by the kernel
itself, not by us. Guest allocations are carved from it with `mmap(MAP_FIXED)` +
`mprotect` at the guest's own virtual addresses. Consequence: `(uint64_t)(uint32_t)guest_ptr` is a
valid host pointer with no arithmetic, and guest code that does pointer arithmetic, hashing,
ordering comparisons or `memcpy` on its own pointers needs no masking.

**Consequences.**

- The low 4 GiB of a 64-bit Linux process is unused by default (PIE executables load at `0x55…`,
  shared objects and stacks at `0x7f…`, the heap follows the executable); measured here: the
  `[65536, 4 GiB)` reservation succeeds at exactly `0x10000`. Startup MUST verify with
  `MAP_FIXED_NOREPLACE` and abort with a clear message if any part is already mapped (e.g. a non-PIE
  host executable linked below 4 GiB, or a launcher that requested it). The `ios6recomp` host binary is
  therefore linked `-pie`; the failing-`MAP_FIXED_NOREPLACE` abort is the enforcement.
- Guest `__PAGEZERO` is unmapped by construction: the kernel guarantees `[0, M)` is never mappable, and
  guest null and near-null dereferences fault in the guest. iOS 6 apps declare `__PAGEZERO` of only
  **0x1000** and place `__TEXT` at `0x1000` (measured in MobileSafari/SpringBoard/MobileMail,
  [research/ios6-platform.md §7](../research/ios6-platform.md)); on a host with `M = 65536` the range
  `[0x1000, 0x10000)` is *not mappable*, so a main executable whose `vmaddr` is below `M` cannot be
  loaded at its link address. Rule: such an image is loaded with a slide that moves `__TEXT` to ≥ `M`
  if (and only if) it has `MH_PIE`; a non-PIE image with `vmaddr < M` is refused with a diagnostic
  that names `vm.mmap_min_addr` (the user may lower it with `sysctl`, which needs root).
  UNVERIFIED: whether the iOS 6 App Store apps the project targets are `MH_PIE`; close by reading
  `flags` of the first user-supplied app (`ios6recomp macho info`, M0).
- Guest `mmap`/`mach_vm_allocate` requests are honoured *inside* the arena, so guest allocators and
  the guest's own memory layout logic behave normally.
- The arena is also where translated-code *metadata* (block headers) live, so guest and block tables
  stay cache-local and 32-bit addressable.
- Host libraries, JIT'd code, and our own heap live above 4 GiB and are therefore unreachable by any
  32-bit guest pointer. That is a feature (see ADR-03, ADR-04) and a constraint (ADR-03).

**Not covered by the guarantee.** (1) `RLIMIT_AS` / `ulimit -v` counts `PROT_NONE` reservations:
measured, `ulimit -v 100000000` (≈ 95 GiB) still admits the 4 GiB arena but refuses the 256 GiB and
1 TiB shadow reservations of ADR-04 (`Cannot allocate memory`). (2) Strict overcommit
(`vm.overcommit_memory = 2`): UNVERIFIED on this host (needs root to switch). Kernel accounting charges
only writable private mappings, so `PROT_NONE|MAP_NORESERVE` should be free until guest pages are
`mprotect`ed writable, at which point they are charged against the commit limit; close by running
`/tmp/aud/m.c` and `/tmp/aud/r.c` after `sysctl vm.overcommit_memory=2` in a VM.

**Rejected.** `host = guest + delta` with a per-process randomized delta: identical benefit, but
requires masking/reserving the *whole* 32-bit range anyway and makes the delta need a register or a
constant that varies, defeating static `lea` folding in the JIT.

---

## ADR-03 — Pointer-width firewall at the guest ABI boundary

**Context.** `builtin` dylibs are host C++ compiled with the host's ILP32/ILP64 rules; the guest is
ILP32. Any struct shared between them, and any pointer handed to the guest, must have exactly the
guest's layout, or memory corruption follows silently.

**Decision.** Guest-visible types are declared once, explicitly, in `include/ios6/abi/` as
`guest_addr_t` (i.e. `uint32_t`), `g_ptr<T>`, and explicit-width integers, with
`static_assert(sizeof(...) == N)` for every struct that crosses the boundary. Rules:

1. Never pass a host pointer to guest code, ever. Host pointers are converted to `guest_addr_t` at
   the boundary; there is no implicit conversion in either direction.
2. Builtins allocate guest-visible memory only from the guest arena (`guest_heap::alloc`), never
   `malloc`/`new`.
3. Layout is fixed by `static_assert`s generated from the ABI notes
   (`docs/research/apple-armv7-abi.md`), not by inspection.
4. A test (`tests/abi/`) recomputes every shared struct's size/offset using the ARMv7 rules and
   fails the build on divergence. This is the cheapest possible guard against the most expensive
   class of bug in the project.

**Consequences.** Verbose but mechanical. `size_t`/`long`/pointer confusion is a compile error
rather than a week of debugging. Native-only code (translator, JIT, kernel shim) uses normal host
types and is unaffected.

---

## ADR-04 — Translated code in a fixed-expansion shadow region — **SPIKE**

**Context.** Guest code addresses are 32-bit *and are data*: they sit in `__DATA` vtables, ObjC
caches, `__DATA,__const` tables, and in registers during `blx r3`. A guest function pointer must
still be a valid guest address because guest code compares, hashes, and even reads the code bytes
(Thumb/ARM discrimination via bit 0, `LC_FUNCTION_STARTS`, `objc` method-list inspection,
anti-tamper checks). So translated code cannot be placed *at* guest VAs.

**Decision (proposed).** Reserve a second, sparse region for translated code with a fixed slot size
`1 << E` bytes per guest *interworking address* (bit 0 set = Thumb, exactly as `bx`/`blx` and every
function pointer already carry it): the translation of the block entered at address `a` is placed at
`K + (a << E)`. Because bit 0 is part of the index, an ARM block at `0x1000` and a Thumb block at
`0x1001` are different slots with no mode bookkeeping. Indirect control transfers then become
`jmp [K + (a << E)]`-style arithmetic with no table lookup, and a guest function pointer held by
native code (e.g. an `IMP` stored by a builtin) becomes directly callable as `(fn_t)(K + (a << E))`.
Unmapped shadow slots are `PROT_NONE`; the first execution of an untranslated block faults, the
handler decodes `a = (fault - K) >> E`, materialises the block (ADR-05), and returns to retry the
faulting instruction. Blocks whose host code exceeds `1 << E` bytes put a jump to an out-of-line
spill block in the slot. The reservation is `2^(32+E)` bytes and must fit the 47-bit x86-64 user
address space alongside everything else: `E ≤ 12` (16 TiB) is the practical ceiling; an `E` of 16 or
more is impossible (256 TiB).

**Consequences.** Zero-cost guest→host code-pointer conversion; one address space (the shadow region
plus the guest arena) fully describes the process. Requires a custom
`llvm::orc::JITLinkMemoryManager` that sub-allocates inside the shadow region, and a fault handler on
the hot path — both are real complexity that a table-based lookup (`va >> 12` → page table →
per-block slot) avoids at the cost of two dependent loads per indirect transfer.

Measured on this host (`/tmp/aud/r.c`: `mmap(K=1 TiB, 2^(32+E), PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE)`,
`vm.overcommit_memory = 0`, `ulimit -v` unlimited): `E` = 6, 8, 10 and 12 (256 GiB, 1 TiB, 4 TiB, 16 TiB)
all succeed. The CPU reports 48 virtual address bits (`/proc/cpuinfo`); the user half is 47 bits. Under
`ulimit -v 100000000` the 256 GiB and 1 TiB cases fail — the reservation is only available when
`RLIMIT_AS` is unlimited (a container/`ulimit` precondition checked at startup).

**SPIKE required** (`M3.5` in `11-milestones.md`): measure (a) the distribution of x86-64 bytes per
guest block for real code out of the local iOS 6 dyld shared cache, to pick `E` from {6, 8, 10} (64 B,
256 B, 1 KiB slots; 256 GiB, 1 TiB, 4 TiB of address space) by spill rate;
(b) indirect-transfer rate on representative guest code; (c) cost of the fault-driven materialisation
under a cold-start workload. **Fallback if the spike fails:** two-level per-page block table
(`va >> 12` → `PageEntry*`, then 1024 × 4-byte slots per 4 KiB guest page), with a monomorphic
inline cache at each indirect site. The rest of the plan is unaffected by which one wins because
both present the same interface to the translator (`BlockAddr translate(guest_va)`).

---

## ADR-05 — Two execution tiers over one semantic table

**Context.** Bring-up cannot wait for a correct optimising compiler, and a JIT cannot be debugged
without a reference.

**Decision.** One machine-readable ARMv7/Thumb-2 semantic table
(`lib/lift/semantics/*.td` → generated C++), consumed by two backends:

- **Tier 0, interpreter.** Written directly from the table. Correctness reference, breakpoint/step
  engine for the debugger, and the universal fallback for opcodes the lifter does not implement.
- **Tier 1, LLVM ORC JIT.** Emits LLVM IR from the same table per basic block, compiles via
  `llvm::orc::LLJIT` + `JITLink`.

A single opcode maps to a pair `(interpret_fn, emit_ir)`. Adding an instruction means adding one
table entry and making both backends agree. Correctness is established by per-instruction
differential testing between tier 0 and tier 1 (see `12-testing.md`), not by review.

**Consequences.** Roughly 2× the work per instruction, repaid immediately in debuggability, in a
never-fail fallback path, and in the ability to ship correctness fixes without regenerating code.
Disagreements between tiers are automatically reduced to a single-instruction reproducer, which is
the only tractable way to make an ISA lifter correct.

**Rejected.** QEMU TCG as tier 1 (fast to bring up, but a second IR and a second register allocator
we would have to maintain and cannot AOT through lld); JS-Baseline-style "just use the interpreter
until the JIT is done" (makes tier 0 load-bearing under a deadline and hides latency defects).

---

## ADR-06 — Guest state is materialised at every boundary

**Context.** Three guest mechanisms restore or inspect guest register state behind our back:
`longjmp` (restores a saved register set from a guest buffer), guest C++/ObjC exceptions, and signal
delivery. On iOS 6 armv7 exceptions are **SjLj** (`_Unwind_SjLj_*`, `__gxx_personality_sj0`; the
images carry no `__unwind_info`, `__eh_frame` or ARM EHABI — [research/apple-armv7-abi.md §5](../research/apple-armv7-abi.md)),
so there is no stack walking: each function registers a context in a per-thread list in guest
memory, and unwinding is a longjmp-style transfer to a landing pad. A JIT that keeps guest
callee-saved registers (`r4-r8`, `r10`, `r11`; `r9` is volatile scratch on iOS, [§1.2](../research/apple-armv7-abi.md))
live in host caller-saved registers across a guest call silently corrupts all three.

**Decision.** Keep guest architectural state in a `GuestThreadContext` (GTC) at a known address,
ILP32 layout, containing `r0..r15`, `cpsr`, `fpscr`, `d0..d31` with per-bank validity masks, the
thread pointer, and a stack of pending guest-call frames. The invariant is:

> At every guest call boundary — every `bl`/`blx` whose target will run in another block, every
> entry to native code, every point a signal can be delivered, and every landing-pad / `longjmp`
> resume — the GTC is fully up to date.

Within a single translated block, registers may live in host registers freely; a block boundary is a
boundary. Registers that the GTC cannot represent cheaply (host temporaries, carry bit) are block-
local.

**Consequences.** A guest `setjmp` buffer saved by guest code, restored by guest `longjmp`, is valid
even if other guest frames were JIT-compiled, because every frame has spilled its state. Enables a
straightforward FFI bridge: the marshalling stub reads arguments straight out of the GTC. Costs one
store/load per live guest register per boundary — measured, not assumed (see `M3.5`).

**Also required by this ADR.** Native frames must never sit between a guest frame and the landing
pad it unwinds to. All native→guest calls go through one trampoline (`runtime::call_guest()`), which
builds a well-formed guest frame; a guest exception or `longjmp` that would cross a native frame is
detected by per-thread native-frame depth markers and aborts with a diagnostic rather than
corrupting the native stack (detail in [06-runtime-core.md](06-runtime-core.md)). This is still the
riskiest area of the project (`13-risks.md` R-03).

---

## ADR-07 — LLVM MC is authoritative; Capstone is an oracle

**Context.** Two candidate decoders: Capstone (small, fast, easy operand access, known
mis-decodes in Thumb-2/NEON corners) and LLVM's MC layer (same ISA database as the compiler we
already link, exposed as `MCInst` + `MCInstrDesc`, precise predicates/IT/operand flags).

**Decision.** Decode authoritatively with `llvm::TargetRegistry`/`MCDisassembler` for triple
`armv7s-apple-ios6.0` and **CPU `swift`** (`+vfp4,+fp16,+hwdiv`): the superset subtarget is required
or valid armv7s code is rejected ([research/llvm-tooling.md §5](../research/llvm-tooling.md)). Required
features are recorded per instruction so an armv7s-only instruction in an armv7 slice is a loader
diagnostic. The lifter's opcode table skeleton is generated from `MCInstrInfo` (all 4531 ARM opcode
names are available at build time via a small generator tool; `-gen-instr-semantics` does not exist)
so every instruction has exactly one row. Use
Capstone for: (a) a **decode-differential test** — the same byte range must decode to the same
mnemonic/operand set under both decoders, with a checked-in allow-list of known divergences; and
(b) user-facing tooling (`ios6recomp disasm`) where speed and a stable C API matter.

**Consequences.** One semantics source of truth, and decoder bugs are found by a mechanical test
rather than by a crash three days later. Cost: MC operand access is target-specific and needs a small
accessor layer. If the decode-differential test shows LLVM MC mis-decoding encodings Capstone gets
right (or vice versa), we fix the allow-list case-by-case and record it in
`docs/plan/14-open-questions.md`.

---

## ADR-08 — Mach IPC is emulated in-process; daemons are native

**Context.** Above `libsystem_kernel.dylib`, `mach_msg` and the BSD syscalls are the whole operating
system surface for the guest. Below it sits Darwin's daemon mesh: `launchd`, `notifyd`, `securityd`,
`backboardd`, `SpringBoard`, `configd`. Recompiling those is possible in principle and is a trap
(they are the parts most entangled with real hardware, `IOKit` device nodes, and each other's
launch ordering).

**Decision.** Implement Mach ports and `mach_msg` in-process (a port registry with send/receive
rights, port sets, timeouts, dead-name notifications, and `mach_msg` receive-side semantics faithful
enough for libdispatch, CF, and observers). Substitute daemons with native services reachable through
the same port names (`bootstrap_port`, `com.apple.system.notification_center`, …). Start
single-process: the guest *is* the process, and its "daemons" are libraries with a mach facade. Only
if a specific app needs real multi-process isolation do we add a guest-process mode routing
`mach_msg` over Unix sockets (the Darling approach) — designed for now, built later
(`14-open-questions.md` Q-02).

**Consequences.** No boot sequence, no launchd plists, no `IOKit` matching. Guest code that expects
`bootstrap_look_up("com.apple.SpringBoard")` gets a native object that behaves like the service.
Risk: apps that introspect daemon state (a jailbreak check asking for a file under
`/var/mobile/Library/…`) see a plausible but synthetic world — acceptable, and by design we control
what the synthetic world looks like (`07-kernel-boundary.md`).

---

## ADR-09 — Framework cut points

**Decision.**

| Image | Mode | Why |
|---|---|---|
| `libsystem_kernel.dylib` | **guest** | Its `svc #0x80` stubs, MIG stubs, `cerror`/errno handling and `mach_*` wrappers are the *user half* of the syscall ABI; our kernel shim ([07](07-kernel-boundary.md)) sits underneath at the trap. |
| `libsystem_c.dylib`, `libsystem_m.dylib`, `libsystem_info/network/notify/blocks/dnssd/sandbox` | **guest** | `setjmp`/`longjmp`, `malloc`, pthread, TSD, `OSSpinLock`, stdio all live here (there is no `libsystem_pthread`/`libsystem_platform` on iOS 6); re-implementing them natively would duplicate an ABI we can get exactly by translation. Hot leaf routines (`memcpy`, `memset`, `strlen`, ...) may be replaced by host builtins through the hook mechanism ([04 §8](04-translation.md)). |
| `libSystem.B.dylib` | **guest** | Umbrella: re-exports the above; nothing to substitute. |
| `libdyld.dylib` | **builtin** | We are the loader. Needs the `_dyld_*` API subset used by guests ([03](03-loader.md)). |
| `libobjc.A.dylib`, `libunwind`, `libc++`, `libc++abi`, `libcompiler_rt` | **guest** | ABI-exact runtimes; recompiling is the only faithful path. libobjc's own `objc_msgSend` stays and is JIT'd well; only the cache-hit path is a candidate for interposition ([08](08-frameworks.md)). |
| `CoreFoundation`, `Foundation`, `libdispatch` | **guest** | Large, private internals, ABI-visible. |
| `OpenGLES.framework` (+ `GLEngine.bundle` and its sub-libraries) | builtin | Driver boundary; iOS 6 has no `libGLESv2`/`libEGL` images — one framework ([09](09-media.md)). |
| `UIKit` | builtin (minimal shim) then reassess | Stage 1 substitutes a small native UIApplication/UIWindow/UIView/EAGLView subset. |
| `IOKit`, `IOSurface`, `CoreSurface` | builtin | Hardware/driver boundary. |
| everything else | guest, else stub | Grow coverage by measurement: the stub log is the TODO list. |

**Consequences.** `stub` mode makes coverage progress impossible to lose track of: the runtime
counts and reports every stub hit at exit; the M4/M5 exit criteria publish that table, and M6's
criterion is zero unknown-opcode hits. Per-image mode switching (Wine's `WINEDLLOVERRIDES`) is the
`IOS6RECOMP_IMAGE_OVERRIDE` flag, not a rebuild.

---

## ADR-10 — One pipeline for JIT and AOT

**Decision.** The translator emits LLVM IR for a guest *region*; a backend then either JITs it
(`orc::LLJIT`, in-process) or compiles it to an object file and links a translated image with `lld`.
Both paths run the same IR, the same optimisation pipeline, and the same symbol naming
(`ios6.guest.<image>.<va>`), so a JIT'd process and an AOT-built image are the same computation, and
the persistent cache is just "the AOT artifacts for the blocks we happened to execute".

**Consequences.** AOT gives us: reproducible test fixtures, offline profiling, and a shipping mode
without a JIT (useful on platforms where RWX is restricted). Cost: the AOT path needs a symbol
layout that survives serialisation (guest VA → block offsets in a `.so`), which is what ADR-04's
`K + (va << E)` encoding or the block table give us either way. `lld` is available both as a library
(`lld/Common/Driver.h`, `liblld*.so` present locally) and as `ld.lld`; the offline build uses the
subprocess ([10-build-tooling.md §0(e)](10-build-tooling.md)).

---

## ADR-11 — Media and UI are native

**Decision.** Substitute at the dylib boundary, never by emulating a driver: `OpenGLES.framework`
(OpenGL ES 1.1 + 2.0) → a builtin that forwards to the host's native Mesa GLES (GLES1 fixed-function
emulated where Mesa lacks it; ANGLE was considered and rejected, see [09-media.md §1.3](09-media.md));
EAGL → X11/Wayland surface + GL context;
`IOSurface` → host shared memory with a real name registry; `CoreAnimation`/`UIKit` → a native
compositor that consumes a layer tree, or a minimal UIKit shim (stage 1); `CoreAudio`/`AudioToolbox`
→ PipeWire/ALSA; `OpenAL.framework` → OpenAL-Soft; input → SDL3 + multitouch
emulation. Details in `09-media.md`.

**Consequences.** Guest code that talks to a driver directly (`IOConnectCallMethod` on
`AppleS5L*`/`IOAccelerator`) is not supported and must fail loudly: it means an app bypassed GLES,
which we treat as an explicit gap, not something to fake.

---

## ADR-12 — Only decrypted, user-supplied binaries

**Context.** App Store downloads carry `LC_ENCRYPTION_INFO` with `cryptid = 1`; the `__TEXT` pages
are encrypted with a per-app key held by the kernel, and are only decrypted in memory on a real
device. There is no public offline decryption.

**Decision.** The loader checks `LC_ENCRYPTION_INFO` and refuses (`E_ENCRYPTED_BINARY` with
instructions) unless `cryptid == 0`. We never implement, host, or document decryption of
*App Store (FairPlay) binaries*. This does not cover Apple's own firmware: the IPSW's rootfs
(`encrcdsa`) and kernelcache (IMG3) are encrypted with per-build keys that are published
third-party data, and the patched `ipsw` (`ipsw extract --lookup`, [10-build-tooling.md §4](10-build-tooling.md))
decrypts them at run time on the user's machine from the user's own IPSW; neither the keys nor any
decrypted output is ever committed or shipped ([research/ios6-platform.md §3.3](../research/ios6-platform.md)).
The `cryptid == 0` check applies to every Mach-O the loader opens, including those extracted from the
cache or rootfs. Test fixtures
are either (a) binaries we build ourselves for `armv7-apple-ios6` with clang, or (b) binaries the
user supplies, decrypted by the user on their own hardware.

**Consequences.** The whole project's input contract is "user provides their own iOS 6 IPSW (for
shared cache/frameworks) and their own decrypted apps", exactly as Wine assumes a user-provided
Windows. Nothing Apple-covered is ever committed to this repo: `docs/plan/README.md` lists what is a
build input, and `.gitignore` excludes all extracted artifacts. This also means performance and
compatibility work MUST be developed against self-built fixtures, which is why `10-build-tooling.md`
makes "build a runnable iOS 6 ARMv7 Mach-O test suite with clang" an M1 deliverable rather than a
convenience.

---

## ADR-13 — Fail loudly

**Decision.** Three explicit failure modes replace silent wrongness:

1. **Unknown opcode** → tier 0 interprets it and increments `lift.missing[opcode]`; in strict mode
   (`IOS6RECOMP_STRICT=1`, on by default for tests) it aborts with the block dump.
2. **Unknown symbol / unresolved import** → the `stub` path logs `(image, symbol, callsite)` and
   returns a benign value only for return types where a benign value exists; unknown pointer-returning
   symbols return `NULL` and the caller is expected to notice. Every stub hit is counted and printed
   in the run summary.
3. **Unimplemented semantic** (e.g. `IOConnectCallMethod` on a driver) → hard error naming the call
   site and the guest symbol, never a fake success.

**Consequences.** Compatibility progress is measurable (the stub/missing tables are the burn-down
chart), and a "works" result means something. Cost: apps fail early and visibly instead of limping,
which is the correct trade for a project of this size.

---

## ADR-14 — In-tree minimal Mach-O linker for fixtures

**Context.** Fixtures must be genuine armv7/armv7s Mach-O executables — the loader, the binder
and the whole guest ABI are under test. Measured on this host (LLVM 23.1.3): `clang
--target=armv7-apple-ios6.0 -c` and `armv7s-apple-ios6.0` emit valid Mach-O objects (`file`:
"Mach-O armv7 object"), but the installed `ld64.lld` rejects `-arch armv7`
("missing or unsupported -arch armv7"); only arm64 links ([12-testing.md §2](12-testing.md)). There
is no Apple SDK or `ld64` on Linux.

**Decision.** Ship `ios6recomp-ld`, a minimal static linker for 32-bit ARM Mach-O: input = clang's
`.o` files (+ optional `.tbd`-like export lists generated from the real cache's export tries for
imports), output = `MH_EXECUTE` with `__PAGEZERO`, `__TEXT`, `__DATA`, `LC_UNIXTHREAD`/`LC_MAIN`,
`LC_LOAD_DYLIB` + `LC_DYLD_INFO_ONLY` (rebase/bind opcodes) when imports exist, and
`LC_FUNCTION_STARTS`. Scope is exactly what the fixture suite needs — no archives beyond `ar` of
objects, no LTO, no dead-stripping beyond sections, no bitcode. Relocation types handled: the
ARM/Thumb set clang emits for freestanding C (`ARM_RELOC_VANILLA/BR24/THUMB_RELOC_BR22/HALF/PAIR/SECTDIFF`).

**Consequences.** M1 gains a deliverable (roughly 1-2 weeks; its output is validated against
`llvm-objdump --macho -p` and against the loader). The linker doubles as a second independent
Mach-O *writer* for the loader's fuzz corpus. Rejected: shipping pre-linked binaries from a macOS
host (not reproducible here, unreviewable blobs); patching lld's MachO backend for armv7 (a large
upstream-scale project — kept as a possible later replacement).
