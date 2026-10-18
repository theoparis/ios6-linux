# 05 — JIT: ORC, JITLink, and the block cache

Decided by [ADR-04](02-decisions.md) (guest code pointers → host code without a lookup table),
[ADR-05](02-decisions.md) (two tiers), [ADR-10](02-decisions.md) (JIT and AOT are one pipeline).
The API surface of the local LLVM 23 install is catalogued in
[research/llvm-tooling.md](../research/llvm-tooling.md); this document is the design that sits on top.

## 1. Responsibility

`lib/jit` answers exactly one question — *"give me a host-callable entry point for guest VA `v` in
mode `m`"* — lazily, safely from multiple threads, and with an invalidation story. It owns:

- the block table and the address-mapping scheme ([ADR-04](02-decisions.md)),
- the ORC/JITLink session,
- block lifetime and invalidation,
- the persistent cache.

It does **not** own decoding, lifting, or guest semantics (`lib/lift`), and it must not depend on
`lib/kernel` or `lib/builtins`, so the translator can be tested standalone.

```cpp
// include/ios6/guestcode/table.hpp
namespace ios6::guestcode {

enum class Mode : uint8_t { Arm, Thumb };
struct BlockKey { uint32_t va; Mode mode; };

struct BlockInfo {
  uint32_t bytes;        // guest instruction bytes in this block
  uintptr_t host_addr;   // where to jump
  uint32_t flags;        // spill-stub, has-fallthrough, needs-retier...
  uint8_t  generation;   // bumped on invalidation
};

class BlockTable {                        // two implementations, one interface
public:
  virtual std::optional<BlockInfo> find(BlockKey) const = 0;
  virtual uintptr_t address_of(BlockKey) = 0;      // materialises on demand
  virtual void invalidate_range(uint32_t lo, uint32_t hi) = 0;
};
} // namespace
```

Both implementations must pass `tests/guestcode/` identically, including the property test in §8.

## 2. Address mapping: shadow region (ADR-04, primary)

```
K            = 1 TiB (aligned so `a << E` folds into one shift + add)
E            = log2(slot bytes); chosen by the M3.5 spike from {6, 8, 10}; reservation must stay <= ~16 TiB (E <= 12)
a            = guest interworking address (bit 0 = Thumb); ARM block @0x1000 and Thumb block @0x1001 are distinct slots
shadow(a)    = K + (a << E)
reservation  = mmap(K, (1ull<<32) << E, PROT_NONE, MAP_NORESERVE|MAP_FIXED_NOREPLACE)   // 2^(32+E) bytes
```

