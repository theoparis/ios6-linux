# 08 — Frameworks: ObjC, CF/Foundation, libdispatch, UIKit

Decided by [ADR-09](02-decisions.md) (framework cut) and [ADR-01](02-decisions.md) (builtin/guest/stub).
Every measured value below is from the local iPad2,1 6.1.3 cache (`work/10B329__iPad2,1/dyld_shared_cache_armv7`, 569 images) unless cited otherwise; see [research/ios6-platform.md §5](../research/ios6-platform.md) and [research/apple-armv7-abi.md](../research/apple-armv7-abi.md).

## 1. Measured dependency graph of the 569 cache images

Reproducible recipe (run from the repo root; throwaway, ~30 lines, Python 3 stdlib only). It walks the
88-byte dyld-212 header, the 3 mappings, the 569 `dyld_cache_image_info` entries, each image's
`mach_header` + load commands, takes the install name from `LC_ID_DYLIB` and the edges from
`LC_LOAD_DYLIB` (0xC), `LC_LOAD_WEAK_DYLIB` (0x80000018), `LC_REEXPORT_DYLIB` (0x8000001F) and
`LC_LOAD_UPWARD_DYLIB` (0x80000023):

```python
import struct, mmap
P = "work/10B329__iPad2,1/dyld_shared_cache_armv7"
F = open(P, "rb"); D = mmap.mmap(F.fileno(), 0, access=mmap.ACCESS_READ)
mo, mc, io, ic = struct.unpack_from("<4I", D, 0x10)            # mappingOffset/Count, imagesOffset/Count
maps = [struct.unpack_from("<QQQII", D, mo + 32 * i) for i in range(mc)]
def off(a): return next(fo + a - ad for ad, sz, fo, _, _ in maps if ad <= a < ad + sz)
def u32(a): return struct.unpack_from("<I", D, off(a))[0]
def cstr(a): o = off(a); return D[o:D.find(b"\0", o)].decode()
EDGE = (0xC, 0x80000018, 0x8000001F, 0x80000023)
img = {}                                                       # install name -> (addr, __TEXT vmsize, deps)
for i in range(ic):
    a = struct.unpack_from("<Q", D, io + 32 * i)[0]
    p, ident, deps, text = a + 28, None, [], 0                 # 32-bit mach_header is 28 bytes
    for _ in range(u32(a + 16)):                               # ncmds
        cmd, cs = u32(p), u32(p + 4)
        if cmd == 0xD: ident = cstr(p + u32(p + 8))            # LC_ID_DYLIB
        elif cmd in EDGE: deps.append(cstr(p + u32(p + 8)))
        elif cmd == 1 and D[off(p + 8):off(p + 8) + 8] == b"__TEXT\0\0": text = u32(p + 28)
        p += cs
    img[ident or "?%x" % a] = (a, text, deps)
def closure(seeds):
    seen, st = set(), list(seeds)
    while st:
        n = st.pop()
        if n not in seen: seen.add(n); st += img[n][2]
    return seen
c = closure(["/System/Library/Frameworks/Foundation.framework/Foundation"])
print(len(c), sum(img[n][1] for n in c))                       # 41 10584064
```

Measured: running it prints `41 10584064`; seeding with the ten UIKit-app frameworks below prints
`106 53551104`. Edge census over all 569 images (same walk): **4560** `LC_LOAD_DYLIB`, **279**
`LC_REEXPORT_DYLIB`, **23** `LC_LOAD_WEAK_DYLIB` and **7** `LC_LOAD_UPWARD_DYLIB` (all to
`libSystem.B.dylib`/`libobjc.A.dylib`); the first three match
[research/ios6-platform.md §5.4](../research/ios6-platform.md). The closure depends on which edge kinds
count:

| edge kinds followed | Foundation closure (images / `__TEXT` B) | UIKit-app closure |
|---|---:|---:|
| `LC_LOAD_DYLIB` only | 18 / 9,158,656 | 76 / 45,936,640 |
| + weak | 19 / 9,211,904 | 77 / 45,989,888 |
| + weak + `LC_REEXPORT_DYLIB` | 41 / 10,584,064 | 106 / 53,551,104 |
| + upward (all four) | 41 / 10,584,064 | 106 / 53,551,104 |

Re-exports must be followed (`libSystem.B.dylib` is a pure umbrella: it re-exports the `libsystem_*` set,
so every app "links" them only through `LC_REEXPORT_DYLIB`); the headline numbers use all four kinds.
Sizes are the sum of `__TEXT` vmsize — code only, since the shared region is identity-mapped and shared
pages must not be double-counted for memory budgeting.

**Alias entries.** 50 of the 569 image entries are alias paths at the *same address* as a canonical image
(e.g. `/usr/lib/libobjc.dylib` and `/usr/lib/libpoll.dylib` alias `libobjc.A.dylib` and
`libSystem.B.dylib`; measured: `len(images) - len({addr}) = 50`). The earlier draft of the tables below
listed the *last alias* of each address (so `libSystem.B.dylib`, the umbrella that every other row depends
on, was missing and `libpoll.dylib` appeared in its place). The tables now key on `LC_ID_DYLIB`
install names, one row per address.

| Closure | Seed frameworks | Images | Code bytes |
|---|---|---:|---:|
| (a) Foundation-only CLI app | `Foundation.framework` | 41 | 10,584,064 |
| (b) UIKit app | `UIKit`, `QuartzCore`, `CoreGraphics`, `CoreText`, `ImageIO`, `OpenGLES`, `AudioToolbox`, `CFNetwork`, `Security`, `SystemConfiguration` (+ their deps) | 106 | 53,551,104 |

### 1.1 Closure (a) — Foundation-only CLI app

Decision key: `builtin` = our native substitute (ADR-09); `guest` = translated Apple code. Bring-up
stage means M4 (objc + CF minimum); anything not yet implemented is exercised via `stub` mode
(ADR-09, ADR-13 loud logging).

