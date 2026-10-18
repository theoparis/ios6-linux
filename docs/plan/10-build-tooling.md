# 10 — Build tooling, CLI, and CI

Verified against the local install (LLVM 23.1.3, Capstone 5.0.9, CMake 4.4.4,
Ninja 1.13.2, clang 23.1.3, Arch x86-64) in a scratch tree under `/tmp/ios6smoke`
(no repo files touched). Every claim below is either cited to
`docs/research/llvm-tooling.md` §N or to a command run in that scratch tree. All of §0 and §4–§5
were re-run from scratch on this machine during the audit (`cmake -S . -B b -G Ninja && ninja -C b && ./b/app`
printed the three lines below); where a command differed from the first draft the text now shows the
command that actually ran.

## 0. Verified build facts (smoke tests)

### (a) LLVM discovery + shared target

`find_package(LLVM 23.1 CONFIG REQUIRED)` + `LLVM` shared target with
`project(... C CXX)` configures, builds and runs on this box:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ios6recomp C CXX)

set(LLVM_DIR /usr/lib/cmake/llvm)           # belt and braces; see Hazard below: 23.1 alone already selects it
find_package(LLVM 23.1 REQUIRED CONFIG)
message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} @ ${LLVM_DIR}")   # -> LLVM 23.1.3 @ /usr/lib/cmake/llvm

