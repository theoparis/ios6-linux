# 06 — Runtime core: threads, context, transitions, signals, unwinding

Depends on [ADR-03](02-decisions.md), [ADR-06](02-decisions.md), [ADR-09](02-decisions.md) and
[ADR-13](02-decisions.md); consumes [04-translation.md](04-translation.md) (LDREX/STREX, barriers) and
[05-jit.md](05-jit.md) (fault-driven materialisation). All ABI facts are from
[research/apple-armv7-abi.md](../research/apple-armv7-abi.md) (cited by section) and the trap/Mach facts from
[research/ios6-platform.md](../research/ios6-platform.md).

## 1. `GuestThreadContext` (GTC)

The GTC is the single source of truth for guest architectural state. It lives in host memory at a stable
address held in a host thread-local slot; guest code never sees it (ADR-03). Its *architectural* prefix
(offsets 0..351) is pure ILP32 — 4-byte fields plus two `uint64_t` at 8-aligned offsets — so the JIT can
address every field with a 32-bit displacement, and the prefix has **identical offsets on x86-64 and
armv7/i386**. The host-only tail (offsets ≥ 368) holds 64-bit host pointers.

```cpp
// include/ios6/runtime/thread.hpp
namespace ios6::runtime {
using guest_addr_t = std::uint32_t;
struct GuestThreadContext {
  std::uint32_t r[16];              // r0..r15; r15 = next guest pc at spill points, bit0 = T (== cpsr.T there)
  std::uint32_t cpsr;               // NZCV 31-28, Q 27, IT[1:0] 26-25, GE 19-16, IT[7:2] 15-10, T 5
  std::uint32_t fpscr;
  std::uint64_t d[32];              // d0..d31 raw bits; s(2i) = low half of d[i], s(2i+1) = high half
  std::uint32_t tpidruro;           // exactly what the guest wrote (machdep trap, §2); guest masks &~3
  guest_addr_t  excl_addr;          // exclusive monitor: armed address (valid iff excl_size != 0)
  std::uint32_t excl_size;          // 0 = disarmed, else 1/2/4/8 (access size of the arming LDREX*)
  std::uint32_t pending_signals;    // bitmask of guest signals queued for delivery
  std::uint64_t excl_value;         // value read by the arming LDREX* (zero-extended)
  std::uint32_t native_frame_depth; // call_guest nesting depth (§3)
  guest_addr_t  altstack_base;      // guest sigaltstack ss_sp
  std::uint32_t altstack_size;      // guest sigaltstack ss_size
  std::uint32_t _pad;
  // ---- host-only tail, LP64 ----
  void*         host_thread;        // host pthread_t of the carrying thread
  void*         thread_desc;        // runtime's per-thread descriptor
};
} // namespace ios6::runtime
```

Layout (host x86-64; the ILP32 prefix rows are the ones the JIT bakes in):

| field | offset | size | end |
|---|---:|---:|---:|
| `r[16]` | 0 | 64 | 64 |
| `cpsr` | 64 | 4 | 68 |
| `fpscr` | 68 | 4 | 72 |
| `d[32]` | 72 | 256 | 328 |
| `tpidruro` | 328 | 4 | 332 |
| `excl_addr` | 332 | 4 | 336 |
| `excl_size` | 336 | 4 | 340 |
| `pending_signals` | 340 | 4 | 344 |
| `excl_value` | 344 | 8 | 352 |
| `native_frame_depth` | 352 | 4 | 356 |
| `altstack_base` | 356 | 4 | 360 |
| `altstack_size` | 360 | 4 | 364 |
| `_pad` | 364 | 4 | 368 |
| `host_thread` | 368 | 8 | 376 |
| `thread_desc` | 376 | 8 | 384 |

`sizeof == 384`, `alignof == 8`. Earlier drafts placed the 8-byte `excl_value` at offset 340 and called the
prefix "376 bytes"; on any target where `uint64_t` is 8-aligned (x86-64, i386 `-malign-double`) that field
lands at 344, so the old offsets could not hold.

The asserts `tests/runtime/test_gtc_layout.cpp` carries (compiled in principle; the same file passed
`clang++ -std=c++20 -fsyntax-only` for the host and, with header-free typedefs, for
`-target armv7-apple-ios6.0` and `-target x86_64-linux-gnu`):

```cpp
static_assert(offsetof(GuestThreadContext, r) == 0);
static_assert(offsetof(GuestThreadContext, cpsr) == 64);
static_assert(offsetof(GuestThreadContext, fpscr) == 68);
static_assert(offsetof(GuestThreadContext, d) == 72);
static_assert(offsetof(GuestThreadContext, tpidruro) == 328);
static_assert(offsetof(GuestThreadContext, excl_addr) == 332);
static_assert(offsetof(GuestThreadContext, excl_size) == 336);
static_assert(offsetof(GuestThreadContext, pending_signals) == 340);
static_assert(offsetof(GuestThreadContext, excl_value) == 344);
static_assert(offsetof(GuestThreadContext, native_frame_depth) == 352);
static_assert(offsetof(GuestThreadContext, altstack_base) == 356);
static_assert(offsetof(GuestThreadContext, altstack_size) == 360);
#if INTPTR_MAX == INT64_MAX
static_assert(offsetof(GuestThreadContext, host_thread) == 368);
static_assert(offsetof(GuestThreadContext, thread_desc) == 376);
static_assert(sizeof(GuestThreadContext) == 384);
#endif
```

Measured: `clang++ -std=c++20 -fsyntax-only gtc.cpp` (host, with `<cstddef>/<cstdint>`) exits 0; the
`-target armv7-apple-ios6.0 -ffreestanding -nostdinc++ -fsyntax-only` variant (typedefs from
`__UINT32_TYPE__`/`__UINT64_TYPE__`, `__builtin_offsetof`) also exits 0, which proves the prefix offsets are
target-independent. `r9` is *not* treated as preserved anywhere in this struct's consumers: it is a normal
entry of `r[9]` that the lifter spills like any other volatile GPR
([research/apple-armv7-abi.md §1.2](../research/apple-armv7-abi.md)).

### 1.1 Boundary invariant (ADR-06) under SjLj

ADR-06 requires the GTC to be fully up to date at every guest call boundary, every entry to native code,
every point a signal can be delivered, and every point a guest unwinder may begin walking. Under SjLj this
has three consequences that pin down *which* boundaries may keep registers in host registers:

1. **Within a single translated block, caller-saved guest registers (`r0-r3`, `r9`, `r12`, `d0-d7`,
   `d16-d31`) MAY live in host registers; callee-saved guest registers (`r4-r8`, `r10`, `r11`,
   `d8-d15`) MUST be spilled to the GTC before any nested call or host callback, but MAY be cached
   across straight-line code.** Justification (measured, §4): SjLj resume (`_Unwind_SjLj_Resume`) and
   `__longjmp` transfer control by loading only `r7`, `sp` and the landing-pad `pc` (SjLj) or the whole
   `r4-r8,r10,r11,sp,lr,d8-d15` set from guest memory (`__longjmp`); every other register is whatever the
   GTC holds at the block entry of the landing pad. So the GTC must hold the throw/`longjmp`-site values of
   all callee-saved registers whenever control can leave the block (rule 2), and `call_guest` (§3.1)
   snapshots the GTC, not host registers. Registers kept only in host registers across a call would be
   invisible to both.