| Image | __TEXT bytes | Decision (bring-up) | Why |
|---|---:|---|---|
| `/System/Library/Frameworks/CFNetwork.framework/CFNetwork` | 811008 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation` | 1257472 | guest | ABI-visible |
| `/System/Library/Frameworks/Foundation.framework/Foundation` | 1851392 | guest | ABI-visible |
| `/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit` | 303104 | builtin | hardware boundary |
| `/System/Library/Frameworks/Security.framework/Security` | 192512 | guest | default (guest else stub) |
| `/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration` | 262144 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/GenerationalStorage.framework/GenerationalStorage` | 53248 | guest | default (guest else stub) |
| `/usr/lib/libCRFSuite.dylib` | 94208 | guest | default (guest else stub) |
| `/usr/lib/libSystem.B.dylib` | 4096 | guest | umbrella; re-exports `libsystem_*` (ADR-09) |
| `/usr/lib/libbsm.0.dylib` | 53248 | guest | default (guest else stub) |
| `/usr/lib/libc++abi.dylib` | 81920 | guest | C++ ABI (SjLj) |
| `/usr/lib/libicucore.A.dylib` | 1351680 | guest | default (guest else stub) |
| `/usr/lib/liblangid.dylib` | 4096 | guest | default (guest else stub) |
| `/usr/lib/libobjc.A.dylib` | 1044480 | guest | objc_msgSend ABI (ADR-09) |
| `/usr/lib/libsqlite3.dylib` | 548864 | guest | default (guest else stub) |
| `/usr/lib/libstdc++.6.dylib` | 311296 | guest | C++ ABI (SjLj) |
| `/usr/lib/libxml2.2.dylib` | 712704 | guest | default (guest else stub) |
| `/usr/lib/libz.1.dylib` | 53248 | guest | default (guest else stub) |
| `/usr/lib/system/libcache.dylib` | 16384 | guest | default (guest else stub) |
| `/usr/lib/system/libcommonCrypto.dylib` | 28672 | guest | default (guest else stub) |
| `/usr/lib/system/libcompiler_rt.dylib` | 12288 | guest | compiler helpers |
| `/usr/lib/system/libcopyfile.dylib` | 24576 | guest | default (guest else stub) |
| `/usr/lib/system/libcorecrypto.dylib` | 221184 | guest | default (guest else stub) |
| `/usr/lib/system/libdispatch.dylib` | 122880 | guest | kernel deps ch.07 (§4) |
| `/usr/lib/system/libdnsinfo.dylib` | 8192 | guest | default (guest else stub) |
| `/usr/lib/system/libdyld.dylib` | 8192 | builtin | we are the loader |
| `/usr/lib/system/libkeymgr.dylib` | 4096 | guest | default (guest else stub) |
| `/usr/lib/system/liblaunch.dylib` | 24576 | guest | default (guest else stub) |
| `/usr/lib/system/libmacho.dylib` | 16384 | guest | default (guest else stub) |
| `/usr/lib/system/libremovefile.dylib` | 8192 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_blocks.dylib` | 4096 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_c.dylib` | 552960 | guest | libc, malloc, pthread, setjmp, OSSpinLock (ADR-09); hot leaf routines and `OSAtomicEnqueue/Dequeue` may get host hooks ([06 §6](06-runtime-core.md)) |
| `/usr/lib/system/libsystem_dnssd.dylib` | 28672 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_info.dylib` | 102400 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_kernel.dylib` | 94208 | guest | user half of the trap ABI: `svc #0x80` stubs, MIG stubs, `cerror`; our kernel shim sits under the trap (ch.07) |
| `/usr/lib/system/libsystem_m.dylib` | 118784 | guest | libm; leaf hooks possible |
| `/usr/lib/system/libsystem_network.dylib` | 61440 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_notify.dylib` | 32768 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_sandbox.dylib` | 8192 | guest | default (guest else stub) |
| `/usr/lib/system/libunwind.dylib` | 4096 | guest | SjLj unwinder |
| `/usr/lib/system/libxpc.dylib` | 90112 | guest | default (guest else stub) |

**Count: 41 images; code bytes: 10584064**

### 1.2 Closure (b) — UIKit app (bring-up target for M5)