- **Page state (why `PROT_NONE` alone is not enough).** Protection is per 4 KiB page and a page holds `4096 >> E` slots (64 / 16 / 4 for E = 6 / 8 / 10). The moment one slot holds code its page must be executable, so its *neighbours* are executable too; zero-filled neighbours would execute as `add [rax], al` instead of faulting. Rule: a shadow page is mapped only when first needed (`mmap(MAP_FIXED|MAP_ANONYMOUS|MAP_PRIVATE, RWX)` over the `PROT_NONE` reservation) and is pre-filled with `0xCC`. Three slot states: *page absent* (`PROT_NONE`, first execution raises `SIGSEGV` with `si_addr` in the shadow range), *slot absent* (`0xCC`, raises `SIGTRAP` with `rip = slot+1`), *slot present*. Every block ends in an explicit transfer, so control never falls off a block into `0xCC` padding; a `SIGTRAP` at an interior byte of a present slot is a lifter bug and aborts with a dump.
- **RWX, not RX.** `jitlink::InProcessMemoryManager` flips each allocation to its final protection (research/llvm-tooling.md §2.8); doing that to a page other threads are executing would make valid code fault. The shadow memory manager therefore never removes `X`: it writes into the RWX page. Hosts that forbid RWX (SELinux `execmem`, PaX) use the fallback: back the reservation with a `memfd` mapped twice — RW alias for writing, RX alias at `K` for execution — which doubles the reserved address space (`IOS6RECOMP_SHADOW=memfd`). `UNVERIFIED:` which of the two is acceptable on target hosts and their finalisation cost; closed by M3.5 measurement 5 (`11-milestones.md`), run once per mode.
- **Materialisation.** Decode/lift/compile the block (object per block, §4). If the machine code fits in `1 << E` bytes, copy it into `shadow(va)`; otherwise allocate from a spill pool (RWX, near-allocated arena of ≤ 2 GiB so rel32 reaches) and write the 14-byte `jmp qword ptr [rip+0]; .quad target` (`FF 25 00 00 00 00` + imm64, no register clobbered) into the slot. Spill rate is an M3.5 metric; above a few percent `E` is too small. Branch reach: a `call/jmp rel32` between two slots reaches only guest-address distance `2^(31-E)` (E = 8: 8 Mi interworking addresses), so cross-image calls (libSystem 0x38.. → app 0x4000.. ) are *far*; JITLink handles this with `BranchPCRel32ToPtrJumpStubBypassable` edges (`llvm/ExecutionEngine/JITLink/x86_64.h:255`: pointer-jump stub, bypassed with a direct branch when in range). Each far callee costs a GOT slot + stub (about 8 + 6 bytes) inside the caller's slot, which counts toward the spill rate.
- **Publishing.** Another thread may execute the slot while it is written. Write bytes `1..n` first, then store byte 0 last with a single one-byte release store replacing the `0xCC`; a thread that hit the old `int3` re-checks the slot state in its `SIGTRAP` handler and retries. This is the int3 patching protocol for cross-modifying code (Intel SDM vol. 3, "handling self- and cross-modifying code"). Compilation runs outside any lock; only the slot-state transition is serialised (one compare-exchange on a per-slot state byte in a side table indexed by `a`).
- **Fault-driven materialisation.**
  ```mermaid
  sequenceDiagram
    participant G as guest code (shadow slot)
    participant H as SIGSEGV/SIGTRAP handler (sigaltstack)
    participant W as compile worker (normal stack)
    G->>H: first execution of slot a (fault)
    H->>H: classify fault address (below)
    H->>W: enqueue(a); futex-wait
    W->>W: decode -> lift -> LLJIT object -> JITLink into slot
    W-->>H: slot published; futex-wake
    H-->>G: return from signal, retry instruction
  ```
  1. `si_addr` (SEGV) or `rip-1` (TRAP) in `[K, K + (1<<32)<<E)` → `a = (addr - K) >> E` (mode = `a & 1`); first map the page if absent, then request materialisation and block in the handler until published, then return to retry.
  2. else if the address is in the guest arena → a guest memory fault → deliver a guest signal (`SIGSEGV`/`SIGBUS`) to the guest thread (07).
  3. else → a genuine host bug: abort with a block/GTC dump; this path must not allocate.
  LLVM compilation is not async-signal-safe, so it never runs on the signal stack: the handler (running on a per-thread `sigaltstack`) only classifies, enqueues and sleeps on a futex (both async-signal-safe), and compilation runs on worker threads. This is sound only because translated code never holds a host lock when it faults (ADR-06: no native frame sits between guest frames; builtins release locks before `call_guest()`). Nested faults inside the handler are fatal.
- **Mode resolution.** There is none to do: bit 0 of the interworking address *is* the mode, so every `bx`/`blx`, vtable entry, `IMP` and fault address already carries it. Direct `bl`/`b` targets know their mode statically from the instruction encoding (`blx imm` switches). The loader's per-image interval map is only a sanity check (a Thumb-tagged address inside an ARM-only section is reported, not "fixed").
- **Guest code pointers as host addresses.** Native code holding an `IMP` or a callback address can call `shadow(a)` directly; the fault path makes this safe even for untranslated targets ([04-translation.md §8](04-translation.md)).

## 3. Address mapping: page table (fallback)

Kept as a *designed* alternative, implemented only if M3.5 rejects the shadow region:

- L1: `1 << 20` × `u32` (4 MiB) indexed by `va >> 12` → `PageEntry*`.
- `PageEntry`: header + `u32 slots[1024]` (one slot per 4 guest bytes = 4 KiB per guest code page),
  allocated lazily, freed on invalidation.
- Lookup in IR: `t = load l1[va>>12]`, `h = load t + ((va>>2) & 1023) * 4`, `if (h == 0) h = slow(va)`,
  then an indirect tail call. A monomorphic inline cache per indirect site is the M7 optimisation.
- Cost: two dependent loads on every indirect transfer, and 1:1 metadata per code page.

## 4. ORC / JITLink integration

