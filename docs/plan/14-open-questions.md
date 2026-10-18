# 14 — Open questions

Every item has an owner chapter, the **experiment** that closes it, the **decision rule** (so the
experiment's result maps to an action without another debate), and the milestone where it must be
closed. Items resolved by measurement move to the owning ADR and are deleted here, not left as
history. Ordered by how soon they block work.

| Id | Question | Closes at |
|---|---|---|
| Q-01 | Which slot size `E` (and does the kernel accept a 1–4 TiB `PROT_NONE` reservation)? | M3.5 |
| Q-02 | Do we ever need real guest processes (Unix-socket `mach_msg`)? | M6 |
| Q-03 | Which libc leaf routines deserve host hooks, and what is the real gain? | M7 |
| Q-04 | Real UIKit as guest vs native shim: when do we cross over? | M5 |
| Q-05 | Main-thread stack base and initial stack/`apple[]` layout | M1 |
| Q-06 | Commpage field map for armv7 libsystem | M1–M2 |
| Q-07 | Does CFRunLoop on iOS 6 use `mk_timer_*` traps, and how must they be emulated? | M4 |
| Q-08 | True `sysent` / `mach_trap_table` contents for xnu-2107 | M2 |
| Q-09 | Exact libdispatch ↔ pthread workqueue contract (`workq_kernreturn`, `wqthread` entry) | M4 |
| Q-10 | HFS-compressed on-disk apps: extractor support and the "first real app" corpus | M5 |
| Q-11 | Does `ld64.lld` upstream ever gain armv7 Mach-O, replacing `ios6recomp-ld`? | — |
| Q-12 | How much of NEON do real iOS 6 binaries actually execute? | M2 |
| Q-13 | How faithful must the Mach-O `LC_ENCRYPTION_INFO` story be for sideloaded decrypted apps? | M5 |
| Q-14 | Project licence | before first public commit |
| Q-15 | Capstone vs LLVM MC disagreement allow-list contents | M2 |
| Q-16 | Are there per-device (iPad2,1 vs iPhone5,x/armv7s) behaviours that need a second cache? | M6 |
| Q-17 | Are the target apps `MH_PIE`? (apps link at `__TEXT` 0x1000, below `mmap_min_addr`) | M0 |
| Q-18 | Does iOS 6 run with `SCTLR.A` = 0 (unaligned `ldr`/`str` allowed; `ldm`/`ldrd` fault as under Linux)? | M2 |
| Q-19 | Does the arena/shadow reservation survive strict overcommit and a memory-limited container? | M3.5 |

---

## Q-01 — Shadow-region slot size

- **Why open.** ADR-04 is a SPIKE. The reservation is `2^(32+E)` bytes; `E ≥ 15` cannot exist in a 47-bit
  address space and `E ≤ 12` is the ceiling. Candidates `E ∈ {6, 8, 10}`.
- **Partly measured.** On the dev box (`vm.overcommit_memory = 0`, `ulimit -v` unlimited, 48-bit VA)
  `mmap(K = 1 TiB, 2^(32+E), PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE)` succeeds for
  `E` = 6, 8, 10, 12 (`/tmp/aud/r.c`); under `ulimit -v 100000000` the 256 GiB and 1 TiB cases fail
  ([ADR-04](02-decisions.md)). What stays open is the container case (Q-19).
- **Experiment.** (1) Repeat the reservation in a container and under strict overcommit (Q-19). (2) Lift ≥ 10⁴ real blocks (from the 569-image cache) and histogram
  x86-64 bytes per block. (3) Measure fault-driven materialisation cost.
- **Decision rule.** Smallest `E` whose spill rate is < 5 % of block entries *and* whose reservation the
  kernel accepts. If no `E` meets both, or materialisation costs > 10 µs, switch to the page-table
  fallback ([05-jit.md §3](05-jit.md)) — the interface is identical, nothing else changes.

## Q-02 — Guest processes

- **Why open.** ADR-08 starts single-process (guest = the process, daemons = native libraries with a Mach
  facade). Some apps fork helpers or talk to a daemon over a port we cannot fake.
- **Experiment.** After M5, run the 5 target apps and list every `fork`/`posix_spawn`/`bootstrap_look_up`
  whose target we answered with a refusal. Count apps blocked by each.
- **Decision rule.** If ≥ 2 of 5 apps are blocked by the same missing multi-process behaviour, design the
  Unix-socket `mach_msg` mode (Darling's architecture, reimplemented — [research/prior-art.md §1](../research/prior-art.md));
  otherwise keep refusing loudly.

## Q-03 — Host hooks for libc leaf routines

- **Experiment.** Profile M4 fixtures and one UIKit-ish workload: what fraction of guest cycles is in
  `memcpy/memmove/memset/strlen/strcmp/bzero` and `OSAtomic*`? Prototype host hooks for the top three
  via the `hooks.td` mechanism ([04 §8](04-translation.md)) and re-measure.
- **Decision rule.** Keep a hook only if it removes ≥ 3 % of runtime *and* passes the differential fuzz
  against the guest implementation (same bytes out for random overlapping inputs).

## Q-04 — UIKit: shim or guest

- **Measure.** At M5 start, run a minimal UIKit app with the shim and with guest UIKit over stub
  services; compare (a) selectors/classes unimplemented, (b) distinct `com.apple.*` Mach services
  contacted, (c) time-to-first-frame.
- **Decision rule** ([08-frameworks.md §5](08-frameworks.md)): move to guest UIKit when the shim's gap list for
  the target apps exceeds ~150 selectors or the services list shrinks to ≤ 6 facades we can build.

## Q-05 — Main-thread stack and initial stack layout

- **Known.** The shared cache occupies `0x30000000+`; user stacks land just below it in device
  crash logs, but the exact base is not measured ([03-loader.md §5](03-loader.md)).
- **Experiment.** Read xnu-2107 `exec_copyout_strings`/`load_machfile` (`~/src/xnu*`) and confirm in the
  kernelcache; for the base, disassemble the kernel's `thread_userstack` for ARM.
- **Decision rule.** Use the kernel's layout verbatim (it is what libdyld/`start` expect for
  `argc/argv/envp/apple[]`); the stack base only needs to avoid collisions.

## Q-06 — Commpage

- **Known.** libsystem reads the ARM commpage; a raw-constant scan of `0xffff4xxx` found nothing in
  libsystem_kernel text ([03-loader.md §5](03-loader.md)), so reads use PC-relative literals.
- **Experiment.** Disassemble `_mach_absolute_time`, `_mach_timebase_info`, `_sysctl*` helpers and
  `__bzero`/`__memcpy` dispatch in libsystem_c and libsystem_kernel; list every literal pool value in
  `0xffff0000–0xffffffff`; cross-check against xnu-2107 `osfmk/arm/commpage` (`_COMM_PAGE_*`).
- **Decision rule.** Synthesize exactly the fields that are read; log any read of an unmodelled field
  as a strict-mode failure.

## Q-07 — `mk_timer_*` and CFRunLoop

- **Experiment.** `grep` the cache for `_mk_timer_create`/`_mk_timer_arm` users; trace CFRunLoop
  timer fixtures on the interpreter with `IOS6RECOMP_SYSCALL_TRACE`.
- **Decision rule.** If CF uses `mk_timer` ports (traps 91–94), implement them as host `timerfd`s
  surfaced as Mach port receive rights with a notification message; otherwise skip ([07 §2](07-kernel-boundary.md)).

## Q-08 — Real kernel tables

- **Status.** Appendix A/B of [07-kernel-boundary.md](07-kernel-boundary.md) are being re-extracted from
  the real xnu-2107 kernelcache; until then rows derived from xnu-2422 sources are marked UNVERIFIED.
- **Decision rule.** The kernel binary wins over any source tree. A syscall number absent from the
  2107 table is `ENOSYS` even if a later xnu defined it.

## Q-09 — Workqueue contract

- **Experiment.** Disassemble libsystem_c's `pthread_workqueue_*` and libdispatch's
  `_dispatch_worker_thread*`; read xnu-2107 `bsd/kern/pthread_synch.c`; fixture: 10⁶ `dispatch_async`
  from 8 threads with counting.
- **Decision rule.** Host thread pool with a guest `wqthread` entry, bounded by `hw.ncpu`; thread
  creation must follow the `bsdthread_register` ABI recorded at registration time.

## Q-10 — HFS-compressed apps and the first real-app corpus

- **Known.** The rootfs contains real, **unencrypted** armv7 apps (MobileSafari, SpringBoard,
  MobileMail …) — the only app binaries we can use without a jailbroken device (ADR-12). Their
  executables are HFS-compressed (decmpfs): the data fork is empty and the kernel HFS driver shows
  0 bytes ([research/ios6-platform.md §3](../research/ios6-platform.md)).
- **Status.** `internal/utils/hfs_decmpfs.go` (+ `_test.go`) is present in the `~/src/ipsw` working tree
  (`git status`, 2026-10-17); verification against `7z` byte-for-byte is the acceptance test in
  [13-risks.md R-17](13-risks.md). UNVERIFIED here: that zlib types 3 and 4 are the only types iOS 6 uses.
- **Decision rule.** M5's "first real app" is chosen from this set by smallest closure; if decmpfs
  support slips, `7z x rootfs.img` is the fallback extractor and the plan does not change.

## Q-11 — Upstream `ld64.lld` for armv7

- **Why.** ADR-14's in-tree linker is a stop-gap. Track LLVM's `lld/MachO` for 32-bit ARM; if it lands,
  retire `ios6recomp-ld` after the fixtures link identically (same load commands, same bytes in
  `__TEXT`).

## Q-12 — NEON usage

- **Experiment.** Count NEON (Q/D register data-processing, `vld/vst`) instructions in `__TEXT` of all
  569 images with the LLVM MC decoder; weight by static count, then by dynamic count in the M4
  fixtures.
- **Decision rule.** Implement NEON semantics in descending order of dynamic count until the
  `lift.missing` table is quiet for the target apps; do not write all ~200 semantics up front
  ([04 §1](04-translation.md)).

## Q-13 — Sideloaded app encryption state

- **Known.** App Store binaries are `cryptid=1` and are refused (ADR-12). Users supply decrypted
  binaries; some "decrypted" dumps keep `LC_ENCRYPTION_INFO` with `cryptid=0`, some drop it.
- **Decision rule.** Accept both; refuse `cryptid != 0` with `E_ENCRYPTED_BINARY` and an instruction
  message. Add a fixture for each form.

## Q-14 — Licence

Decide before the first public commit; constraints in [research/prior-art.md §7–§8](../research/prior-art.md):
nothing GPL-derived enters the tree (Darling, QEMU, Unicorn), Apple-sourced APSL code is read for
behaviour but not copied, LLVM/Capstone/lld are permissively licensed. A permissive licence
(Apache-2.0 with LLVM exception, matching our largest dependency) keeps AOT outputs unencumbered.

## Q-15 — Decoder disagreement allow-list

Seeded by the measured LLVM-MC↔Capstone differential ([research/llvm-tooling.md §6](../research/llvm-tooling.md):
0 decode misses on ARM, 6 disagreements in coprocessor/undefined space; Thumb halfword-resync artefacts
are a test-harness issue, not a decoder disagreement). Each entry records encoding, both decodes, and
which decoder is right (verified against `qemu-arm`). Target: allow-list < 50 entries at M2.

## Q-16 — Second device family

iPad2,1 is armv7 (Cortex-A9); iPhone 5 / iPad 4 are armv7s with a different cache
(`dyld_shared_cache_armv7s`). The loader and the lifter handle both subtypes; what is open is whether
behavioural differences (IDIV, VFMA, FP16, different `hw.cpufamily` values) change any answer the
kernel shim gives. Close by extracting an armv7s IPSW through the same `ipsw extract` path and diffing
`hw.*` sysctl answers and the `swift`-only instruction census.

## Q-17 — Are the target apps `MH_PIE`?

- **Why.** Real iOS 6 apps declare `__PAGEZERO` 0x1000 and `__TEXT` at 0x1000
  ([research/ios6-platform.md §7](../research/ios6-platform.md)); the host forbids mappings below
  `vm.mmap_min_addr` (65536 here), so a non-PIE app cannot be loaded at its link address
  ([ADR-02](02-decisions.md), R-18). `MH_PIE` (0x200000) was not measured on the rootfs apps.
- **Experiment.** Read `flags` of MobileSafari/SpringBoard/MobileMail (extracted with the decmpfs-capable `ipsw`,
  R-17) and of the first user-supplied app with `ios6recomp macho info`.
- **Decision rule.** All PIE: slide to ≥ 0x10000, nothing else. Some non-PIE: document
  `sysctl vm.mmap_min_addr=4096` as a requirement for those apps and refuse with that message otherwise;
  do not rebase non-PIE code (absolute addresses are baked into its data).

## Q-18 — Unaligned access on iOS 6

- **Why.** [04 §7](04-translation.md) emits `align 1` for `ldr/str/ldrh/strh` and a guest `SIGBUS` for
  unaligned `ldm/stm/ldrd/strd/ldrex/vldr`. Measured under `qemu-arm` (Linux semantics, `SCTLR.A` = 0)
  that `ldr`/`ldrh` succeed and `ldm`, `ldrd`, `ldrex`, `vldr`, `swp` raise SIGBUS; iOS 6's setting is not measured.
- **Experiment.** Search the xnu-2107 kernelcache/source (`~/src/xnu`, `osfmk/arm/`) for the `SCTLR`/`cp15` setup
  (the `mcr p15, 0, rX, c1, c0, 0` writes), or run the unaligned test on a device.
- **Decision rule.** `A` = 0: as implemented. `A` = 1: every unaligned access faults; emit alignment checks everywhere.

## Q-19 — Reservations under strict overcommit and containers

- **Experiment.** In a VM with `sysctl vm.overcommit_memory=2` and in a container with a cgroup memory limit,
  run `/tmp/aud/m.c` (arena) and `/tmp/aud/r.c` (shadow) and then `mprotect` 1 GiB of the arena to RW.
- **Decision rule.** If the PROT_NONE reservations succeed and only the RW `mprotect` is charged, document the
  commit-limit requirement; if the reservations fail, the page-table fallback ([05-jit.md §3](05-jit.md)) becomes
  the default for restricted environments and the arena reservation is shrunk to what the guest has mapped.