| Image | __TEXT bytes | Decision (bring-up) | Why |
|---|---:|---|---|
| `/System/Library/Frameworks/Accelerate.framework/Accelerate` | 4096 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Accelerate.framework/Frameworks/vImage.framework/vImage` | 1306624 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/libBLAS.dylib` | 933888 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/libLAPACK.dylib` | 2842624 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/libvDSP.dylib` | 368640 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/libvMisc.dylib` | 73728 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/vecLib` | 4096 | guest | default (guest else stub) |
| `/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox` | 2658304 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CFNetwork.framework/CFNetwork` | 811008 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreAudio.framework/CoreAudio` | 356352 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation` | 1257472 | guest | ABI-visible |
| `/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics` | 1282048 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreImage.framework/CoreImage` | 745472 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreMedia.framework/CoreMedia` | 417792 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony` | 356352 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreText.framework/CoreText` | 405504 | guest | default (guest else stub) |
| `/System/Library/Frameworks/CoreVideo.framework/CoreVideo` | 65536 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Foundation.framework/Foundation` | 1851392 | guest | ABI-visible |
| `/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit` | 303104 | builtin | hardware boundary |
| `/System/Library/Frameworks/ImageIO.framework/ImageIO` | 1937408 | guest | default (guest else stub) |
| `/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices` | 368640 | guest | default (guest else stub) |
| `/System/Library/Frameworks/OpenGLES.framework/OpenGLES` | 32768 | builtin | hardware/driver boundary (ADR-11) |
| `/System/Library/Frameworks/OpenGLES.framework/libCVMSPluginSupport.dylib` | 4096 | builtin | hardware/driver boundary (ADR-11) |
| `/System/Library/Frameworks/OpenGLES.framework/libCoreVMClient.dylib` | 12288 | builtin | hardware/driver boundary (ADR-11) |
| `/System/Library/Frameworks/OpenGLES.framework/libGFXShared.dylib` | 20480 | builtin | hardware/driver boundary (ADR-11) |
| `/System/Library/Frameworks/OpenGLES.framework/libGLImage.dylib` | 253952 | builtin | hardware/driver boundary (ADR-11) |
| `/System/Library/Frameworks/QuartzCore.framework/QuartzCore` | 1138688 | guest | default (guest else stub) |
| `/System/Library/Frameworks/Security.framework/Security` | 192512 | guest | default (guest else stub) |
| `/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration` | 262144 | guest | default (guest else stub) |
| `/System/Library/Frameworks/UIKit.framework/UIKit` | 5591040 | builtin | minimal shim, reassess (ADR-09) |
| `/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox` | 258048 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/AggregateDictionary.framework/AggregateDictionary` | 16384 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/AppSupport.framework/AppSupport` | 245760 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/AssetsLibraryServices.framework/AssetsLibraryServices` | 40960 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices` | 98304 | guest | default (guest else stub); talks to the daemon-substitute ports of §5.3 |
| `/System/Library/PrivateFrameworks/Bom.framework/Bom` | 151552 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/CommonUtilities.framework/CommonUtilities` | 24576 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/CoreSurface.framework/CoreSurface` | 8192 | builtin | surface boundary |
| `/System/Library/PrivateFrameworks/CrashReporterSupport.framework/CrashReporterSupport` | 24576 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/DictionaryServices.framework/DictionaryServices` | 106496 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/FaceCoreLight.framework/FaceCoreLight` | 4276224 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/GenerationalStorage.framework/GenerationalStorage` | 53248 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/GraphicsServices.framework/GraphicsServices` | 49152 | guest | default (guest else stub); `GSEvent`/`PurpleFBServer` ports, §5.3 |
| `/System/Library/PrivateFrameworks/IMFoundation.framework/IMFoundation` | 315392 | guest | ABI-visible |
| `/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator` | 8192 | guest | default (guest else stub); a driver-level `IOConnectCall*` from it must fail loudly (ADR-11) |
| `/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer` | 24576 | guest | default (guest else stub); same ADR-11 rule |
| `/System/Library/PrivateFrameworks/IOSurface.framework/IOSurface` | 20480 | builtin | surface boundary |
| `/System/Library/PrivateFrameworks/JavaScriptCore.framework/JavaScriptCore` | 1732608 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/MobileAsset.framework/MobileAsset` | 188416 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation` | 16384 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag` | 28672 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/MobileSystemServices.framework/MobileSystemServices` | 16384 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/OpenCL.framework/OpenCL` | 155648 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/PrintKit.framework/PrintKit` | 167936 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/ProofReader.framework/ProofReader` | 479232 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices` | 73728 | guest | default (guest else stub); `com.apple.springboard.services`, §5.3 |
| `/System/Library/PrivateFrameworks/TCC.framework/TCC` | 12288 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/TelephonyUtilities.framework/TelephonyUtilities` | 57344 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/UIFoundation.framework/UIFoundation` | 659456 | guest | ABI-visible |
| `/System/Library/PrivateFrameworks/WebBookmarks.framework/WebBookmarks` | 131072 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/WebCore.framework/WebCore` | 9633792 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/WebKit.framework/WebKit` | 909312 | guest | default (guest else stub) |
| `/System/Library/PrivateFrameworks/XPCObjects.framework/XPCObjects` | 32768 | guest | default (guest else stub) |
| `/usr/lib/libAccessibility.dylib` | 28672 | guest | default (guest else stub) |
| `/usr/lib/libCRFSuite.dylib` | 94208 | guest | default (guest else stub) |
| `/usr/lib/libMobileGestalt.dylib` | 53248 | guest | default (guest else stub) |
| `/usr/lib/libSystem.B.dylib` | 4096 | guest | umbrella; re-exports `libsystem_*` (ADR-09) |
| `/usr/lib/libbsm.0.dylib` | 53248 | guest | default (guest else stub) |
| `/usr/lib/libbz2.1.0.dylib` | 40960 | guest | default (guest else stub) |
| `/usr/lib/libc++.1.dylib` | 307200 | guest | C++ ABI (SjLj) |
| `/usr/lib/libc++abi.dylib` | 81920 | guest | C++ ABI (SjLj) |
| `/usr/lib/libiconv.2.dylib` | 974848 | guest | default (guest else stub) |
| `/usr/lib/libicucore.A.dylib` | 1351680 | guest | default (guest else stub) |
| `/usr/lib/liblangid.dylib` | 4096 | guest | default (guest else stub) |
| `/usr/lib/liblockdown.dylib` | 32768 | guest | default (guest else stub) |
| `/usr/lib/libobjc.A.dylib` | 1044480 | guest | objc_msgSend ABI (ADR-09) |
| `/usr/lib/libresolv.9.dylib` | 90112 | guest | default (guest else stub) |
| `/usr/lib/libsqlite3.dylib` | 548864 | guest | default (guest else stub) |
| `/usr/lib/libstdc++.6.dylib` | 311296 | guest | C++ ABI (SjLj) |
| `/usr/lib/libtidy.A.dylib` | 159744 | guest | default (guest else stub) |
| `/usr/lib/libxml2.2.dylib` | 712704 | guest | default (guest else stub) |
| `/usr/lib/libxslt.1.dylib` | 135168 | guest | default (guest else stub) |
| `/usr/lib/libz.1.dylib` | 53248 | guest | default (guest else stub) |
| `/usr/lib/system/libcache.dylib` | 16384 | guest | default (guest else stub) |
| `/usr/lib/system/libcommonCrypto.dylib` | 28672 | guest | default (guest else stub) |
| `/usr/lib/system/libcompiler_rt.dylib` | 12288 | guest | compiler helpers |
| `/usr/lib/system/libcopyfile.dylib` | 24576 | guest | default (guest else stub) |
| `/usr/lib/system/libcorecrypto.dylib` | 221184 | guest | default (guest else stub) |
| `/usr/lib/system/libdispatch.dylib` | 122880 | guest | kernel deps ch.07 (§4) |
| `/usr/lib/system/libdnsinfo.dylib` | 8192 | guest | default (guest else stub) |
| `/usr/lib/system/libdyld.dylib` | 8192 | builtin | we are the loader |
| `/usr/lib/system/libkeymgr.dylib` | 4096 | guest | default (guest else stub) |
| `/usr/lib/system/liblaunch.dylib` | 24576 | guest | default (guest else stub) |
| `/usr/lib/system/libmacho.dylib` | 16384 | guest | default (guest else stub) |
| `/usr/lib/system/libremovefile.dylib` | 8192 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_blocks.dylib` | 4096 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_c.dylib` | 552960 | guest | libc, malloc, pthread, setjmp, OSSpinLock (ADR-09); hot leaf routines and `OSAtomicEnqueue/Dequeue` may get host hooks ([06 §6](06-runtime-core.md)) |
| `/usr/lib/system/libsystem_dnssd.dylib` | 28672 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_info.dylib` | 102400 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_kernel.dylib` | 94208 | guest | user half of the trap ABI: `svc #0x80` stubs, MIG stubs, `cerror`; our kernel shim sits under the trap (ch.07) |
| `/usr/lib/system/libsystem_m.dylib` | 118784 | guest | libm; leaf hooks possible |
| `/usr/lib/system/libsystem_network.dylib` | 61440 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_notify.dylib` | 32768 | guest | default (guest else stub) |
| `/usr/lib/system/libsystem_sandbox.dylib` | 8192 | guest | default (guest else stub) |
| `/usr/lib/system/libunwind.dylib` | 4096 | guest | SjLj unwinder |
| `/usr/lib/system/libxpc.dylib` | 90112 | guest | default (guest else stub) |

**Count: 106 images; code bytes: 53551104**

### 1.3 Bring-up policy

- M4 ships `guest` for `libobjc`, `CoreFoundation`, `Foundation`, `libdispatch` **and for the whole
  `libsystem_*` set and `libSystem.B`** (ADR-09: only `libdyld` and the hardware boundary are builtin; the
  first draft of this bullet said `builtin` for `libsystem_*`, contradicting ADR-09 and the tables above).
  Closure (a) is the M4 exit fixture set.
- M5 adds the `builtin` UIKit shim + GLES + compositor + audio and runs closure (b) as `guest` where
  already covered, `stub` otherwise; the stub-hit counter is the M5 burn-down list (ADR-09).
- `WebCore`/`WebKit` (9.6 MB + 0.9 MB code) are deliberately **not** in the M5 target; see §7.

## 2. `objc_msgSend` hot path

### 2.1 objc4-532 data structures in the real cache (measured)

Shipped `libobjc.A.dylib` = objc4-532.2, version 228.0.0 ([research/apple-armv7-abi.md §7](../research/apple-armv7-abi.md)).
Everything below was read out of the cache: the static `_OBJC_CLASS_$_NSObject`/`_OBJC_METACLASS_$_NSObject`
objects in libobjc's `__DATA` (exports at 0x3af71b48/0x3af71b5c) and the disassembly of libobjc's runtime
accessors. The abi test (`tests/abi/`) re-derives them from the ARMv7 layout (ADR-03).

| structure | layout (ILP32) | evidence |
|---|---|---|
| `objc_class` | `isa @0, superclass @4, cache @8, vtable @12, data @16` = 20 bytes | NSObject metaclass `class_ro_t.instanceSize == 20`; words at 0x3af71b48 = `{meta, 0, 0x3af76b48 (empty cache), 0x3af71c2c (empty vtable), 0x3af71a64}`; `_objc_msgSend`: `ldr r4,[r0]; ldr r3,[r4,#8]` (cls→cache) |
| `class_ro_t` (static `data`) | `flags, instanceStart, instanceSize, ivarLayout, name, baseMethods, baseProtocols, ivars, weakIvarLayout, baseProperties` = 10 words | NSObject ro @0x3af71a64: `{0x2 (RO_ROOT), 0, 4, 0, "NSObject", baseMethods, baseProtocols, ivars, 0, 0}`; metaclass flags `0x3` (RO_META\|RO_ROOT), start/size 20/20 |
| `data` field | pointer with **low 2 bits = flags**; `data & ~3` is `class_rw_t*` once realised, `class_ro_t*` in the static image | `_class_getInstanceSize`: `ldr r0,[r1,#0x10]; bic r0,r0,#3; ldr r0,[r0,#8]; ldr r0,[r0,#8]` |
| `class_rw_t` | `flags @0, version @4, ro @8, methods, properties, protocols, firstSubclass, nextSiblingClass` = 8 words | `ro` at +8 and `class_ro_t.instanceSize` at +8 proven by `_class_getInstanceSize` above; fields beyond `ro` from objc4-532 headers (`UNVERIFIED:` — close by disassembling `_class_getSuperclass`/`addSubclass`) |
| `method_list_t` | `entsize_and_flags @0, count @4, method_t[]` | NSObject baseMethods: `entsize_and_flags = 0xf` (`entsize = 0xf & ~3 = 12`, low bits `3`), `count = 35` |
| `method_t` | `name (SEL) @0, types @4, imp @8` = 12 bytes | `_method_getName`: `ldr r0,[r1]`; `_method_getImplementation`: `ldr r0,[r1,#8]`; NSObject's `isFault`/`methodSignatureForSelector:` rows read back with `types` strings `c8@0:4`, `@12@0:4:8` |
| `objc_cache` | `mask @0, occupied @4, buckets[] @8` where each bucket slot is a **pointer** to a `cache_entry { SEL sel @0; unsigned unused @4; IMP imp @8 }` (12 bytes) | `_objc_msgSend` (below): `ldr ip,[r3,#-8]` (mask), `ldr r4,[r3,sb,lsl #2]` (slot), `ldr ip,[r4]` (sel), `ldr ip,[r4,#8]` (imp) |
| empty cache | `_objc_empty_cache` = 4 zero words at 0x3af76b48 (`mask = 0`, `buckets[0] = NULL`) | read from the cache file |

**Correction.** The previous draft described `objc_cache` as `bucket_t { SEL sel; IMP imp; }` (8 bytes)
indexed by `sel & mask`. That is the layout of a *later* runtime (objc4-6xx). The shipped 532 code uses a
table of **pointers to 12-byte entries** and the index is `(sel >> 2) & mask`.

### 2.2 What libobjc's own `objc_msgSend` does (measured disassembly, 0x38fc25a0)

`llvm-mc`/Capstone disassembly of `_objc_msgSend` (Thumb-2, exported at 0x38fc25a1), with the `_stret` and
`_Super2` variants checked at 0x38fc2680 and 0x38fc27c0:

```
38fc25a0: teq.w r0, #0              ; nil receiver?
38fc25a4: beq   0x38fc25e6          ;   -> mov r1,#0 ; bx lr   (r0 stays 0, r1 cleared for 64-bit returns)
38fc25a6: push.w {r3, r4}
38fc25aa: ldr   r4, [r0]            ; cls = receiver->isa
38fc25ac: lsr.w sb, r1, #2          ; hash = sel >> 2
38fc25b0: ldr   r3, [r4, #8]        ; cache = cls->cache
38fc25b2: add.w r3, r3, #8          ; r3 = &cache->buckets[0]
38fc25b6: ldr   ip, [r3, #-8]       ; mask                        <-- probe loop head
38fc25ba: and.w sb, sb, ip          ; i = hash & mask
38fc25be: ldr.w r4, [r3, sb, lsl #2]; entry = buckets[i]  (pointer)
38fc25c2: teq.w r4, #0              ; empty slot -> miss
38fc25c6: add.w sb, sb, #1          ; ++i (linear probe, wraps via the mask at loop head)
38fc25ca: beq   0x38fc25e0
38fc25cc: ldr.w ip, [r4]            ; entry->sel
38fc25d0: teq.w r1, ip
38fc25d4: bne   0x38fc25b6
38fc25d6: ldr.w ip, [r4, #8]        ; hit: ip = entry->imp (Thumb bit in bit 0)
38fc25da: pop.w {r3, r4}
38fc25de: bx    ip                  ; tail-call the IMP, r0..r3 untouched
38fc25e0: pop.w {r3, r4}
38fc25e4: b     0x38fc2600          ; miss path
38fc2600: push.w {r0,r1,r2,r3,r7,lr}
38fc2604: add   r7, sp, #0x10
38fc2606: ldr   r2, [r0]            ; r2 = cls (isa)
38fc2608: ldr.w ip, [pc, #8]        ; ip = *(local pointer -> 0x38fc2f55)  (a rebased __DATA word)
38fc2618: blx   ip                  ; imp = _class_lookupMethodAndLoadCache3(obj=r0, sel=r1, cls=r2)
38fc261a: mov   ip, r0              ; the callee itself fills the cache
38fc261c: teq.w r4, r4              ; sets Z=1 (cache-hit path leaves Z=0, see below)
38fc2620: pop.w {r0,r1,r2,r3,r7,lr}
38fc2624: bx    ip
```

Findings that drive the design:

1. **There is no separate `_cache_getImp`-style lookup**: the probe is inlined in `objc_msgSend`. The
   `_stret` and `_Super2` variants repeat it with other register assignments: `_stret` tests `r1`, loads
   `isa` from `[r1]` and uses `r2` as the SEL; `_msgSendSuper2` takes `r0 = &objc_super`, uses
   `cls = objc_super.super_class->superclass` (`ldr r4,[r0,#4]; ldr r4,[r4,#4]`) and reloads `r0 = [r0]`
   (the receiver) just before the final `bx ip`.
2. **The miss path calls an in-image function through a pointer in `__DATA`** — a *local* function at
   0x38fc2f54 (`_class_lookupMethodAndLoadCache3`, not exported) taking `(obj = r0, sel = r1, cls = r2)`. It
   calls `lookUpMethod` at 0x38fc2f6c with `(cls, sel, initialize = 1, cache = 0, obj)` and returns the IMP
   in `r0` (libobjc fills the cache inside the lookup; `UNVERIFIED:` by disassembly of 0x38fc2f6c). Because
   the pointer is a rebased `__DATA` word, the loader's rebase makes it point at guest code and nothing
   special is required.
3. **The IMP's Thumb bit is consumed by `bx ip`**: the lifter lowers `bx` as an interworking indirect branch
   (bit 0 selects the mode, [04](04-translation.md)); libobjc masks nothing itself.
4. **`cpsr` is observable across the jump.** The miss path ends with `teq.w r4,r4` (Z = 1) before
   `bx ip`; the `_stret` hit path ends with `tst r4,r4` (Z = 0, `r4` is a non-null entry). The apparent
   consumer is `__objc_msgForward_impcache`, which would use Z to tell a fresh lookup from a cache hit
   (`UNVERIFIED:` the consumer was not disassembled). Either way `cpsr` must be exact at the `bx ip` into
   an IMP; ADR-06 spills `cpsr` at every boundary, so this holds by construction.
5. **Nil receiver:** `_objc_msgSend` leaves `r0` (nil) and sets `r1 = 0` (so `long long` and a `double` in
   `r0:r1` read as zero); `_objc_msgSend_stret` does `bxeq lr` **without writing the sret buffer**. The
   previous draft's "zero-fill the sret buffer" is wrong for this runtime.
6. No `objc_msgSend_fpret` is exported (measured), and soft-float returns `double` in `r0:r1`, so one path
   serves every non-stret return.

The ARC fast path is guest code too: `_objc_retain` (0x38fc4521) loads `isa`, reads byte 2 of
`class_rw_t.flags` through `(data & ~3) + 2` (`ldrb; tst #2` — bit 17, which the objc4-532 headers name
`RW_HAS_CUSTOM_RR`) and either branches to the in-image retain implementation (`b.w 0x38fd3110`) or sends
`retain` through `objc_msgSend` (`b.w 0x38fc25a0`). Nothing here needs interposing; the refcount storage
(`SideTable`) is internal to libobjc and is `UNVERIFIED:` — relevant only if a hook is ever added.

### 2.3 Keep libobjc's own `objc_msgSend` — decision

**Decision: keep libobjc's own `objc_msgSend` family and JIT it well; do not write ARM assembly
replacements.** (The first draft of §2.2 specified a full replacement family — nil check, `sel & mask`
probe over 8-byte buckets, `objc_retain` side-table fast paths — against a cache layout that does not
exist in 532; that design is deleted.) Reasoning:

- The shipped code is already a ~20-instruction hot path (0x38fc25a0..0x38fc25de) with the exact layout, nil handling and
  miss/`+initialize` machinery; replacing it can only introduce ABI drift (ADR-09 chose guest libobjc
  for fidelity).
- The IMP call is the only thing that crosses into our machinery: `bx ip` → indirect-branch lowering via
  the shadow region (ADR-04) → the callee block; no marshalling is involved.
- Interposing the whole function adds an FFI boundary on the hottest path in the process.

What the JIT *can* do for it without changing behaviour: inline-cache the indirect `bx ip` per call site
(M7, [05-jit.md](05-jit.md)); lift the whole of `_objc_msgSend` as one block (about 20 instructions
including the loop back-edge). Measurement plan (M4/M7): fixtures of §5 of [12-testing.md](12-testing.md) run under
tier 1 with `objc_msgSend` lifted normally; record cycles per message send for hit and miss paths
against the `qemu-arm` figure; revisit only if the three-number table of
[12-testing.md §7](12-testing.md) shows message sends dominating.

### 2.4 `+load` and selector fixup: what libobjc registers with dyld (measured)

libobjc does **not** use `_dyld_register_func_for_add_image` (the first draft said it did). Its dyld
imports, measured from the `__picsymbolstub4` table of `libobjc.A.dylib`, are
`_dyld_register_image_state_change_handler`, `__dyld_register_func_for_remove_image`,
`__dyld_get_image_header`, `__dyld_image_count`, `_dyld_image_path_containing_address`. So the loader
([03-loader.md](03-loader.md)) must implement `dyld_register_image_state_change_handler(state, batch,
handler)` with the dyld-212 semantics. Per objc4-532 `_objc_init` (source knowledge; the `state` arguments
were not read from the disassembly, `UNVERIFIED:`) libobjc registers one handler for the *bound* state
(`map_images`: class/selector registration, category attach) and one for *dependents-initialized*
(`load_images`: `+load`), and the loader must invoke them with the image info arrays for every image, including the images
already mapped at registration time. The `+initialize` is lazy (first message send via the miss path).
Also present: `__TEXT,__objc_opt_ro` (946,176 bytes) in libobjc and `__DATA,__objc_opt_rw` (16,384 bytes) —
the shared-cache optimisation tables (pre-uniqued selectors/classes). Whether `map_images` trusts them when
the image is *not* flagged as optimised is `UNVERIFIED:`; the experiment is running closure (a) with
`__objc_opt_ro` zero-filled and checking that selector fixup still completes.

## 3. CoreFoundation / Foundation

### 3.1 Measured dependency edges and imports

Direct edges (load commands, same walk as §1): `CoreFoundation` → `libicucore.A`, `libobjc.A`, `libz.1`,
`libSystem.B` (4 `LC_LOAD_DYLIB`). `Foundation` → `libicucore.A`, `libxml2.2`, `libz.1`, `CFNetwork`,
`SystemConfiguration`, `Security`, `IOKit`, `libCRFSuite`, `liblangid`, `libSystem.B` (10 `LC_LOAD_DYLIB`), plus
`LC_REEXPORT_DYLIB` of `libobjc.A` and `CoreFoundation` and an `LC_LOAD_WEAK_DYLIB` of `GenerationalStorage`.
So `CoreFoundation` has no direct dependency on `libxml2` (the earlier draft's list mixed the two).
All kernel access of both goes through `libSystem.B`'s re-exports; the `__picsymbolstub4` table of
`CoreFoundation` imports (measured): `_mach_msg`, `_mach_port_{allocate,deallocate,destroy,extract_member,
insert_member,insert_right,mod_refs,get_attributes,set_attributes,get_set_status,type}`,
`_mach_absolute_time`, `_mach_timebase_info`, `_mach_vm_allocate/deallocate/map`,
`_mach_make_memory_entry_64`, `_mk_timer_{create,arm,cancel,destroy}`, `_sysctl`, `_sysctlbyname`,
`_getenv`, `_notify_{register_dispatch,post,cancel}`, `_bootstrap_{check_in,look_up2,register2}`,
`_xpc_connection_create_mach_service`, `_pthread_*` (incl. `_pthread_atfork`, `_pthread_getspecific`,
`_pthread_key_init_np`) and the `_dispatch_{semaphore,queue,source,group,once}*` family.

### 3.2 What they need from below

- **Mach.** `mach_msg` for run-loop ports (port sets: `mach_port_insert_member`/`extract_member`),
  `mach_absolute_time` (r12 = -3 fast path, [research/ios6-platform.md §8](../research/ios6-platform.md)) and
  `mach_timebase_info`, `mach_vm_*` for zones. Measured trap-vs-MIG split in `libsystem_kernel`:
  `mach_port_allocate` is the direct trap `_kernelrpc_mach_port_allocate_trap` (`mvn ip,#0xf` = -16),
  `mach_msg_trap` is -31, `mach_wait_until` -90, `mk_timer_*` -91..-94, `semaphore_wait_trap` -36,
  whereas `vm_map`/`mach_make_memory_entry_64` are MIG stubs that build the request on the stack and go
  through `mach_msg` (chapter 07 must dispatch those messages; the message ids are `UNVERIFIED:`).
- **`sysctl`.** CF imports both `sysctl` and `sysctlbyname`; the cstring tables of CF/Foundation contain no
  `hw.ncpu`/`hw.pagesize`/`kern.ostype` text (measured: absent), so those queries arrive as *numeric* MIBs
  (`CTL_HW`/`CTL_KERN` + id) through `sysctl`, with only `hw.machine` (present in CF) and `hw.memsize`
  (present in Foundation) as by-name strings. Chapter 07's `sysctl` must therefore implement the numeric
  MIB space, not just names. Which ids are used is `UNVERIFIED:` (close by tracing `___sysctl` in the M4
  fixture with `IOS6RECOMP_SYSCALL_TRACE`).
- **Environment.** CF-side strings measured in `CoreFoundation`'s `__cstring`: `CFFIXED_USER_HOME`,
  `CFLOG_FORCE_STDERR`, `CFNETWORK_LIBRARY_PATH`, `CFPREFERENCES_LOG_EQUAL_VALUES`, `CF_CHARSET_PATH`,
  `__CFPREFERENCES_AUTOSYNC_INTERVAL`, `__CFPREFERENCES_AVOID_DAEMON`, `__CFPREFERENCES_LOG_FAILURES`,
  `__CFPREFERENCES_USE_OLD_UID_BEHAVIOR`, `__CF_DEBUG_EXPANDED_SET`, `__CF_USER_TEXT_ENCODING`, and
  `NSZombieEnabled` (also in Foundation). `CFNETWORK_DIAGNOSTICS` and `OBJC_DEBUG_*` are *not* in CF or
  Foundation (the latter belongs to libobjc). `TZ`/`LANG` are read through libc/ICU, not from CF strings.
  The shim passes host variables through; the only ones with required values are `CFFIXED_USER_HOME` (set to
  the fake container, §6) and `__CFPREFERENCES_AVOID_DAEMON=1` (see next bullet).
- **Daemons CF talks to (ADR-08 facades).** CF imports `bootstrap_look_up2`/`check_in`/`register2`,
  `xpc_connection_create_mach_service` and `notify_*`; measured notification keys in CF:
  `com.apple.system.timezone`, `com.apple.system.clock_set`, `com.apple.system.memorystatus`
  (`libsystem_notify` itself talks to `com.apple.system.notification_center`). With
  `__CFPREFERENCES_AVOID_DAEMON` set the preferences code avoids the `cfprefsd` XPC service (the service
  name itself is not in `__cstring`; `UNVERIFIED:` whether it is in `__cfstring`) — the M4 default is to set it.
- **`CFRunLoop` timers use `mk_timer` — measured.** `CoreFoundation` imports all four `mk_timer_*` stubs;
  direct call sites found by scanning every function range for `bl`/`blx`/`b` to the stub: `mk_timer_create`
  from one function (0x3113654c), `mk_timer_cancel` from `_CFRunLoopRemoveTimer` (0x310aada8),
  `mk_timer_destroy` from one function (0x311386fc); the `mk_timer_arm` stub is imported but no direct
  call site was found by that scan (calls through a lazy pointer from an undecoded range are possible;
  `UNVERIFIED:` — close by running the M4 `CFRunLoop` timer fixture with the syscall trace on and
  checking for trap -93). Traps in `libsystem_kernel`: `mk_timer_create` = -91, `destroy` = -92,
  `arm` = -93 (`port`, 64-bit deadline in `r1:r2`), `cancel` = -94 (`port`, `uint64*`) — matching
  `osfmk/kern/syscall_sw.c` (`mk_timer_create_trap` … `mk_timer_cancel_trap`). **Decision:** chapter 07
  implements the four traps natively: `mk_timer_create` returns a Mach receive right whose expiry sends a
  `mk_timer_expire_msg`-shaped message into the in-process port registry (ADR-08), backed by one host
  `timerfd`/timer wheel; `CFRunLoop` then needs no special case. (The first draft's fallback — a bare
  `timerfd` outside the Mach port space — is rejected: `CFRunLoop` waits for the timer through the same
  `mach_msg` port set as its sources.)
- Locale/ICU data: `libicucore` is a direct dependency; its data comes from the user's rootfs (CF has no
  `/usr/share/icu` string, so the ICU data path is `UNVERIFIED:` — close by reading `libicucore`'s
  `__cstring` for `icudt`). CoreFoundation resource bundles and `/System/Library/Frameworks/*/Resources`
  likewise come from the **user's rootfs**, never committed (README input contract). The loader exposes
  them via the filesystem namespace in 07-kernel-boundary.md.

## 4. libdispatch (guest)

### 4.1 Kernel dependencies

- `kqueue`/`kevent` (used by dispatch's manager thread) → chapter 07 maps to epoll.
- `__workq_kernreturn` / `workq_open` (workqueue API in iOS 6 `libsystem_c` pthread) → 07 implements.
- `bsdthread_register`, `bsdthread_create` → 07.
- `mach_msg` for the main queue's run-loop integration.
- `mach_absolute_time` for timers.

### 4.2 `pthread_workqueue` design

libdispatch on iOS 6 uses a kernel workqueue (`workq_open` returns a port; `__workq_kernreturn`
requests threads). Our host thread pool contract:

- A pool of host pthreads (size = `min(nproc*2, 64)`) waits on a futex/condvar queue of dispatch
  blocks.
- `workq_open` returns a synthetic mach port; `__workq_kernreturn(op, ...)` with `WORKQ_REQTHREADS`
  enqueues a request that the pool services by calling the guest worker entry (`_dispatch_worker_thread2`
  equivalent) via `runtime::call_guest`.
- Thread-local state (`pthread_getspecific` keys, `errno`) is per host thread but stored in guest
  thread contexts; each pool thread gets its own GTC.
- Priority classes map to host `SCHED_OTHER` (no real-time); QoS is best-effort.

This is the known hard part; if libdispatch's manager thread deadlocks, M4's CFRunLoop timer test
fails first.

## 5. UIKit strategy

### 5.1 Measured imports

`UIKit.framework` links 37 dylibs (+1 re-export) ([research/ios6-platform.md §5.4](../research/ios6-platform.md));
its closure (§1.2) pulls 106 images including `WebCore` (9.6 MB), `JavaScriptCore` (1.7 MB),
`FaceCoreLight` (4.3 MB). A typical UIKit app's symbol usage is dominated by `UIView`, `UIWindow`,
`UIApplication`, `CALayer`, `EAGLContext`, `UIEvent`, `UIGestureRecognizer`.

### 5.2 Real UIKit as guest vs native shim

Running real UIKit as guest needs: `CARenderServer` (CoreAnimation's render server), `backboardd`/
`BKSHID`/`FrontBoardServices`, `IOSurface`, `GSEvent`, `PurpleSystemEventPort`. These are daemons we
substitute natively (ADR-08). Feasible but large (M9 territory).

**Decision (ADR-09 refined):** Stage 1 = native minimal shim (`UIApplication`, `UIWindow`, `UIView`,
`CALayer` stub, `EAGLView`, `UIEvent`/touch, `NSTimer`/`CADisplayLink`). Stage 2 (M6+) reassess:
measure stub-hit rate and missing-opcode count after running 5 apps; if a single app needs > 50 UIKit
symbols we don't shim, switch that app's UIKit to guest mode via `IOS6RECOMP_IMAGE_OVERRIDE`.

### 5.3 Mach services UIKit contacts at launch

Strings `com.apple.*` found in UIKit/GraphicsServices/BackBoardServices/QuartzCore/SpringBoardServices
in the cache (grep the `__TEXT,__cstring` of those images). Known set (verify by `strings` on the
extracted images):

- `com.apple.backboard.hid.services` (BKSHID)
- `com.apple.SpringBoard` (legacy, for `GSEvent`)
- `com.apple.coreanimation.render-server` (CARenderServer)
- `com.apple.iosurface` (IOSurface)
- `com.apple.system.notification_center`
- `com.apple.mobile.installd` (only at install time)

Our native facades register under these names in the port registry (ADR-08).

### 5.4 Staged plan with objective gates

| Stage | Gate | Milestone |
|---|---|---|
| S0 | UIKit shim compiles, exports the 40 most-used symbols, app links | M5 |
| S1 | One real app launches, renders, accepts touch, 60 s @ ≥15 fps | M5 exit |
| S2 | 5 apps of different genres run to usable state, zero unknown-opcode hits | M6 |
| S3 | If stub-hit rate for UIKit > 5 % of calls on S2 apps, switch UIKit to guest for those apps and measure again | M6/M9 |

## 6. Bundles / resources / app launch

- `.app` layout: `Payload/<Name>.app/` with `Info.plist`, executable (`CFBundleExecutable`),
  resources, `Frameworks/` (none in iOS 6 apps — they link system frameworks only).
- `Info.plist` is a binary plist; parse with a minimal bplist reader (vendor ~200 lines, M0).
- `NSBundle` paths: resource lookups resolve against the app bundle root and the sandbox container
  `/var/mobile/Applications/<UUID>/`. We fake the container under `~/.ios6recomp/containers/<uuid>/`.
- Entitlements are ignored (ADR-12: we are not a sandbox enforcer; log them).
- App launch contract: `UIApplicationMain(argc, argv, principalClassName, delegateClassName)` →
  creates `UIApplication`, loads the main nib/storyboard (iOS 6 uses nibs, not storyboards for most),
  calls `application:didFinishLaunchingWithOptions:` on the delegate, then runs the main run loop
  (`NSRunLoop`/`CFRunLoop` owned by UIKit, driven by our compositor's frame clock).
- `ios6recomp run` fakes the SpringBoard side: registers a fake `UIApplication` with the compositor,
  sends a synthetic `UIApplicationDidFinishLaunchingNotification`, and drives `CFRunLoopRun` from the
  frame clock (M5).

## 7. Third-party code patterns that bite

- **Crash reporters** (PLCrashReporter, HockeyApp): install signal handlers and read `/proc`-like paths;
  our signal delivery (06) must deliver `SIGSEGV`/`SIGBUS` with a valid `ucontext`; PLCrashReporter's
  in-process unwinder uses `r7` frame pointers (works). Stub its server upload.
- **`dlopen`ed code**: our loader supports `dlopen`/`dlsym`/`dlclose` on guest images; invalidation on
  `dlclose` (05 §7).
- **JavaScriptCore JIT**: supported via the write-fault dance (05 §7); JSC's ARM JIT emits Thumb-2.
- **UIWebView/WebKit**: WebCore is 9.6 MB of guest code plus JSC JIT; runnable but heavy. Position:
  treat as M6 stretch, not M5. Its JIT pages use the write-fault path.
- **Unity / cocos2d / GLES1 apps**: GLES1 fixed-function needs emulation (touchHLE-style, 09 §1);
  Unity's IL2CPP not present on iOS 6 (Mono runtime instead) — Mono's JIT is another guest-JIT case.
- **Anti-debug / jailbreak checks**: return plausible values (`sysctl` hw, file existence); never
  crash. Logged in strict mode.