- `llvm::orc::LLJIT` from `LLJITBuilder` (`llvm/ExecutionEngine/Orc/LLJIT.h`) with `setJITTargetMachineBuilder(JTMB)` for the host triple. The guest's ILP32 `DataLayout` is **not** applied to the JIT'd host code: the JIT compiles *host* code that manipulates 32-bit guest state; the `DataLayout` is the host's.
- `ObjectLinkingLayer` (`Orc/ObjectLinkingLayer.h:55,63`: `ObjectLinkingLayer(ExecutionSession&, jitlink::JITLinkMemoryManager&)`) over a custom `jitlink::JITLinkMemoryManager` subclass (`ShadowMemMgr`), installed with `LLJITBuilder::setMemoryManagerCreator` (`LLJIT.h:422`) or `setObjectLinkingLayerCreator` (`LLJIT.h:443`); API surface and skeleton in research/llvm-tooling.md §2.8. `allocate(const JITLinkDylib*, LinkGraph&, OnAllocatedFunction)` places the graph's code segment at `shadow(a)`, where `a` is parsed from `LinkGraph::getName()` (`JITLink.h:1047`; one `LinkGraph` per block, named like the block symbol below). It writes directly into the RWX shadow page (§2), publishes per §2, and never calls `mprotect`. This is also the piece to prototype first (M3.5 measurement 5). Constraint it imposes on the lifter: a block's object may contain only `.text` (and GOT/jump-stub sections JITLink synthesises, which must also fit in the slot); constants are materialised as immediates or an in-`.text` literal pool — no `.rodata`/`.data` sections, because a separate data segment cannot be placed within rel32 reach of a slot in a `2^(32+E)`-byte region.
- **In-process execution only.** `SelfExecutorProcessControl` (`Orc/SelfExecutorProcessControl.h`). An out-of-process executor would place JIT'd code outside the identity-mapped arena and break both guest pointer validity and the fault-driven materialisation.
- **Symbol scheme** (stable, used by AOT and the cache):

  | Kind | Name | Notes |
  |---|---|---|
  | guest block (defined) | `ios6.b.<va>_<arm\|thumb>` | one per materialised block; defined by its own object, `<va>` is the interworking address's even part; the image hash lives in the cache key (§6), not the name |
  | callee slot (referenced) | `ios6.s.<a>` | an *external* reference from a caller block to another block's slot; resolved by the generator below to the constant `K + (a << E)` |
  | host interposition | `ios6.h.<image>.<symbol>` | [04-translation.md §8](04-translation.md) |
  | guest data address | — | never a symbol: `AbsoluteSymbols` with the guest VA when a block needs `&guest_global` |

- **Direct branches.** The IR emitter declares the callee as `ios6.s.<a>` and emits a real `call`; a `DefinitionGenerator` (`Orc/Core.h:664`, pure virtual `tryToGenerate(LookupState&, LookupKind, JITDylib&, JITDylibLookupFlags, const SymbolLookupSet&)`) on the main JITDylib answers each such lookup with `JD.define(absoluteSymbols({{name, {ExecutorAddr(K + (a << E)), JITSymbolFlags::Exported}}}))` (`Orc/AbsoluteSymbols.h:53`). JITLink then emits a `rel32` branch when in reach, or a bypassable pointer-jump stub otherwise (§2). The callee need not exist yet: the target is just an address, and its *slot* is the lazy stub — the first call into an unmaterialised slot faults and materialises it (§2). No `ios6.p.*` pending-stub symbols, no `LazyReexportsManager`, no `RedirectableSymbolManager` are used by the shadow-region design. Cost: one generator hit per distinct callee per compiled block (symbol-table growth is bounded by distinct callees, not by guest code size; see research/llvm-tooling.md §4.1 for the per-symbol memory budget).
- **Page-table fallback only (§3).** There the callee address is not a constant, so a call goes through the L1/PageEntry load and a `slow(va)` miss path; `ios6.s.*` is unused and no generator is needed.
- **Why not stock lazy layers.** `LLLazyJIT`, `CompileOnDemandLayer`, `IRPartitionLayer`, `lazyReexports(...)`/`LazyCallThroughManager` are **present** in LLVM 23.1.3 (`Orc/LLJIT.h:289`, `Orc/CompileOnDemandLayer.h`, `Orc/LazyReexports.h:135,171`; research/llvm-tooling.md §3.1) — they are not removed. They are not used because they partition *IR functions in a module*, create stubs at JIT-allocated addresses (the position-determined slot is already the stub here), and `OrcABISupport.h` has no benefit for a design whose callee addresses are constants. `createJITLinkLazyReexportsManager(ObjectLinkingLayer&, RedirectableSymbolManager&, JITDylib&, Listener*)` (`Orc/JITLinkReentryTrampolines.h:64`) and `JITLinkRedirectableSymbolManager::Create(ObjectLinkingLayer&, MemoryAccess&)` (`Orc/JITLinkRedirectableSymbolManager.h:27`) exist and are the reserve option for the fallback design (§3) or tier-1b re-optimisation. `UNVERIFIED:` whether the reentry trampolines require an ORC runtime on this target (header comment says "using JITLink"); experiment: build `LLJITWithLazyReexports` from the LLVM examples against the local install.

