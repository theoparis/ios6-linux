# 13 — Risk register

Each risk has an id (referenced from other chapters), a trigger that tells us it is materialising, a
mitigation that is already part of the plan, and the milestone where it is retired or priced.
Severity = impact × likelihood (H/M/L). Ordered by severity, then by how early it bites.

| Id | Risk | Sev | Retired at |
|---|---|---|---|
| R-01 | `objc_msgSend` / ObjC ABI fidelity | H | M4 |
| R-02 | Translator throughput makes the 53.6 MB UIKit closure unusable | H | M3.5 / M7 |
| R-03 | Exception / `longjmp` / signal state across native frames | H | M3–M4 |
| R-04 | Shadow-region design (ADR-04) fails its spike | M | M3.5 |
| R-05 | Mach IPC fidelity (libdispatch, CFRunLoop, `mk_timer`, notifyd) | H | M4 |
| R-06 | Real UIKit as guest needs a window server we do not have | H | M5 |
| R-07 | Missing in-tree Mach-O linker delays every fixture | M | M1 |
| R-08 | GLES 1.1 / driver-extension fidelity (PowerVR SGX) | M | M5 |
| R-09 | Threading: pthread workqueue (`workq_*`) and `psynch_*` semantics | H | M4 |
| R-10 | Legal / EULA exposure | M | continuous |
| R-11 | Patched `ipsw` is a fork we must carry | L | M0 |
| R-12 | Self-modifying code and guest JITs (JavaScriptCore) | M | M6 |
| R-13 | Floating-point and NaN/flag semantic drift (VFP vs SSE) | M | M2–M3 |
| R-14 | Compile latency / cold start | M | M3, M7 |
| R-15 | Apps depend on state we synthesise (jailbreak/sandbox checks, `MobileGestalt`) | L | M6 |
| R-16 | Bus factor / scope: 21–36 engineer-weeks before a real app renders ([11-milestones.md](11-milestones.md)) | M | continuous |
| R-17 | The only offline-available real apps (rootfs) are HFS-compressed (decmpfs); extractors silently yield empty or truncated Mach-Os | M | M0 (extractor), M5 (corpus) |
| R-18 | Host environment refuses the address-space reservations (`mmap_min_addr`, `RLIMIT_AS`, overcommit, container limits) or real apps link below `mmap_min_addr` | M | M0, M1 |

---

## R-01 — ObjC ABI fidelity

**Why it is a risk.** Every iOS app lives inside `objc_msgSend`. libobjc is objc4-532 with its own
cache layout (`objc_cache{mask@0,occupied@4,buckets@8}`, `sel>>2` index, Thumb-tagged IMPs, no tagged
pointers; [research/apple-armv7-abi.md §7](../research/apple-armv7-abi.md)). Because ADR-09 keeps libobjc as guest
code, layout fidelity is free; the risk moved to *performance* and to the loader-time contract
(`_dyld_register_func_for_add_image` callbacks, `__objc_*` section registration).

**Trigger.** `NSLog`/`+initialize` ordering divergences; a selector-cache miss rate that does not
settle; guest `objc_msgSend` > 15 % of cycles in the M4 profile.

**Mitigation.** Keep libobjc's own messenger and JIT it well; interpose only the cache-hit path if the
profile demands ([08-frameworks.md §2](08-frameworks.md)). Loader calls the add-image callbacks for
every cache image ([03-loader.md §4](03-loader.md)). M4's fixture list covers category, super, stret,
`NSInvocation`, and autorelease pools.

## R-02 — Translator throughput vs. the size of the real code base

**Why.** A Foundation-only CLI app pulls **41 images / 10.6 MB** of code; a UIKit app **106 images /
53.6 MB** ([08-frameworks.md §1](08-frameworks.md)). Even touching 5 % of that cold is ~2.7 MB of ARM
(~700 k instructions), at a first-pass compile cost of 300 µs–2 ms per block ([05-jit.md §5](05-jit.md)). A naive
per-basic-block LLVM pipeline is the classic recompiler failure: correct but 20 s to the first frame.

**Trigger.** Time-to-first-`main()` for the Foundation fixture > 5 s; time-to-first-frame for a
UIKit app > 30 s warm.

