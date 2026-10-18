# ios6-recomp — plan

A recompiler + Wine-like runtime that runs iOS 6 (ARMv7 / ARMv7s) binaries on modern x86-64 Linux.
`armv7` Mach-O in, native x86-64 execution out, with a substituted kernel boundary underneath.

This directory is the build plan. It is written to be executed by an engineer who has never seen the
project, and it is deliberately specific: exact toolchain versions, exact interfaces, exact exit
criteria. Where a decision is still open it is marked, with the experiment that closes it.

## Why this is plausible

Three facts make this a bounded engineering project rather than a research gamble:

1. **The guest is userland-only and single-ISA-family.** iOS 6 was the last 32-bit-only iOS. Every
   binary of interest is ARMv7 or ARMv7s with Thumb-2, VFPv3 and (armv7s) NEON. There is no 64-bit
   guest code, no second ISA, no page-table or MMU semantics to emulate — guest virtual addresses
   can be *identity-mapped* onto the host ([ADR-02](02-decisions.md)).
2. **Apple's frameworks are just code we can translate.** Unlike Wine, we do not have to reimplement
   `CoreFoundation`, `Foundation`, or `libobjc`: they ship as ARMv7 dylibs inside the device's dyld
   shared cache, and we can run their translated code with exact ABI fidelity. What we *do* have to
   replace is much smaller: the kernel trap surface and the hardware-services boundary
   ([ADR-01](02-decisions.md), [ADR-09](02-decisions.md)).
3. **The hard parts have working prior art.** Darling (Mach IPC and a Linux `libsystem_kernel`),
   touchHLE (iOS 2.x/3.x apps on modern hosts, `objc_msgSend` and UIKit substitution), FEX-Emu and
   Box64 (register caching, flag optimisation, thunks), RPCS3/Yuzu (LLVM recompilers with persistent
   translation caches). See [research/prior-art.md](../research/prior-art.md).

## Non-goals

- **Not** a bootable iOS emulator. We do not run SpringBoard, `launchd`, or the boot chain
  ([ADR-08](02-decisions.md)).
- **Not** an ARMv7 instruction-set simulator project: we target exactly what iOS 6 system dylibs and
  App Store apps contain, verified by measurement, not the full ARMv7-A manual.
- **Not** a jailbreak, decryption, or DRM tool. Encrypted binaries are rejected
  ([ADR-12](02-decisions.md)).
- **Not** a general macOS layer. Darwin-specific APIs are implemented only as far as iOS 6 apps use
  them.
- **No ARM hardware in the loop, ever.** Everything must run from files on an x86-64 Linux box.

## Input contract

Exactly like Wine, but sharper, because Apple's firmware is not redistributable:

| Input | Who provides | Used for |
|---|---|---|
| iOS 6.x IPSW for an armv7 device | user | dyld shared cache (all frameworks), kernelcache (trap-table cross-check), device tree. Rootfs is `encrcdsa`-encrypted; its key is public (theapplewiki) and fetched by `ipsw --lookup` |
| Decrypted app binary (+ its `Payload/*.app` resources) | user | the thing we actually run |
| iOS 6 SDK headers/frameworks, if available | user, optional | building test fixtures and `.tbd` symbol tables; not required to run |

Nothing derived from Apple's binaries is committed to this repo. The docs quote observed values
(addresses, sizes, counts, symbol names) as measurements only. `.gitignore` excludes `work/`,
`*.ipsw`, `*.dsc`, `artifacts/`.

## Local environment (measured, this machine)

| Component | Observed |
|---|---|
| clang / clang++ | 23.1.3 (`/usr/bin/clang`), `Target: x86_64-pc-linux-gnu` |
| llvm-config | 23.1.3 |
| `llvm-mc`, `llvm-tblgen`, `lld`, `ld.lld` | present |
| cmake, ninja | present |
| `qemu-arm` / `qemu-arm-static` | 11.1.2 |
| `ipsw` CLI (blacktop/ipsw) | `/usr/bin/ipsw` (stock) cannot read this IPSW; the **patched** build from `~/src/ipsw` (see below) extracts the dyld cache and kernelcache with no root |
| Input firmware | `iPad2,1_6.1.3_10B329_Restore.ipsw` (iPad 2 Wi-Fi, S5L8940, Cortex-A9, armv7), `OS` image `048-2634-005.dmg` (`encrcdsa`), `kernelcache.release.k93` (IMG3), `RestoreRamDisk` `048-2516-005.dmg` (IMG3) |