## 5. Compilation policy

| Tier | Trigger | Pipeline |
|---|---|---|
| 0 | interpreter fallback, cold, or `IOS6RECOMP_TIER=interp` | — |
| 1a | first execution | lift → `mem2reg`, `instcombine`, `simplifycfg` → object → link |
| 1b | hot (counter ≥ N, default 1000) | re-lift with `-O2` + lazy flags ([M7](11-milestones.md)) |
| AOT | offline (`ios6recomp build`) | same IR + `-O2`, linked by `lld` |

- Per-block execution counters live in the block's own slot: an 8-byte counter at the end of the block's `.text` (so `inc qword [rip+disp]` always reaches; a separate counter array would be out of rel32 reach of most slots, §2), enabled by default. It costs 8 of the slot's `1 << E` bytes and counts toward the spill rate; the ~1 % runtime cost is a target, `UNVERIFIED:` — measure on the M3.5 fixture. Counters feed tiering, coverage reporting and the hot list; a tier-1b re-translation inherits the count.
- **Latency budgets** (targets, `UNVERIFIED:` no measurement exists yet; M3.5 measurement 4 reports the first numbers and the test then pins them): p50 ≤ 300 µs, p99 ≤ 2 ms per block for tier 1a. Blocks that exceed the budget are split (already bounded by the 4 KiB cap in [04-translation.md §4](04-translation.md)).
- Compilation is off-thread where possible: on a fault-driven materialisation we cannot return until
  the block exists, but *speculative* pre-materialisation of discovered blocks (M6+) is done by a
  worker pool fed by the discovery pass.

## 6. Persistent cache (M8)

- Key: `hash(translator version, lifter semantic-table version, LLVM version (23.1.3), target triple + CPU features, opt level, tier, image UUID (`LC_UUID`; every cache image has one), content hash of the guest bytes the block was lifted from)`. The content hash of the *bytes* (not just the image hash) keeps the cache safe under guest code that patches itself or is loaded at a different address.
- Granularity: one relocatable object per block, as §4 produces (`llvm::ObjectCache`, `ObjectCache.h:33,38`: `notifyObjectCompiled(const Module*, MemoryBufferRef)` / `getObject(const Module*)`, wired through `TMOwningSimpleCompiler`, research/llvm-tooling.md §2.10; the key above is carried in the `Module` identifier). A block of the same guest bytes and slot compiles to identical bytes, so lookups are exact. A `va → object` index file is written alongside. On warm start the object is handed to `ObjectLinkingLayer` exactly like a freshly compiled one (JITLink re-applies relocations), so a hit skips lifting and LLVM but not linking.
- **Position-dependence, stated precisely.** The slot address of a block is a pure function of `(va, mode)`, so *calls between blocks* need no cache-specific fixups: the generator recomputes `K + (a << E)`. Calls into host runtime functions (call_guest, helpers) are *not* position-independent across runs because the host binary is ASLR'd: they go through a fixed-address helper table mapped at `K − 64 KiB` (`MAP_FIXED_NOREPLACE`, reserved with the shadow region) via `call [rip+disp]`, so the cached object's relocations against it stay valid. `UNVERIFIED:` the displacement from a slot to that table must fit rel32 — true only for slots within 2 GiB of `K − 64 KiB`, i.e. low `a`; far slots call through a GOT entry JITLink creates (§2). Experiment: M3.5 measurement 5 reports how many blocks need it.
- **All-or-nothing validation per entry.** Any hash mismatch drops that cache entry; there is no partial repair of a half-valid object, because a partially valid translation cache is the worst possible failure mode.
- Derived from the RPCS3/Yuzu PPTC designs ([research/prior-art.md](../research/prior-art.md)); the difference is the positional guest→host mapping ([ADR-04](02-decisions.md)).

## 7. Invalidation

Detecting writes to translated code: translated code lives in the **shadow region**, never at guest VAs, so the shadow slots themselves are never written by the guest. The thing that can go stale is the *guest bytes* a block was lifted from, which sit in guest arena pages (guest VA == host VA, ADR-02). Four triggers, in order of reliability:

| Trigger | Action |
|---|---|
| guest calls `sys_icache_invalidate(addr, len)` — a `svc #0x80` with `r12 = 0x80000000`, `r3 = 0`, `r0 = addr`, `r1 = len` (measured: libsystem_c 0x3942a054 `mov r3,#0; mov r12,#0x80000000; svc #128`; 07 owns the trap) | `invalidate_range(addr, addr+len)`: ARM requires this call after writing code, so every correct JIT issues it; this is the primary, exact mechanism |
| guest `mprotect`/`mach_vm_protect`/`vm_protect` giving `PROT_EXEC` to a range that holds no previously translated code, or removing it; `munmap`/`vm_deallocate`/`mach_vm_deallocate` of a range with translated code | invalidate every block intersecting the range; bump generation; drop cache entries for them |
| image unloaded (`dlclose`) | invalidate the image's blocks |
| (optional, tests and misbehaving guests) write-protect mode, below | invalidate the page's blocks on the first write |

Invalidation of a block (the slot may be executing on another thread): (1) store `0xCC` into byte 0 only — bytes `1..n` stay intact, so a thread already inside the old block runs it to the end (stale-until-flush is architecturally legal on ARM) while every later entry traps; (2) mark the slot `STALE` and bump `BlockInfo::generation`; (3) the next entry traps to the handler, which compiles the *new* translation into the spill pool and resumes the thread by setting `rip` in the signal context to the new code — the slot's old bytes are not touched; (4) the slot is rewritten (`jmp`-to-spill or the new code in place) only after an epoch barrier: every guest thread has since passed through the runtime (syscall, fault, `call_guest`, safe point); threads blocked in syscalls count as quiescent. Callers that branched directly to the slot (`rel32`) need no patching — they always re-enter through the slot. Tier-1b re-optimisation of a hot block uses the same STALE protocol.

**Write-protect mode (`IOS6RECOMP_WP=1`, default off).** Pages of the guest arena that contain translated-from bytes are set `PROT_READ` (or `PROT_READ|PROT_EXEC` if guest-executable) *on the guest arena mapping* — not on the shadow region. A guest store faults with `si_addr` in the arena; the handler looks the page up in the "translated pages" bitmap, invalidates that page's blocks, restores the guest's own protection (`RW`), single-steps/retries the store, and re-arms the page the next time a block from it is translated. Cost ≈ one signal round trip per first store (`UNVERIFIED:` microsecond figure; measure in M3.5). It exists for correctness testing (isolating "invalidation wrong" from "translation wrong") and for guests that skip `sys_icache_invalidate`; it is off by default because it taxes every data write into a page that shares a 4 KiB page with code (loader-patched `__DATA` next to `__TEXT` does not, as segments are page-aligned). Because the guest's recorded protection is separate from the host protection in force, the arena keeps a per-page *guest* protection table (07 `mach_vm_protect` reads it).

**Guest JIT.** A guest that generates code (JavaScriptCore's JIT; the framework is a cache image, `/System/Library/PrivateFrameworks/JavaScriptCore.framework/JavaScriptCore`, present in the image list) is handled by the `sys_icache_invalidate` and `mprotect`/`vm_protect` triggers above. Whether iOS 6 `UIWebView` hosts run the JIT in-process is `UNVERIFIED:` (platform lore says only MobileSafari holds the dynamic-code-signing entitlement); close by checking MobileSafari's entitlements and JavaScriptCore's executable-allocator strings in the rootfs. App Store apps do not generate code, so guest-JIT support is not scheduled in `11-milestones.md`; the debugging mode `IOS6RECOMP_JIT_PAGES=<range>` re-translates every entry into the range (correct, slow, trivial) and is available from M3.

We do **not** model I-cache/D-cache incoherence beyond this: after `sys_icache_invalidate` our runtime sees the new code; between the write and the flush the device may execute either old or new bytes, which no correct guest relies on.

## 8. Tests specific to this layer

- **Property test:** for 10⁶ random `(va, mode)` and a synthetic client, `address_of` is stable,
  `invalidate_range` removes exactly the intersecting set, and fault-driven materialisation produces
  the same address as lookup-driven materialisation.
- **Concurrency:** 32 threads hammering `address_of` on overlapping blocks; assert exactly one
  compilation per block and no torn slot writes (run under TSan).
- **Latency:** p50/p99 compile latency asserted against the §5 budgets on the fixture corpus.
- **Cache:** compile, persist, wipe the JIT, reload, assert zero lifting (`lift.stats()` counters)
  and identical results.