**Mitigation.** (1) Tier 0 interpreter handles cold code; only counted-hot blocks (≥ 1000 executions)
go through LLVM ([05 §5](05-jit.md)). (2) Persistent cache keyed by image UUID + page hash ([05 §6](05-jit.md));
second launch skips lifting entirely. (3) AOT mode (M8) translates the whole 106-image closure
offline once. (4) Worker-pool speculative compilation of discovered successors. (5) The M3.5 spike
measures bytes-per-block and cold-start cost before the design is locked.

## R-03 — Guest state across native frames (exceptions, `longjmp`, signals)

**Why.** Exceptions on iOS 6 are SjLj ([research/apple-armv7-abi.md §5](../research/apple-armv7-abi.md)): a throw
unwinds by *longjmp-like* transfers through registered guest contexts. If a native (host) frame sits
between the throw and the landing pad — e.g. a guest callback invoked by a builtin `qsort`, or a
`dispatch_async` block that throws — control returns into guest code while the host stack still holds
the native frame, corrupting the host stack. Signals and `longjmp` have the same shape.

**Trigger.** Any intermittent host-stack corruption; the native-frame-depth marker check firing.

**Mitigation.** ADR-06 + [06-runtime-core.md §3–§4](06-runtime-core.md): one `call_guest` trampoline,
per-thread native-frame depth markers, detection → diagnostic abort (never silent). Most builtins that
call back into guest are avoided by running libc as guest code (ADR-09), so the set of native frames
that can sit under a guest frame is small and auditable. Fixtures: C++ throw through 3 frames,
signal handler + `sigreturn`, `longjmp` out of a signal.

## R-04 — ADR-04 shadow region fails its spike

**Why.** `host = K + (a << E)` needs a sparse reservation of `2^(32+E)` virtual bytes (`a` = interworking
address). With a 47-bit x86-64 user address space `E` ≥ 15 is impossible and `E` ≤ 12 (16 TiB) is the
practical ceiling. Candidates are `E` ∈ {6, 8, 10}: small slots spill more blocks into the
out-of-line pool (a jump per entry), large ones spend address space. The spike must pick `E` from the
measured block-size distribution. Kernel acceptance of a 1–4 TiB `PROT_NONE` `MAP_NORESERVE`
reservation is already measured on the dev box (`E` = 6, 8, 10, 12 all succeed at `K` = 1 TiB with
`vm.overcommit_memory = 0`, `ulimit -v` unlimited, [ADR-04](02-decisions.md)); it fails under `ulimit -v 100000000`.
UNVERIFIED: acceptance under strict overcommit (mode 2) and inside a container with a memory cgroup /
`RLIMIT_AS` (R-18).

**Trigger.** Spike shows a spill rate ≥ 5 % of block entries for every `E` whose reservation the kernel
accepts ([Q-01](14-open-questions.md)), or fault-driven materialisation costs > 10 µs per block.

**Mitigation.** Fallback table design is already specified ([05 §3](05-jit.md)); both present the same
`BlockTable` interface, so the translator and the runtime do not change.

## R-05 — Mach IPC fidelity

**Why.** libdispatch (main queue), CFRunLoop (ports, `mk_timer_*` traps 91–94), notifyd, and
`bootstrap_look_up` all run on Mach messages. The semantics that matter (timeouts, `MACH_RCV_LARGE`,
port sets, dead-name notifications, out-of-line descriptors) are easy to approximate and hard to get
exactly right; an approximation shows up as a hung run loop, not a crash.

**Trigger.** `CFRunLoopRun` never wakes; a `dispatch_main` fixture hangs; the stub log shows an
unserved `com.apple.*` bootstrap name.

**Mitigation.** [07-kernel-boundary.md §2](07-kernel-boundary.md) lists the measured `com.apple.*`
service names and the native-vs-refuse decision for each; M4's `CFRunLoop` timer + mach-port source
fixtures; Darling's `darlingserver` and `xnu/osfmk/ipc` are the reference for semantics
([research/prior-art.md §1](../research/prior-art.md)) — read, do not copy (GPL).

## R-06 — Real UIKit needs a window server

**Why.** UIKit expects `backboardd`/FrontBoard services, `CARenderServer`, GSEvent delivery and
`IOSurface`-backed layers. Running it as guest drags in a window-server protocol we would have to
implement. The minimal native UIKit shim avoids that but diverges from real UIKit behaviour (layout,
animation timing, `UIView` subclass hooks apps override).

**Trigger.** The shim's list of unimplemented UIKit selectors exceeds what a target app touches at
launch by an order of magnitude; apps crash on `-[UIView _private…]`.