2. **Every block boundary, every `bl`/`blx` to another block, every `svc`, and every host callback
   boundary MUST spill all guest registers to the GTC.** These are the points at which a signal can be
   delivered, a guest unwinder can begin (SjLj registration happens in function prologues that the JIT
   recompiles verbatim), or a non-local transfer can land. Within-block caching is legal only because
   signals that arrive mid-block cannot be delivered until the next spill point without corrupting the
   cached set; the host signal handler therefore sets a "materialise requested" flag and the generated
   block epilogue performs the spill before any branch out. If a `longjmp` fires before that spill, the
   cached caller-saved values are abandoned, which is correct because they are caller-saved and the
   post-jump code reloads them from the GTC.

3. **Host frames must never appear between two guest frames in a way a guest unwinder can observe.**
   All native→guest calls go through `call_guest` (§3), which builds a well-formed guest frame and keeps
   `native_frame_depth` consistent. A C++ exception MUST NOT propagate across a native frame into guest
   code: a non-local transfer that skips a native frame aborts with a diagnostic (§3.2, ADR-06, ADR-13).

## 2. Guest threads

- **1:1 host threads.** Each guest thread is one host `pthread` carrying one GTC in its thread-local
  slot. Host-only threads (JIT workers, compositor, audio) never install a GTC and must never call
  `call_guest` without one.
- **Guest stacks.** Guest threads created by `pthread_create` get their stack from *guest* code:
  `_pthread_create` calls `vm_map`/`vm_protect` itself (guard page at the low end, default size
  0x80000 = 512 KiB — `_pthread_init` stores `0x80000` into the default attribute block) and passes the
  finished stack to `__bsdthread_create`. The runtime therefore allocates exactly one stack: the **main**
  thread's, 1 MiB (iOS default) with a 4 KiB unmapped guard page at the low end so an overflow faults as a
  guest `SIGSEGV`. `sp` is 4-byte aligned on entry and need not be 8-aligned
  ([research/apple-armv7-abi.md §1.3](../research/apple-armv7-abi.md)).
  Measured: `llvm-mc --disassemble` of `libsystem_c` `__pthread_create` (cache 0x3941ee51) shows
  `movw r2,#2244; blx memset` (so `sizeof(struct _pthread)` = 2244) and `str r5,[r5,#72]`
  (`tsd[0] = self`, TSD array at `_pthread+0x48`).
- **`pthread_create` path (measured, `libsystem_c` + `libsystem_kernel` 6.1.3 disassembly).**
  `_pthread_create` (0x3941de45) ends in a stub call with `r0 = start_routine`, `r1 = arg`, `r2 = stack`,
  `r3 = pthread*`, `[sp] = flags` (0x01000000 = custom stack | schedpolicy bits) — the BSD syscall
  `bsdthread_create(func, func_arg, stack, pthread, flags)` = **syscall 360**; the 5th argument is passed in
  `r4` (stub: `mov r12,sp; push {r4,r5}; ldr r4,[r12]; mov r12,#360; svc #128`). Registration:
  `___bsdthread_register` = **syscall 366** with 7 argument words (`r0..r3`, then `r4,r5,r6` loaded from the
  stack by the stub), matching `bsd/kern/syscalls.master` `(threadstart, wqthread, flags/pthsize,
  stack_addr_hint, targetconc_ptr, dispatchqueue_offset, tsd_offset)`; at the `_pthread_init` call site
  (0x3941edd6) `r2 = 0x8c4` (= 2244 = `sizeof(struct _pthread)`), `r5 = dispatchqueue_offset = 0x50`,
  `r6 = tsd_offset = 0`. `___bsdthread_terminate` = syscall 361, `___workq_open` = 367,
  `___workq_kernreturn` = 368 (stub immediates 0x169/0x16f/0x170, same as `bsd/kern/syscalls.master`).

  What the shim must do for syscall 360 — the user-visible start contract, measured from the two ARM-mode
  entry thunks that `_pthread_init` registers (`threadstart` = 0x3941e1d0, `wqthread` = 0x3941389c; both are
  `push {r4,r5}; blx <thumb C function>`, i.e. **`r4`/`r5` become the 5th/6th C arguments**):

  | reg | `threadstart` (→ `_pthread_start`) | `wqthread` (→ `_pthread_wqthread`) |
  |---|---|---|
  | `r0` | `pthread*` (the 4th `bsdthread_create` arg) | `pthread*` |
  | `r1` | `kport` (Mach thread port of the new thread) | `kport` |
  | `r2` | `func` (user start routine) | `stackaddr` |
  | `r3` | `func_arg` | work item |
  | `r4` | stack size | flags (`tst #0x40000` / `#0x20000` tested in the callee) |
  | `r5` | `pflags` (`tst #0x1000000` = custom stack) | unused |
  | `sp` | top of the new stack | top of the pool stack |
  | `pc` | `threadstart` (ARM mode, even) | `wqthread` (ARM mode) |

  Both `_pthread_start` and `_pthread_wqthread` call `_pthread_set_self(self)` themselves (measured: stub
  call list of both functions contains `__pthread_set_self`), which stores `tsd[0] = self` and issues the
  machdep trap `mov r3,#2; mov r12,#0x80000000; svc #0x80` with `r0 = &self->tsd[0]` (`___pthread_set_self`
  at 0x3942d974). **So the guest, not the runtime, sets `TPIDRURO`**: the shim implements machdep selector 2
  as `GTC.tpidruro = r0`, and a fresh GTC starts with `tpidruro = 0`. (`___error` handles `tsd[0] == 0`
  by returning a fallback errno, so code that runs before `_pthread_set_self` is safe.) The runtime's host
  thread entry is `runtime::thread_entry(start_regs)`: create GTC, load `r0-r5,sp,pc` per the table, run
  as a depth-0 `call_guest`-style entry (§3.1) until the guest calls `bsdthread_terminate` (syscall 361) —
  the shim then `siglongjmp`s to `thread_entry`'s exit point, legal because JIT/guest frames carry no host
  destructors (§3.2), and frees the host thread; `_pthread_exit`'s TSD destructors and joiner signalling
  already ran in guest code.
