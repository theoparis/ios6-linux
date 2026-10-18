# 03 — Loader, Mach-O, and the dyld shared cache

Scope: `lib/macho`, `lib/loader`, `lib/guestmem`. Non-goals: translation (`04-translation.md`),
JIT (`05-jit.md`), kernel boundary (`07-kernel-boundary.md`). Depends on `ADR-01`..`ADR-13`,
`01-architecture.md`, `research/ios6-platform.md`, `research/apple-armv7-abi.md`,
`research/llvm-tooling.md`.

## 1. Component responsibilities

| Component | Owns | Does NOT own |
|---|---|---|
| `lib/macho` (`ios6/macho/image.hpp`, `ios6/macho/cache.hpp`) | 32-bit Mach-O header + load commands; fat header; `LC_DYLD_INFO_ONLY` opcode streams; export trie; indirect symbol table; `LC_FUNCTION_STARTS`/`LC_DATA_IN_CODE` ingestion; dyld-cache header/mappings/images/slide-info/local-symbols; `.tbd` emission | address-space policy, substitution policy |
| `lib/loader` (`ios6/loader/loader.hpp`) | per-install-name builtin/guest/stub decision (`IOS6RECOMP_IMAGE_OVERRIDE`), segment mapping into the arena, rebase/bind/weak-bind/lazy-bind application, 2-level symbol resolution, ObjC/dyld metadata, initializer ordering, refusal of encrypted/unsupported images | decoding, JIT |
| `lib/guestmem` (`ios6/guestmem/arena.hpp`) | low-4-GiB reserve (`MAP_FIXED_NOREPLACE`), `guest_addr_t ⇄ void*`, per-guest-VA `mmap`/`mprotect`, guest heap used by builtins | Mach-O parsing |

### 1.1 Load commands acted on vs. rejected

Acted on (all others are parsed and ignored, never fatal):

| cmd | handling |
|---|---|
| `LC_SEGMENT` (0x01) | map each segment at `vmaddr` with `maxprot`/`initprot`; `__PAGEZERO` is left unmapped |
| `LC_SYMTAB` (0x02), `LC_DYSYMTAB` (0x0b) | symbol/import resolution, indirect symbol table |
| `LC_LOAD_DYLIB` (0x0c), `LC_ID_DYLIB` (0x0d), `LC_LOAD_WEAK_DYLIB` (0x80000018), `LC_REEXPORT_DYLIB` (0x8000001f) | two-level namespace graph, weak/re-export edges |
| `LC_DYLD_INFO`/`LC_DYLD_INFO_ONLY` (0x22/0x80000022) | rebase/bind/weak-bind/lazy-bind/export trie |
| `LC_MAIN` (0x80000028) | entry point = `__TEXT.vmaddr + entryoff` (file offset relative to `__TEXT.fileoff`); `stacksize` usually 0 |
| `LC_UNIXTHREAD` (0x05) | kernelcache, test fixtures, and apps whose deployment target is below iOS 6.0. Entry = `pc` from the `arm_thread_state32` payload (flavor 1, count 17). `UNVERIFIED:` that App Store binaries of the era use it (ld64 emits `LC_MAIN` only for `-ios_version_min >= 6.0`); close by extracting `MobileSafari` from the rootfs and running `llvm-objdump --macho --private-headers` on it. |
| `LC_FUNCTION_STARTS` (0x26), `LC_DATA_IN_CODE` (0x29) | block discovery seeds / data ranges |
| `LC_ENCRYPTION_INFO` (0x21) | refused if `cryptid != 0` (`E_ENCRYPTED_BINARY`) |
| `LC_VERSION_MIN_IPHONEOS`, `LC_UUID`, `LC_SOURCE_VERSION`, `LC_SUB_FRAMEWORK`, `LC_SUB_CLIENT` | recorded, not enforced |
| `LC_CODE_SIGNATURE`, `LC_SEGMENT_SPLIT_INFO`, `LC_DYLIB_CODE_SIGN_DRS` | recorded, signature ignored (stale after any dump) |

Rejected (loud abort, never silently skipped):

- `cputype != CPU_TYPE_ARM (12)` or `cpusubtype` not in {9,11}: `E_UNSUPPORTED_ARCH`.
- `cryptid != 0`: `E_ENCRYPTED_BINARY` (ADR-12).
- fat binary with no armv7/armv7s slice: `E_NO_ARM_SLICE`. (No iOS 6 system image is fat, but apps may be; parse `fat_header`/`fat_arch` BE.)
- (no `LC_UNIXTHREAD` rejection: both entry-point forms are accepted for apps; see §5 step 8.)
- Overlapping segments, segment `vmaddr+vmsize` outside the arena, `maxprot` granting write+execute without an explicit override: `E_BAD_SEGMENT`.

### 1.2 Segment mapping and `__PAGEZERO`

Mapping is `mmap(MAP_FIXED, addr=vmaddr, ...)` inside the arena (ADR-02), with `mprotect` to `initprot` after copy. `__PAGEZERO` (0x0..0x1000, `maxprot=0`) is **not** mapped: the arena's `mmap_min_addr=65536` already makes the first 64 KiB unmappable, which is exactly the 4 KiB page-zero behaviour we want (near-null derefs fault).