**Mitigation.** Staged strategy in [08-frameworks.md §5](08-frameworks.md): shim first (fast to a
first frame), measure UIKit's real Mach-service dependencies, then migrate to guest UIKit +
native `CARenderServer`/`backboardd` facades when the shim's gap list says so. Gate decision on
measured data at M5, not before.

## R-07 — No linker for armv7 Mach-O fixtures

**Why.** `ld64.lld` rejects `-arch armv7` on this host, so the plan's original `-fuse-ld=lld`
fixture build does not exist ([12-testing.md §2](12-testing.md)). ADR-14 commits to an in-tree minimal
linker, which is real work (1–2 weeks) on the critical path of M1.

**Trigger.** Relocation kinds clang emits for the fixture corpus exceed the supported set; fixtures
that need dyld lazy binding slip.

**Mitigation.** Fixtures start freestanding (raw `svc`, no imports) — those need only `__TEXT`,
`__DATA` and `LC_UNIXTHREAD`; imports are added incrementally (M4). Output validated against
`llvm-objdump --macho -p`. Fallback: hand-assembled Mach-O produced by a Python script for the very
first fixtures.

## R-08 — GLES fidelity

**Why.** iOS 6 devices expose an SGX543MP2 with PowerVR/Apple extensions; apps probe the extension
string and some rely on `GL_OES_*`/`GL_IMG_texture_compression_pvrtc`. GLES 1.1 fixed-function is not
in Mesa's GLES1 on every driver.

**Trigger.** Black frames in GLES1 apps; shader compile failures on `precision` qualifiers.