- **TSD / `TPIDRURO` model** ([research/apple-armv7-abi.md §4](../research/apple-armv7-abi.md)):
  `TPIDRURO & ~3` is the address of `tsd[0]`, which is *inside* `struct _pthread` at offset 0x48.
  Measured (`llvm-mc` of `_pthread_getspecific` 0x3942d950 and `_pthread_self` 0x3942d964):
  `mrc p15,0,r1,c13,c0,3; bic r1,r1,#3; add r0,r1,r0,lsl #2; ldr r0,[r0]` and
  `…; ldr r0,[r0]` for `pthread_self` (= `tsd[0]`); `_pthread_setspecific` (0x394121d5) does
  `r3 = TP[0]; str r1,[r3 + 0x48 + 4*key]`. Key table: `_pthread_key_create` (0x3941fc29) scans an array of
  8-byte `{used, destructor}` entries from `__pthread_tsd_start` to `__pthread_tsd_end`; the cache `__DATA`
  holds `__pthread_tsd_start = 256`, `__pthread_tsd_end = 512` (words at 0x3afaa8a8/0x3afaa8ac), and
  exhaustion returns `0x23` = `EAGAIN`. **`errno` is not `tsd[1]`**: `___error` (0x3944523d) loads
  `self = TP[0]`, checks `self->sig == 0x54485244` (`'THRD'`), and returns `self + 0x44` — the word
  immediately *before* `tsd[0]`, i.e. `TP[-1]` (the earlier "slot 1 is errno" in
  [research/apple-armv7-abi.md §4.4] does not match the shipped code). The SjLj chain head is `tsd[18]`
  (`TP + 0x48`: `_Unwind_SjLj_Register` does `ctx->prev = TP[18]; TP[18] = ctx`, §4). Slots 2-9
  (`MIG_REPLY`, `MACH_THREAD_SELF`, …) follow the research note and are `UNVERIFIED:` — close by
  disassembling `_mig_get_reply_port`'s TSD index. The lifter implements `MRC p15,0,rX,c13,c0,3` as a load
  of `GTC.tpidruro` followed by the guest's own `bic …,#3`, so guest code indexing the TP directly keeps
  working; the runtime never writes `tsd[]` itself.
- **Thread exit / join / destructors are guest code.** `_pthread_exit` runs `__cleanup_stack`, then
  `_pthread_tsd_cleanup` (dynamic keys first, then static; `PTHREAD_DESTRUCTOR_ITERATIONS` = 4 per
  research §4.4, `UNVERIFIED:` in the shipped binary — close by reading the loop bound in
  `_pthread_tsd_cleanup`), then signals the joiner through the guest's own semaphore/`__psynch_*` path and
  ends with `__bsdthread_terminate`. The runtime keeps **no** `pthread_t → host thread` table and never
  calls host `pthread_join`; join is whatever the guest does on top of `__psynch_*`/`semaphore_*` traps
  (chapter 07). Handles are recycled by guest code ([research §8.5]), which is invisible to the runtime.
- **Fork policy (unsupported).** BSD `fork` (syscall 2) and `vfork` (66) return `-1`/`ENOSYS` through the
  guest's `cerror`. Measured: `libsystem_c` `_fork` (0x394452ac) calls `___fork` (a `libsystem_kernel`
  stub); `pthread_atfork` (0x39420144) only calls `malloc`/`__spin_lock`/`__spin_unlock`, i.e. it is
  guest code that just records handlers (`CoreFoundation` and `libdispatch` import it). No builtin is
  involved. Which side runs the *prepare* handlers before the failing syscall is `UNVERIFIED:` (close by
  disassembling `___fork` in `libsystem_kernel` for a call to `_cthread_fork_prepare`); it is harmless either
  way. Rationale: the JIT's shadow region, ORC session and GTC state cannot be made fork-safe at
  acceptable cost, and no iOS 6 app we target requires fork.
- **Main-thread bring-up** (hand-off with [03-loader.md](03-loader.md)): after the loader maps images,
  applies rebases/binds and runs `__mod_init_func` in order, it calls
  `runtime::bring_up_main(entry_va, argc, argv, envp)` where `entry_va` comes from `LC_MAIN.entryoff`.
  That function allocates the main guest stack (1 MiB), creates the main GTC with `tpidruro = 0`, and
  enters the guest at `entry_va` via `call_guest`. `TPIDRURO` for the main thread is established by the
  guest's own `_pthread_init` (`__pthread_set_self` on the static main-thread `_pthread`, run from
  `libSystem`'s initializer), not by the runtime. The loader does not itself execute guest code.

## 3. Native↔guest transitions

### 3.1 `call_guest(fn_va, args)` — exact algorithm

```cpp
// runtime::call_guest — algorithm (sketch compiles in principle; helper names are the contract)
inline constexpr guest_addr_t kSentinelBase = 0xFFFE'0000u;  // kernel half of the arena (research/ios6-platform.md §7: iOS user VA < 0x80000000); never mapped RW
inline constexpr std::uint32_t kMaxNativeDepth = 256;

template <typename R, typename... A>
R call_guest(guest_addr_t fn, A... args) {
  GuestThreadContext* g = current_gtc();             // host TLS; must be non-null
  // 0. precondition: the GTC is current (ADR-06). A builtin is entered from a trap/stub boundary where
  //    every guest register was already spilled (§1.1 rule 2); a depth-0 entry has a zeroed GTC.
  const std::uint32_t d = g->native_frame_depth;
  if (d >= kMaxNativeDepth) abort_with_dump(g, "native depth overflow");
  // 1. snapshot what the *caller's* guest frame needs back: the nested guest run overwrites the GTC.
  const SavedGuestState saved = snapshot(g);         // r0..r15, cpsr, fpscr, d8..d15; excl_size = 0 after
  // 2. marshal per the soft-float APCS rules (research §1.5): stack arguments FIRST, then r0..r3.
  std::uint32_t sp = g->r[13] & ~3u;
  sp -= 4u * stack_words<A...>();                    // 4-byte alignment is sufficient (research §1.3)
  store_stack_args<A...>(sp, args...);               // writes guest memory through the arena
  load_reg_args<A...>(g, args...);                   // sret pointer, if any, goes in r0 and shifts the rest
  g->r[13] = sp;
  g->r[14] = kSentinelBase + 4u * d;                 // sentinel #d, ARM state (bit0 = 0)
  g->r[15] = fn;                                     // bit0 = Thumb
  g->cpsr  = (g->cpsr & ~CPSR_T) | (fn & 1u ? CPSR_T : 0);
  g->native_frame_depth = d + 1;
  // 3. run. host_entry(g) returns only through sentinel stub #d (below); a longjmp/SjLj resume that
  //    never reaches it is a §3.2 violation.
  guestcode::address_of({fn, fn & 1u ? Mode::Thumb : Mode::Arm})(g);
  // 4. back: the sentinel stub already verified depth and pc.
  R r = unmarshal<R>(g);                             // r0 | r0:r1 | read sret buffer
  restore(g, saved);                                 // caller-saved r0-r3,r12 are dead per ABI;
  g->native_frame_depth = d;                         // the builtin's own return glue sets r0/r1
  return r;
}
```