Protections: r/x for `__TEXT`, rw for `__DATA`/`__DATA_CONST`, r for `__LINKEDIT` (but see §2.4: cache images' `__LINKEDIT` segment is a placeholder). Guest pages keep the guest's own protections; translated code lives in the shadow region (ADR-04), never in guest pages ([05-jit.md §2, §7](05-jit.md)).

### 1.3 `LC_DYLD_INFO_ONLY` opcode interpreters (iOS 6 dyld2)

All four streams are byte streams with ULEB/SLEB. Implement per `dyld`'s `ImageLoaderMachOCompressed` (dyld-239.3 is the closest published source; iOS 6 is dyld-212.3.2, same stream format). Order of application at load time:

1. **Rebase** — add the image slide to all internal pointers. **For cache images the slide is 0** (§2.3), so this is a no-op but the opcodes (if any) are still consumed. For on-disk apps the slide is chosen by the loader (default 0 in the arena, honoured verbatim).
2. **Bind** (non-lazy) — resolve each entry against the 2-level namespace and write the target address into the pointer slot.
3. **Weak bind** — bind only if the symbol resolves; otherwise zero.
4. **Lazy bind** — `__la_symbol_ptr` slots. Cache images arrive pre-bound (§1.3 Measured); only the residual slots still point at the image's own `__stub_helper` and are resolved on first call through `dyld_stub_binder` (§4.5).

Structs/pointers handled:

- `__nl_symbol_ptr` (non-lazy): direct pointer slots, bound at load.
- `__la_symbol_ptr` (lazy): pointer slots. In the cache a slot is either pre-bound to the final target (Thumb targets carry bit 0) or still points into its own image's `__stub_helper`.
- `__picsymbolstub4` (armv7): **16-byte ARM-mode stubs** (section `reserved2 = 16` in all 551 images that have the section). Disassembled (`libsystem_c` stub 0 @0x3948527c, `llvm-mc --disassemble -triple=armv7-apple-ios`): `ldr r12,[pc,#4]; add r12,pc,r12; ldr pc,[r12]; .long <lazy_ptr - (stub+12)>`. The `ldr pc` interworks, so a Thumb target in the slot (odd address) is entered in Thumb state. Stub 0's literal `0x01b24094` resolves to `0x3948527c+12+0x01b24094 = 0x3afa931c`, the first `__la_symbol_ptr` slot.
- `__stub_helper`: per-symbol 12-byte entries `ldr r12,[pc,#0]; b common; .long <lazy-bind-info offset>`, then one common routine: `str r12,[sp,#-4]!` (push lazy offset); `ldr r12,[pc,#16]; add r12,pc,r12; str r12,[sp,#-4]!` (push the **address** of `__nl_symbol_ptr[1]`, dyld's per-image cookie slot); `ldr r12,[pc,#8]; add r12,pc,r12; ldr pc,[r12]` (jump through `__nl_symbol_ptr[0]` = `dyld_stub_binder`). The ABI of `dyld_stub_binder` is therefore stack-based, not `r0/r1` (§4.5).

Measured: `python3` walking all 569 images' `LC_SEGMENT` section tables (`/tmp/lj/dsc.py` helper; not committed): 551 images have `__picsymbolstub4`/`__stub_helper`/`__la_symbol_ptr`; of 76,651 lazy slots, 3,451 still point at their own `__stub_helper`, 1,308 point into libdyld's text (0x393fd000..0x3940d000), the rest into other images. `__nl_symbol_ptr[0]` is `0x393fe1e8` (= `dyld_stub_binder`, libdyld) in all 551 images. Consequence for ADR-09: because libdyld is builtin but every cache image already holds libdyld's *original* VAs in its pointer slots, builtin symbols of a cache image must be installed as **hooks at the original export VAs** (04 §8 mechanism: the lifter treats a block entry at such a VA as a builtin call site) — not only as synthetic `__BUILTIN` addresses (§3.2).

`LC_REEXPORT_DYLIB` (279/569 images) makes an image re-export another's symbols; `LC_LOAD_WEAK_DYLIB` (23/569) makes a dependency optional (missing → zero, no error); `LC_LOAD_UPWARD_DYLIB` (0x80000023, 7/569) is an upward link (treat as a normal dependency for lookup, but exclude it from initializer ordering edges); `LC_SUB_FRAMEWORK`/`LC_SUB_CLIENT` (18/1) and `LC_ROUTINES` (1) are recorded. Measured: counting load-command ids over the 569 images (`lcs()` helper): `LC_SEGMENT` 1703, `LC_ID_DYLIB` 569, `LC_DYLD_INFO_ONLY` 569, `LC_SYMTAB` 569, `LC_DYSYMTAB` 569, `LC_UUID` 569, `LC_VERSION_MIN_IPHONEOS` 569, `LC_FUNCTION_STARTS` 569, `LC_SOURCE_VERSION` 533, `LC_LOAD_DYLIB` 4560; no `LC_CODE_SIGNATURE`, `LC_SEGMENT_SPLIT_INFO`, `LC_DATA_IN_CODE` or `LC_ENCRYPTION_INFO` in any cache image.

### 1.4 Export trie

Walked per `dyld`'s `trie_walk`. Node = ULEB terminal size, then edges (label C-string + ULEB child offset), then terminal info (flags ULEB, address ULEB, other ULEB). Used for: symbol resolution, `stub` auto-generation (§3.2), and `_dyld_*`/`dlsym` lookups. Cache images keep `LC_SYMTAB` with cache-absolute offsets into the shared string pool (§2.5), so exports are reachable both ways; the trie is authoritative for re-exports.

### 1.5 `LC_FUNCTION_STARTS` / `LC_DATA_IN_CODE`

- `LC_FUNCTION_STARTS`: ULEB128 deltas from `__TEXT` start; seed list for block discovery (04 §3). Present in 569/569 cache images (measured above); apps: UNVERIFIED (expected, ld64 default).
- `LC_DATA_IN_CODE`: authoritative "this range in `__TEXT` is data" (jump tables). Absent in 0/569 cache images (measured above); do not require it, honour it if present. Without it, jump tables/literal pools must be recognised by the lifter (04 §3).
- `LC_DATA_IN_CODE`/function-starts ingestion lands in `lib/macho` and is consumed by `lib/lift` via `ios6/macho/image.hpp` only.

### 1.6 ObjC metadata

Ingest (do not interpret beyond addresses): `__objc_classlist`, `__objc_catlist`, `__objc_protolist`, `__objc_imageinfo`, `__objc_selrefs`, `__objc_classrefs`, `__objc_superrefs`, `__mod_init_func`, `__mod_term_func`. Initializer ordering: `__mod_init_func` entries run in link order after all dependencies' initializers (dyld2 ordering: depth-first over `LC_LOAD_DYLIB`), then `LC_MAIN`/`LC_UNIXTHREAD`. `__mod_term_func` runs at exit in reverse order.

## 2. The dyld-212 shared cache model

### 2.1 Header (88 bytes, `mappingOffset = 0x58`, no uuid)

`[research/ios6-platform.md §5.2]`. The cache is dyld-212.3.2 format: do **not** use `sizeof(dyld_cache_header)` or the dyld-239.3 layout. Parse `mappingOffset`/`imagesOffset` from the header.

| Off | Size | Field | This cache |
|---|---|---|---|
| 0x00 | 16 | magic | `"dyld_v1   armv7\0"` |
| 0x10 | 4 | mappingOffset | 0x58 |
| 0x14 | 4 | mappingCount | 3 |
| 0x18 | 4 | imagesOffset | 0xb8 |
| 0x1c | 4 | imagesCount | 569 |
| 0x20 | 8 | dyldBaseAddress | 0x2fe00000 |
| 0x28 | 8 | codeSignatureOffset | 0xea37000 |
| 0x30 | 8 | codeSignatureSize | 0 (= to EOF) |
| 0x38 | 8 | slideInfoOffset | 0xcaa3000 |
| 0x40 | 8 | slideInfoSize | 0x63000 |
| 0x48 | 8 | localSymbolsOffset | 0xcb06000 |
| 0x50 | 8 | localSymbolsSize | 0x1f30fdf |

No `uuid`, `cacheType`, `branchPools*`, `accelerateInfo*`, `imagesText*`, `sharedRegionStart/Size`, `maxSlide`, `subCacheArray*`, `objcOpts`, `dynamicData*`. `sharedRegionStart` is derived from `mapping[0].address`.

### 2.2 Mappings (slide 0)

```
mapping[0] addr=0x30000000 size=0x09518000 fileOff=0x00000000  maxProt=5 initProt=5  (r-x) TEXT
mapping[1] addr=0x39518000 size=0x01aa6000 fileOff=0x09518000  maxProt=3 initProt=3  (rw-) DATA
mapping[2] addr=0x3afbe000 size=0x01b48000 fileOff=0x0afbe000  maxProt=1 initProt=1  (r--) LINKEDIT
```

We map all three at their preferred addresses with **slide 0** (`mmap(MAP_FIXED, addr=mapping[i].address)`). Rationale: the cache was built for `SHARED_REGION_BASE_ARM = 0x30000000` and the images' `LC_ID_DYLIB`/pointers are unslid; `LC_DYLD_INFO.rebase_off/size = 0` in every image, so there are no rebase opcodes to apply. The shared region (0x30000000..0x3cb06000) sits below the kernel (0x80000000) and above dyld (0x2fe00000), and does not collide with the arena's reserved low 4 GiB policy because the arena *is* the low 4 GiB (ADR-02) — the cache mappings are carved into it at their fixed VAs.

**Slide info v1 (present, unused at slide 0).** `slideInfoOffset=0xcaa3000`, `slideInfoSize=0x63000` hold a `dyld_cache_slide_info` v1 header — measured with `struct.unpack_from('<6I', data, 0xcaa3000)` → `version=1, toc_offset=0x18, toc_count=0x1aa6, entries_offset=0x3580, entries_count=0xbec, entries_size=0x80`. `toc_count = mapping[1].size/4096`: one `u16` per 4 KiB page of the DATA mapping (3,052 distinct values, max 0xbeb) indexing one of 0xbec 128-byte bitmaps; each set bit marks a 32-bit word on that page that dyld must add the slide to. It exists so a real device can ASLR the shared region. We map at the preferred addresses (slide 0), so no bit is applied: the images' `LC_DYLD_INFO.rebase_off/size` are 0 in 569/569 images (measured above) and the DATA words already hold final addresses. Sliding would break guest VA == host VA for the cache (ADR-02) and the fixed shadow indexing of ADR-04. `cache info` prints the header fields only.

### 2.3 Per-image Mach-O headers

Each `dyld_cache_image_info` (32 B: `address u64, modTime u64, inode u64, pathFileOffset u32, pad u32`) points at an image whose Mach-O header is at `address` (a VA in mapping 0). `pathFileOffset` is a file offset for the NUL-terminated install path. Measured: 569 entries, 519 distinct addresses (31 addresses carry 2-12 alias paths, §2.6); all 569 headers have `magic=0xfeedface`, `cputype=12`, `cpusubtype=9`, `filetype=6 (MH_DYLIB)`, `flags & 0x80000000` (in-cache bit; other flag values: 0x801000b5 ×439, 0x801100b5 ×72, 0x801180b5 ×39, 0x800000b5 ×17, 0x800180b5 ×1, 0x800100b5 ×1); 0 are under `/Applications`.

Image's file offset in the cache = `image.address - mapping[k].address + mapping[k].fileOffset` (where k is the mapping containing the address). This is how `ipsw`/`dsc_extractor` recover standalone Mach-Os.

### 2.4 The shared `LC_SYMTAB` string pool

Cache images keep `LC_SYMTAB`/`LC_DYSYMTAB`, but the offsets are **cache-file absolute** and the string pool is **shared by all images**: e.g. `libsystem_kernel` `symoff=0xc1de07e nsyms=862 stroff=0xc42ee24 strsize=0x673558`; `UIKit` `symoff=0xbbde786 nsyms=30955` with the **same** `stroff/strsize`. `stroff+strsize` (0xca6237c) stays inside mapping 2. So: build one shared string table per cache; resolve `n_strx` against it. Per-image `__LINKEDIT` segment is a placeholder (`vmaddr=0x3b046000, vmsize=0x40ce000` for every image, beyond EOF) — never use it; use `LC_SYMTAB` offsets + mappings.

### 2.5 Local-symbols blob

At `localSymbolsOffset=0xcb06000` (outside all mappings — unmapped file data): `dyld_cache_local_symbols_info` with measured fields `nlistOffset=0x186c, nlistCount=0x92fb2 (602,034), stringsOffset=0x6e54c4, stringsSize=0x184bb1b, entriesOffset=0x18, entriesCount=0x207 (519)` (offsets relative to the blob start; read with `struct.unpack_from('<6I', data, 0xcb06000)`). Entries are `dyld_cache_local_symbols_entry { dylibOffset, nlistStartIndex, nlistCount }`, `dylibOffset` = cache-file offset of the image's Mach-O header. `entriesCount = 519` equals the number of **distinct image addresses** (§2.3), not the 569 paths. `nlist` records are 12 bytes (32-bit `struct nlist`): `nlistOffset + nlistCount*12 == stringsOffset` exactly (no discrepancy; the earlier "0x100 gap" note was an artefact of assuming 16-byte records), and `localSymbolsOffset + localSymbolsSize == stringsOffset+stringsSize` (end `0xea36fdf`; `codeSignatureOffset=0xea37000` is the next page; file size 0xeb5bd5e). Local function names live here, not in the mapped `LC_SYMTAB`; always read `stringsOffset`, never derive it.

### 2.6 Image resolution (`LC_LOAD_DYLIB` → cache image)

An image's `LC_LOAD_DYLIB` install name (e.g. `/usr/lib/libSystem.B.dylib`, `/System/Library/Frameworks/UIKit.framework/UIKit`) is matched against the cache image list by path string. `LC_REEXPORT_DYLIB` forwards; `LC_LOAD_WEAK_DYLIB` is optional.

Duplicate aliases exist: measured 31 addresses carry more than one install path (e.g. `0x38893000` has 12: `/usr/lib/libSystem.B.dylib`, `libSystem.dylib`, `libc.dylib`, `libpthread.dylib`, `libm.dylib`, `libdl.dylib`, `libinfo.dylib`, `libproc.dylib`, `libpoll.dylib`, `libdbm.dylib`, `librpcsvc.dylib`, `/System/Library/Frameworks/System.framework/Versions/B/System`). Resolve by address, not by string, after the first match.

### 2.7 Key image addresses (measured)

Measured with a throwaway script (not committed): open the cache, read header via `mappingOffset`, walk `imagesOffset`/`imagesCount`, match `path` suffix; all twelve rows below were re-measured and agree.

```python
import struct
data = open('work/10B329__iPad2,1/dyld_shared_cache_armv7','rb').read()
mo = struct.unpack_from('<I', data, 0x10)[0]; mc = struct.unpack_from('<I', data, 0x14)[0]
io = struct.unpack_from('<I', data, 0x18)[0]; ic = struct.unpack_from('<I', data, 0x1c)[0]
mappings = [struct.unpack_from('<QQQII', data, mo+i*32) for i in range(mc)]
for i in range(ic):
    addr, mt, ino, po, pad = struct.unpack_from('<QQQII', data, io+i*32)
    path = data[po:data.find(b'\0',po)].decode()
    if path.endswith(('libSystem.B.dylib','libobjc.A.dylib',...)): print(hex(addr), path)
```

| Image | VA (base) |
|---|---|
| `libSystem.B.dylib` | 0x38893000 |
| `libobjc.A.dylib` | 0x38fbf000 |
| `libsystem_kernel.dylib` | 0x394b4000 |
| `libsystem_c.dylib` | 0x3940d000 |
| `libdyld.dylib` | 0x393fd000 |
| `libdispatch.dylib` | 0x393dd000 |
| `CoreFoundation.framework/CoreFoundation` | 0x310a1000 |
| `Foundation.framework/Foundation` | 0x319c9000 |
| `UIKit.framework/UIKit` | 0x32f68000 |
| `QuartzCore.framework/QuartzCore` | 0x32d15000 |
| `OpenGLES.framework/OpenGLES` | 0x325c8000 |
| `OpenGLES.framework/GLEngine.bundle/GLEngine` | 0x32506000 |

dyld itself: 0x2fe00000 (from `dyldBaseAddress` and on-disk `/usr/lib/dyld`).

## 3. Builtin/guest/stub substitution (ADR-01, ADR-09)

### 3.1 Policy table

Per install name, one row:

```cpp
struct ImagePolicy {
  std::string install_name;
  enum Mode { Builtin, Guest, Stub } mode;
  std::string builtin_lib;   // for Builtin: host .so providing the symbols
  bool        allow_override; // IOS6RECOMP_IMAGE_OVERRIDE can flip
};
```

Defaults (ADR-09; every name below exists in the cache, measured: `/usr/lib/system/{libdyld,libsystem_*,libcompiler_rt,libunwind,libdispatch}.dylib`, `/usr/lib/{libSystem.B,libobjc.A,libc++.1,libc++abi,libstdc++.6}.dylib`, `IOKit`, `UIKit`): `libdyld` = **builtin**; every `libsystem_*`, `libSystem.B`, `libobjc.A`, `libunwind`, `libc++.1`, `libc++abi`, `libcompiler_rt`, `CoreFoundation`, `Foundation`, `libdispatch` = **guest** (the kernel shim sits under `svc #0x80`, not above libsystem_kernel); `OpenGLES.framework/OpenGLES` with its `GLEngine.bundle`, `lib*.dylib` siblings and `IMGSGX543GLDriver` ([09 §1.1](09-media.md)), `UIKit` (minimal shim, ADR-09), `IOKit`, `IOSurface`, `CoreSurface` = **builtin**; everything else = guest, else stub.

`IOS6RECOMP_IMAGE_OVERRIDE=libobjc.A.dylib=guest,libSystem.B.dylib=builtin,...` overrides per image without rebuild.

### 3.2 Symbol export synthesis for `builtin` images

A builtin image has no ARM bytes. The loader synthesises an image record whose export table lists the builtin's guest-visible symbols (read from the host `.so`'s ELF dynamic symbol table, filtered by a per-builtin manifest). Each symbol is bound to a **builtin entry VA**: (a) for an image that exists in the cache (libdyld, OpenGLES, UIKit, IOKit, IOSurface, CoreSurface), the symbol's **original export VA in the cache image** (e.g. `dyld_stub_binder` @0x393fe1e8), because cache images are pre-bound and already hold those addresses in `__nl_symbol_ptr`/`__la_symbol_ptr` slots (§1.3 Measured); (b) for a symbol with no cache counterpart, a synthetic address in a reserved `__BUILTIN` segment at 0x00010000.. (the first mappable arena page). The translator recognises both as a "builtin call site" via `lib/lift/hooks.td` (04 §8). A host function is above 4 GiB and unreachable by a guest branch, so no ARM trampoline is generated: builtin IMPs are host function pointers in a dispatch table; guest calls go through the FFI bridge (`lib/ffi`), which spills the GTC, marshals per ADR-03, calls the host fn and writes the result back. This is the same mechanism as hot-path interposition, so `objc_msgSend` (a guest hot path) and a builtin `_malloc` are handled uniformly. ObjC classes exported by a builtin (`EAGLContext`, `EAGLSharegroup`; 09 §1.4) are published by emitting class metadata into the synthetic image's `__objc_classlist` so the *guest* libobjc registers them. `UNVERIFIED:` close by loading a synthetic image with one class in a fixture and calling `[Cls alloc]` (M5 prerequisite).

### 3.3 `stub` auto-generation

For an image in stub mode, the loader walks its export trie and creates one stub function per exported symbol: logs `(symbol, caller VA, r0..r3)`, returns 0/NULL/false. Stub hits are counted; `IOS6RECOMP_STRICT=1` aborts on any stub hit. The stub table is the compatibility burn-down list (M5).

### 3.4 Hard rules for mixed mode

1. A guest image that **re-exports a builtin symbol** (`LC_REEXPORT_DYLIB`): the re-exported VA must resolve to the builtin entry VA (§3.2), not to a guest block; the loader detects re-export chains and terminates them at the builtin. Never create a guest block at a builtin VA.
2. `objc_msgSend` interposition (04 §8): performance-critical hot paths are written in ARMv7 assembly, compiled for `armv7-apple-ios6`, and loaded as guest code. The loader binds the symbol to the guest object's entry point and the lifter treats it as pre-translated. Applies uniformly to calls, `dlsym` results, ObjC IMPs, C function pointers.
3. A builtin may **not** be called with a guest function pointer that is then invoked by the builtin without going through `runtime::call_guest()` (ADR-06) — native frames must never sit between two guest frames observable to an unwinder.
4. Mixed-mode symbol resolution is by (image, name) pair; the same name in two images with different modes resolves per the caller's dependency edge (two-level namespace), never globally.

## 4. What the guest expects from dyld at runtime

### 4.1 `_dyld_*` API subset actually imported

Measured over all 569 image paths (519 distinct images; counts are per distinct image address, aliases counted once) by parsing the `LC_DYLD_INFO_ONLY` bind/weak/lazy opcode streams for symbol names containing `dyld` or starting `_dl` (`/tmp/lj/imp.py`, not committed), cross-checked against undefined external symbols in `LC_SYMTAB` (identical except where noted):

| Symbol | images importing | notes |
|---|---|---|
| `dyld_stub_binder` | 500 (bind streams) / 516 (undefined in `LC_SYMTAB`) | lazy-bind resolver; reached through `__nl_symbol_ptr[0]` (§1.3) |
| `_dlopen` | 95 | |
| `_dlsym` | 94 | |
| `_dlerror` | 26 | |
| `_dlclose` | 21 | |
| `_dladdr` | 19 | |
| `__dyld_get_image_header` | 6 | CoreFoundation, CoreLocation, CoreMotion, libmacho, libobjc, libxpc |
| `__dyld_image_count` | 6 | same set |
| `__dyld_get_image_name` | 6 | CoreFoundation, CoreLocation, CoreMotion, UIKit, libmacho, libxpc |
| `_dyld_image_path_containing_address` | 3 | CoreFoundation, UIKit, libobjc |
| `_dyld_register_image_state_change_handler` | 3 | AccessibilityUtilities, libobjc, libxpc — **libobjc's** registration path |
| `__dyld_get_image_vmaddr_slide` | 2 | CoreFoundation, libmacho |
| `__dyld_register_func_for_remove_image` | 2 | libkeymgr, libobjc |
| `_dlopen_preflight` | 1 | CoreFoundation |
| `__dyld_initializer` | 1 | libSystem.B (the one image behind 12 alias paths) |
| `__dyld_fork_child` | 1 | libSystem.B |
| `__dyld_register_func_for_add_image` | 1 | libkeymgr only (**not** libobjc) |
| `__dyld_get_image_slide` | 1 | libsystem_c |

libdyld exports (`LC_SYMTAB` of `/usr/lib/system/libdyld.dylib` @0x393fd000, measured) the complete set to implement: `dyld_stub_binder` 0x393fe1e8, `__dyld_{image_count 0x393fe388, get_image_header 0x393fe3b4, get_image_vmaddr_slide 0x393fe604, get_image_slide 0x393fe664, get_image_name 0x393fe634, image_containing_address 0x393fe694, get_image_header_containing_address 0x393fe6d8, register_func_for_add_image 0x393fe584, register_func_for_remove_image 0x393fe5c4, get_all_image_infos 0x393fea28, initializer 0x393fe818, fork_child 0x393feae4, moninit 0x393fe71c}`, `_dyld_{image_path_containing_address 0x393fea88, register_image_state_change_handler 0x393fe9d8, register_tlv_state_change_handler 0x393feb10, enumerate_tlv_storage 0x393feb14, get_sdk_version 0x393fe3e4, get_min_os_version 0x393fe4e8, get_program_sdk_version 0x393fe4d8, get_program_min_os_version 0x393fe510, shared_cache_some_image_overridden 0x393feab8}`, `_dl{open 0x393fe918, open_preflight 0x393fe94c, sym 0x393fe990, close 0x393fe8d4, error 0x393fe84c, addr 0x393fe88c}`, `__NSGetExecutablePath 0x393fe540`, `_NSVersionOfLinkTimeLibrary 0x393fe21c`, `_NSVersionOfRunTimeLibrary 0x393fe328`, data `_NXArgv`, `_environ`, `___progname` (§4.3), `_dyldVersionString 0x393fefd0` (`"@(#)PROGRAM:dyld  PROJECT:dyld-212.3.2"`), `_dyldVersionNumber 0x393feff8` (`0x9999999a` double). Builtin libdyld implements all of these; the `_tlv_*` entries are accepted but never called (no `__thread`/TLV in iOS 6, research/apple-armv7-abi.md).

`_dyld_all_image_infos` (the data struct) is not exported by any image; libdyld exposes it through `__dyld_get_all_image_infos` (0x393fea28). The string `"Attempt to get _dyld_all_image_infos failed"` is in the cache at VA 0x367edf93 (measured: `data.find` over mapping 0, `ad + i - fo`). Crash reporters read the struct; we synthesise it (§4.3).

Implementation requirement: builtin libdyld implements the full export list above. Stub-hit logs (§3.3) are the only extension mechanism; no other `_dyld_*` names are imported by cache images (measured: the import census above covers every `dyld`/`_dl` symbol in all 569 images).

### 4.2 `dlopen/dlsym/dlclose`

Provided by builtin libdyld. Semantics: load an on-disk Mach-O (or a cache image by name), map into the arena, apply rebase/bind, run initializers, return a handle. `dlsym` walks the image's export trie (+ re-exports). `dlclose` decrements refcount and runs terminators. For cache images, "loading" returns the already-mapped image.

### 4.3 Process globals

- `NXArgv`, `environ`, `___progname`: data symbols in libdyld (measured: `_NXArgv` @0x3afa316c, `_environ` @0x3afa3170, `___progname` @0x3afa3174, `__NSGetExecutablePath` @0x393fe540 in `libdyld.dylib`). We populate them at bring-up (§5).
- `__dso_handle`: per-image, defined by the linker; for guest images it's the image's `MH` address (or a synthetic for builtins). Used by `__cxa_atexit`.
- `dyld_all_image_infos`: struct returned by `__dyld_get_all_image_infos`, read by crash reporters. Fields: `version`, `infoArrayCount`, `infoArray`, `notification` (function pointer), `processDetachedFromSharedRegion`, `libSystemInitialized`, `dyldPath`, `errorMessage`. `UNVERIFIED:` exact iOS 6 layout; use dyld-239.3's `dyld_all_image_infos` as the template and verify by disassembling `__dyld_get_all_image_infos` (0x393fea28) and its callers in the cache (CoreSymbolication/CrashReporter-class images) for the offsets they read.
- `__DATA,__dyld` (libdyld, VA 0x3afa3000, size 0x1c): measured words `[0x2fe01000, 0x2fe01008, 0, 0x3afa3168, 0x3afa316c, 0x3afa3170, 0x3afa3174]` — two function pointers into dyld proper and the addresses of libdyld's own `NXArgc`-class data (`_NXArgv` 0x3afa316c, `_environ` 0x3afa3170, `___progname` 0x3afa3174). Only builtin-replaced libdyld code reads them, so nothing to emulate while libdyld is builtin; if ADR-09's libdyld mode is ever flipped to `guest` (`IOS6RECOMP_IMAGE_OVERRIDE`), these two function pointers must be hooked (UNVERIFIED which dyld functions they are).

### 4.4 Image add/remove callbacks

`_dyld_register_image_state_change_handler(state, batch, handler)` is **libobjc's** hook (measured: imported by libobjc, libxpc, AccessibilityUtilities; libobjc does not import `__dyld_register_func_for_add_image`): our libdyld records the handler, and for the requested state calls it with every image already at that state (batched) and with each later image as it reaches the state, passing `dyld_image_info` records. `_dyld_register_func_for_add_image` (only libkeymgr imports it) and `_dyld_register_func_for_remove_image` (libkeymgr, libobjc): record the callback; add-callbacks fire for every image currently loaded and every later one with `(const mach_header*, intptr_t slide)`; remove symmetric. The exact dyld state numbers/semantics of `dyld_image_states` are `UNVERIFIED:` — take them from the dyld-239.3 header and confirm by disassembling libobjc's `map_images` registration site (find the caller of the stub for `_dyld_register_image_state_change_handler` in libobjc @0x38fbf000).

### 4.5 `dyld_stub_binder` ABI

Measured ABI (disassembly of `dyld_stub_binder`, libdyld @0x393fe1e8, ARM mode, `llvm-mc --disassemble -triple=armv7-apple-ios`): `push {r0-r3,r7,lr}; add r7,sp,#16; ldr r0,[sp,#24]; ldr r1,[sp,#28]; blx <dyld fastBindLazySymbol-class helper>; mov r12,r0; pop {r0-r3,r7,lr}; add sp,sp,#8; bx r12`. So on entry (from the `__stub_helper` common routine, §1.3) the **stack** holds `[sp+0] = &__nl_symbol_ptr[1]` (per-image cookie slot) and `[sp+4] = lazy-bind-info offset`; `r0-r3` are the *callee's* arguments and are preserved; the helper returns the bound target address in `r0`; the binder pops both words and `bx r12` (interworking via bit 0). Our builtin implementation: map the cookie slot address to the image (the slot's image owns the address), run the lazy-bind opcodes at `lazy_bind_off + offset`, store the target in the `__la_symbol_ptr` slot for that bind, return the target to the translator as an indirect-branch target with all of `r0-r3`, `sp`, `lr` as at the original call. It must not disturb `r0-r3`/`lr` (hot path). Cache images mostly never reach it (§1.3 Measured: 4.5% of lazy slots unbound); app images (non-cache) always do for their first call per import.