add_executable(app main.cpp)
target_include_directories(app PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(app PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(app PRIVATE LLVM)
set_target_properties(app PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)   # main.cpp below also builds as 20
```

Observed: `LLVM 23.1.3 @ /usr/lib/cmake/llvm`. `project(... C CXX)` is required
because `LLVMConfig.cmake` calls `find_package(LibEdit)` → `check_include_file()`
(C language); with only CXX enabled configure dies with
`try_compile() works only for enabled languages. Currently these are: CXX`
([research/llvm-tooling.md §1.6](../research/llvm-tooling.md)).
`llvm_map_components_to_libnames()` MUST NOT be used: the distro ships only
`libLLVMDemangle/Support/TableGen/HTTP/Testing*.a`; per-component archives are
absent and the link fails with `mold: fatal: library not found: LLVMARMDisassembler`
(verified: `ls /usr/lib/libLLVM*.a` → only `Debuginfod, Demangle, HTTP, Support, TableGen, TestingAnnotations, TestingSupport`;
the real code is in `libLLVM.so` → `libLLVM.so.23.1`). Link the exported `LLVM` shared target only.

### (b) MC disassembler smoke (`armv7s-apple-ios6`, CPU `swift`)

Decodes `sdiv r0,r1,r2` (ARM bytes `11 f2 10 e7`) and an `IT` (Thumb bytes `08 bf`) and prints
`MCInstrInfo::getName`. The program is `main.cpp` of the scratch tree, calling exactly the signatures
in [04-translation.md §2](04-translation.md); measured output:

```
ARM sdiv: status=3 size=4 name=SDIV nops=4531
Thumb IT: status=3 size=2 name=t2IT nops=4531
```

(`DecodeStatus` `Success` = 3.)

Key recipe (from [research/llvm-tooling.md §1.7](../research/llvm-tooling.md)):
keep one `MCTargetOptions` alive for the whole session (it is stored by
*reference* in `MCAsmInfo`), decode with CPU `swift` (plain `armv7` rejects
valid `armv7s` encodings), and select Thumb mode via the `+thumb-mode`
feature string (or address bit 0, depending on the call path). Observed opcode
counts: `MII->getNumOpcodes() == 4531` for ARM.

### (c) LLJIT hello

Compiles an IR `add(i32,i32)->i32` and calls it (same `main.cpp`; needs `InitializeAll*`):

```
LLJIT hello: add(7,35)=42
```

Requires `InitializeAllTargetInfos/Targets/MCs/...` before `LLJITBuilder().create()`,
otherwise `Unable to find target for this triple`.

### (d) Capstone

```
pkg-config --modversion capstone   -> 5.0.9
Capstone version: 5.0
ARM:   sdiv r1, r2, r1
Thumb: it eq
Thumb: moveq r0, r1
```

Measured with `clang++ c.cpp $(pkg-config --cflags --libs capstone)`: `CS_API_MAJOR.MINOR` = 5.0; ARM
`11 f2 10 e7` → `sdiv r0, r1, r2`; Thumb `08 bf 01 46` → `it eq` / `moveq r1, r0`. Used as the independent
oracle (ADR-07) and by `ios6recomp disasm`. BSD-3-Clause, freely linkable.

### (e) lld: library vs. subprocess

`liblldCommon.so`, `liblldELF.so`, `liblldMachO.so`, … and
`/usr/include/lld/Common/Driver.h` (with `lldMain()` + `LLD_ALL_DRIVERS`) exist
locally, so **both** paths are available. The plan uses:

- **library** (`lldMain` with `LLD_ALL_DRIVERS`) for in-process AOT inside
  `lib/aot` where avoiding a process spawn per link matters. `Driver.h` itself warns that after a crash
  `lldMain` may not be callable again (use `exitLld()`), so the library path gets no crash-recovery
  promise; the offline `build` uses the subprocess;
- **subprocess** (`ld.lld`) for the offline `ios6recomp build` path and for the ELF-twin
  fixtures (`ld.lld -m armelf_linux_eabi`, [12-testing.md §2.2](12-testing.md)), because it is simpler,
  debuggable, and matches the linker command lines users can reproduce.
- **Not lld:** the armv7 *Mach-O* fixtures. `ld64.lld -arch armv7` fails with
  `missing or unsupported -arch armv7` (measured, with and without `-platform_version`); they are linked by
  the in-tree `ios6recomp-ld` ([ADR-14](02-decisions.md)). `ld64.lld` is only useful here as a
  test oracle for arm64 Mach-O.

### (f) Sanitizers

`-fsanitize=address,undefined` and `-fsanitize=thread` both link and run against
the shared `libLLVM.so`:

```
san ok   (ASan+UBSan)
san ok   (TSan)
```

### (g) Host compiler/linker and C++ standard

- Default host linker on this box is **mold** (`/usr/bin/ld -> mold`, `ld --version` = mold 3.0.0);
  `ld.lld` and `mold` are both installed. Use `lld` for reproducible AOT output (ADR-10),
  `mold` for dev builds if desired.
- LLVM's own baseline is **C++17** (`llvm-config --cxxflags` → `-std=c++17`,
  `-fno-exceptions`). LLVM is built with **RTTI ON, EH OFF**.
  Project-wide flags that follow:
  - `-frtti` (default) — do **not** pass `-fno-rtti`; ORC's `RTTIExtends<>`
    and `dynamic_cast` across the boundary rely on it.
  - `-fno-exceptions` is **not** imposed project-wide: our own code may use
    exceptions, but they MUST NOT propagate through LLVM frames (LLVM is not
    exception-safe). Catch at our boundaries.
  - `-std=c++20` compiles cleanly against the LLVM 23 headers (verified);
    LLVM itself stays C++17.
- `ccache`: **not installed** on this box (install `ccache` for dev builds).
  Not a hard dependency.

### (h) `compile_commands.json`, clang-tidy, clang-format

`clang-tidy` 23.1.3 and `clang-format` 23.1.3 are installed. Emit
`compile_commands.json` via `CMAKE_EXPORT_COMPILE_COMMANDS=ON` (preset default)
for clangd/clang-tidy.

### Hazard: the second LLVM at `/usr/local/lib/cmake/llvm`

That tree reports `LLVM_PACKAGE_VERSION 24.0.0git`, `LLVM_ENABLE_RTTI OFF`
(`grep` of `/usr/local/lib/cmake/llvm/LLVMConfig.cmake`), static-only archives (`/usr/local/lib/libLLVM*.a`),
and `/usr/local/include/llvm` does not exist. CMake
searches `/usr/local` first, so a naive `find_package(LLVM CONFIG REQUIRED)`
picks it and fails at "cannot find `llvm/Support/...`". Mitigation, verified:

```cmake
set(LLVM_DIR /usr/lib/cmake/llvm)          # explicit hint
find_package(LLVM 23.1 REQUIRED CONFIG)     # major.minor exact match
```

`find_package(LLVM 23 REQUIRED CONFIG)` **fails** ("Could not find a configuration file for
package "LLVM" that is compatible with requested version "23"", listing
`/usr/local/lib/cmake/llvm/LLVMConfig.cmake, version: 24.0.0git`); `23.1` succeeds and selects
`/usr/lib/cmake/llvm` **even without the `LLVM_DIR` hint** (re-run with `LLVM_DIR` unset:
`got /usr/lib/cmake/llvm 23.1.3`), because 24.0.0git is a different major.minor. The hint is kept
as documentation. Document this in the top-level `CMakeLists.txt` and in `CONTRIBUTING`/CI.

### (i) AOT `.so` from an LLVM object with a fixed address

`clang -target x86_64-linux-gnu -fPIC -c f.ll -o f.o && ld.lld -shared --image-base=0x100000000 -o f.so f.o`
produces an `ET_DYN` x86-64 object whose first `PT_LOAD` has `p_vaddr = 0x100000000`; `dlopen` + call
returns `result=42` with `fn=0x100001270`, i.e. the kernel/ld.so honoured the link address.
With `--image-base=0x10000000000` (1 TiB, the ADR-04 `K`) and `-z noseparate-code`: `fn=0x10000001270`,
`result=42`. `-z noseparate-code` made no difference to the segment layout in this test (still three `PT_LOAD`s:
R, R E, RW) — it is not needed. A linker script is only required for the spill region and was not needed for the smoke test.
(clang prints `-Woverride-module` for the `.ll` triple; add `-Wno-override-module` or set the triple in the IR.)

## 1. Repository / CMake structure

Layout per [01-architecture.md](01-architecture.md) §Repository layout: one
static library per component.

```
CMakeLists.txt                 # LLVM discovery, global flags, presets hooks
cmake/ios6recompConfig.cmake   # (later) install export
lib/<component>/CMakeLists.txt  # one static lib per component
include/ios6/<component>/       # public headers only
tools/ios6recomp/CMakeLists.txt
tests/...
CMakePresets.json
```

Per-component:

```cmake
add_library(ios6_<comp> STATIC ${sources})
target_include_directories(ios6_<comp>
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ios6_<comp> PUBLIC ios6_<dep> LLVM)   # only where needed
```

**Dependency-rule enforcement** (so `lift` cannot include `kernel`):

1. Per-target `target_include_directories(... PRIVATE ...)` only — components
   may include their *own* `include/ios6/<comp>/` and the explicitly listed
   deps' public dirs. `lift`'s `target_include_directories` lists
   `include` (for its own `ios6/lift/`) plus `ios6/guestcode` and
   `ios6/guestmem`, but **not** `ios6/kernel` or `ios6/builtins`.
2. A CI script `scripts/check-includes.py` parses `#include "ios6/..."` in
   every `lib/<comp>/*.cpp`/`*.hpp` and fails the build if an edge violates
   the table in [01-architecture.md](01-architecture.md) (lift/jit/aot must not
   depend on kernel/builtins; runtime must not depend on lift). Runs in CI
   before compile.
3. Link-time: `target_link_libraries` only wires the allowed edges; an
   accidental include of a forbidden header still fails at compile because the
   include dir is absent.

**CMakePresets.json** (minimum set):

| Preset | Purpose |
|---|---|
| `dev` | Ninja, clang, `-O1 -g`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, mold or lld |
| `asan` | `-fsanitize=address,undefined`, `-O1 -g`, tests under ctest |
| `tsan` | `-fsanitize=thread`, for JIT concurrency tests (05-jit §8) |
| `release` | `-O3 -DNDEBUG`, lld, assertions off |
| `fixtures` | builds `tests/fixtures/` with clang `--target=armv7-apple-ios6.0` (and `armv7s-`) `-c`, links them with `ios6recomp-ld` ([ADR-14](02-decisions.md)), and builds the ELF twins (`armv7-linux-gnueabi`, `ld.lld -m armelf_linux_eabi`) |

**Translator versioning / cache key** (05-jit.md §6): a single
`IOS6_TRANSLATOR_VERSION` macro (bump on any change to semantics table, IR
shape, or lowering) feeds the persistent-cache key:
`hash(translator version, target triple, opt level, tier, image UUID, page content hash)`.
Bump policy: any commit touching `lib/lift/semantics/` or `lib/lift/emit/`
bumps it; CI checks that the macro is referenced in the cache-key test.

## 2. `ios6recomp` CLI surface

Subcommands (M0 must provide `ipsw`, `cache`, `macho`):

| Subcommand | Purpose |
|---|---|
| `ipsw` | `ipsw list <file.ipsw>` prints roles/filenames from `BuildManifest.plist` |
| `cache` | `cache info <dsc>` prints header fields, arch, image count, dylib list, subcache presence |
| `macho` | `macho info <path>` prints header, cputype/cpusubtype, load commands, encryption info |
| `disasm` | Capstone-backed disassembly (ADR-07 tooling decoder) |
| `map` | `map app` shows segments at guest vmaddr with protections |
| `run` | execute an app/image under the runtime |
| `build` | AOT: `build app.ipa -o app.ios6` (M8) |
| `trace` | replay/inspect `IOS6RECOMP_SYSCALL_TRACE` |
| `stats` | dump `IOS6RECOMP_LIFT_STATS` counters |

Exit codes: `0` success; `1` usage/args; `2` input not found/unreadable;
`3` encrypted binary (`E_ENCRYPTED_BINARY`, ADR-12); `4` unsupported
arch/format; `5` runtime fault/abort in strict mode. Errors go to stderr with a
single-line `ios6recomp: <code>: <message>`.

Env vars (from [01-architecture.md](01-architecture.md) §Control surface):
`IOS6RECOMP_STRICT`, `IOS6RECOMP_LIFT_STATS`, `IOS6RECOMP_SYSCALL_TRACE`,
`IOS6RECOMP_TIER`, `IOS6RECOMP_IMAGE_OVERRIDE`, `IOS6RECOMP_BLOCKDUMP`,
`IOS6RECOMP_STRICT_FP`, `IOS6RECOMP_JIT_PAGES`.

Config/cache dirs: `$XDG_CACHE_HOME/ios6recomp` (default
`~/.cache/ios6recomp`), with subdirs `inputs/`, `cache/` (persistent
translation cache, 05-jit §6), `logs/`. Inputs are never modified in place;
`IOS6RECOMP_HOME` overrides the root.

Logging: use a **minimal own logger** (timestamped lines to stderr + optional
file), not `spdlog`. Rationale: dependency-minimal policy (§3) and only two
call sites need it (CLI + runtime). Revisit only if structured logging becomes
a requirement.

## 3. Third-party dependency policy

Policy: prefer system packages; vendor only what is not packaged or is
license-restricted; no GPL/APSL code in the runtime binary or process tree
(prior-art.md §7–8).

| Dependency | License | Source | Notes |
|---|---|---|---|
| LLVM 23.1.3 | Apache-2.0 WITH LLVM-exception | system `/usr` | shared `libLLVM.so`; the whole ORC/JITLink stack is inside it |
| lld | Apache-2.0 WITH LLVM-exception | system | library + subprocess |
| Capstone 5.0.9 | BSD-3-Clause | system | oracle + `disasm` |
| zlib | zlib | system | not needed by M0: the IPSW rootfs/kernelcache are unwrapped by the patched `ipsw`, C++ only reads the extracted cache/Mach-O |
| A plist parser | — | vendored minimal XML-plist reader (~200 lines) | no C++ plist lib is standard; M0 needs `BuildManifest.plist` |
| SDL3 (or SDL2) | zlib | system, optional | windowing for M5; defer until M5 |
| Mesa (GLES, EGL) | MIT | system | host GLES for `OpenGLES.framework` ([ADR-11](02-decisions.md); ANGLE was rejected, [09-media.md §1.3](09-media.md)); M5 |
| OpenAL-Soft | LGPL-2.0 | system, optional | audio; M5 |
| PipeWire | MIT | system, optional | audio; M5 |
| lzss | — | not linked | IMG3 kernelcache LZSS is handled inside the patched `ipsw` ([research/ios6-platform.md §3.5](../research/ios6-platform.md)); the C++ tree never decompresses IMG3 |

**Not depended on (stays out of tree):** Darling (GPL-3.0), QEMU
(GPL-2.0-or-later), Unicorn (GPL-2.0), RPCS3/Dolphin (GPL), and all Apple
sources (APSL-2.0) — read-only reference only, never linked or copied
(prior-art.md §7, §8). Only permissively licensed code (MIT/ISC/BSD/Apache)
may be linked.

**Project license recommendation:** MIT or Apache-2.0 for our own code, so the
runtime stays permissively licensed and can be embedded; document that Apple
binaries and decrypted apps are user-supplied and never redistributed
(prior-art.md §8 bottom line). Apache-2.0 is preferred if we vendor any
Apache code (LLVM, libdispatch) to keep notice obligations simple.

## 4. Input-prep tooling

Supported no-root path ([research/ios6-platform.md §3.3–§3.5](../research/ios6-platform.md)):

```bash
ipsw extract --dyld   --lookup -o work <ipsw>
ipsw extract --kernel --lookup -o work <ipsw>
```

The patched `~/src/ipsw` is required, **built from its current working tree** (`cd ~/src/ipsw && go build -o ipsw ./cmd/ipsw`;
`cmd/ipsw` exists). Measured: the binary `~/src/ipsw/ipsw` (built 2026-10-16 23:34, byte-identical to
`/usr/bin/ipsw`, sha1 `2b2df349…`) predates the DeviceTree fix and fails both commands
(`failed to decrypt Img3 DeviceTree: IMG3 data is not a multiple of the block size`, then
`DMG is encrypted ('encrcdsa') and no firmware key is available`); `/tmp/ipsw-new` (built after the fixes)
reproduced both commands on `~/Downloads/iPad2,1_6.1.3_10B329_Restore.ipsw`:
`--dyld` 10.9 s, `--kernel` 0.14 s, no root, output
`246,791,518 B sha1 8a9d9587d0bc91871458d50bce71682eb341d26a` (dyld cache) and
`13,987,840 B sha1 3a65ceeba525af4e0959ffcf7b45edc9c6fd8a74` (kernelcache). The kernelcache file is the
13,987,783-byte Mach-O (`research/ios6-platform.md` §3.5) followed by 57 zero bytes (measured: `d[13987783:] == b'\0'*57`,
`d[:4] == ce fa ed fe`), so `prepare-inputs.sh` checks the size *and* sha1, and the loader must not read past
`LC_*` extents. `scripts/prepare-inputs.sh` therefore MUST rebuild or refuse a stale `ipsw`
(refuse when `ipsw extract` prints `failed to decrypt Img3 DeviceTree`).

Changed files (from `git -C ~/src/ipsw status --short`, re-run during the audit; 14 entries — the last
two `hfs_decmpfs*` files appeared while the decmpfs work was in progress, so re-run the command before relying on this list):

```
 M internal/commands/extract/extract.go
 M internal/utils/macos.go
 M pkg/devicetree/image.go
 M pkg/dyld/extract.go
 M pkg/img3/img3.go
 M pkg/kernelcache/kernelcache.go
?? internal/utils/dmg_decrypt.go
?? internal/utils/hfs_decmpfs.go
?? internal/utils/hfs_decmpfs_test.go
?? internal/utils/hfs_userspace.go
?? internal/utils/hfs_userspace_test.go
?? pkg/devicetree/image_test.go
?? pkg/img3/decrypt_test.go
?? pkg/kernelcache/img3_test.go
```

The fixes behind those: userspace HFS+ extraction for UDIF/APM images
(`hfs_userspace.go`), `encrcdsa` decryption plumbed through `extract`
(`dmg_decrypt.go`), IMG3 DeviceTree tag-pad decrypt fix (`image.go`), and IMG3
kernelcache support (`kernelcache.go`, `img3.go`), and HFS+ compression (decmpfs) in the userspace extractor (`hfs_decmpfs.go`;
needed because iOS 6 on-disk binaries are HFS-compressed, [research/ios6-platform.md §3.6](../research/ios6-platform.md),
[13-risks.md R-17](13-risks.md)). Rootfs is `encrcdsa`-encrypted; the 72-hex key is auto-fetched by `--lookup`
(third-party key database, run-time fetch only, [ADR-12](02-decisions.md)).

Command lines and expected hashes are documented in
[research/ios6-platform.md](../research/ios6-platform.md) §3.4–3.5 (rootfs DMG
decrypt → UDZO → HFSX partition 5; kernelcache IMG3 → AES-256-CBC → complzss
→ LZSS → 13,987,783-byte Mach-O (the extracted file is 13,987,840 B, zero-padded), `cputype 12 cpusubtype 9`,
`Darwin Kernel Version 13.0.0 … xnu-2107.7.55.2.2`).

`scripts/prepare-inputs.sh` design: idempotent; checks for the patched `ipsw`
binary; runs the two extract commands into `work/10B329__iPad2,1/`;
verifies size+sha1 of the produced dyld cache (246,791,518 B, `8a9d9587…`) and kernelcache (13,987,840 B, `3a65ceeb…`); refuses to run if
artefacts already present unless `--force`; never commits artefacts (all under
gitignored `work/`).

## 5. AOT build path

`ios6recomp build` emits LLVM objects and drives `lld` (ADR-10). Verified
invocation for a loadable `.so` from an LLVM/clang object at a fixed address:

```bash
clang -target x86_64-linux-gnu -fPIC -Wno-override-module -c out.ll -o out.o
ld.lld -shared --image-base=0x100000000 -o out.so out.o
```

Observed (§0(i)): `out.so` is `DYN (Shared object file)`, `Machine: Advanced Micro Devices X86-64`, and
`dlopen` + call returns the expected value. For the shadow region use `--image-base=0x10000000000` (1 TiB);
`-z noseparate-code` was measured to change nothing; a linker script is needed only for the spill pool.
UNVERIFIED: that an AOT `.so` can be *placed inside* a region already reserved `PROT_NONE` by the
runtime (ADR-04): `dlopen` honoured the link address only because nothing occupied it. Close in M3.5
by `dlopen`ing after the reservation, or by mapping the `.so`'s `PT_LOAD`s manually with `MAP_FIXED`.
`cryptid != 0` images are refused before this path (ADR-12).

## 6. CI

- Matrix: `dev` (clang 23), `asan`, `tsan`, `release`; Linux x86-64 only for M0.
- Required tools on a fresh Arch box: `clang llvm lld cmake ninja capstone
  pkgconf python3` (package names: `clang llvm lld cmake ninja capstone
  pkgconf python`). `ccache` optional.
- Cache: ccache (if present) + the LLVM install (already system); no network
  fetch of Apple content.
- Time budget: M0 under 10 minutes including unit tests; include-graph checker
  runs before compile.
- Never run heavy builds in agent/CI exploratory steps; only the M0 smoke
  targets above.

## 7. Verification

- M0 exit: `ipsw list` prints `OS`/`KernelCache`/`RestoreRamDisk`; `cache info`
  prints magic `dyld_v1   armv7`, header size 0x58 (`mappingOffset`), mappings 3, image
  count 569, `dyldBaseAddress` 0x2fe00000 and a shared-region start derived from `mapping[0].address`
  (0x30000000; the iOS 6 header has no such field, [research/ios6-platform.md §5.2](../research/ios6-platform.md)); `macho info` on a fixture reports
  `cputype=12 cpusubtype=9`; CI green under 10 min. Tied to `12-testing.md`
  fixture corpus and `11-milestones.md` M0.
- Each smoke test above is re-runnable from `/tmp/ios6smoke` and is the
  acceptance check for the corresponding build claim. The audit re-ran (a)–(d), (f), (i) and §4
  successfully; (g) `-std=c++20` was exercised by the same `main.cpp`; (e)'s `lldMain` path was **not**
  exercised (only the headers/libraries were confirmed present: `/usr/include/lld/Common/Driver.h`
  declares `lldMain(ArrayRef<const char*>, raw_ostream&, raw_ostream&, ArrayRef<DriverDef>)` and `LLD_ALL_DRIVERS`).
  UNVERIFIED: in-process `lldMain` re-entrancy across many links; close in M8 with a loop of 1000
  `lldMain` calls under ASan.