- **Return-address sentinels.** Sentinel `k` is the guest VA `kSentinelBase + 4k` for `k < kMaxNativeDepth`;
  `guestcode::address_of` returns, for these VAs only, a runtime-provided stub (not translated code) with
  semantics `if (g->native_frame_depth != k + 1 || g->r[15] != kSentinelBase + 4k) abort_with_dump(g,
  "non-local transfer crossed a native frame"); return;`. Using one sentinel per nesting depth (rather than
  a single shared VA, as an earlier draft did) is what makes the check decidable: a stale `lr` or a jump
  buffer captured at depth *k* that fires at depth ≠ *k* lands on sentinel *k* with the wrong depth and
  aborts, whereas a single shared sentinel would silently return from the *innermost* `call_guest` frame.
  Guest code that compares or hashes `lr` sees `kSentinelBase + 4k` (4-aligned, no Thumb bit), a stable,
  documented value; both `bx lr` and `pop {…,pc}` reach the stub through the ordinary indirect-branch
  path ([05-jit.md §2](05-jit.md)), which indexes the shadow region by the interworking address.
- **What must be saved.** Callee-saved registers are the interesting part: `snapshot` copies
  `r4-r11`, `sp`, `lr`, `pc`, `cpsr`, `fpscr`, `d8-d15`, and `restore` writes them back, because the nested
  guest function legitimately clobbers `r4-r11`/`d8-d15` *in the GTC* (it saves/restores them in its own
  frame, but the GTC is a single copy). `r0-r3`, `r12`, `d0-d7`, `d16-d31` are caller-saved by the ABI and
  need no snapshot.
- **Reentrancy.** Guest→native→guest→native nesting is nested host C calls; `native_frame_depth` equals
  the number of live `call_guest` frames on this host thread. Depth 0 = no native frame between the
  thread entry and the running guest code.
- **Guest stack discipline.** `call_guest` does not switch host stacks; the guest's own `sp` lives in the
  GTC and the compiled block moves it. The host C stack only carries the `call_guest` frames, so a guest
  stack overflow cannot corrupt host frames (it faults on the guard page first).

### 3.2 Exceptions / `longjmp` crossing a native frame

Decision: **abort with a diagnostic, do not attempt native unwinding.** Concretely:

- `native_frame_depth` is incremented by `call_guest` (every builtin that calls back into guest code goes
  through it) and restored on normal return.
- The sentinel stub for depth *k* checks `native_frame_depth == k + 1` (§3.1). If a non-local transfer
  (`longjmp`, `_Unwind_SjLj_Resume`, `sigreturn`) skipped an intermediate native frame, the depth no longer
  matches the sentinel the guest eventually returns to; the stub prints
  `IOS6RECOMP: guest non-local control transfer crossed a native frame (depth=%u)` with a GTC dump and
  calls `std::abort()` (ADR-13). No host unwinding is attempted, because guest frames have no host unwind
  info and host destructors on skipped frames would not run anyway.
  Limits of the check (honest scope): it fires only when control *returns* to a sentinel with the wrong
  depth. A `longjmp` that skips a native frame and then runs forever, never returning to any sentinel, is
  not detected; it leaves `native_frame_depth` too high (leaked host frames). Closing that hole needs the
  jump-site check: `__longjmp`/`_Unwind_SjLj_Resume` are guest code, so the runtime hooks them
  (hook mechanism, [04 §8](04-translation.md)) to compare the target frame's `sp` with the `sp` snapshot of
  the innermost `call_guest` (`saved.r[13]`): a jump target with `sp` *above* that snapshot crosses a native
  frame. `UNVERIFIED:` hook cost; the experiment is the M4 fixture 2 (§9) with the hook on and off.
- Builtins that themselves catch guest exceptions (none in M4; revisit for `dispatch_async`) must be
  structured so that the guest throw lands and is handled entirely within guest code, with the builtin
  only observing the result via `call_guest`'s return. A host `try/catch` around `call_guest` is permitted
  only to convert an *unexpected* host-side failure into a guest-visible abort, never to implement guest
  `try/catch`.
- A guest `_Unwind_SjLj_Resume` that targets a context registered above a native frame would load
  `r7`/`sp`/`pc` from guest memory (§4) and discard the native frames without running their destructors;
  the depth check in the sentinel (and the jump-site hook above) is what catches it. This is the concrete
  form of ADR-06's "native frames must never appear between two guest frames" rule.

### 3.3 FFI bridge for builtins

- Signature descriptors live in `builtins/*.td` (TableGen-style) and describe a guest-ABI signature:
  argument types, return type, sret, variadic. The build generates `ios6/ffi/gen_<image>.cpp` marshalling
  stubs from them.
