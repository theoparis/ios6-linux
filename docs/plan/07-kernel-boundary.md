# 07 — Kernel boundary (the substituted kernel: `lib/kernel`)

ADR-08 stays: in-process Mach IPC, native daemons. This chapter specifies the syscall/trap
dispatcher, Mach IPC surface, BSD layer, VM, IOKit facade, and the trace/replay harness.

## 1. Trap entry

### 1.1 Conventions (measured)

- Guest issues `svc #0x80`. r12 selects class:
  - r12 == 0: indirect BSD syscall (number supplied by libc stub in r0 — verify).
  - r12 > 0: BSD syscall number → sysent[r12].
  - r12 < 0, r12 != -3: Mach trap, index = -r12 into mach_trap_table.
  - r12 == -3: fast path mach_absolute_time (not a table slot).
  - r12 == 0x80000000: machdep, selector in r3 (thread_set_cthread_self etc.).
- Return: carry clear + r0 result on success; carry set + r0 = errno (cerror) on failure.
- Args per apcs-gnu soft-float (r0-r3/stack). Dispatcher spills GTC (ADR-06), calls native handler, writes back.

### 1.2 Dispatcher

Handlers are C++ functions taking TrapFrame& and returning void, setting r0/carry.

### 1.3 Extracted tables

Appendix A: `sysent` as extracted from the 10B329 kernel — 443 rows (`nsysent`), columns
number, name (kernel's own `syscallnames[]`), `narg`/`sy_arg_bytes` (32-bit words), kernel handler VA,
userspace stub, class. Appendix B: `mach_trap_table` likewise — 128 rows (`mach_trap_count`),
84 of them empty (`kern_invalid`).

### 1.4 Status classes

A required for CF/Foundation; B common app; C never supported → ENOSYS loud; D fake/stub.

### 1.5 machdep selectors

r12 = 0x80000000, selector in r3. `[measured]` from the shipped stubs (all in `libsystem_c`, each verified by
disassembling the stub body out of the shared cache): selector 0 = `sys_icache_invalidate`
(`_sys_icache_invalidate` @0x3942a054), 1 = `sys_dcache_flush` (`_sys_dcache_flush` @0x3942a064),
2 = `thread_set_cthread_self` (`___pthread_set_self` @0x3942d974 sets `r3 = 2`). Selector 3
(`thread_get_cthread_self`) `UNVERIFIED:` — attested by the `xnu-4570.71.2` dispatcher (`osfmk/arm/locore.s`,
`fleh_swi_trap`), and **no** image among the 569 in the 6.1.3 cache issues it.

### 1.6 mach_absolute_time fast path

r12=-3. Use host clock with timebase numer/denom (S5L8940). UNVERIFIED.

## 2. Mach IPC (ADR-08)

Port namespace in-process; rights SEND/RECV/SEND_ONCE/DEAD_NAME/PORT_SET. mach_msg with timeouts,
MACH_RCV_LARGE, MACH_MSG_OVERWRITE, OOL descriptors, port descriptors, trailers. Notification ports
(dead-name, no-senders). mach_port_allocate/insert_right/mod_refs. VM traps: `[measured]` 10/12/14 only
(`_kernelrpc_mach_vm_allocate_trap` 5 args, `…_vm_deallocate_trap` 5, `…_vm_protect_trap` 7, all present in
libsystem_kernel); slots 15 (`mach_vm_map_trap`) and 24/25 (`mach_port_construct/destruct_trap`) are **empty
(`kern_invalid`)** on 2107, so a guest that issues them must get `KERN_INVALID_ARGUMENT`, not a stub.
task_self/thread_self/host_self, thread_*, semaphore_*, bootstrap_look_up service registry.
`[measured]` the yield paths userspace actually issues: trap 59 `swtch_pri` — 13 Thumb `mvn ip,#0x3a; svc #0x80`
sites inside `libdispatch` (its spin/yield path) plus the `libsystem_kernel` stub; trap 60 `swtch` and 61
`thread_switch` exist as stubs (`_swtch`, `_syscall_thread_switch`) but no cache image other than
`libsystem_kernel` issues them directly; trap 100 `iokit_user_client_trap` is the only syscall/trap issued from
outside `libsystem_kernel`/`libsystem_c`/`libdispatch` in the whole cache (from `IOKit.framework`).

Native services vs refuse decided by scanning com.apple.* strings in key images. MIG subsystems:
mach_host, task, thread_act, vm_map, clock, exc, notify with routine ids from xnu headers.

## 3. BSD layer

Filesystem overlay root; path translation; HFSX case-sensitive (verify 'HX' signature). fd table.
open/read/write/stat64/getdirentries/fcntl/ioctl/mmap/munmap/mprotect/madvise/fsync, pread/pwrite,
dup, pipe, select/poll. sysctl/sysctlbyname list measured. getattrlist, proc_info. kqueue/kevent →
epoll/timerfd/signalfd. __semwait_signal. psynch_* → futex. select/poll. socket AF_UNIX/AF_INET.
sigaction/sigprocmask/__pthread_kill. getrlimit. gettimeofday. mach_absolute_time timebase.
__bsdthread_*. proc_*. csops (validated). fsgetpath. shared_region_*. ptrace(PT_DENY_ATTACH) no-op.

## 4. Memory

vm_* traps, mmap flags translation (MAP_ANON 0x1000 vs 0x20, MAP_FIXED, VM_MAKE_TAG), mach_vm_region
info, purgeable, 4 GiB arena, `shared_region_map_and_slide_np` (438) in the loader, mprotect invalidation.
`[measured]` 2107 has exactly two shared-region syscalls — 294 `shared_region_check_np` and 438
`shared_region_map_and_slide_np`; there is **no** `shared_region_map_file_np` / `shared_region_slide_np`
(295 and 437 are `nosys`).

## 5. IOKit/hardware facade

IOServiceMatching/GetMatchingService/IOConnectCall*. Fake registry; known services succeed for
harmless queries, driver calls fail loudly. MobileGestalt answers (iPad2,1, 10B329). notify_* emulation.

## 6. Trace and error policy

IOS6RECOMP_SYSCALL_TRACE logs every trap. Unknown → ENOSYS + loud log + counted. Out of scope:
fork/exec, ptrace, kext, sandbox.

## 7. Verification

M2 `exit`(1)/`write`(4)/`mmap`(197)/`read`(3) — all class A in Appendix A; M4 `mach_msg_trap`(31, 7 args),
`mach_msg_overwrite_trap`(32, 8), `semaphore_*`(33-39), `mach_reply_port`(26), `task_self_trap`(28),
`host_self_trap`(29), `mach_absolute_time` (r12 = -3, not a table slot), `mach_timebase_info_trap`(89),
`mach_wait_until_trap`(90), `thread_switch`(61, 3 args), `mk_timer_*`(91-94) — Appendix B.
Fixtures list.

---

## Appendix A — BSD `sysent` (extracted from the 10B329 kernel)
**Method.** Kernel: `work/10B329__iPad2,1/kernelcache.release.iPad2,1` (IMG3 `krnl` -> AES-256-CBC `DATA` with the published IV/key -> `complzss` -> LZSS -> 32-bit Mach-O, `cpusubtype 9`, `Darwin Kernel Version 13.0.0 … root:xnu-2107.7.55.2.2~1/RELEASE_ARM_S5L8940X`).
`sysent[]` was found **structurally, not from source**: in `__DATA.__const` at **VA 0x802f00b8** there is a run of 443 records of stride 24 that satisfies, for every record, `0 <= sy_narg <= 12`, `sy_arg_bytes == 4*sy_narg`, `0 <= sy_return_type <= 8`, `sy_call` in `__TEXT`, `sy_arg_munge32/64` either 0 or in `__TEXT`; the same 443 slots appear as a constant-stride run in `LC_DYSYMTAB` local relocations (`locreloff 0x2ff000`, `nlocrel 19335`). **nsysent = 443** (a u32 `0x1bb` at VA 0x802e5d0c — the two words at 0x802d0c74/0x802d0c78 in `__DATA.__nl_symbol_ptr` are pointer-sized references to `nsysent` and `sysent` respectively).
**Names** are the kernel's own `syscallnames[]` (`VA 0x802e8fd0`, 443 x char*), so they are iOS-6.1.3-exact; `#N` marks a number the kernel ships without a name.
**Empty slots**: `sy_call == nosys` = **0x801eaff1** (Thumb entry of the stub at 0x801eaff0 whose body is `push {r7,lr}; movs r1,#0xc; bl …; movs r0,#0x4e; pop {r7,pc}` — returns ENOSYS(78) via the SIGSYS(12) path); 122 rows are empty.
**class** = measured userspace use: every symbol of `/usr/lib/system/libsystem_kernel.dylib` and `/usr/lib/system/libsystem_c.dylib` was disassembled out of the 6.1.3 dyld shared cache and the `r12` value before each `svc #0x80` parsed, and each cache image's `LC_DYLD_INFO` bind stream was walked for imports. A = the userspace stub is imported by the bring-up set (libsystem_kernel, libsystem_c, libSystem.B, libsystem_m, libsystem_blocks, libdispatch, libobjc, CoreFoundation, Foundation); B = imported by other images (or a stub exists but nothing in the cache imports it); C = `nosys` slot on this build (guest sees ENOSYS); D = implemented in the kernel but no userspace stub and no importer (dead surface — safe to fake), plus `ptrace` (plan decision, §3).
`narg`/`arg_bytes` are the kernel's own `sy_narg`/`sy_arg_bytes` (32-bit words, 64-bit arguments count as two). Reproduce with the kernelcache pipeline of `docs/research/ios6-platform.md` §3.5 (IMG3 `krnl` -> `ipsw img3 extract -r` -> AES-256-CBC with the published IV/key -> `complzss` LZSS -> 32-bit Mach-O) plus the structural anchors above.
- `sy_flags`: measured values = [0] (no per-syscall funnel/flags bits set on this build)
- `sy_arg_munge32/64` non-zero on: none
- vs the previous xnu-2422 table: 22 rows implemented in 2422 are `nosys` here (155 `nfssvc`, 161 `getfh`, 247 `nfsclnt`, 248 `fhopen`, 251-265 System V IPC subset, 293 `identitysvc`, 337 `sendfile`, 418 `msgsnd_nocancel`, 419 `msgrcv_nocancel`); numbers 443-455 of the 2422 table do not exist here (`nsysent` 443); 435/436 are `pid_hibernate`/`pid_shutdown_sockets` (CONFIG_EMBEDDED branch) where the 2422 OS-X branch had `nosys`.

| # | name | narg | arg_bytes | handler | userspace stub | class |
|---|---|---|---|---|---|---|
| 0 | `syscall` | 0 | 0 | 0x801eaff1 | - | C |
| 1 | `exit` | 1 | 4 | 0x801d4375 | `___exit` | A |
| 2 | `fork` | 0 | 0 | 0x801d7281 | `___fork` | A |
| 3 | `read` | 3 | 12 | 0x801eb011 | `_read` | A |
| 4 | `write` | 3 | 12 | 0x801eb3e5 | `_write` | A |
| 5 | `open` | 3 | 12 | 0x800b0b5d | `___open` | A |
| 6 | `close` | 1 | 4 | 0x801cc3b5 | `_close` | A |
| 7 | `wait4` | 4 | 16 | 0x801d4fbd | `___wait4` | A |
| 8 | `#8` | 0 | 0 | 0x801eaff1 | - | C |
| 9 | `link` | 2 | 8 | 0x800b10a1 | `_link` | A |
| 10 | `unlink` | 1 | 4 | 0x800b17a9 | `___unlink` | B |
| 11 | `#11` | 0 | 0 | 0x801eaff1 | - | C |
| 12 | `chdir` | 1 | 4 | 0x800b0419 | `_chdir` | A |
| 13 | `fchdir` | 1 | 4 | 0x800b02a9 | `_fchdir` | A |
| 14 | `mknod` | 3 | 12 | 0x800b0c75 | `_mknod` | B |
| 15 | `chmod` | 2 | 8 | 0x800b22f9 | `___chmod` | B |
| 16 | `chown` | 3 | 12 | 0x800b2455 | `_chown` | A |
| 17 | `#17` | 0 | 0 | 0x801eaff1 | - | C |
| 18 | `getfsstat` | 3 | 12 | 0x800b0045 | - | D |
| 19 | `#19` | 0 | 0 | 0x801eaff1 | - | C |
| 20 | `getpid` | 0 | 0 | 0x801dbb0d | `___getpid` | B |
| 21 | `#21` | 0 | 0 | 0x801eaff1 | - | C |
| 22 | `#22` | 0 | 0 | 0x801eaff1 | - | C |
| 23 | `setuid` | 1 | 4 | 0x801dbdc1 | `_setuid` | B |
| 24 | `getuid` | 0 | 0 | 0x801dbb91 | `_getuid` | A |
| 25 | `geteuid` | 0 | 0 | 0x801dbba1 | `_geteuid` | A |
| 26 | `ptrace` | 4 | 16 | 0x801e7bb9 | - | D |
| 27 | `recvmsg` | 3 | 12 | 0x8020a389 | `___recvmsg` | B |
| 28 | `sendmsg` | 3 | 12 | 0x80209ed1 | `___sendmsg` | B |
| 29 | `recvfrom` | 6 | 24 | 0x80209fb5 | `___recvfrom` | A |
| 30 | `accept` | 3 | 12 | 0x80209889 | `___accept` | B |
| 31 | `getpeername` | 3 | 12 | 0x8020a655 | `___getpeername` | B |
| 32 | `getsockname` | 3 | 12 | 0x8020a5a5 | `___getsockname` | B |
| 33 | `access` | 2 | 8 | 0x800b1c65 | `_access` | A |
| 34 | `chflags` | 2 | 8 | 0x800b20e1 | `_chflags` | A |
| 35 | `fchflags` | 2 | 8 | 0x800b21a9 | `_fchflags` | B |
| 36 | `sync` | 0 | 0 | 0x800afad9 | `_sync` | B |
| 37 | `kill` | 3 | 12 | 0x801df6cd | `___kill` | A |
| 38 | `#38` | 0 | 0 | 0x801eaff1 | - | C |
| 39 | `getppid` | 0 | 0 | 0x801dbb15 | `_getppid` | B |
| 40 | `#40` | 0 | 0 | 0x801eaff1 | - | C |
| 41 | `dup` | 1 | 4 | 0x801ca405 | `_dup` | A |
| 42 | `pipe` | 0 | 0 | 0x801ed671 | `___pipe` | B |
| 43 | `getegid` | 0 | 0 | 0x801dbc19 | `_getegid` | A |
| 44 | `#44` | 0 | 0 | 0x801eaff1 | - | C |
| 45 | `#45` | 0 | 0 | 0x801eaff1 | - | C |
| 46 | `sigaction` | 3 | 12 | 0x801de7e9 | `___sigaction` | A |
| 47 | `getgid` | 0 | 0 | 0x801dbc09 | `_getgid` | A |
| 48 | `sigprocmask` | 3 | 12 | 0x801ded2d | `_sigprocmask` | A |
| 49 | `getlogin` | 2 | 8 | 0x801dc9e9 | `___getlogin` | A |
| 50 | `setlogin` | 1 | 4 | 0x801dca61 | `___setlogin` | A |
| 51 | `acct` | 1 | 4 | 0x801c4ded | `_acct` | B |
| 52 | `sigpending` | 1 | 4 | 0x801deed1 | `_sigpending` | B |
| 53 | `sigaltstack` | 2 | 8 | 0x801df611 | `___sigaltstack` | A |
| 54 | `ioctl` | 3 | 12 | 0x801eb7a9 | `___ioctl` | B |
| 55 | `reboot` | 2 | 8 | 0x801e7b1d | `_reboot` | B |
| 56 | `revoke` | 1 | 4 | 0x800b3bb1 | `_revoke` | B |
| 57 | `symlink` | 2 | 8 | 0x800b1311 | `_symlink` | A |
| 58 | `readlink` | 3 | 12 | 0x800b1fe5 | `_readlink` | A |
| 59 | `execve` | 3 | 12 | 0x801d3d49 | `_execve` | A |
| 60 | `umask` | 1 | 4 | 0x800b3b89 | `_umask` | A |
| 61 | `chroot` | 1 | 4 | 0x800b04e9 | `_chroot` | B |
| 62 | `#62` | 0 | 0 | 0x801eaff1 | - | C |
| 63 | `#63` | 0 | 0 | 0x801eaff1 | - | C |
| 64 | `#64` | 0 | 0 | 0x801eaff1 | - | C |
| 65 | `msync` | 3 | 12 | 0x801d7dd1 | `___msync` | B |
| 66 | `vfork` | 0 | 0 | 0x801d6919 | - | D |
| 67 | `#67` | 0 | 0 | 0x801eaff1 | - | C |
| 68 | `#68` | 0 | 0 | 0x801eaff1 | - | C |
| 69 | `#69` | 0 | 0 | 0x801eaff1 | - | C |
| 70 | `#70` | 0 | 0 | 0x801eaff1 | - | C |
| 71 | `#71` | 0 | 0 | 0x801eaff1 | - | C |
| 72 | `#72` | 0 | 0 | 0x801eaff1 | - | C |
| 73 | `munmap` | 2 | 8 | 0x801d7e7d | `___munmap` | B |
| 74 | `mprotect` | 3 | 12 | 0x801d7eb1 | `___mprotect` | B |
| 75 | `madvise` | 3 | 12 | 0x801d7f69 | `_madvise` | A |
| 76 | `#76` | 0 | 0 | 0x801eaff1 | - | C |
| 77 | `#77` | 0 | 0 | 0x801eaff1 | - | C |
| 78 | `mincore` | 3 | 12 | 0x801d7fd5 | `_mincore` | B |
| 79 | `getgroups` | 2 | 8 | 0x801dbc29 | `_getgroups` | B |
| 80 | `setgroups` | 2 | 8 | 0x801dc92d | `_setgroups` | B |
| 81 | `getpgrp` | 0 | 0 | 0x801dbb1d | `_getpgrp` | A |
| 82 | `setpgid` | 2 | 8 | 0x801dbcc9 | `_setpgid` | A |
| 83 | `setitimer` | 3 | 12 | 0x801e7605 | `_setitimer` | A |
| 84 | `#84` | 0 | 0 | 0x801eaff1 | - | C |
| 85 | `swapon` | 0 | 0 | 0x8021b91d | `_swapon` | B |
| 86 | `getitimer` | 2 | 8 | 0x801e74bd | `_getitimer` | B |
| 87 | `#87` | 0 | 0 | 0x801eaff1 | - | C |
| 88 | `#88` | 0 | 0 | 0x801eaff1 | - | C |
| 89 | `getdtablesize` | 0 | 0 | 0x801c9fdd | `_getdtablesize` | A |
| 90 | `dup2` | 2 | 8 | 0x801ca855 | `_dup2` | A |
| 91 | `#91` | 0 | 0 | 0x801eaff1 | - | C |
| 92 | `fcntl` | 3 | 12 | 0x801cad21 | `___fcntl` | A |
| 93 | `select` | 5 | 20 | 0x801eba55 | `___select` | B |
| 94 | `#94` | 0 | 0 | 0x801eaff1 | - | C |
| 95 | `fsync` | 1 | 4 | 0x800b29f1 | `_fsync` | A |
| 96 | `setpriority` | 3 | 12 | 0x801dcd95 | `_setpriority` | A |
| 97 | `socket` | 3 | 12 | 0x80209331 | `_socket` | A |
| 98 | `connect` | 3 | 12 | 0x802098a9 | `___connect` | B |
| 99 | `#99` | 0 | 0 | 0x801eaff1 | - | C |
| 100 | `getpriority` | 2 | 8 | 0x801dcc89 | `_getpriority` | A |
| 101 | `#101` | 0 | 0 | 0x801eaff1 | - | C |
| 102 | `#102` | 0 | 0 | 0x801eaff1 | - | C |
| 103 | `#103` | 0 | 0 | 0x801eaff1 | - | C |
| 104 | `bind` | 3 | 12 | 0x802093fd | `___bind` | B |
| 105 | `setsockopt` | 5 | 20 | 0x8020a4bd | `_setsockopt` | A |
| 106 | `listen` | 2 | 8 | 0x80209569 | `___listen` | B |
| 107 | `#107` | 0 | 0 | 0x801eaff1 | - | C |
| 108 | `#108` | 0 | 0 | 0x801eaff1 | - | C |
| 109 | `#109` | 0 | 0 | 0x801eaff1 | - | C |
| 110 | `#110` | 0 | 0 | 0x801eaff1 | - | C |
| 111 | `sigsuspend` | 1 | 4 | 0x801deef9 | `___sigsuspend` | B |
| 112 | `#112` | 0 | 0 | 0x801eaff1 | - | C |
| 113 | `#113` | 0 | 0 | 0x801eaff1 | - | C |
| 114 | `#114` | 0 | 0 | 0x801eaff1 | - | C |
| 115 | `#115` | 0 | 0 | 0x801eaff1 | - | C |
| 116 | `gettimeofday` | 2 | 8 | 0x801e72cd | `___gettimeofday` | A |
| 117 | `getrusage` | 2 | 8 | 0x801ddb2d | `_getrusage` | A |
| 118 | `getsockopt` | 5 | 20 | 0x8020a521 | `_getsockopt` | A |
| 119 | `#119` | 0 | 0 | 0x801eaff1 | - | C |
| 120 | `readv` | 3 | 12 | 0x801eb29d | `_readv` | B |
| 121 | `writev` | 3 | 12 | 0x801eb63d | `_writev` | A |
| 122 | `settimeofday` | 2 | 8 | 0x801e7329 | `___settimeofday` | A |
| 123 | `fchown` | 3 | 12 | 0x800b2565 | `_fchown` | B |
| 124 | `fchmod` | 2 | 8 | 0x800b2429 | `___fchmod` | B |
| 125 | `#125` | 0 | 0 | 0x801eaff1 | - | C |
| 126 | `setreuid` | 2 | 8 | 0x801dc10d | `___setreuid` | B |
| 127 | `setregid` | 2 | 8 | 0x801dc4a1 | `___setregid` | B |
| 128 | `rename` | 2 | 8 | 0x800b2be1 | `___rename` | B |
| 129 | `#129` | 0 | 0 | 0x801eaff1 | - | C |
| 130 | `#130` | 0 | 0 | 0x801eaff1 | - | C |
| 131 | `flock` | 2 | 8 | 0x801cdb0d | `_flock` | B |
| 132 | `mkfifo` | 2 | 8 | 0x800b0f51 | `_mkfifo` | B |
| 133 | `sendto` | 6 | 24 | 0x80209bf5 | `___sendto` | A |
| 134 | `shutdown` | 2 | 8 | 0x8020a48d | `_shutdown` | A |
| 135 | `socketpair` | 4 | 16 | 0x80209a99 | `___socketpair` | B |
| 136 | `mkdir` | 2 | 8 | 0x800b34d5 | `_mkdir` | A |
| 137 | `rmdir` | 1 | 4 | 0x800b3515 | `___rmdir` | B |
| 138 | `utimes` | 2 | 8 | 0x800b2619 | `_utimes` | A |
| 139 | `futimes` | 2 | 8 | 0x800b27ed | `_futimes` | B |
| 140 | `adjtime` | 2 | 8 | 0x801e742d | `_adjtime` | B |
| 141 | `#141` | 0 | 0 | 0x801eaff1 | - | C |
| 142 | `gethostuuid` | 2 | 8 | 0x801ed131 | `_gethostuuid` | B |
| 143 | `#143` | 0 | 0 | 0x801eaff1 | - | C |
| 144 | `#144` | 0 | 0 | 0x801eaff1 | - | C |
| 145 | `#145` | 0 | 0 | 0x801eaff1 | - | C |
| 146 | `#146` | 0 | 0 | 0x801eaff1 | - | C |
| 147 | `setsid` | 0 | 0 | 0x801dbc85 | `_setsid` | A |
| 148 | `#148` | 0 | 0 | 0x801eaff1 | `_setquota` | C |
| 149 | `#149` | 0 | 0 | 0x801eaff1 | `_quota` | C |
| 150 | `#150` | 0 | 0 | 0x801eaff1 | - | C |
| 151 | `getpgid` | 1 | 4 | 0x801dbb25 | `_getpgid` | B |
| 152 | `setprivexec` | 1 | 4 | 0x801dbaf5 | `_setprivexec` | B |
| 153 | `pread` | 5 | 20 | 0x801eb201 | `_pread` | B |
| 154 | `pwrite` | 5 | 20 | 0x801eb55d | `_pwrite` | A |
| 155 | `#155` | 0 | 0 | 0x801eaff1 | `_nfssvc` | C |
| 156 | `#156` | 0 | 0 | 0x801eaff1 | - | C |
| 157 | `statfs` | 2 | 8 | 0x800afb79 | - | D |
| 158 | `fstatfs` | 2 | 8 | 0x800afe31 | - | D |
| 159 | `unmount` | 2 | 8 | 0x800af641 | `_unmount` | B |
| 160 | `#160` | 0 | 0 | 0x801eaff1 | - | C |
| 161 | `#161` | 0 | 0 | 0x801eaff1 | `_getfh` | C |
| 162 | `#162` | 0 | 0 | 0x801eaff1 | - | C |
| 163 | `#163` | 0 | 0 | 0x801eaff1 | - | C |
| 164 | `#164` | 0 | 0 | 0x801eaff1 | - | C |
| 165 | `quotactl` | 4 | 16 | 0x800afb75 | `_quotactl` | B |
| 166 | `#166` | 0 | 0 | 0x801eaff1 | - | C |
| 167 | `mount` | 4 | 16 | 0x800ae821 | `_mount` | B |
| 168 | `#168` | 0 | 0 | 0x801eaff1 | - | C |
| 169 | `csops` | 4 | 16 | 0x801da8d1 | `_csops` | B |
| 170 | `csops_audittoken` | 5 | 20 | 0x801dadbd | `_csops_audittoken` | B |
| 171 | `#171` | 0 | 0 | 0x801eaff1 | - | C |
| 172 | `#172` | 0 | 0 | 0x801eaff1 | - | C |
| 173 | `waitid` | 4 | 16 | 0x801d53b5 | `_waitid` | B |
| 174 | `#174` | 0 | 0 | 0x801eaff1 | - | C |
| 175 | `#175` | 0 | 0 | 0x801eaff1 | - | C |
| 176 | `#176` | 0 | 0 | 0x801eaff1 | - | C |
| 177 | `#177` | 0 | 0 | 0x801eaff1 | - | C |
| 178 | `#178` | 0 | 0 | 0x801eaff1 | - | C |
| 179 | `#179` | 0 | 0 | 0x801eaff1 | - | C |
| 180 | `kdebug_trace` | 6 | 24 | 0x801c26b5 | `___kdebug_trace` | B |
| 181 | `setgid` | 1 | 4 | 0x801dc2a5 | `_setgid` | B |
| 182 | `setegid` | 1 | 4 | 0x801dc3b1 | `_setegid` | B |
| 183 | `seteuid` | 1 | 4 | 0x801dc011 | `_seteuid` | B |
| 184 | `sigreturn` | 2 | 8 | 0x8021e299 | `___sigreturn` | A |
| 185 | `chud` | 12 | 48 | 0x8021cfa9 | `___chud` | B |
| 186 | `#186` | 0 | 0 | 0x801eaff1 | - | C |
| 187 | `fdatasync` | 1 | 4 | 0x800b2a69 | `_fdatasync` | B |
| 188 | `stat` | 2 | 8 | 0x800b1d41 | - | D |
| 189 | `fstat` | 2 | 8 | 0x801cc8ed | - | D |
| 190 | `lstat` | 2 | 8 | 0x800b1e8d | - | D |
| 191 | `pathconf` | 2 | 8 | 0x800b1f81 | `_pathconf` | A |
| 192 | `fpathconf` | 2 | 8 | 0x801cc949 | `_fpathconf` | B |
| 193 | `#193` | 0 | 0 | 0x801eaff1 | - | C |
| 194 | `getrlimit` | 2 | 8 | 0x801dd975 | `___getrlimit` | B |
| 195 | `setrlimit` | 2 | 8 | 0x801dd23d | `___setrlimit` | B |
| 196 | `getdirentries` | 4 | 16 | 0x800b374d | `_getdirentries` | B |
| 197 | `mmap` | 7 | 28 | 0x801d78c1 | `___mmap` | B |
| 198 | `#198` | 0 | 0 | 0x801eaff1 | - | C |
| 199 | `lseek` | 4 | 16 | 0x800b1821 | `___lseek` | B |
| 200 | `truncate` | 3 | 12 | 0x800b286d | `_truncate` | B |
| 201 | `ftruncate` | 3 | 12 | 0x800b292d | `_ftruncate` | A |
| 202 | `__sysctl` | 6 | 24 | 0x801e1d79 | `___sysctl` | A |
| 203 | `mlock` | 2 | 8 | 0x801d8121 | `_mlock` | B |
| 204 | `munlock` | 2 | 8 | 0x801d8179 | `_munlock` | B |
| 205 | `undelete` | 1 | 4 | 0x800b14a9 | `_undelete` | B |
| 206 | `#206` | 0 | 0 | 0x801eaff1 | `_ATsocket` | C |
| 207 | `#207` | 0 | 0 | 0x801eaff1 | `_ATgetmsg` | C |
| 208 | `#208` | 0 | 0 | 0x801eaff1 | `_ATputmsg` | C |
| 209 | `#209` | 0 | 0 | 0x801eaff1 | `_ATPsndreq` | C |
| 210 | `#210` | 0 | 0 | 0x801eaff1 | `_ATPsndrsp` | C |
| 211 | `#211` | 0 | 0 | 0x801eaff1 | `_ATPgetreq` | C |
| 212 | `#212` | 0 | 0 | 0x801eaff1 | `_ATPgetrsp` | C |
| 213 | `#213` | 0 | 0 | 0x801eaff1 | - | C |
| 214 | `#214` | 0 | 0 | 0x801eaff1 | - | C |
| 215 | `#215` | 0 | 0 | 0x801eaff1 | - | C |
| 216 | `open_dprotected_np` | 5 | 20 | 0x800b0a7d | `___open_dprotected_np` | B |
| 217 | `#217` | 0 | 0 | 0x801eaff1 | - | C |
| 218 | `#218` | 0 | 0 | 0x801eaff1 | - | C |
| 219 | `#219` | 0 | 0 | 0x801eaff1 | - | C |
| 220 | `getattrlist` | 5 | 20 | 0x8009a819 | `___getattrlist` | B |
| 221 | `setattrlist` | 5 | 20 | 0x8009a891 | `___setattrlist` | B |
| 222 | `getdirentriesattr` | 8 | 32 | 0x800b3c99 | `_getdirentriesattr` | B |
| 223 | `exchangedata` | 3 | 12 | 0x800b3e55 | `_exchangedata` | B |
| 224 | `#224` | 0 | 0 | 0x801eaff1 | - | C |
| 225 | `searchfs` | 6 | 24 | 0x800b4095 | `_searchfs` | B |
| 226 | `delete` | 1 | 4 | 0x800b17e5 | `___delete` | B |
| 227 | `copyfile` | 4 | 16 | 0x800b2a85 | `___copyfile` | B |
| 228 | `fgetattrlist` | 5 | 20 | 0x80097c41 | `_fgetattrlist` | B |
| 229 | `fsetattrlist` | 5 | 20 | 0x8009af99 | `_fsetattrlist` | B |
| 230 | `poll` | 3 | 12 | 0x801ec1b9 | `_poll` | B |
| 231 | `watchevent` | 2 | 8 | 0x801ecae1 | `_watchevent` | B |
| 232 | `waitevent` | 2 | 8 | 0x801ecc85 | `_waitevent` | B |
| 233 | `modwatch` | 2 | 8 | 0x801ecdf5 | `_modwatch` | B |
| 234 | `getxattr` | 6 | 24 | 0x800b4d09 | `_getxattr` | A |
| 235 | `fgetxattr` | 6 | 24 | 0x800b4e45 | `_fgetxattr` | A |
| 236 | `setxattr` | 6 | 24 | 0x800b4f45 | `_setxattr` | A |
| 237 | `fsetxattr` | 6 | 24 | 0x800b5051 | `_fsetxattr` | A |
| 238 | `removexattr` | 3 | 12 | 0x800b514d | `_removexattr` | A |
| 239 | `fremovexattr` | 3 | 12 | 0x800b5215 | `_fremovexattr` | B |
| 240 | `listxattr` | 4 | 16 | 0x800b52d5 | `_listxattr` | A |
| 241 | `flistxattr` | 4 | 16 | 0x800b53b9 | `_flistxattr` | B |
| 242 | `fsctl` | 4 | 16 | 0x800b458d | `_fsctl` | A |
| 243 | `initgroups` | 3 | 12 | 0x801dc7a9 | `___initgroups` | B |
| 244 | `posix_spawn` | 5 | 20 | 0x801d2e1d | `___posix_spawn` | A |
| 245 | `ffsctl` | 4 | 16 | 0x800b4c2d | `_ffsctl` | A |
| 246 | `#246` | 0 | 0 | 0x801eaff1 | - | C |
| 247 | `#247` | 0 | 0 | 0x801eaff1 | `_nfsclnt` | C |
| 248 | `#248` | 0 | 0 | 0x801eaff1 | `_fhopen` | C |
| 249 | `#249` | 0 | 0 | 0x801eaff1 | - | C |
| 250 | `minherit` | 3 | 12 | 0x801d7f31 | `_minherit` | B |
| 251 | `#251` | 0 | 0 | 0x801eaff1 | `___semsys` | C |
| 252 | `#252` | 0 | 0 | 0x801eaff1 | `___msgsys` | C |
| 253 | `#253` | 0 | 0 | 0x801eaff1 | `___shmsys` | C |
| 254 | `#254` | 0 | 0 | 0x801eaff1 | `___semctl` | C |
| 255 | `#255` | 0 | 0 | 0x801eaff1 | `_semget` | C |
| 256 | `#256` | 0 | 0 | 0x801eaff1 | `_semop` | C |
| 257 | `#257` | 0 | 0 | 0x801eaff1 | - | C |
| 258 | `#258` | 0 | 0 | 0x801eaff1 | `___msgctl` | C |
| 259 | `#259` | 0 | 0 | 0x801eaff1 | `_msgget` | C |
| 260 | `#260` | 0 | 0 | 0x801eaff1 | `_msgsnd` | C |
| 261 | `#261` | 0 | 0 | 0x801eaff1 | `_msgrcv` | C |
| 262 | `#262` | 0 | 0 | 0x801eaff1 | `_shmat` | C |
| 263 | `#263` | 0 | 0 | 0x801eaff1 | `___shmctl` | C |
| 264 | `#264` | 0 | 0 | 0x801eaff1 | `_shmdt` | C |
| 265 | `#265` | 0 | 0 | 0x801eaff1 | `_shmget` | C |
| 266 | `shm_open` | 3 | 12 | 0x8020e5b1 | `___shm_open` | B |
| 267 | `shm_unlink` | 1 | 4 | 0x8020f091 | `_shm_unlink` | A |
| 268 | `sem_open` | 4 | 16 | 0x8020da0d | `___sem_open` | B |
| 269 | `sem_close` | 1 | 4 | 0x8020e1a5 | `_sem_close` | B |
| 270 | `sem_unlink` | 1 | 4 | 0x8020df6d | `_sem_unlink` | B |
| 271 | `sem_wait` | 1 | 4 | 0x8020e1f9 | `_sem_wait` | B |
| 272 | `sem_trywait` | 1 | 4 | 0x8020e2c1 | `_sem_trywait` | B |
| 273 | `sem_post` | 1 | 4 | 0x8020e365 | `_sem_post` | B |
| 274 | `sem_getvalue` | 2 | 8 | 0x8020e409 | `_sem_getvalue` | B |
| 275 | `sem_init` | 3 | 12 | 0x8020e401 | `_sem_init` | B |
| 276 | `sem_destroy` | 1 | 4 | 0x8020e405 | `_sem_destroy` | B |
| 277 | `open_extended` | 6 | 24 | 0x800b0991 | `___open_extended` | A |
| 278 | `umask_extended` | 2 | 8 | 0x800b3b39 | `___umask_extended` | A |
| 279 | `stat_extended` | 4 | 16 | 0x800b1ce9 | `___stat_extended` | B |
| 280 | `lstat_extended` | 4 | 16 | 0x800b1e35 | `___lstat_extended` | B |
| 281 | `fstat_extended` | 4 | 16 | 0x801cc6d1 | `___fstat_extended` | B |
| 282 | `chmod_extended` | 5 | 20 | 0x800b21e9 | `___chmod_extended` | A |
| 283 | `fchmod_extended` | 5 | 20 | 0x800b232d | `___fchmod_extended` | A |
| 284 | `access_extended` | 4 | 16 | 0x800b1959 | `___access_extended` | B |
| 285 | `settid` | 2 | 8 | 0x801dc62d | `___settid` | B |
| 286 | `gettid` | 2 | 8 | 0x801dbbb1 | `___gettid` | B |
| 287 | `setsgroups` | 2 | 8 | 0x801dc93d | `___setsgroups` | B |
| 288 | `getsgroups` | 2 | 8 | 0x801dbc7d | `___getsgroups` | B |
| 289 | `setwgroups` | 2 | 8 | 0x801dc941 | `___setwgroups` | B |
| 290 | `getwgroups` | 2 | 8 | 0x801dbc81 | `___getwgroups` | B |
| 291 | `mkfifo_extended` | 5 | 20 | 0x800b0ead | `___mkfifo_extended` | A |
| 292 | `mkdir_extended` | 5 | 20 | 0x800b32e9 | `___mkdir_extended` | A |
| 293 | `#293` | 0 | 0 | 0x801eaff1 | `___identitysvc` | C |
| 294 | `shared_region_check_np` | 1 | 4 | 0x8021be59 | `___shared_region_check_np` | B |
| 295 | `#295` | 0 | 0 | 0x801eaff1 | - | C |
| 296 | `vm_pressure_monitor` | 3 | 12 | 0x8021c5bd | `_vm_pressure_monitor` | B |
| 297 | `psynch_rw_longrdlock` | 5 | 20 | 0x80215439 | `___psynch_rw_longrdlock` | B |
| 298 | `psynch_rw_yieldwrlock` | 5 | 20 | 0x802156ed | `___psynch_rw_yieldwrlock` | B |
| 299 | `psynch_rw_downgrade` | 5 | 20 | 0x802156f5 | `___psynch_rw_downgrade` | B |
| 300 | `psynch_rw_upgrade` | 5 | 20 | 0x802156f1 | `___psynch_rw_upgrade` | B |
| 301 | `psynch_mutexwait` | 6 | 24 | 0x80212665 | `___psynch_mutexwait` | A |
| 302 | `psynch_mutexdrop` | 6 | 24 | 0x80213629 | `___psynch_mutexdrop` | A |
| 303 | `psynch_cvbroad` | 11 | 44 | 0x8021367d | `___psynch_cvbroad` | A |
| 304 | `psynch_cvsignal` | 11 | 44 | 0x80213c4d | `___psynch_cvsignal` | A |
| 305 | `psynch_cvwait` | 11 | 44 | 0x802140d5 | `___psynch_cvwait` | A |
| 306 | `psynch_rw_rdlock` | 5 | 20 | 0x80214809 | `___psynch_rw_rdlock` | A |
| 307 | `psynch_rw_wrlock` | 5 | 20 | 0x8021543d | `___psynch_rw_wrlock` | A |
| 308 | `psynch_rw_unlock` | 5 | 20 | 0x802156f9 | `___psynch_rw_unlock` | A |
| 309 | `psynch_rw_unlock2` | 5 | 20 | 0x802159f1 | `___psynch_rw_unlock2` | B |
| 310 | `getsid` | 1 | 4 | 0x801dbb55 | `_getsid` | B |
| 311 | `settid_with_pid` | 2 | 8 | 0x801dc6cd | `___settid_with_pid` | B |
| 312 | `psynch_cvclrprepost` | 7 | 28 | 0x80214709 | `___psynch_cvclrprepost` | A |
| 313 | `aio_fsync` | 2 | 8 | 0x801c57d1 | `_aio_fsync` | B |
| 314 | `aio_return` | 1 | 4 | 0x801c59a9 | `_aio_return` | B |
| 315 | `aio_suspend` | 3 | 12 | 0x801c5c31 | `_aio_suspend` | B |
| 316 | `aio_cancel` | 2 | 8 | 0x801c5349 | `_aio_cancel` | B |
| 317 | `aio_error` | 1 | 4 | 0x801c5725 | `_aio_error` | B |
| 318 | `aio_read` | 1 | 4 | 0x801c5989 | `_aio_read` | B |
| 319 | `aio_write` | 1 | 4 | 0x801c5e45 | `_aio_write` | B |
| 320 | `lio_listio` | 4 | 16 | 0x801c5e65 | `_lio_listio` | B |
| 321 | `#321` | 0 | 0 | 0x801eaff1 | - | C |
| 322 | `iopolicysys` | 2 | 8 | 0x801ddd21 | `___iopolicysys` | A |
| 323 | `process_policy` | 8 | 32 | 0x8021a1e1 | `___process_policy` | A |
| 324 | `mlockall` | 1 | 4 | 0x801d81b5 | `_mlockall` | B |
| 325 | `munlockall` | 1 | 4 | 0x801d81b9 | `_munlockall` | B |
| 326 | `#326` | 0 | 0 | 0x801eaff1 | - | C |
| 327 | `issetugid` | 0 | 0 | 0x801dbdb1 | `_issetugid` | A |
| 328 | `__pthread_kill` | 2 | 8 | 0x801df345 | `___pthread_kill` | A |
| 329 | `__pthread_sigmask` | 3 | 12 | 0x801df3a5 | `___pthread_sigmask` | A |
| 330 | `__sigwait` | 2 | 8 | 0x801df455 | `___sigwait` | A |
| 331 | `__disable_threadsignal` | 1 | 4 | 0x801df021 | `___disable_threadsignal` | A |
| 332 | `__pthread_markcancel` | 1 | 4 | 0x801df03d | `___pthread_markcancel` | A |
| 333 | `__pthread_canceled` | 1 | 4 | 0x801df085 | `___pthread_canceled` | A |
| 334 | `__semwait_signal` | 7 | 28 | 0x801df225 | `___semwait_signal` | A |
| 335 | `#335` | 0 | 0 | 0x801eaff1 | - | C |
| 336 | `proc_info` | 7 | 28 | 0x802180a5 | `___proc_info` | A |
| 337 | `#337` | 0 | 0 | 0x801eaff1 | `_sendfile` | C |
| 338 | `stat64` | 2 | 8 | 0x800b1d8d | `_stat` | A |
| 339 | `fstat64` | 2 | 8 | 0x801cc929 | `_fstat` | A |
| 340 | `lstat64` | 2 | 8 | 0x800b1ed9 | `_lstat` | A |
| 341 | `stat64_extended` | 4 | 16 | 0x800b1ddd | `___stat64_extended` | A |
| 342 | `lstat64_extended` | 4 | 16 | 0x800b1f29 | `___lstat64_extended` | A |
| 343 | `fstat64_extended` | 4 | 16 | 0x801cc90d | `___fstat64_extended` | A |
| 344 | `getdirentries64` | 4 | 16 | 0x800b3af9 | `___getdirentries64` | A |
| 345 | `statfs64` | 2 | 8 | 0x800afe99 | `_statfs` | A |
| 346 | `fstatfs64` | 2 | 8 | 0x800affe1 | `_fstatfs` | A |
| 347 | `getfsstat64` | 3 | 12 | 0x800b01f1 | `_getfsstat` | A |
| 348 | `__pthread_chdir` | 1 | 4 | 0x800b04e1 | `___pthread_chdir` | B |
| 349 | `__pthread_fchdir` | 1 | 4 | 0x800b0411 | `___pthread_fchdir` | B |
| 350 | `audit` | 2 | 8 | 0x801c1375 | `_audit` | B |
| 351 | `auditon` | 3 | 12 | 0x801c1379 | `_auditon` | B |
| 352 | `#352` | 0 | 0 | 0x801eaff1 | - | C |
| 353 | `getauid` | 1 | 4 | 0x801c137d | `_getauid` | B |
| 354 | `setauid` | 1 | 4 | 0x801c1381 | `_setauid` | B |
| 355 | `#355` | 0 | 0 | 0x801eaff1 | - | C |
| 356 | `#356` | 0 | 0 | 0x801eaff1 | - | C |
| 357 | `getaudit_addr` | 2 | 8 | 0x801c1385 | `_getaudit_addr` | B |
| 358 | `setaudit_addr` | 2 | 8 | 0x801c1389 | `_setaudit_addr` | B |
| 359 | `auditctl` | 1 | 4 | 0x801c138d | `_auditctl` | B |
| 360 | `bsdthread_create` | 5 | 20 | 0x80216545 | `___bsdthread_create` | A |
| 361 | `bsdthread_terminate` | 4 | 16 | 0x802167bd | `___bsdthread_terminate` | A |
| 362 | `kqueue` | 0 | 0 | 0x801cee95 | `_kqueue` | A |
| 363 | `kevent` | 6 | 24 | 0x801cef15 | `_kevent` | B |
| 364 | `lchown` | 3 | 12 | 0x800b254d | `___lchown` | B |
| 365 | `stack_snapshot` | 5 | 20 | 0x801c4b0d | `___stack_snapshot` | B |
| 366 | `bsdthread_register` | 7 | 28 | 0x80216821 | `___bsdthread_register` | A |
| 367 | `workq_open` | 0 | 0 | 0x80217475 | `___workq_open` | A |
| 368 | `workq_kernreturn` | 4 | 16 | 0x802178dd | `___workq_kernreturn` | A |
| 369 | `kevent64` | 7 | 28 | 0x801cf1ad | `_kevent64` | A |
| 370 | `__old_semwait_signal` | 5 | 20 | 0x801df0f9 | `___old_semwait_signal` | B |
| 371 | `__old_semwait_signal_nocancel` | 5 | 20 | 0x801df12d | `_____old_semwait_signal_nocancel` | B |
| 372 | `thread_selfid` | 0 | 0 | 0x80217de1 | `___thread_selfid` | A |
| 373 | `ledger` | 4 | 16 | 0x801ed199 | `_ledger` | B |
| 374 | `#374` | 0 | 0 | 0x801eaff1 | - | C |
| 375 | `#375` | 0 | 0 | 0x801eaff1 | - | C |
| 376 | `#376` | 0 | 0 | 0x801eaff1 | - | C |
| 377 | `#377` | 0 | 0 | 0x801eaff1 | - | C |
| 378 | `#378` | 0 | 0 | 0x801eaff1 | - | C |
| 379 | `#379` | 0 | 0 | 0x801eaff1 | - | C |
| 380 | `__mac_execve` | 4 | 16 | 0x801d3d69 | `___mac_execve` | B |
| 381 | `__mac_syscall` | 3 | 12 | 0x8027d129 | `___mac_syscall` | B |
| 382 | `__mac_get_file` | 2 | 8 | 0x8027cdd1 | `___mac_get_file` | B |
| 383 | `__mac_set_file` | 2 | 8 | 0x8027d019 | `___mac_set_file` | B |
| 384 | `__mac_get_link` | 2 | 8 | 0x8027cef5 | `___mac_get_link` | B |
| 385 | `__mac_set_link` | 2 | 8 | 0x8027d119 | `___mac_set_link` | B |
| 386 | `__mac_get_proc` | 1 | 4 | 0x8027c8c5 | `___mac_get_proc` | B |
| 387 | `__mac_set_proc` | 1 | 4 | 0x8027c985 | `___mac_set_proc` | B |
| 388 | `__mac_get_fd` | 2 | 8 | 0x8027cc7d | `___mac_get_fd` | B |
| 389 | `__mac_set_fd` | 2 | 8 | 0x8027cf05 | `___mac_set_fd` | B |
| 390 | `__mac_get_pid` | 2 | 8 | 0x8027c7f9 | `___mac_get_pid` | B |
| 391 | `__mac_get_lcid` | 2 | 8 | 0x8027ca39 | `___mac_get_lcid` | B |
| 392 | `__mac_get_lctx` | 1 | 4 | 0x8027cafd | `___mac_get_lctx` | B |
| 393 | `__mac_set_lctx` | 1 | 4 | 0x8027cbb9 | `___mac_set_lctx` | B |
| 394 | `setlcid` | 2 | 8 | 0x801dcb29 | `___setlcid` | B |
| 395 | `getlcid` | 1 | 4 | 0x801dcc11 | `___getlcid` | B |
| 396 | `read_nocancel` | 3 | 12 | 0x801eb031 | `___read_nocancel` | B |
| 397 | `write_nocancel` | 3 | 12 | 0x801eb405 | `___write_nocancel` | B |
| 398 | `open_nocancel` | 3 | 12 | 0x800b0bed | `___open_nocancel` | A |
| 399 | `close_nocancel` | 1 | 4 | 0x801cc3d1 | `___close_nocancel` | B |
| 400 | `wait4_nocancel` | 4 | 16 | 0x801d4fdd | `___wait4_nocancel` | A |
| 401 | `recvmsg_nocancel` | 3 | 12 | 0x8020a3a9 | `___recvmsg_nocancel` | B |
| 402 | `sendmsg_nocancel` | 3 | 12 | 0x80209ef1 | `___sendmsg_nocancel` | B |
| 403 | `recvfrom_nocancel` | 6 | 24 | 0x80209fd5 | `___recvfrom_nocancel` | A |
| 404 | `accept_nocancel` | 3 | 12 | 0x802095a9 | `___accept_nocancel` | B |
| 405 | `msync_nocancel` | 3 | 12 | 0x801d7de9 | `___msync_nocancel` | B |
| 406 | `fcntl_nocancel` | 3 | 12 | 0x801cad41 | `___fcntl_nocancel` | A |
| 407 | `select_nocancel` | 5 | 20 | 0x801eba71 | `___select_nocancel` | B |
| 408 | `fsync_nocancel` | 1 | 4 | 0x800b2a61 | `___fsync_nocancel` | B |
| 409 | `connect_nocancel` | 3 | 12 | 0x802098c1 | `___connect_nocancel` | B |
| 410 | `sigsuspend_nocancel` | 1 | 4 | 0x801defb5 | `___sigsuspend_nocancel` | B |
| 411 | `readv_nocancel` | 3 | 12 | 0x801eb2bd | `___readv_nocancel` | B |
| 412 | `writev_nocancel` | 3 | 12 | 0x801eb65d | `___writev_nocancel` | B |
| 413 | `sendto_nocancel` | 6 | 24 | 0x80209c15 | `___sendto_nocancel` | A |
| 414 | `pread_nocancel` | 5 | 20 | 0x801eb221 | `___pread_nocancel` | B |
| 415 | `pwrite_nocancel` | 5 | 20 | 0x801eb57d | `___pwrite_nocancel` | B |
| 416 | `waitid_nocancel` | 4 | 16 | 0x801d53d1 | `___waitid_nocancel` | B |
| 417 | `poll_nocancel` | 3 | 12 | 0x801ec1d9 | `___poll_nocancel` | B |
| 418 | `#418` | 0 | 0 | 0x801eaff1 | `___msgsnd_nocancel` | C |
| 419 | `#419` | 0 | 0 | 0x801eaff1 | `___msgrcv_nocancel` | C |
| 420 | `sem_wait_nocancel` | 1 | 4 | 0x8020e215 | `___sem_wait_nocancel` | B |
| 421 | `aio_suspend_nocancel` | 3 | 12 | 0x801c5c51 | `___aio_suspend_nocancel` | B |
| 422 | `__sigwait_nocancel` | 2 | 8 | 0x801df48d | `_____sigwait_nocancel` | B |
| 423 | `__semwait_signal_nocancel` | 7 | 28 | 0x801df259 | `___semwait_signal_nocancel` | A |
| 424 | `__mac_mount` | 5 | 20 | 0x800ae845 | `___mac_mount` | B |
| 425 | `__mac_get_mount` | 2 | 8 | 0x8027d321 | `___mac_get_mount` | B |
| 426 | `__mac_getfsstat` | 5 | 20 | 0x800b0069 | `___mac_getfsstat` | B |
| 427 | `fsgetpath` | 5 | 20 | 0x800b549d | `___fsgetpath` | B |
| 428 | `audit_session_self` | 0 | 0 | 0x801c1369 | `_audit_session_self` | B |
| 429 | `audit_session_join` | 1 | 4 | 0x801c136d | `_audit_session_join` | B |
| 430 | `fileport_makeport` | 2 | 8 | 0x801cdbf1 | `_fileport_makeport` | B |
| 431 | `fileport_makefd` | 1 | 4 | 0x801cdd95 | `_fileport_makefd` | A |
| 432 | `audit_session_port` | 2 | 8 | 0x801c1371 | `_audit_session_port` | B |
| 433 | `pid_suspend` | 1 | 4 | 0x8021bc35 | `_pid_suspend` | B |
| 434 | `pid_resume` | 1 | 4 | 0x8021bca5 | `_pid_resume` | B |
| 435 | `pid_hibernate` | 1 | 4 | 0x8021bd1d | `_pid_hibernate` | B |
| 436 | `pid_shutdown_sockets` | 2 | 8 | 0x8021bd75 | `_pid_shutdown_sockets` | B |
| 437 | `#437` | 0 | 0 | 0x801eaff1 | - | C |
| 438 | `shared_region_map_and_slide_np` | 6 | 24 | 0x8021c409 | `___shared_region_map_and_slide_np` | B |
| 439 | `kas_info` | 3 | 12 | 0x8021c605 | `_kas_info` | B |
| 440 | `memorystatus_control` | 5 | 20 | 0x801e5d2d | `_memorystatus_control` | B |
| 441 | `guarded_open_np` | 5 | 20 | 0x801ce3d1 | `___guarded_open_np` | B |
| 442 | `guarded_close_np` | 2 | 8 | 0x801ce4dd | `_guarded_close_np` | B |

---

## Appendix B — Mach trap table (`mach_trap_table`)
**Method.** Same kernel image. `mach_trap_table` is a run of **128 records of stride 8** in `LC_DYSYMTAB` local relocations (first relocated word 0x802ed4a4 … 0x802ed89c, `count=128`), i.e. table base **VA 0x802ed4a0**; record layout `{ u32 mach_trap_arg_count; kern_return_t (*mach_trap_function)(void *) }` — validated by the argument counts (31 -> 7, 32 -> 8, 61 -> 3, 44 -> 3, 89 -> 1) which match `osfmk/kern/syscall_sw.c`'s `MACH_TRAP` comments, and by the `kern_invalid` slot pattern. **mach_trap_count = 128** (`int mach_trap_count = (sizeof(mach_trap_table) / sizeof(mach_trap_table[0]));`, `xnu-2050.18.24/osfmk/kern/syscall_sw.c:375`). Names are the kernel's parallel `mach_syscall_name_table[128]` at **VA 0x802d22b8** (128 x char*); the 84 slots whose name is `kern_invalid` are the empty ones and share handler VA **0x80028495** (85 occurrences of that word in kernel data, 84 of them inside the table). Userland reaches trap *n* with `r12 = -n` (except `mach_absolute_time`, `r12 = -3`, which never goes through the table).
**class**: A = a `libsystem_kernel`/`libsystem_c` stub exists (the trap is part of the shipped C-runtime ABI, i.e. iOS 6 userspace can issue it), B = implemented but no stub, referenced by name from some cache image, C = empty slot (`kern_invalid`), D = implemented with neither stub nor importer (dead surface). `args` = 32-bit **words** the trap reads (so `mach_vm_allocate` = 5 words because its 64-bit `size` occupies two, while `xnu-2050.18.24`'s `MACH_TRAP(name, arg_count, …)` counts *arguments* = 4; `mach_wait_until_trap` = 2 words vs 1 argument; `mk_timer_arm_trap` = 3 vs 2). Reproduce with the kernelcache pipeline of `docs/research/ios6-platform.md` §3.5 (IMG3 `krnl` -> `ipsw img3 extract -r` -> AES-256-CBC with the published IV/key -> `complzss` LZSS) and then the structural anchors above.

| idx | name | args | handler | userspace stub | class |
|---|---|---|---|---|---|
| 0 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 1 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 2 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 3 | `kern_invalid` | 0 | 0x80028495 | `_mach_absolute_time` | C |
| 4 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 5 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 6 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 7 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 8 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 9 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 10 | `_kernelrpc_mach_vm_allocate_trap` | 5 | 0x80014609 | `__kernelrpc_mach_vm_allocate_trap` | A |
| 11 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 12 | `_kernelrpc_mach_vm_deallocate_trap` | 5 | 0x80014675 | `__kernelrpc_mach_vm_deallocate_trap` | A |
| 13 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 14 | `_kernelrpc_mach_vm_protect_trap` | 7 | 0x800146b9 | `__kernelrpc_mach_vm_protect_trap` | A |
| 15 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 16 | `_kernelrpc_mach_port_allocate_trap` | 3 | 0x8001470d | `__kernelrpc_mach_port_allocate_trap` | A |
| 17 | `_kernelrpc_mach_port_destroy_trap` | 2 | 0x8001475d | `__kernelrpc_mach_port_destroy_trap` | A |
| 18 | `_kernelrpc_mach_port_deallocate_trap` | 2 | 0x80014799 | `__kernelrpc_mach_port_deallocate_trap` | A |
| 19 | `_kernelrpc_mach_port_mod_refs_trap` | 4 | 0x800147d5 | `__kernelrpc_mach_port_mod_refs_trap` | A |
| 20 | `_kernelrpc_mach_port_move_member_trap` | 3 | 0x80014815 | `__kernelrpc_mach_port_move_member_trap` | A |
| 21 | `_kernelrpc_mach_port_insert_right_trap` | 4 | 0x80014859 | `__kernelrpc_mach_port_insert_right_trap` | A |
| 22 | `_kernelrpc_mach_port_insert_member_trap` | 3 | 0x800148b9 | `__kernelrpc_mach_port_insert_member_trap` | A |
| 23 | `_kernelrpc_mach_port_extract_member_trap` | 3 | 0x800148fd | `__kernelrpc_mach_port_extract_member_trap` | A |
| 24 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 25 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 26 | `mach_reply_port` | 0 | 0x8001ba91 | `_mach_reply_port` | A |
| 27 | `thread_self_trap` | 0 | 0x8001ba75 | `_thread_self_trap` | A |
| 28 | `task_self_trap` | 0 | 0x8001ba55 | `_task_self_trap` | A |
| 29 | `host_self_trap` | 0 | 0x80019de1 | `_host_self_trap` | A |
| 30 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 31 | `mach_msg_trap` | 7 | 0x80014f91 | `_mach_msg_trap` | A |
| 32 | `mach_msg_overwrite_trap` | 8 | 0x80014e3d | `_mach_msg_overwrite_trap` | A |
| 33 | `semaphore_signal_trap` | 1 | 0x8002775d | `_semaphore_signal_trap` | A |
| 34 | `semaphore_signal_all_trap` | 1 | 0x800277e1 | `_semaphore_signal_all_trap` | A |
| 35 | `semaphore_signal_thread_trap` | 2 | 0x800276e9 | `_semaphore_signal_thread_trap` | A |
| 36 | `semaphore_wait_trap` | 1 | 0x80027a85 | `_semaphore_wait_trap` | A |
| 37 | `semaphore_wait_signal_trap` | 2 | 0x80027c2d | `_semaphore_wait_signal_trap` | A |
| 38 | `semaphore_timedwait_trap` | 3 | 0x80027b6d | `_semaphore_timedwait_trap` | A |
| 39 | `semaphore_timedwait_signal_trap` | 4 | 0x80027d11 | `_semaphore_timedwait_signal_trap` | A |
| 40 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 41 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 42 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 43 | `map_fd` (name table) — **slot is `kern_invalid`**, name is stale | 0 | 0x80028495 | - | C |
| 44 | `task_name_for_pid` | 3 | 0x8021bb1d | `_task_name_for_pid` | A |
| 45 | `task_for_pid` | 3 | 0x8021b96d | `_task_for_pid` | A |
| 46 | `pid_for_task` | 2 | 0x8021b921 | `_pid_for_task` | A |
| 47 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 48 | `macx_swapon` | 5 | 0x8021c705 | `_macx_swapon` | A |
| 49 | `macx_swapoff` | 3 | 0x8021c959 | `_macx_swapoff` | A |
| 50 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 51 | `macx_triggers` | 4 | 0x8021c6e5 | `_macx_triggers` | A |
| 52 | `macx_backing_store_suspend` | 1 | 0x8021c661 | `_macx_backing_store_suspend` | A |
| 53 | `macx_backing_store_recovery` | 1 | 0x8021c609 | `_macx_backing_store_recovery` | A |
| 54 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 55 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 56 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 57 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 58 | `pfz_exit` | 0 | 0x80027ded | - | D |
| 59 | `swtch_pri` | 0 | 0x80027eb9 | `_swtch_pri` | A |
| 60 | `swtch` | 0 | 0x80027df1 | `_swtch` | A |
| 61 | `thread_switch` | 3 | 0x800280a9 | `_syscall_thread_switch` | A |
| 62 | `clock_sleep_trap` | 5 | 0x800179cd | `_clock_sleep_trap` | A |
| 63 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 64 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 65 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 66 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 67 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 68 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 69 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 70 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 71 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 72 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 73 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 74 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 75 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 76 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 77 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 78 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 79 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 80 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 81 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 82 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 83 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 84 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 85 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 86 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 87 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 88 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 89 | `mach_timebase_info_trap` | 1 | 0x80016b05 | `_mach_timebase_info` | A |
| 90 | `mach_wait_until_trap` | 2 | 0x800171cd | `_mach_wait_until` | A |
| 91 | `mk_timer_create_trap` | 0 | 0x8001fb19 | `_mk_timer_create` | A |
| 92 | `mk_timer_destroy_trap` | 1 | 0x8001fd25 | `_mk_timer_destroy` | A |
| 93 | `mk_timer_arm_trap` | 3 | 0x8001fd69 | `_mk_timer_arm` | A |
| 94 | `mk_timer_cancel_trap` | 2 | 0x8001fded | `_mk_timer_cancel` | A |
| 95 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 96 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 97 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 98 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 99 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 100 | `iokit_user_client_trap` (not named in the 2107 name table; confirmed by the shipped userspace stub `_iokit_user_client_trap` in `IOKit.framework/Versions/A/IOKit` @0x31d4c268 — `mov ip,sp; push {r4,r5,r6,r8}; ldm ip,{r4,r5,r6,r8}; mvn ip,#0x63; svc #0x80`, i.e. r12 = -100 with 8 words) | 8 | 0x8026d441 | `_iokit_user_client_trap` (IOKit.framework) | B |
| 101 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 102 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 103 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 104 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 105 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 106 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 107 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 108 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 109 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 110 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 111 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 112 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 113 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 114 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 115 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 116 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 117 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 118 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 119 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 120 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 121 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 122 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 123 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 124 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 125 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 126 | `kern_invalid` | 0 | 0x80028495 | - | C |
| 127 | `kern_invalid` | 0 | 0x80028495 | - | C |