Local artifacts are a convenience for development, **not** part of the build. The test suite
(`12-testing.md`) must be runnable from self-built fixtures alone.

## Getting the inputs (verified)

```
ipsw extract --dyld   --lookup -o work iPad2,1_6.1.3_10B329_Restore.ipsw   # 6 s, no root
ipsw extract --kernel --lookup -o work iPad2,1_6.1.3_10B329_Restore.ipsw   # 3 s, no root
# -> work/10B329__iPad2,1/dyld_shared_cache_armv7   246,791,518 B  sha1 8a9d9587d0bc91871458d50bce71682eb341d26a
# -> work/10B329__iPad2,1/kernelcache.release.iPad2,1  13,987,840 B  Mach-O armv7 (xnu-2107)
```

Stock ipsw fails on iOS 6: it mounts the rootfs through a loop device (root) and cannot handle the
UDIF/APM/HFSX payload, never decrypts `encrcdsa` in `extract`, rejects the IMG3 DeviceTree
(its DATA tag carries ciphertext in the pad), and cannot parse IMG3 kernelcaches. The fixes live in
`~/src/ipsw` ([10-build-tooling.md](10-build-tooling.md) lists the changed files); extracted output
was verified byte-identical to an independent decrypt+`7z` extraction.

## Reading order

| Doc | Contents |
|---|---|
| [02-decisions.md](02-decisions.md) | The ADR log. Read first; everything else depends on it. |
| [01-architecture.md](01-architecture.md) | Pipeline, components, address space, execution model, interfaces. |
| [03-loader.md](03-loader.md) | Mach-O, fat binaries, the dyld shared cache, bind/rebase, symbol resolution. |
| [04-translation.md](04-translation.md) | Decoding, block discovery, the semantic table, IR shape, flags, VFP/NEON, memory. |
| [05-jit.md](05-jit.md) | ORC/JITLink integration, block materialisation, linking, persistence, tiering. |
| [06-runtime-core.md](06-runtime-core.md) | Thread context, threads, TSD, signals, SjLj exceptions, the FFI bridge to builtins. |
| [07-kernel-boundary.md](07-kernel-boundary.md) | BSD syscalls, Mach traps, `mach_msg`, VM, kqueue→epoll, time, filesystem, IOKit. |
| [08-frameworks.md](08-frameworks.md) | `objc_msgSend` and the ObjC runtime, CF/Foundation, libdispatch, UIKit strategy. |
| [09-media.md](09-media.md) | OpenGLES/EAGL (Mesa GLES), IOSurface, compositor, CoreAudio, touch/input, sensors. |
| [10-build-tooling.md](10-build-tooling.md) | CMake, LLVM integration, the `ios6recomp` CLI, fixtures, AOT mode, CI. |
| [11-milestones.md](11-milestones.md) | M0–M9 with exit criteria and numbers. |
| [12-testing.md](12-testing.md) | Differential testing, instruction fuzzing, syscall tracing, benchmarks. |
| [13-risks.md](13-risks.md) | Risk register with triggers and mitigations. |
| [14-open-questions.md](14-open-questions.md) | Open questions, each with the experiment that closes it. |
| [research/](../research/) | Raw verified reference notes (platform facts, ABI, LLVM tooling, prior art). |

## Status

Planning. No code yet. The first executable artefact is `M0` in [11-milestones.md](11-milestones.md):
a CLI that parses the local IPSW, parses the dyld shared cache, and prints the image list — chosen
because it is the fastest way to falsify the plan's platform assumptions
([ADR-04](02-decisions.md)'s spike and [03-loader.md](03-loader.md)'s cache model both depend on it).