**Mitigation.** [09-media.md §1](09-media.md): Mesa GLES with a GLES1.1 fixed-function emulation
layer (touchHLE's design); advertise only extensions we can honour (PVRTC → software decode). Visual
verification against golden frames when the user supplies them.

## R-09 — pthread workqueue and `psynch_*`

**Why.** libdispatch on iOS 6 uses `workq_open`/`workq_kernreturn` and `bsdthread_register` (the
kernel calls back into a userspace `wqthread` entry). `pthread_mutex`/`cond`/`rwlock` sit on
`psynch_*` syscalls with generation-counter protocols (syscalls 297–312). These are kernel-internal
contracts, not documented APIs.

**Trigger.** A `dispatch_async` fixture deadlocks or spawns unbounded threads; a `pthread_cond_wait`
misses a wakeup.

**Mitigation.** [07 §3](07-kernel-boundary.md): implement `psynch_*` on futexes following xnu's
`bsd/kern/pthread_synch.c` generation-count semantics (syscall numbers 297–312 are from the local `~/src/xnu` tree's
`syscalls.master`; the xnu-2107 table is not yet confirmed, [Q-08](14-open-questions.md)); implement the workqueue as a bounded host
thread pool that enters the guest `wqthread` entry. Stress fixtures (32 threads × atomics, 10⁶
`dispatch_async`).

## R-10 — Legal

**Why.** The iOS 6 SLA restricts use to Apple-branded hardware; redistributing Apple binaries is not
allowed ([research/prior-art.md §8](../research/prior-art.md)).

**Mitigation.** ADR-12 and the input contract: nothing Apple-derived in the repo or in any release
artefact; users supply their own IPSW and apps; the firmware key database is third-party public data
we fetch at run time, never ship. Do not borrow code from GPL projects (Darling, QEMU, Unicorn) —
architecture lessons only. Choose the project's own licence deliberately and early
([10-build-tooling.md](10-build-tooling.md)). This is not legal advice; get one real review before the first public release.

## R-11 — Carrying a patched `ipsw`

**Why.** The input pipeline depends on fixes that are not (yet) upstream in blacktop/ipsw. Measured
hazard: the prebuilt `~/src/ipsw/ipsw` (also installed as `/usr/bin/ipsw`) predates the fixes and fails
both extract commands; only a binary rebuilt from the working tree succeeds
([10-build-tooling.md §4](10-build-tooling.md)).

**Mitigation.** The fixes are small, tested and self-contained (userspace HFS extraction, `encrcdsa`
plumbing, IMG3 tag-pad decrypt, IMG3 kernelcache). Upstream them; until then pin the commit (the working tree has 14 uncommitted changed files) and keep
`scripts/prepare-inputs.sh` building from source and checking the output size and sha1.

## R-12 — Self-modifying code / guest JITs

**Mitigation.** Page-granular write-fault invalidation ([05-jit.md §7](05-jit.md)); a debug mode that
retranslates on every execution isolates "translation wrong" from "invalidation wrong". JavaScriptCore
is out of scope for the first apps ([08 §7](08-frameworks.md)).

## R-13 — FP drift

**Mitigation.** Mirror FPSCR into MXCSR; no fusing for armv7 code; per-instruction differential fuzz
against `qemu-arm` including NaN/denormal/rounding-mode corners ([12-testing.md §3](12-testing.md)); strict-FP
mode with constrained intrinsics for triage.

## R-14 — Compile latency

Covered by R-02; additionally enforce the 05-jit latency budgets (p50 ≤ 300 µs, p99 ≤ 2 ms) as a
test, and split oversized blocks.

## R-15 — Synthesised world detected by apps

`MobileGestalt`, jailbreak probes (`/Applications/Cydia.app`, `fork()` success), sandbox-path
checks. Return the stock-device answers; `fork` fails like a sandboxed app. Tracked in
[07 §5](07-kernel-boundary.md).

## R-16 — Scope and sequencing

The plan front-loads cheap falsifiers (M0 platform facts, M3.5 address-mapping spike, M4 ObjC fixture)
so that a failed assumption costs weeks, not months. If R-02 or R-04 trips and the fallback also
fails, the escalation is: drop JIT-everywhere for AOT-only (ADR-10 already makes the artefacts
identical) and accept slower cold start.

## R-17 — HFS-compressed rootfs binaries

**Why.** The rootfs is the only source of real, unencrypted app binaries that does not need a
jailbroken device (ADR-12), and its executables are stored with HFS+ compression (decmpfs): the data
fork is empty, so the Linux `hfsplus` driver shows size 0 even though `7z` on the decrypted DMG reports
the real size (`MobileSafari` 1,128,816 B logical vs 524,288 B allocated;
[research/ios6-platform.md §3.2, §3.6](../research/ios6-platform.md)). A pipeline that concludes "empty
stub" or extracts only the data fork yields a 0-byte or truncated Mach-O. App Store IPAs are plain zip
payloads and are not affected; the dyld shared cache is not compressed (extracted whole). Without a
decmpfs-capable extractor the "first real app" corpus (Q-10) reduces to what `7z` can extract by hand.

**Trigger.** An extracted executable has size 0, a Mach-O header with `filesize` larger than the file, or
differs from `7z x` output.

**Mitigation.** Decmpfs support in the patched `ipsw` userspace extractor (`hfs_decmpfs.go`, zlib types
3 and 4, [10-build-tooling.md §4](10-build-tooling.md)); acceptance test: extract `MobileSafari` and require
1,128,816 B, magic `0xfeedface`, byte-identical to `7z x rootfs.dec <member>` ([Q-10](14-open-questions.md)). `scripts/prepare-inputs.sh`
asserts the sizes of the three reference apps. Fallback: `7z x` as extractor.

## R-18 — Host refuses the address-space reservations

**Why.** ADR-02/04 depend on three kernel/host properties: the arena `[mmap_min_addr, 4 GiB)` is free,
a 256 GiB–4 TiB `PROT_NONE` reservation is accepted, and guest images map at their link addresses.
Measured on this host: `mmap(0, 4 GiB, …)` → `EPERM` (so the arena starts at `vm.mmap_min_addr` = 65536),
reservations succeed at `K` = 1 TiB for `E` ≤ 12, and `ulimit -v 100000000` blocks the shadow region while still admitting the
arena ([ADR-02, ADR-04](02-decisions.md)). Real apps declare `__PAGEZERO` 0x1000 and `__TEXT` at 0x1000
(measured), which is below `mmap_min_addr`, so a non-`MH_PIE` app cannot be loaded at its link address on a
stock host.

**Trigger.** `MAP_FIXED_NOREPLACE` failure at startup; loader error naming `vm.mmap_min_addr`; a CI
container with a memory limit.

**Mitigation.** Startup check prints the failing range and the governing limit (`vm.mmap_min_addr`,
`RLIMIT_AS`, `vm.overcommit_memory`); PIE apps are slid above 64 KiB; non-PIE low-base apps are refused with the sysctl hint
(needs root to lower). UNVERIFIED: how many target apps are `MH_PIE` (read `flags` of the first
user-supplied app in M0); if most are not, escalate by running under a user namespace or documenting `sysctl vm.mmap_min_addr=4096`.