## 5. Process bring-up: `ios6recomp run app` → `main()`

1. **Arena reserve**: `mmap(NULL, 4 GiB, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE, -1, 0)` at address 0; verify success (abort with clear message otherwise) (ADR-02).
2. **Map the cache**: map the 3 cache mappings at slide 0 (file-backed `MAP_FIXED|MAP_PRIVATE`; the DATA mapping must be copy-on-write). Refuse if any mapping collides. dyld itself (`/usr/lib/dyld`, not present in the cache) is **not** loaded: libdyld is builtin (ADR-09), so dyld's code is never executed. Reserve `[0x2fe00000, 0x30000000)` (`dyldBaseAddress` up to the cache start, 2 MiB) as `PROT_NONE` so stray stack/heap placement cannot land there.
3. **Load the app** (and its `LC_LOAD_DYLIB`s): parse Mach-O, map segments into the arena at their `vmaddr`s, apply rebase/bind. Refuse `cryptid != 0`.
4. **Main-thread stack**: top = `0x2fe00000` (`USRSTACK`), growing down. Measured: in the kernelcache the function matching `thread_userstack(thread, flavor, tstate, count, *user_stack, *customstack)` (Thumb, VA 0x8008a42a; `cmp r1,#1` flavor, `cmp r3,#17` count = `ARM_THREAD_STATE_COUNT`, `ldr r2,[r2,#52]` = `tstate->sp`) stores `*user_stack = tstate->sp` with `*customstack = 1` when `sp != 0`, else `movs r2,#0; movt r2,#12256 (0x2fe0)` → `*user_stack = 0x2fe00000`, `*customstack = 0`. (Found by scanning the kernel for `movt rX,#0x2fe0`; disassembled with `llvm-mc --disassemble -triple=thumbv7-apple-ios`; the kernelcache is symbol-stripped except KPI exports, so the function name is inferred from its signature.) The default region is `[0x2fe00000 - size, 0x2fe00000)`; 1 MiB main thread / 512 KiB secondary threads are the commonly cited iOS defaults (`UNVERIFIED:` not measured; confirm from `RLIMIT_STACK` in the kernelcache or `pthread_get_stacksize_np(pthread_main_thread_np())` in a device-run fixture). The strings area (step 5) is carved from the top, so the initial `sp` is below `0x2fe00000`. The range `0x2fe00000..0x30000000` is dyld's, and the cache starts at `0x30000000` (§2.2); the stack never extends above `0x2fe00000`.
5. **Initial stack layout** (`argv`/`envp`/`apple[]`): follow `exec_copyout_strings` (`~/src/xnu/bsd/kern/kern_exec.c:6452-6523`, the ASCII figure there; that tree is a **current** xnu, not xnu-2107, so the byte layout for 2107 is `UNVERIFIED:` — validate with the fixture below). From `sp` upward: `argc` (int), `argv[0..argc-1]`, `NULL`, `envp[0..n-1]`, `NULL`, `apple[0]` = executable path, `apple[1..]`, `NULL`, then the PATH area and STRING area, 16 bytes of padding at the top. When a dynamic linker is used the kernel additionally pushes the main executable's `mach_header*` one word *below* `argc` (`kern_exec.c:2542-2548`); crt1's `start` for `LC_UNIXTHREAD` apps expects `sp -> argc`, so we push no `mach_header*` and instead start the app as dyld would (post-dyld state). Validate with a self-built armv7 fixture that prints `argv`, `environ`, `apple[]` under `qemu-arm`.
6. **Commpage** at `0xffff1000` (one 4 KiB page inside the arena's 4 GiB). The base is **measured**, not `0xffff4000` (that is the base in a *current* xnu `osfmk/arm/cpu_capabilities.h:240`, a later layout): scanning every `/usr/lib/system/*.dylib` `__text` for `movw rX,#imm; movt rX,#0xffff` (Thumb-2 and ARM encodings) plus aligned literal-pool words in `0xffff0000..0xffff7fff` yields exactly these accesses (later disassembled):

| Address | Width | Reader | Observed use |
|---|---|---|---|
| `0xffff1020` | u32 | `libsystem_c` `OSSpinLockLock`, `_spin_lock`, `__spin_lock` | `flags & 0x9000`: `== 0x1000` selects one implementation, else bit `0x8000` set (uniprocessor-style) selects another, else the default. Value 0 picks the default. |
| `0xffff1021` | u8 | `libsystem_c` OSAtomic `*Barrier` resolvers | bit 7 selects the barrier-less variant (reported by peer agent `AuditRuntimeFrameworksTesting`; `UNVERIFIED:` not re-measured here — either 0 or 0x80 works) |
| `0xffff1030` | u32 | `libcache` | `cmp r0,#51`; below 51 takes the early branch (`UNVERIFIED:` meaning) |
| `0xffff1034`, `0xffff1036`, `0xffff1035` | u8 each | `libdispatch` (two sites) | `ldrb` each, stored into a 3-word struct at offsets 0, 4, 8 respectively: CPU counts (logical/physical/active order `UNVERIFIED:`) |
| `0xffff1038`, `0xffff103c` | u32 each | `libsystem_c` `create_scalable_zone` (malloc) | low/high words of a 64-bit physical-memory size (`UNVERIFIED:` field name) |
| `0xffff1050`, `0xffff1058`, `0xffff105c`, `0xffff1060` | u32 | `libsystem_c` `___commpage_gettimeofday` | timestamp/scale/seqlock data; the function does `orrs r10,r8; beq` on the first two words and returns "use the syscall" when both are zero (inference from disassembly), so a **zero-filled** page makes `gettimeofday` fall back to the `gettimeofday` syscall |

   Decision: map the page `PROT_READ`, zero-fill, then write: `0xffff1020 = 0`, `0xffff1034..36 = 2` (iPad2,1 is dual-core; no host-derived value, determinism), `0xffff1038 = 0x20000000` (512 MiB, iPad 2), `0xffff1030 = 0`. Not needed: `mach_absolute_time` (trap `r12 = -3`, `mvn r12,#2; svc #128` measured at libsystem_c 0x3940e240), `mach_timebase_info` (trap `r12 = -89`, i.e. `mach_timebase_info_trap`, measured at libsystem_kernel 0x394b4fdc), `sys_icache_invalidate`/`sys_dcache_flush`/`_thread_set_tsd_base` (`r12 = 0x80000000` with `r3 = 0/1/2`, measured at libsystem_c 0x3942a054/0x3942a064/0x3942d96c): all are kernel-boundary traps owned by 07. The kernelcache has the string `"commpage cpus==0"` (commpage init asserts cpu count), consistent with a populated page. Any guest access in `0xffff0000..0xffffffff` outside the fields above is logged once (ADR-13). `UNVERIFIED:` fields outside this list; close by running real apps and logging commpage faults.
7. **Initializers**: run `__mod_init_func` of each image in dependency order (§1.6), libSystem's first, then the app's.
8. **Entry**: `LC_MAIN`: `pc = __TEXT.vmaddr + entryoff`, `r0 = argc`, `r1 = argv`, `r2 = envp`, `r3 = apple`, `lr` = a hook that calls `exit(r0)`. `LC_UNIXTHREAD`: load the `arm_thread_state32` (`pc`, `sp`, `r0..r12`, `lr`, `cpsr`) from the load command, then set `sp` from step 5 (the saved `sp` in an app image is 0); Thumb entry iff `pc & 1` or `cpsr` T bit. `TPIDRURO` is set to the main thread's TSD base before step 7.

## 6. Extraction pipeline and CLI

### 6.1 Extraction (no root)

```bash
# patched ipsw from ~/src/ipsw (the stock /usr/bin/ipsw cannot read this IPSW; see README "Getting the inputs")
ipsw extract --dyld   --lookup -o work iPad2,1_6.1.3_10B329_Restore.ipsw   # -> work/10B329__iPad2,1/dyld_shared_cache_armv7
ipsw extract --kernel --lookup -o work iPad2,1_6.1.3_10B329_Restore.ipsw   # -> work/10B329__iPad2,1/kernelcache.release.iPad2,1
```

Expected (re-measured with `sha1sum` / `ls -l` / `llvm-objdump --macho -f`; same values as README.md):
- dyld shared cache: **246,791,518 B**, sha1 `8a9d9587d0bc91871458d50bce71682eb341d26a`
- kernelcache: **13,987,840 B**, sha1 `3a65ceeba525af4e0959ffcf7b45edc9c6fd8a74`; the extracted file already **is** the decompressed armv7 Mach-O (first word `0xfeedface`, `__TEXT` at 0x80001000, `__DATA` 0x802d0000, `__KLD` 0x8033e000, `__LAST` 0x80340000, `__PRELINK_TEXT` 0x80394000; `strings` shows `xnu-2107.7.55.2.2`). The firmware input is an IMG3 (README); the patched `ipsw` unwraps it. `10-build-tooling.md` quotes a 13,987,783-byte decompressed size that does not match this file (13,987,840) — mismatch for that chapter's owner.

Rootfs is `encrcdsa`-encrypted (per-build key from theapplewiki, auto-fetched by `--lookup`). Only App Store `cryptid=1` payloads need a device (ADR-12): refuse them.

### 6.2 CLI commands (M0)

```
ios6recomp ipsw list <file.ipsw>             # roles from BuildManifest.plist: OS, KernelCache, RestoreRamDisk, ...
ios6recomp cache info <dyld_shared_cache>     # header fields, arch, image count, dylib list, subcache presence,
                                             # sharedRegionStart/Size, slide, dyld version string
ios6recomp macho info <path>                 # header, cputype/cpusubtype, load commands, encryption info,
                                             # segments, exports count
```

`cache info` output fields (minimum): magic, mappingOffset, mappingCount + per-mapping (addr,size,fileOff,maxProt,initProt), imagesCount (+ distinct-address count), dyldBaseAddress, codeSignatureOffset/Size (effective, to EOF if 0), slideInfoOffset/Size + version, localSymbolsOffset/Size, sharedRegionStart (=mapping[0].address), sharedRegionSize (= last mapping end − first mapping start = `0x3afbe000 + 0x1b48000 − 0x30000000 = 0xcb06000`), slide (=0), dyld version string (read from libdyld's `_dyldVersionString` @0x393fefd0, §4.1; the cache holds no dyld image — `dyldBaseAddress` only names where dyld lives in a process).

`macho info` output fields (minimum): magic, cputype, cpusubtype, filetype, flags, ncmds, sizeofcmds, per-load-command summary, segments (segname, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects), encryption info (cryptoff, cryptsize, cryptid), has LC_MAIN/LC_UNIXTHREAD and entryoff, function-starts count, data-in-code count.

## 7. Verification

- M0: `ipsw list`/`cache info`/`macho info` run on the local IPSW and print the values in §2.1/§6.2; `cache info` reports `sharedRegionStart=0x30000000`, `sharedRegionSize=0xcb06000`, slide=0, image count=569 (519 distinct), dyld version `dyld-212.3.2` (string `@(#)PROGRAM:dyld  PROJECT:dyld-212.3.2` read from libdyld's `_dyldVersionString`).
- M1: `macho info` agrees field-by-field with `llvm-objdump --macho -p -s -exports-trie` on every fixture and on a real extracted cache dylib; `map app` shows every segment at its guest vmaddr with correct protections and `/proc/self/maps` confirms guest VA == host VA; `cryptid=1` image rejected with `E_ENCRYPTED_BINARY`.
- Loader negative/fuzz tests (add to `12-testing.md`): truncated load commands (cmdsize < struct size → `E_TRUNCATED_LC`), overlapping segments → `E_OVERLAP_SEGMENT`, out-of-range bind ordinals → `E_BAD_BIND_ORDINAL`, lazy-bind offset beyond image → `E_BAD_LAZY_BIND`, export-trie cycle → `E_BAD_EXPORT_TRIE`, image with `cpusubtype=11` loaded into an `armv7`-only lifter → diagnostic (not crash), fat binary with only x86 slice → `E_NO_ARM_SLICE`, rebase/bind stream with unknown opcode → `E_BAD_DYLD_INFO` (ADR-13).
- M1 cache checks (all reproduced above with the throwaway scripts; make them real `ctest` cases against the local cache when present): `__picsymbolstub4` decodes to the 3-instruction form and its literal resolves to a `__la_symbol_ptr` slot for every stub in `libsystem_c`; `__nl_symbol_ptr[0]` equals the `dyld_stub_binder` export VA in all 551 images with stubs; commpage page contains exactly the §5 fields.