- Marshalling follows the soft-float rules ([research/apple-armv7-abi.md §1.5], §1.6):
  - `float`/`double` arguments arrive in `r0-r3`/stack as 32/64-bit words; a `double` occupies two
    consecutive GPRs and may start on an odd register or be split across `r3` and the stack.
  - 64-bit integers likewise occupy two consecutive GPRs with no 8-byte alignment.
  - Composites up to 64 bytes are passed as `ceil(size/4)` 32-bit words in registers then stack; larger
    ones are copied by the caller into the stack argument area and passed by value there.
  - Return: `float` in `r0`; `double`/`long long`/`long double(=double)` in `r0,r1`; integer-like
    composites ≤4 bytes in `r0`; every other composite via sret with the destination pointer in `r0`
    first (so the callee's `self`/`_cmd` shift by one).
  - Variadic: unnamed arguments continue filling `r0-r3` then spill to a 4-byte-aligned stack area;
    `va_list` is a bare `void*`/`char*` pointer into the register-save/stack area, never the AAPCS
    struct ([research/apple-armv7-abi.md §1.7]).
- **Callbacks from builtins** (`qsort`, `dispatch_async`, `atexit`, `pthread_create`): the builtin stores
  the guest function VA and, when the callback fires, invokes it through `call_guest`. Pointer arguments
  cross the boundary as guest addresses only (ADR-03); for host APIs that pass host pointers to the
  callback, the builtin must either (a) prefer the guest implementation of the routine (so the callback
  stays guest→guest, as `libsystem_c`'s own `qsort` does), or (b) provide a copying adapter. A host
  `qsort` whose comparator receives host pointers cannot be used directly.

## 4. Exceptions & unwinding (SjLj)

- **The runtime supplies nothing special for guest unwinding.** `libunwind.dylib`, `libc++abi.dylib`,
  `libcompiler_rt.dylib` and `libobjc.A.dylib` run as guest code: the loader maps them and resolves
  `_Unwind_SjLj_Register/Unregister/Resume/RaiseException`, `__gxx_personality_sj0`,
  `__objc_personality_v0`, `__cxa_*`, `objc_exception_throw`, etc. from the real cache
  ([research/apple-armv7-abi.md §5.4](../research/apple-armv7-abi.md)). Measured definers (export tries of
  all 569 images; Mach-O names carry one extra `_`): `__Unwind_SjLj_Register`/`Unregister`/`Resume`/
  `RaiseException`/`Resume_or_Rethrow` → `libunwind.dylib` (+0xc15/+0xc2d/+0xc91/+0xc3d/+0xd31),
  re-exported by `libSystem.B.dylib`; `___gxx_personality_sj0`, `___cxa_throw`, `___cxa_rethrow` →
  `libc++abi.dylib` (also `libc++`, `libstdc++`); `___objc_personality_v0`, `_objc_exception_throw` →
  `libobjc.A.dylib`. Two corrections to research §5.4: `RaiseException` lives in **libunwind** (libc++abi
  imports it through a `__picsymbolstub4` stub), and `__gxx_personality_sj0` is exported by **libc++abi**,
  not `libcompiler_rt`. `_Unwind_RaiseException`, `_Unwind_Resume`, `_Unwind_ForcedUnwind` and
  `___gcc_personality_v0` are exported by *no* image.
- **`_Unwind_FunctionContext` layout (ARM, 52 bytes) — measured.** Disassembling the prologue of a
  libobjc function that registers one (0x38fc3e19: `add r0,sp,#0x1c; …; blx _Unwind_SjLj_Register`) and
  `_Unwind_SjLj_Register`/`Resume`/`RaiseException` in libunwind gives:

  | offset | field | evidence |
  |---:|---|---|
  | 0x00 | `prev` | `Register`: `ctx->prev = TP[18]` (`ldr r1,[r1,#0x48]; str r1,[r0]`) |
  | 0x04 | `resumeLocation` (call-site index; `-1` outside any region) | caller stores `-1` at `sp+0x20` = ctx+4 |
  | 0x08..0x14 | `resumeParameters[4]` | caller `stm sp,{…}` arguments fill `sp+0x24..` |
  | 0x18 | `personality` | `Resume`: `ldr r6,[r5,#0x18]; blx r6`; caller stores `__objc_personality_v0` there |
  | 0x1c | `lsda` | caller stores a pc-relative address at `sp+0x38` |
  | 0x20 | `jbuf[0]` = frame pointer `r7` | landing: `ldr r7,[r0]` with `r0 = ctx+0x20` |
  | 0x24 | `jbuf[1]` = landing-pad `pc`, Thumb bit set (`orr r1,r1,#1`) | landing: `ldr r1,[r0,#4]; bx r1` |
  | 0x28 | `jbuf[2]` = `sp` | landing: `ldr r1,[r0,#8]; mov sp,r1` |
  | 0x2c..0x33 | `jbuf[3..4]` | unused by the landing sequence |

  This is the 52-byte struct `{ prev; resumeLocation; resumeParameters[4]; personality; lsda; jbuf[5] }`
  (asserted in the compile test of §1: `sizeof == 52`, `offsetof(personality) == 0x18`,
  `offsetof(jbuf) == 0x20`). **The landing sequence restores only `r7`, `sp` and `pc`**: the phase-2
  helper at 0x39501b58 (called from `_Unwind_SjLj_Resume`) and the forced-unwind branch inside
  `_Unwind_SjLj_Resume` (0x39501c90) both end in
  `str r5,[TP+0x48]; add r0,r5,#0x20; ldr r1,[r0,#8]; mov sp,r1; ldr r1,[r0,#4]; ldr r7,[r0]; bx r1`
  (the chain head is also reset to the catching context `r5`, dropping all inner contexts).
  `lr`, `r4-r6`, `r8`, `r10`, `r11` and `d8-d15` are *not* reloaded; compiler-generated landing pads
  rebuild what they need from their own frame. The earlier statement (research §5.4, this chapter's
  first draft) that the SjLj resume path restores the `setjmp` register set is therefore wrong; only
  `__longjmp` does that.
  Personality call convention (measured at the `blx` sites in the phase-2 helper):
  `personality(1, actions, class_lo, class_hi, exception_object, context)` = `r0=1, r1=actions,
  r2:r3=exception_class, [sp]=exception, [sp+4]=context`, with `actions = 2` (`_UA_CLEANUP_PHASE`) or `6`
  (`_UA_CLEANUP_PHASE|_UA_HANDLER_FRAME`) when `context == exception->private_2` (`ldr r0,[r8,#0x10]`), `0xa`
  (`CLEANUP|FORCE_UNWIND`) in the forced-unwind branch; a return of `7` (`_URC_INSTALL_CONTEXT`) triggers the
  landing sequence. Phase-1 (`actions = 1`) is in `_Unwind_SjLj_RaiseException` (+0xc3d) and is `UNVERIFIED:`
  here (not disassembled).
- **State location (measured).** The SjLj chain head is `tsd[18]`, i.e. the word at `(TPIDRURO & ~3) + 0x48`
  (`_Unwind_SjLj_Register`: `ctx->prev = TP[0x48/4]; TP[0x48/4] = ctx`; `_Unwind_SjLj_Unregister`:
  `TP[0x48/4] = ctx->prev`) — not a dynamic key and not a field of the GTC; it lives in the guest's own
  `_pthread` (offset `0x48 + 4*18 = 0x90`). The runtime never mirrors it. A crash reporter or the debugger
  that wants the chain reads guest memory at `tsd[0] + 0x90`.
- **JIT implications.** The lifter must (a) have the GTC hold all callee-saved registers before any call
  that may throw (§1.1), and (b) treat `setjmp`/`_setjmp`/`sigsetjmp` as `returns_twice`: no reordering of
  non-volatile locals across the call, and a reload from memory at the landing target. The JIT does not
  synthesise unwind tables for guest functions (there are none in iOS 6 armv7); re-raising through a
  JIT frame with no LSDA is legal and the frame is simply skipped.
- **`setjmp`/`longjmp` are guest code, not builtins.** They are defined in `libsystem_c.dylib`
  (`arm/sys/_setjmp.s`, `_longjmp.s`, …) and re-exported. Reasons: (i) `jmp_buf` is an ABI-visible
  112/116-byte structure that must match byte-for-byte; (ii) `setjmp`/`longjmp` (but not
  `_setjmp`/`_longjmp`) save and restore the signal mask, and the Mach-O name mapping is off by one
  (`__setjmp` = C `_setjmp`, `_setjmp` = C `setjmp`); (iii) a host implementation would use the host's
  `jmp_buf` layout and break guest binaries.

  **Measured jmp_buf layout** (`llvm-mc --disassemble` of the cache's `__setjmp` 0x39415cdc, `_setjmp`
  0x39415c8c, `__longjmp` 0x3942c6e0, `_longjmp` 0x3942c6a0):

  | offset | contents | instruction |
  |---:|---|---|
  | 0x00-0x23 | `r4,r5,r6,r7,r8,r10,r11,sp,lr` (9 words) | `__setjmp`: `stm r0!, {r4-r8,r10,r11,sp,lr}` |
  | 0x24-0x63 | `d8-d15` (64 bytes) | `vstmia r0, {d8-d15}` (r0 was advanced by 36) |
  | 0x64 | unused | — |
  | 0x68 | `sigset_t` | C `setjmp` (`_setjmp`): `mov r0,#1; mov r1,#0; add r2,r8,#104; <call>` — arguments match `sigprocmask(SIG_BLOCK, NULL, &jb[0x68])`; C `longjmp` (`_longjmp`): `ldr r0,[r0,#104]; <call>` — matches `sigsetmask(mask)` (callee names per research §6.2; the indirect-call targets were not resolved) |
  | 0x70 | `sigflag` (`sigjmp_buf` only) | research §6.1 |

  `__setjmp` returns `mov r0,#0; bx lr`; `__longjmp` reloads via `ldm r0!,{r4-r8,r10,r11,sp,lr}` +
  `vldmia r0,{d8-d15}`, then `movs r0,r1; moveq r0,#1; bx lr` (value 0 → 1). Size: `_JBLEN = 10+16+2 = 28`
  words = 112 bytes, `sigjmp_buf` 116 (research §6.1; the 0x68 offset + `sigset_t` 4 B + `sigflag` at 0x70
  is consistent with that). Not saved: `r9`, `r12`, `r0-r3`, `d0-d7`, `d16-d31`, `FPSCR`.

## 5. Signals & faults

- **Host signal → guest delivery.** The host installs handlers for `SIGSEGV`, `SIGBUS`, `SIGFPE`,
  `SIGILL`, `SIGTRAP` that look up the faulting guest address in the arena:
  - fault in the shadow region → [05-jit.md §2] materialisation path (not a guest signal);
  - fault in the guest arena → queue the corresponding guest signal (`SIGSEGV`/`SIGBUS`/…) and emulate
    `sigreturn` on delivery; run the guest handler on the guest's `sigaltstack` if installed;
  - otherwise → abort with a GTC dump (ADR-13).
- **Per-thread state.** A `sigaction` table (per guest process) and a per-thread `sigaltstack` (host
  memory backing a guest VA) are maintained by the kernel shim. `sigaltstack` is BSD syscall 53
  (`___sigaltstack` stub: `mov ip,#0x35; svc #0x80`, measured), `sigreturn` is BSD syscall 184
  (`___sigreturn`: `mov ip,#0xb8; svc #0x80`, measured; `bsd/kern/syscalls.master:280`
  `sigreturn(struct ucontext *uctx, int infostyle, user_addr_t token)`).
- **Delivery contract (measured from the guest's own `__sigtramp`, libsystem_c 0x39445e69).** The kernel
  half builds the frame on the guest stack (or the `sigaltstack`) and enters the user trampoline registered
  in `struct __sigaction.sa_tramp` with `r0 = catcher`, `r1 = infostyle`, `r2 = sig`, `r3 = siginfo_t*`,
  `[sp] = ucontext_t*` (5 arguments: `ldr r5,[r7,#8]` is the stack one). `__sigtramp` calls
  `catcher(sig)` if `infostyle == 1` (`UC_TRAD`) else `catcher(sig, siginfo, ucontext)`, then tail-calls
  `__sigreturn(ucontext, 0x1e)` (`movs r1,#0x1e` — value 30; xnu names it `UC_FLAVOR`, `UNVERIFIED:` — the
  2107 header is not in the local tree). `struct __sigaction` (`bsd/sys/signal.h:375`) is
  `{ sa_handler; sa_tramp; sa_mask; sa_flags }` = 16 bytes on ILP32. The shim implements `sigreturn` by
  copying `uctx->uc_mcontext` back into the GTC (and the VFP state if `uc_mcsize` includes it), clearing
  the exclusive monitor (§6) and resuming at the restored `pc`, re-establishing the Thumb state from
  `cpsr.T`.
- **Context layout.** Guest signal frames use the Darwin ARM `ucontext_t`/`mcontext_t`. Sizes derived from
  the XNU header definitions (`bsd/sys/_types/_ucontext.h`, `bsd/arm/_mcontext.h`,
  `osfmk/mach/arm/_structs.h`: `arm_thread_state` = `r[13], sp, lr, pc, cpsr` = 17 words,
  `arm_vfp_state` = `r[64], fpscr` = 65 words, `arm_exception_state` = `exception, fsr, far` = 3 words) and
  proven by compiling them:

  | type | layout | size |
  |---|---|---:|
  | `arm_thread_state` | `r[13]; sp; lr; pc; cpsr` | 68 |
  | `arm_vfp_state` | `r[64]; fpscr` | 260 |
  | `arm_exception_state` | `exception; fsr; far` | 12 |
  | `mcontext32` | `es; ss; fs` (12 + 68 + 260) | 340 |
  | `sigaltstack` | `ss_sp; ss_size; ss_flags` | 12 |
  | `ucontext_t` | `uc_onstack; uc_sigmask; uc_stack; uc_link; uc_mcsize; uc_mcontext` | 32 (+340 inline `__mcontext_data` if `_XOPEN_SOURCE`) |
  | `siginfo_t` | `si_signo, si_errno, si_code, si_pid, si_uid, si_status, si_addr, si_value, si_band, __pad[7]` | 64 |
  | `jmp_buf` / `sigjmp_buf` | `int[28]` / `int[29]` | 112 / 116 |

  Measured: the `static_assert`s for these rows are in the same `clang++ -std=c++20 -fsyntax-only` unit as
  the GTC asserts (§1) and pass. Which of the two `ucontext_t` forms the 6.1.3 kernel writes (`uc_mcontext`
  pointing to the inline copy vs a separate one) is `UNVERIFIED:`; `uc_mcsize` tells the shim (340 ⇒ full
  state) and guest code must follow `uc_mcontext`, not assume adjacency. There is no `struct sigcontext`
  in this contract: the handler gets `siginfo_t*` + `ucontext_t*` only.
- **Mach exception path.** Crash reporters (PLCrashReporter/KSCrash) register `task_set_exception_ports`
  / `thread_set_exception_ports` for `EXC_BAD_ACCESS`, `EXC_BAD_INSTRUCTION`, `EXC_ARITHMETIC`,
  `EXC_SOFTWARE`, `EXC_BREAKPOINT` and run a `mach_msg` server. The shim must provide real semantics for
  `task_threads`, `thread_get_state`/`thread_set_state`, `thread_suspend`/`thread_resume`,
  `task_get/set_exception_ports`, `mach_msg`, `mach_vm_read_overwrite`, `sigaltstack`, etc., and
  `thread_get_state` must be able to synchronise a suspended guest thread's registers from its GTC
  ([research/apple-armv7-abi.md §9]). `thread_set_state` with `ARM_THREAD_STATE` (17 words) and
  `ARM_VFP_STATE` (65 words) must round-trip `r0-r15`, `cpsr`, `d0-d31`, `fpscr`, including the Thumb
  bit in `pc`/`cpsr`. A Mach handler replying `KERN_SUCCESS` means the thread resumes with no BSD signal
  generated, so the shim must honour that. Minimum surface so reporters do not hang: see
  [research/apple-armv7-abi.md §9.3].

## 6. Timers / atomics

- **`OSSpinLock`** is guest code in `libsystem_c.dylib` (also used by libobjc). Measured slow path
  (0x3940f5a8, `_spin_lock`/`OSSpinLockLock` body): `ldrex r1,[r3]; cbz` → `strex` of the constant 1; when
  the lock is held it calls `_syscall_thread_switch(0, 1, 1)` = `thread_switch(MACH_PORT_NULL,
  SWITCH_OPTION_DEPRESS, 1 ms)` in a loop and **does not escalate to `SWITCH_OPTION_WAIT`** (the
  escalation described in research §8.2 belongs to a later libplatform; not present in this routine).
  Unlock is a plain `movs r1,#0; str r1,[r0]` with no barrier. The kernel shim must provide `thread_switch`
  (Mach trap 61) and `mach_absolute_time`/`mach_wait_until`; `os_unfair_lock` and `os_once` do *not* exist
  on iOS 6 and must not be offered.
- **STUB_AND_RESOLVER exports (loader contract).** 36 `libsystem_c` exports — all `_OSAtomic*Barrier`/non-
  barrier RMW and CAS entries, `OSSpinLock*`, `OSAtomicEnqueue/Dequeue` — are `EXPORT_SYMBOL_FLAGS_STUB_AND_
  RESOLVER` (flag `0x10`: two ulebs, stub offset then resolver offset, e.g. `_OSAtomicCompareAndSwap32Barrier`
  stub +0x794dc, resolver +0x1334d). The resolver (+0x1334c, Thumb) does
  `movt r2,#0xffff; movw r2,#0x1021; ldrb r2,[r2]; tst r2,#0x80; it ne; movne r0,r1; bx lr`, i.e. it reads the
  **commpage byte at guest VA 0xFFFF1021** and picks between two `ldrex/strex` bodies: bit 7 clear → body
  with `dmb ish`/`dmb ishst`, bit 7 set → the same body without barriers (uniprocessor variant).
  The binder must call the resolver and bind the returned address, and the synthesised commpage must
  map a byte at 0xFFFF1021 (value `0x00` selects the barrier bodies; `0x80` is also correct but loses the
  barriers). Open commpage questions stay in [14-open-questions.md Q-06](14-open-questions.md).
- **Exclusive monitor: algorithm.** Per-thread state is `{excl_addr, excl_size, excl_value}` in the GTC.
  - `LDREX{B,H,,D} Rt,[Rn]`: `excl_addr = Rn`, `excl_size = access size`, `excl_value = zero-extended
    value read` (the read itself is a single host atomic load; `LDREXD` requires 8-byte alignment, which
    makes the host 8-byte load atomic on x86-64).
  - `STREX{B,H,,D} Rd,Rt,[Rn]`: if `excl_size == access size && excl_addr == Rn`, perform
    `cmpxchg(*Rn, expected = excl_value, desired = Rt)` (`lock cmpxchg`/`cmpxchg8b` on the guest word);
    `Rd = success ? 0 : 1`. **In every case (success or failure, and including an address mismatch)
    `excl_size = 0` afterwards**, as the ARM ARM requires of Store-Exclusive; the guest re-arms by
    executing `LDREX` again. (The first draft left the monitor armed on failure; that differs from
    hardware only for guest code that executes a second `STREX` without an intervening `LDREX`, which is
    undefined behaviour in guest code, but clearing keeps the model exact.)
  - `CLREX` (never emitted in the cache: 0 occurrences, measured below) and *every exception entry*
    (signal delivery, `sigreturn`, `svc`/trap return, context switch) set `excl_size = 0`.
  - Barriers: `dmb ish` and `dsb` → `fence seq_cst`; `dmb ishst` → compiler barrier only (x86-TSO already
    orders stores); `isb` → compiler barrier (0 occurrences in the cache).

  **Why a plain host `cmpxchg` loop is acceptable, precisely.** Let a *window* be the instruction sequence
  between `LDREX` (reads `v` at address `a`) and the matching `STREX` (writes `n`). Call it **pure** if no
  instruction in it reads or writes memory (no loads, no stores, no calls) and `n` is a function of `v` and
  registers only. Claim: for a pure window, the `cmpxchg(*a, v, n)` emulation yields only outcomes that a
  real ARMv7 can produce. Proof: if the `cmpxchg` succeeds, `*a == v` at that instant `t`. Consider the
  hardware schedule in which this thread executes the whole window atomically at `t` (all of the window's
  operands are thread-local, so nothing in it observes any other thread). In that schedule `LDREX` reads
  `v`, nothing writes `a` before `STREX`, so the monitor is intact and `STREX` succeeds, storing `n` — the
  same final memory and registers. Hence success-with-ABA (other threads changed `a` away and back
  between the real `LDREX` and `t`) is indistinguishable from a legal hardware execution. If the
  `cmpxchg` fails, `*a != v`; hardware also fails in every schedule with that interleaving, and spurious
  `STREX` failure is architecturally permitted anyway. So the model is *sound* (never admits a behaviour
  hardware cannot show) and, for lock-free progress, *at least as live* as hardware. This is the C11
  `compare_exchange` argument; it covers everything clang emits for `__sync_*`/`__atomic_*` RMW on ≤4-byte
  objects and all `OSAtomicAdd/Or/And/Xor/CompareAndSwap/TestAndSet` bodies.
  **It does not hold when the window loads memory that the `STREX` value depends on** (the ABA problem):
  `ldrex head; ldr next,[head,#off]; strex head←next` succeeds under `cmpxchg` if `head` was popped, the
  next element popped, and the first re-pushed (head again equals `v`, but `next` is stale) — hardware
  would have failed because those intervening stores cleared the global monitor. (The first draft's claim
  that comparing against current memory "defeats ABA" is the reverse of the truth: a value compare is
  exactly what ABA defeats.)

  **Measured census** (throwaway Capstone script: every function range from `LC_FUNCTION_STARTS` of
  `__text` in all 569 images, Thumb/ARM chosen by the start address' bit 0; a window is "pure" if no load/store/call occurs
  between `ldrex*` and the next `strex*` within 15 instructions): 3,684 `ldrex*` sites (3,667 `ldrex`,
  13 `ldrexb`, 4 `ldrexd`; `strex*` 3,674/13/4+2; `clrex` 0, `isb` 0, `dsb` 3, `dmb` 9,895); 3,673 pure
  windows, 6 not matched within 15 instructions (all `libdispatch`; 5 of 6 inspected: loop-rotated, the
  `strex` precedes the `ldrex` in code order and the window is pure), **5 impure, all in `libsystem_c`**: the four `OSAtomicEnqueue`/
  `OSAtomicDequeue` bodies (barrier and non-barrier variants, 0x3942ea24, 0x3942ea34, 0x3942ec98,
  0x3942ecb0) and the `OSSpinLock` slow path (0x3940f5b0, its window contains the `thread_switch` call and
  the `STREX` is preceded by a fresh `LDREX`, so it is effectively pure). `OSAtomicEnqueue`'s window stores
  `elem->next = head` before the `STREX` (a store to a *different* address); it is ABA-safe under
  `cmpxchg` (a restored `head == v` still makes `elem->next == head` correct). **Only `OSAtomicDequeue`
  is ABA-unsafe** (`ldrex r_sb,[r0]; … ldr r3,[sb,r1]; strex r2,r3,[r0]`, bodies at 0x3942ea34 and
  0x3942ecb0).

  **Decision.** (1) Default lowering = M1 above (`cmpxchg`, no version counter). (2) The lifter classifies
  each `LDREX…STREX` window as pure/impure at lift time. Impure windows (4 sites in the whole cache) are
  not given the M1 lowering; instead `OSAtomicDequeue`/`OSAtomicEnqueue` are replaced through the hook
  mechanism ([04 §8](04-translation.md)) by a host implementation serialised by one process-global mutex,
  applied to the resolver-selected bodies (so the loader's override table is keyed by the export name
  `_OSAtomicDequeue`/`_OSAtomicEnqueue` and replaces the *resolver*, §6 bullet above). Both functions
  must be hooked together, since a host-locked `Dequeue` racing a lock-free guest `Enqueue` is again
  ABA-prone. `OSAtomicFifoEnqueue/Dequeue` are not exported by any of the 569 images (measured: no `Fifo`
  export except unrelated `IAP*`/`Gaia_Fifo` names), so there is nothing to hook. (3) Any impure
  window found in a user-supplied app emits a one-time `IOS6RECOMP: impure LDREX window at <va>` warning
  (ADR-13); `UNVERIFIED:` whether real apps contain hand-written LL/SC; the burn-down counter from
  chapter 12 answers it.
- **`__sync_*` / `__atomic_*`**: 32-bit and sub-word forms are inlined by clang into `ldrex`/`strex` +
  `dmb ish` and follow the monitor model; the 64-bit exclusives occur at only 4 `ldrexd` sites in the cache
  (all pure ALU windows under the same criterion), so 64-bit atomics need `cmpxchg8b` support but no
  helper hook. Claims about `libcompiler_rt` atomic helpers are removed: the cache's `libcompiler_rt` is
  12 KiB (research §3 lists what it does export).

## 7. Memory

- Guest `malloc` is guest code: `libsystem_c.dylib`'s malloc zones call `vm_allocate`/`mmap`, which the
  kernel shim must honour inside the arena ([ADR-02](02-decisions.md), [ADR-09](02-decisions.md)).
- Builtins allocate guest-visible memory only through `guest_heap::alloc(size, align)` (from
  `lib/guestmem`), which returns a `guest_addr_t` (i.e. `uint32_t`) pointing into the arena. Host
  `malloc`/`new` are never used for guest-visible data (ADR-03).
- `ios6/abi/` headers define `guest_addr_t`, `g_ptr<T>` and explicit-width integers; every struct that
  crosses the boundary carries `static_assert(sizeof(...) == N)` and `static_assert(alignof(...) == A)`
  generated from [research/apple-armv7-abi.md](../research/apple-armv7-abi.md) (4-byte alignment for
  `double`/`long long`, `long double` = `double`, ILP32 pointers). `tests/abi/` recomputes sizes using
  the ARMv7 rules and fails the build on divergence.

## 8. Debuggability

- **Guest backtrace** is a frame-pointer walk via `r7`: `[r7]` = caller's r7, `[r7+4]` = return address,
  epilogue `mov sp,r7; pop {r7,pc}`. This is valid on iOS 6 because `r7` is the mandated frame pointer
  and there is no `__unwind_info`/`__eh_frame` anywhere in the 569 cache images
  ([research/apple-armv7-abi.md §5.1](../research/apple-armv7-abi.md), §9.2). Crash reporters depend on it.
- **`IOS6RECOMP_BLOCKDUMP=va`** dumps decode, IR and host code for one block; `IOS6RECOMP_LIFT_STATS=1`
  reports the missing-opcode table. These are the primary debugging surfaces (ADR-13).
- **GDB remote stub:** tier 0 (interpreter) only. The interpreter is the breakpoint/step engine; JIT'd
  blocks are not directly steppable. A tier-0 run with `IOS6RECOMP_TIER=interp` is the supported
  debugging configuration; the stub speaks the GDB remote serial protocol over a TCP socket and exposes
  guest registers from the GTC and guest memory from the arena.

## 9. Verification

Ties to milestones and [12-testing.md](12-testing.md):

- **M2:** `GuestThreadContext` layout test (`tests/runtime/test_gtc_layout.cpp`) asserts every offset and
  size in §1; `tests/abi/` asserts ILP32 struct rules (ADR-03).
- **M2/M3:** `tests/runtime/test_call_guest.cpp` — nested guest→native→guest→native to depth 4 returns
  correct values; sentinel-return path exercised.
- **M3:** tier 0 vs tier 1 agreement including cpsr/fpscr at every block boundary
  ([11-milestones.md](11-milestones.md) M3 exit).
- **M4:** fixtures required and their acceptance:
  1. `setjmp`/`longjmp` round-trip through three frames, including `_setjmp`/`_longjmp` (no mask) vs
     `setjmp`/`longjmp` (mask restored), and a `siglongjmp` out of a signal handler installed with
     `sigaltstack`.
  2. SjLj C++ `throw`/`catch` through 3 frames, including a destructor on the skipped frame running.
  3. ObjC `@try`/`@catch`/`@finally` round-trip.
  4. Signal handler + `sigreturn`: `SIGSEGV` delivered to a handler that reads `ucontext_t`, then
     returns; nested signal on altstack.
  5. 32-thread atomics stress: `OSAtomic*` and a `dispatch_once` gate under contention, verified against
     a reference oracle.
  6. TSD destructor order: `pthread_key_create` destructors run to iteration 4 in the documented order.
  7. `OSSpinLock` contention test exercising the `thread_switch` fallback.
  8. `OSAtomicEnqueue`/`OSAtomicDequeue` ABA stress: 8 threads pop/push 64 elements for 10⁷ operations with
     a payload checksum; must never lose or duplicate an element (exercises the §6 hook decision; fails
     with the plain `cmpxchg` lowering if the hook is removed).
  9. Thread start ABI: a guest thread created by `pthread_create` observes `pthread_self() == tsd[0]`,
     `errno` at `TP[-1]` (`&errno == (char*)pthread_self() + 0x44`), and `pthread_getspecific` on a fresh key
     returning `NULL`; `sigaltstack`/`sigreturn` trap numbers 53/184 round-trip (§2, §5).
- **M4:** crash-reporter smoke: PLCrashReporter/KSCrash init + a deliberate `*(int*)0 = 0` records a
  backtrace via the r7 walk without hanging (ties to [research/apple-armv7-abi.md §9.3]).
- **M8:** AOT mode runs the above with JIT disabled and no RWX mappings.
