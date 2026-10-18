# iOS 6 platform reference (for ios6-recomp)

Target: iOS 6.1.3 (10B329), iPad2,1 (k93ap, S5L8940, Cortex-A9, armv7), host Arch Linux x86-64.
Local artifacts: `/home/theo/src/ios6-recomp/iPad2,1_6.1.3_10B329_Restore.ipsw` (832,740,317 B).

Conventions used below:
- `[measured]` = value read out of the local IPSW or out of the filesystem I decrypted from it, in this session; the command/path is given on the same line.
- `[src]` = value from primary source (URL or source file + symbol).
- `UNVERIFIED:` = could not confirm; do not rely on it.
- Offsets are hexadecimal, file-relative unless stated.

---

## 0. Evidence base / how to reproduce

Everything marked `[measured]` below came from this pipeline (all read-only w.r.t. the repo; scratch work in `/tmp/ios6x`):

```bash
# 1. inventory
unzip -l iPad2,1_6.1.3_10B329_Restore.ipsw
# 2. rootfs DMG (encrcdsa/AES) -> plain UDZO DMG, then partition 5 = HFSX root volume
ipsw disk dmg --decrypt --key 70f5ac054bf50a522fd39071f6acbd92954804599c1507b881d7d9c4026005e3867bfab0 \
     -p 5 -o part5.bin 048-2634-005.dmg          # 1,115,074,560 B raw HFSX
sudo mount -o loop,ro part5.bin /mnt/ios6x       # Linux hfsplus driver: see §3.6 caveat
# 3. kernelcache: IMG3 -> AES-256-CBC DATA -> complzss -> LZSS -> 32-bit Mach-O kernel
# 4. shared cache + binaries parsed directly out of part5.bin / the decrypted DMG
```

Tooling present on this host: `ipsw` (blacktop, `/usr/bin/ipsw`), `7z` 26.04, `clang/llvm-objdump` 23.1.3, `capstone` 5.0.9 (python), `qemu-img`, `sudo` (loop mounts work). `ipsw info` **crashes** on this IPSW (it tries IMG4/ASN.1 paths); `ipsw kernel *`/`ipsw dyld *` parse IMG4/IM4P-era inputs and reject iOS 6 IMG3 unless noted.

---

## 1. Devices that ran iOS 6.x, SoCs, and per-device slice architecture

`cpusubtype` column = the arch Apple shipped that device's iOS 6 system binaries as (Mach-O `cpusubtype`, §4). 22 model identifiers shipped iOS 6.

| Model | Product | Board | SoC (platform string) | CPU core | iOS 6 slice | Final iOS 6 build |
|---|---|---|---|---|---|---|
| iPhone2,1 | iPhone 3GS | n88ap | S5L8920 (`s5l8920x`) | Cortex-A8 | armv7 (9) | **6.1.6 (10B500)** |
| iPhone3,1/3,2/3,3 | iPhone 4 | n90ap/n90bap/n92ap | A4 S5L8930 (`s5l8930x`) | Cortex-A8 | armv7 (9) | 6.1.3 (10B329) |
| iPhone4,1 | iPhone 4S | n94ap | A5 S5L8940 (`s5l8940x`) | Cortex-A9 ×2 | armv7 (9) | 6.1.3 (10B329) |
| iPhone5,1/5,2 | iPhone 5 | N41AP/N42AP | A6 S5L8950 (`s5l8950x`) | Swift ×2 | **armv7s (11)** | 6.1.4 (10B350) |
| iPod4,1 | iPod touch 4G | n81ap | A4 S5L8930 | Cortex-A8 | armv7 (9) | **6.1.6 (10B500)** |
| iPod5,1 | iPod touch 5G | n78ap | A5 S5L8942 (`s5l8942x`) | Cortex-A9 ×2 | armv7 (9) | 6.1.3 (10B329) |
| iPad2,1 | iPad 2 Wi-Fi | k93ap | A5 S5L8940 | Cortex-A9 ×2 @1 GHz | armv7 (9) | **6.1.3 (10B329)** |
| iPad2,2/2,3 | iPad 2 GSM/CDMA | k94ap/k95ap | A5 S5L8940 | Cortex-A9 ×2 | armv7 (9) | 6.1.3 (10B329) |
| iPad2,4 | iPad 2 (Mid 2012) | k93aap | A5 **S5L8942** | Cortex-A9 ×2 | armv7 (9) | 6.1.3 (10B329) |
| iPad2,5/2,6/2,7 | iPad mini 1 | p105ap/p106ap/p107ap | A5 S5L8942 | Cortex-A9 ×2 | **armv7 (9)** — *not* armv7s | 6.1.3 (10B329) |
| iPad3,1/3,2/3,3 | iPad 3 | j1ap/j2ap/j2aap | A5X S5L8945 (`s5l8945x`) | Cortex-A9 ×2 | armv7 (9) | 6.1.3 (10B329) |
| iPad3,4/3,5/3,6 | iPad 4 | P101AP/P102AP/P103AP | A6X S5L8955 (`s5l8955x`) | Swift ×2 | **armv7s (11)** | 6.1.3 (10B329) |

Sources: model/board/platform lists — `https://api.ipsw.me/v4/device/<id>` and `https://api.ipsw.me/v4/devices`; final-build column — same API (`iPad2,1`: 6.1.3/10B329 is its last iOS 6, and its last iOS ever is 9.3.5); SoC↔model — Wikipedia `IPhone_3GS`, `Apple_A4`, `Apple_A5`, `Apple_A5X`, `Apple_A6`, `Apple_A6X`; armv7s identity described in `https://theapplewiki.com/wiki/ARMv7s`; iPad mini 1 = A5/armv7 per `https://ios.cfw.guide/get-started/select-ipad-mini/` and `https://theapplewiki.com/wiki/A5`.

* **armv7s ⊂ { iPhone 5 (iPhone5,1/5,2), iPad 4 (iPad3,4/3,5/3,6) }** in the iOS 6 era. The iPad mini 1 is A5/armv7, so any "armv7s = A6 *and A5X*" claim is wrong. iPhone5,3/5,4 (5c) are armv7s but never ran iOS 6.
* Final iOS 6 release overall: **6.1.6 (10B500), 2014-02-21**, for iPhone2,1 and iPod4,1 only. iPhone5,1/5,2 got **6.1.4 (10B350)** as their last. `[src] https://en.wikipedia.org/wiki/IOS_6` (version history).
* **Final iOS 6 for iPad2,1 = 6.1.3 (10B329)** `[src] https://api.ipsw.me/v4/device/iPad2,1`; matches local IPSW name.
* Per-slice cpusubtype values: `CPU_TYPE_ARM = 12`, `CPU_SUBTYPE_ARM_V7 = 9`, `CPU_SUBTYPE_ARM_V7S = 11` (`osfmk/mach/machine.h`; comment on 11 is `/* Swift */`).
* iOS 6 dropped: iPad1,1, iPod3,1 (last 5.1.1) `[src] https://en.wikipedia.org/wiki/IOS_6`. iPod3,1 is armv7 — that drop is policy, not ISA.
* Apple TV 2/3 ran "Apple TV Software 6.x" (11A/11B/11D builds) — *not* iOS 6 builds; kernel arch mapping not verified here.

---

## 2. iPad2,1 (S5L8940, Cortex-A9) vs armv7s (Swift): what the recompiler must model

| Property | armv7 slice on iPad2,1 (A5/Cortex-A9) | armv7s slice (A6/A6X, Swift) | Evidence |
|---|---|---|---|
| `cpusubtype` | 9 (`CPU_SUBTYPE_ARM_V7`) | 11 (`CPU_SUBTYPE_ARM_V7S`) | `[measured]` all 569 cache images on iPad2,1 = 9; `[src] osfmk/mach/machine.h` |
| Architecture | ARMv7-A | ARMv7-A sub-arch "7-S" | `[src]` LLVM `ARMTargetParser.def`: `ARM_ARCH("armv7s", ARMV7S, "7-S", "+v7s", …)`, default CPU `ARM_CPU_NAME("swift", ARMV7S, FK_NEON_VFPV4, true, AEK_HWDIVARM|AEK_HWDIVTHUMB)` |
| VFP | **VFPv3-D32** (32 × 64-bit d-regs; d0–d15 caller-saved, d8–d15 callee-saved, d16–d31 exist and are caller-saved) | **VFPv4-D32** | clang 23.1.3 macro diff `[measured]`: `-mcpu=cortex-a9` → `__ARM_VFPV2__`/`__ARM_FP=0xe`; `-mcpu=swift` → `__ARM_VFPV4__`, `__ARM_FEATURE_FMA=1` |
| FMA (VFMA/VFMS/VFNMA/VFNMS) | absent | present | `[measured]` clang macro `__ARM_FEATURE_FMA` defined only for swift; `[measured]` zero VFMA-ish encodings in local armv7 app binaries |
| NEON (Advanced SIMD) | **present** (NEON = ARMv7-A Advanced SIMD v1) | present, v2 (adds FP16 convert, `__ARM_NEON_FP=0x6`) | `[measured]` clang: `__ARM_NEON 1` for both; byte scan of `MobileSafari`/`SpringBoard` finds 16,494 / 51,647 words with unconditional Advanced-SIMD top-byte encodings (`0xF2xxxxxx`/`0xF4xxxxxx`) |
| Hardware integer divide (SDIV/UDIV) | **absent** | **present** (`__ARM_FEATURE_IDIV`, `__ARM_ARCH_EXT_IDIV__`) | `[measured]` clang macro diff; `[measured]` byte scan: 0 ARM-mode SDIV/UDIV encodings in local armv7 binaries |
| Thumb-2 (incl. `IT`, `CBZ`) | **present** (ARMv7-A always has Thumb-2) | present | `[src]` ARMv7-A architecture; `[measured]` local app binaries are mixed ARM/Thumb-2 (llvm-objdump reports invalid encodings when forced to ARM mode) |
| Unaligned access | allowed for Normal memory: `LDR`/`LDRH`/`STR`/`STRH` (and `LDM`/`STM` non-writeback to Normal memory, ARMv7-A); **not** allowed for `LDRD`/`STRD`, `LDM`/`STM` writeback, and Device memory | same | `[src]` ARM ARM (ARMv7-A/R) `A3.2.1` unaligned access rules. Whether iOS 6's kernel leaves `SCTLR.A` clear `UNVERIFIED:` (kernel sources for XNU 2107 unpublished) |
| LPAE (Large Physical Address Extension) | **not implemented** by Cortex-A9 | `UNVERIFIED:` for Swift | LPAE is optional in ARMv7-A and is not in the Cortex-A9 feature set (it appears in Cortex-A7/A12/A15/A17 class cores) `[src] https://support.arm.com/documentation/den0013/0400/…/Large-Physical-Address-Extension--LPAE-`; iOS 6's own kernel/segments use plain 4 KB-granular, 2-level-table addresses (§7), and Apple never shipped an LPAE 32-bit iOS kernel |
| Endianness / float ABI | little-endian, hard-float AAPCS-VFP (`__ARM_PCS_VFP`) | same | `[measured]` Mach-O magic `0xfeedface` + NEON/VFP use in local binaries; `[src]` Apple iOS ABI docs `UNVERIFIED:` (no Apple doc fetched; consistent with every binary inspected) |

Notes for a recompiler on x86-64: the armv7 targets here are **VFPv3-D32 + NEON**, so NEON intrinsics/`q` registers must be mapped (SSE2/AVX is the usual lowering); armv7s only *adds* IDIV + FMA + FP16 convert, so an armv7s binary can run on an armv7-modeled core with IDIV/FMA emulated — nothing in the armv7s ISA is missing from armv7 that changes the core model beyond those three.

---

## 3. The local IPSW: inventory, container formats, offline usability

### 3.1 Top-level members (`[measured] unzip -l iPad2,1_6.1.3_10B329_Restore.ipsw`, 28 entries)

| Path | Size (B) | Container | Identifier | Notes |
|---|---|---|---|---|
| `048-2634-005.dmg` | 803,633,664 | **`encrcdsa` (Apple AES UDIF v2)** | — | Root filesystem (`Restore.plist: SystemRestoreImages/User`); *is* the `OS`/rootfs image. No `koly` trailer **in the encrypted file** — `qemu-img` ("Could not locate UDIF trailer"), plain `7z` ("Cannot open the file as [Dmg] archive") and `ipsw extract --dyld` all fail on it, because the whole UDIF structure (including `koly`) is inside the AES stream (§3.4) |
| `048-2679-005.dmg` | 9,951,588 | IMG3 | `rdsk` | Update RamDisk (`RamDisksByPlatform/s5l8940x/Update`) |
| `048-2516-005.dmg` | 9,955,684 | IMG3 | `rdsk` | Restore RamDisk (`…/User`) |
| `kernelcache.release.k93` | 7,744,228 | IMG3 | `krnl` | tags `TYPE,DATA,SEPO,KBAG×2,SHSH,CERT` |
| `BuildManifest.plist` | 18,888 | XML plist | — | digests/TSS info |
| `Restore.plist` | 2,092 | XML plist | — | `DeviceMap` CPID 35136, `Platform s5l8940x`, `ProductBuildVersion 10B329`, `ProductType iPad2,1`; **no keys** |
| `Firmware/all_flash/all_flash.k93ap.production/iBoot.k93ap.RELEASE.img3` | 293,348 | IMG3 | `ibot` | |
| `…/LLB.k93ap.RELEASE.img3` | 149,988 | IMG3 | `illb` | |
| `…/DeviceTree.k93ap.img3` | 76,196 | IMG3 | `dtre` | |
| `…/recoverymode~ipad.s5l8940x.img3` | 83,876 | IMG3 | `recm` | |
| `…/applelogo,batterycharging0/1,batteryfull,batterylow0/1,glyphcharging,glyphplugin` | 7,524 / 5,092 / 15,972 / 249,636 / 47,524 / 17,700 / 6,180 / 5,092 | IMG3 | `logo,chg0,chg1,batF,bat0,bat1,glyC,glyP` | |
| `…/manifest` | 320 | text | — | flash manifest |
| `Firmware/dfu/iBEC.k93ap.RELEASE.dfu` | 289,252 | IMG3 | `ibec` | |
| `Firmware/dfu/iBSS.k93ap.RELEASE.dfu` | 76,260 | IMG3 | `ibss` | |
| `Firmware/usr/local/standalone/` | 0 B (empty dir) | — | — | directory placeholder |

**There is no dyld shared cache anywhere at IPSW top level.** `[measured] unzip -l` shows exactly the 28 entries above; the cache exists only *inside* the root filesystem image (§3.2, §5).

### 3.2 Where the shared cache actually is

`[measured]` after decrypting the rootfs DMG and reading its HFSX volume:

* `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7` — **246,791,518 bytes**, `-r--r--r-- root:80`, mtime 2013-03-09. This is the only file in that directory (one cache per device, named by arch; no `dyld_shared_cache_armv7s` in this iPad2,1 image).
* `/System/Library/dyld/` **does not exist** on iOS 6 (that layout is macOS/iOS-11+ `[src] https://github.com/apple-oss-distributions/dyld/blob/main/doc/CacheLayout.md` and `ipsw`'s constants `MacOSCacheFolder`/`IPhoneCacheFolder`).
* Rootfs has **no on-disk copies of cached dylibs**: `[measured]` `7z l` on the decrypted DMG (logical sizes) lists exactly 6 entries under `/usr/lib/`: `dyld` (196,064 B) plus four symlinks (`libIOKit.A.dylib`, `libIOKit.dylib`, `libMatch.dylib`, `libexslt.dylib`, whose 59/16/16/16-byte "sizes" are their target strings); `/usr/lib/system/` exists but is empty; `/System/Library/dyld/` does not exist. There is no on-disk `/usr/lib/libSystem.B.dylib`, `/usr/lib/libobjc.A.dylib`, `/usr/lib/system/libcache.dylib`, `/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation`. Anything whose path appears in the cache image list must be read from the cache (§5.4).
* Rootfs *does* have on-disk app executables: `[measured]` `7z l` on the decrypted DMG reports `MobileSafari` 1,128,816 B (524,288 B allocated), `MobileMail` 1,299,120 B, `SpringBoard` 2,765,344 B. **No `/Applications/*` image exists in the cache** (0 of 569 image paths start with `/Applications/`), so app code must come from the rootfs, and system dylib code from the cache.

### 3.3 Usability without a jailbroken device

Every IPSW member above is obtainable **offline** for 6.1.3/iPad2,1 because the community published the per-build keys (`https://theapplewiki.com/wiki/Keys:BrightonMaps_10B329_(iPad2,1)`). Verified end-to-end in this session:

| Artifact | Container + crypto | Key needed | Offline-reachable? |
|---|---|---|---|
| rootfs `048-2634-005.dmg` | `encrcdsa` v2 (AES-128-CBC + per-block HMAC-SHA1 IV) | 72-hex key (AES 16 B ‖ HMAC 20 B), published | **yes** — decrypted + mounted ✓ |
| `kernelcache.release.k93` | IMG3 `krnl`, DATA = AES-256-CBC, plaintext = `complzss`/LZSS | IV+Key (32+16 B hex), published | **yes** — decrypted + decompressed to Mach-O ✓ |
| `048-2516-005.dmg`, `048-2679-005.dmg` | IMG3 `rdsk`, DATA = AES-CBC | IV+Key pairs, published | **yes** — decrypted; ramdisk is a raw HFS+ image ✓ |
| iBoot/iBEC/iBSS/LLB/DeviceTree/logos | IMG3 (KBAG + CERT inside), DATA AES | IV+Key pairs, published | yes (same recipe as kernelcache; not decrypted here) |
| dyld shared cache | inside rootfs DMG | (comes with rootfs) | **yes** — parsed from the mounted volume ✓ |
| App Store binaries | Mach-O with `LC_ENCRYPTION_INFO cryptid=1` | **device UID key**, not published | **no** (§6) |

So: the *only* thing that needs a jailbroken armv7 device is any third-party/App-Store Mach-O payload. All Apple-shipped content (kernel, system dylibs, system apps) is offline-accessible for this build.

### 3.4 Recipe A — decrypt the `encrcdsa` rootfs DMG offline (verified)

Header (all fields **big-endian**; struct = `github.com/blacktop/go-apfs/pkg/disk/dmg` `EncryptionHeader`, `/home/theo/go/pkg/mod/github.com/blacktop/go-apfs@v1.0.28/pkg/disk/dmg/enc.go`):

| Off | Size | Field | `[measured]` value for `048-2634-005.dmg` |
|---|---|---|---|
| 0x00 | 8 | `magic` | `"encrcdsa"` |
| 0x08 | 4 | `Version` | 2 |
| 0x0c | 4 | `EncIvSize` | 16 |
| 0x10 | 4 | `Unknown1` | 5 |
| 0x14 | 4 | `Unknown2` | 0x80000001 (keyblob wrapping = CMS/3DES) |
| 0x18 | 4 | `DataEncKeyBits` | **128** (AES-128) |
| 0x1c | 4 | `Unknown3` | 0x5b |
| 0x20 | 4 | `HmacKeyBits` | **160** (HMAC-SHA1 key) |
| 0x24 | 16 | `UUID` | `fec723997e02415ebd8161d711acd237` |
| 0x34 | 4 | `Blocksize` | **512** |
| 0x38 | 8 | `Datasize` | 0x2fe49aff |
| 0x40 | 8 | `Dataoffset` | **0x1de00** (encrypted blob starts here) |
| 0x48 | 24 | `Unknown4` | |
| 0x60… | | KDF/keyblob fields (`KdfAlgorithm`, salt, `EncryptedKeyblob1/2`) | only used for password-based decryption |

Decryption algorithm (`decryptDMGWithKeys` in the file above), for block index *i* = 0,1,2,… over the region `[Dataoffset, EOF)` in `Blocksize`-byte chunks:

```
iv[i]  = HMAC-SHA1(hmacKey, BE32(i))[0:16]
plain[i] = AES-128-CBC-Decrypt(key= aesKey, iv= iv[i], ciphertext= chunk i)
```

where the 72-hex-char wiki key is `aesKey(16 B) ‖ hmacKey(20 B)` — `decryptDMGWithKey()` requires exactly 72 hex chars and rejects anything else. The result is **not** a raw image: it is a compressed UDIF (UDZO) DMG whose `koly` footer is the last 512 bytes (`[measured]` superblock found at `len-512` of the decrypted stream, and `7z l` then reports `Type = Dmg … Method = Copy Zero2 ZLIB CRC`, `unpack-size: 1115117568`).

Working commands (no macOS, no root, no loop device needed for the partition extraction):

```bash
ipsw disk dmg --decrypt --key <72hex> -o rootfs.dec 048-2634-005.dmg   # whole decrypted stream (~803 MB)
7z l rootfs.dec                                                        # UDZO DMG, HFSX volume "BrightonMaps10B329.K93OS"
ipsw disk dmg --decrypt --key <72hex> -p 5 -o part5.bin 048-2634-005.dmg   # raw HFSX, 1,115,074,560 B
sudo mount -o loop,ro part5.bin /mnt/ios6x
```

Partition layout `[measured] ipsw disk dmg -p 0..5`: 0 `DDM` (512 B), 1 Apple_Free, 2 `Apple_partition_map` (30,720 B), 3 `Apple_Driver_ATAPI`, 4 Apple_Free, **5 `Apple_HFSX` "disk image" = 1,115,074,560 B** (block size 8192, 136,117 blocks, `freeBlocks 0`, case-sensitive HFS+ signature `HX` v5, `lastMountedVersion HFSJ`).

### 3.5 Recipe B — iOS 6 IMG3 containers on Linux

IMG3 layout `[measured]`: 4-byte magic `33 67 6d 49` (∘ `"Img3"` LE), 3 × u32 LE (`fullSize`, `sizeNoPack`, `sigCheckArea`), then a 4-byte *reversed* identifier (`krnl`,`rdsk`,`ibot`,`illb`,`dtre`,`ibec`,`ibss`,`logo`,`chg0`,`chg1`,`batF`,`bat0`,`bat1`,`glyC`,`glyP`,`recm`) at 0x10; tags start at 0x14, each = `name(4, reversed) ‖ totalSize(u32 LE, incl. 12-byte tag header) ‖ valueLen(u32 LE) ‖ value`, tags padded by ::16 alignment of `totalSize`. `ipsw img3 info <file>` parses iOS 6 IMG3 fine (`ipsw kernel *` does not).

Kernelcache (verified, in full, in Python — no Apple tooling):

1. `ipsw img3 extract -r -o kc_raw.bin kernelcache.release.k93` shows the raw file; the DATA tag value is at file offset 0x40, length 7,741,832 (`valueLen`).
2. AES-256-CBC decrypt with `IV=247ea923c137404f4faec7da75822b08`, `Key=2bbdf3fea41600cd7ae46157af7f19b1726900802f2ec2022aa60b9a27a3261f` (published). `cryptsize` is **not** a multiple of 16 (7,741,832 ≡ 8 mod 16) → `ipsw img3 extract -k` refuses it; drop the trailing 8 bytes yourself.
3. Plaintext starts with `"complzss"`: `magic(8) ‖ u32le 0x186734a3 ‖ u32le 0x0070d500 ‖ u32le 0x08207600 ‖ u32le 0x00000001`, then zeros to **0x180**, then the LZSS stream. (`0x0070d500` = 7,394,560 is plausibly the compressed/decompressed size; exact field semantics `UNVERIFIED:` — decompression does not need them.)
4. LZSS = Apple/iBoot variant, reference implementation `github.com/blacktop/lzss` `Decompress()` (`/home/theo/go/pkg/mod/github.com/blacktop/lzss@v0.1.8/lzss.go`):
   * 4096-byte ring buffer (`n=4096`, `f=18`, `threshold=2`) **pre-filled with 0x20** — early matches legitimately resolve to spaces; do **not** reject back-references beyond the output produced so far;
   * flag byte consumed LSB-first; `bit==1` → next byte is a literal;
   * match token = 2 bytes `b1,b2`: `offset = b1 | ((b2 & 0xF0) << 4)` (12-bit), `len = (b2 & 0x0F) + threshold` with `threshold = 2`, and the copy is `for k = 0..len` → **`len+1` = `(b2 & 0x0F) + 3` bytes** taken from `ring[(offset + k) mod 4096]` (the Go loop is `for k = 0; k <= j; k++`).
5. Result `[measured]`: **13,987,783 bytes**, 32-bit Mach-O `magic 0xfeedface`, `cputype 12`, `cpusubtype 9`, `filetype 2 (MH_EXECUTE)`, `ncmds 15`, `sizeofcmds 0x924`, shipped version string
   `Darwin Kernel Version 13.0.0: Wed Feb 13 21:37:19 PST 2013; root:xnu-2107.7.55.2.2~1/RELEASE_ARM_S5L8940X`
   → **iOS 6.1.3 kernel = XNU 2107.7.55.2.2** (and `libsystem_kernel.dylib`'s `LC_ID_DYLIB current_version` is exactly `2107.7.55`, §5.5).

RestoreRamDisk (verified): `ipsw img3 extract -k <IV><Key> -o rd.bin 048-2516-005.dmg` with `IV=8775b711d2e09e332f8ebfbebe63cce7`, `Key=d406dc4343eedf9d6567e8303ba39a21f81f99bf701840c888963af58a84fb8f` → **9,953,280 B raw HFS+ volume** (not a DMG; volume header `H+` at 0x400). It is a *minimal* system: 158 files, 1.2 MB used, and it contains **no shared cache**; contents `[measured]` include `/bin/{cat,expr,launchctl,ln,mkdir,mv,rm}`, `/sbin/{fsck,fsck_hfs,halt,launchd,mount,mount_hfs,newfs_hfs,reboot}`, `/usr/lib/dyld`, `/usr/lib/libSystem.B.dylib`, `/usr/lib/libobjc.A.dylib`, `/usr/lib/libsqlite3.0.dylib`, `/usr/lib/system/lib{dyld,cache,commonCrypto,compiler_rt,copyfile,corecrypto,dispatch,dnsinfo,keymgr,…}.dylib`, `/System/Library/Frameworks/{CoreFoundation,CoreGraphics,ImageIO,IOKit,Security,SystemConfiguration}.framework/*`, `/System/Library/PrivateFrameworks/{Bom,CoreSurface,DiskImages,IOSurface,MediaKit,MobileKeyBag}.framework/*`, `/System/Library/Filesystems/hfs.fs/hfs.util`, `/System/Library/Extensions/IOHID*/IOUSB*.kext/PlugIns/*`.

*Practical consequence*: the restore ramdisk is a usable, self-contained *static* armv7 userland (it has a real on-disk `dyld` + `libSystem` + `CoreFoundation` and no cache), i.e. the cheapest source of small, non-cache armv7 Mach-O test binaries. It still needs the same IMG3 AES key path.

### 3.6 Caveat: the Linux `hfsplus` driver and HFS-compressed files

`[measured]` files stored with HFS+ compression (decmpfs / `com.apple.decmpfs` attrs) show **size 0** through the Linux hfsplus mount (`stat` → `0 bytes, 0 blocks`) even though the data is present: `7z` on the *decrypted* DMG reports the real logical sizes (e.g. `/usr/lib/dyld` 196,064 B logical vs 106,496 B allocated; `MobileSafari` 1,128,816 vs 524,288). Do not conclude "0-byte stub" from the mount; use `7z` (or a decmpfs-aware HFS+ reader, e.g. blacktop `go-apfs`) to extract. Extracting with `7z x rootfs.dec <member>` produced byte-identical real Mach-Os (`[measured]` `MobileSafari` = 1,128,816 B, `0xfeedface`).

---

## 4. 32-bit Mach-O as the loader sees it

### 4.1 Headers and magics

```c
struct mach_header {          /* 28 bytes, file-relative; MH_MAGIC = 0xfeedface little-endian, bytes "ce fa ed fe" */
  uint32_t magic;             /* 0xfeedface (LE) | 0xcafebabe fat (BE-read) */
  cpu_type_t cputype;         /* 12 = CPU_TYPE_ARM */
  cpu_subtype_t cpusubtype;   /* 9 = ARM_V7, 11 = ARM_V7S (low 24 bits meaningful) */
  uint32_t filetype;          /* 2 MH_EXECUTE, 6 MH_DYLIB, 7 MH_DYLINKER, 8 MH_BUNDLE, 9 MH_DYLIB_STUB, 11 MH_KEXT_BUNDLE */
  uint32_t ncmds;             /* load commands follow at +28 */
  uint32_t sizeofcmds;
  uint32_t flags;             /* MH_TWOLEVEL|MH_DYLDLINK|MH_NOUNDEFS|…; 0x80000000 = MH_DYLIB_IN_CACHE */
};                            /* no reserved field (that is mach_header_64, 32 bytes, magic 0xfeedfacf "cf fa ed fe") */
```
`[measured]` magic bytes `ce fa ed fe` in every extracted binary; `dyld` = `filetype 7`; cache images = `filetype 6` **with `flags & 0x80000000` set** (MH_DYLIB_IN_CACHE) — 569/569.

Fat/universal header (for completeness; **no iOS 6 system binary is fat** `[measured]`: every extracted/cached image is a thin armv7 Mach-O):
```c
struct fat_header { uint32_t magic; /* 0xcafebabe, big-endian on disk */ uint32_t nfat_arch; };
struct fat_arch   { uint32_t cputype, cpusubtype; uint32_t offset, size, align; };  /* BE, 20 bytes */
```
armv7 vs armv7s slices are stored as separate `fat_arch` entries with `cpusubtype` 9 vs 11; `align` is log2 (e.g. 14 = 16 KB). A loader that only needs iPad2,1 can ignore fat entirely, but must not confuse `0xcafebabe` (fat, BE) with `0xbebafeca` (32-bit fat in a *little-endian* Mach-O? — actually `FAT_CIGAM`); `[src] llvm/BinaryFormat/MachO.h` (`FAT_MAGIC`/`FAT_CIGAM`, `mach_header`/`mach_header_64`).

### 4.2 Load commands that matter (32-bit field layouts)

From `llvm/BinaryFormat/MachO.h` (identical to Apple's `<mach-o/loader.h>`); `lc_str` is a 32-bit offset from the *start of the command*.

| cmd | value | 32-bit struct | Fields / observed use |
|---|---|---|---|
| `LC_SEGMENT` | 0x01 | `struct segment_command` (56 B) + `nsects`×`struct section` (68 B) | `segname[16]`, `vmaddr`, `vmsize`, `fileoff`, `filesize`, `maxprot`, `initprot`, `nsects`, `flags`. `section`: `sectname[16]`, `segname[16]`, `addr`, `size`, `offset`, `align`, `reloff`, `nreloc`, `flags`, `reserved1/2` |
| `LC_SYMTAB` | 0x02 | `symoff,nsyms,stroff,strsize` (u32×4) | 32-bit `struct nlist` = 12 B (`n_strx:u32, n_type:u8, n_sect:u8, n_desc:u16, n_value:u32`) |
| `LC_DYSYMTAB` | 0x0b | 18 × u32: `ilocalsym,nlocalsym,iextdefsym,nextdefsym,iundefsym,nundefsym,tocoff,ntoc,modtaboff,nmodtab,extrefsymoff,nextrefsyms,indirectsymoff,nindirectsyms,extreloff,nextrel,locreloff,nlocrel` | `nindirectsyms`/`indirectsymoff` drive `__stub_helper`/`__nl_symbol_ptr` resolution |
| `LC_LOAD_DYLIB` | 0x0c | `dylib_command`: `lc_str name; uint32 timestamp; uint32 current_version; uint32 compatibility_version` (version = `x<<16|y<<8|z`) | `[measured]` counts: MobileSafari 34, MobileMail 40, SpringBoard 79 (+4 `LC_LOAD_WEAK_DYLIB`), cache `UIKit` 37 (+1 `LC_REEXPORT_DYLIB`) |
| `LC_ID_DYLIB` | 0x0d | same struct | `[measured]` `/usr/lib/system/libsystem_kernel.dylib` → `current_version 2107.7.55` |
| `LC_LOAD_WEAK_DYLIB` | 0x18\|REQ = 0x80000018 | `dylib_command` | 23/569 cache images |
| `LC_REEXPORT_DYLIB` | 0x1f\|REQ = 0x8000001f | `dylib_command` | 279/569 cache images |
| `LC_DYLD_INFO`, `LC_DYLD_INFO_ONLY` | 0x22, 0x22\|REQ = 0x80000022 | `dyld_info_command`: `rebase_off,rebase_size,bind_off,bind_size,weak_bind_off,weak_bind_size,lazy_bind_off,lazy_bind_size,export_off,export_size` (u32×10) | `[measured]` MobileSafari: rebase 0x100000/0x2270, bind 0x102270/0x3450, weak 0, lazy 0x1056c0/0x2f74, export 0x108634/0xb0 |
| `LC_MAIN` | 0x28\|REQ = 0x80000028 | `entryoff:u64, stacksize:u64` | `[measured]` all local app executables: MobileSafari `entryoff 0xc861 stacksize 0`; `entryoff` is a file offset into `__TEXT` (relative to `__TEXT.fileoff`, which is 0 for these images → it equals the raw file offset of the entry point). **iOS 6 apps use LC_MAIN; LC_UNIXTHREAD is for the kernel** |
| `LC_UNIXTHREAD` | 0x05 | `flavor, count` + `arm_thread_state32` (17 × u32: `r0..r12, sp, lr, pc, cpsr`) | `[measured]` kernel: `flavor 1, count 17, pc 0x800860c8, sp 0, lr 0` |
| `LC_ENCRYPTION_INFO` | 0x21 | `cryptoff:u32, cryptsize:u32, cryptid:u32` | §6 |
| `LC_VERSION_MIN_IPHONEOS` | 0x25 | `version:u32, sdk:u32` | `[measured]` 569/569 cache images = `0x60100`/`0x60100` (6.1.0) |
| `LC_FUNCTION_STARTS` | 0x26 | `dataoff,datasize` | present in 569/569 cache images and all local app binaries (ULEB128 deltas from `__TEXT` start) |
| `LC_DATA_IN_CODE` | 0x29 | `dataoff,datasize` | absent (0/569) in iOS 6 cache images and local apps — do not require it |
| `LC_CODE_SIGNATURE` | 0x1d | `dataoff,datasize` | present in on-disk apps (`[measured]` MobileSafari `0x111b50/0x1e20`), **absent in all 569 cache images** (signature lives in the cache, §5.6) |
| `LC_SEGMENT_SPLIT_INFO` | 0x1e | `dataoff,datasize` | `[measured]` on-disk `dyld` has it (505 `locrel` entries); absent in cache images |
| `LC_UUID` | 0x1b | `uuid[16]` | everywhere |
| `LC_SOURCE_VERSION` / `LC_SUB_FRAMEWORK` / `LC_SUB_CLIENT` | 0x2a / 0x12 / 0x14 | u64 / `lc_str` | 533/569, 18/569, 1/569 |
| `LC_DYLIB_CODE_SIGN_DRS` | 0x2b | `dataoff,datasize` | present in on-disk apps, absent in cache images |
| `LC_LOAD_DYLINKER` / `LC_ID_DYLINKER` | 0x0e / 0x0f | `lc_str` | `[measured]` executables carry `LC_LOAD_DYLINKER` (`/usr/lib/dyld`); `dyld` itself has `LC_ID_DYLINKER` |

`REQ` above is `LC_REQ_DYLD = 0x80000000`; a loader must mask it when matching (`cmd & ~LC_REQ_DYLD`).

### 4.3 Observed reference objects (`[measured]`)

| Object | arch | Type | Layout |
|---|---|---|---|
| `/Applications/MobileSafari.app/MobileSafari` | 12/9 | MH_EXECUTE, 1,128,816 B | `__PAGEZERO` 0x0/0x1000; `__TEXT` vmaddr 0x1000, vmsize 0xdc000, **fileoff 0**, filesize 0xdc000; `__DATA` 0xdd000/0x24000; `__LINKEDIT` 0x101000/0x15000; 49 commands; `LC_MAIN entryoff 0xc861` |
| `SpringBoard` | 12/9 | MH_EXECUTE, 2,765,344 B | `__PAGEZERO` 0x1000; `__TEXT` 0x1000/0x213000; `__DATA` 0x214000/0x5d000; `__LINKEDIT` 0x271000/0x36000; 98 commands; `LC_MAIN entryoff 0x3189`; 4 × `LC_LOAD_WEAK_DYLIB` |
| `/usr/lib/dyld` (on rootfs) | 12/9 | **MH_DYLINKER**, 196,064 B | `__TEXT` **0x2fe00000**/0x21000; `__DATA` 0x2fe21000; `__LINKEDIT` 0x2fe4b000; 13 commands; `LC_ID_DYLINKER`; `LC_SYMTAB` nsyms 954; version string `@(#)PROGRAM:dyld  PROJECT:dyld-212.3.2` |
| cache image `libsystem_kernel.dylib` | 12/9 | MH_DYLIB + `MH_DYLIB_IN_CACHE` | `__TEXT` 0x394b4000/0x17000; `__DATA` 0x3afb4000; `__LINKEDIT` 0x3b046000 with vmsize=filesize=0x40ce000 (**bogus/placeholder in cache images** — read the cache mappings, not the segment) |
| kernel (decompressed kernelcache) | 12/9 | MH_EXECUTE, 13,987,783 B | `__TEXT` 0x80001000/0x2cf000; `__DATA` 0x802d0000/0x6e000; `__KLD` 0x8033e000/0x2000; `__PRELINK_TEXT` 0x80394000/0x970000; `__PRELINK_INFO` 0x80d04000/0x94000; `__LINKEDIT` 0x80340000/0x53db4; `LC_SYMTAB` nsyms 4072; `LC_UNIXTHREAD` |

---

## 5. The iOS 6 dyld shared cache

### 5.1 File, magic, version

* Path: `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7`, **246,791,518 B** `[measured]`.
  `[src]` `IPHONE_DYLD_SHARED_CACHE_DIR` + `DYLD_SHARED_CACHE_BASE_NAME` + `ARCH_NAME`: `https://github.com/apple-oss-distributions/dyld/blob/dyld-239.3/launch-cache/dyld_cache_format.h` (last lines) and `src/dyld.cpp` (`openSharedCacheFile`, arch names `"armv7"`, `"armv7s"`, `"armv7f"`, `"armv7k"`, `"i386"`, `"x86_64"`).
* Magic `[measured]` first 16 bytes = `d y l d _ v 1` + **3 spaces** + `a r m v 7` + `0x00`, i.e. the C string `"dyld_v1   armv7"` (15 chars) NUL-padded. armv7s would be `"dyld_v1  armv7s"` (2 spaces) `[src]` same file / `dsc_extractor` magic table.
* **The device that built this file was dyld-212.3.2** (`[measured]` rootfs `/usr/lib/dyld` version string), not dyld-239.3. This matters: dyld-239.3's header has `uuid[16]` at 0x58 and `sizeof = 0x68`, while the shipping 6.1.3 cache has `mappingOffset = 0x58` (i.e. an 88-byte header **with no `uuid`**). **Never assume `sizeof(dyld_cache_header)`; always use `mappingOffset`.** dyld-239.3 tag: `…/blob/dyld-239.3/launch-cache/dyld_cache_format.h` (a `src/dyld_cache_format.h` path does not exist at that tag; `dyld-212.3.2` is not published on GitHub — its header layout is inferred from the on-disk file plus the 239.3 struct minus the trailing `uuid`).

### 5.2 `dyld_cache_header` as actually present (all values `[measured]` from the local cache)

| Off | Size | Field (dyld-239.3 name) | Value in this cache |
|---|---|---|---|
| 0x00 | 16 | `magic` | `"dyld_v1   armv7\0"` |
| 0x10 | 4 | `mappingOffset` | **0x58** → header size is 0x58, **no `uuid` field exists** |
| 0x14 | 4 | `mappingCount` | **3** |
| 0x18 | 4 | `imagesOffset` | 0xb8 (= 0x58 + 3×32, exactly as the builder formula implies) |
| 0x1c | 4 | `imagesCount` | **569** |
| 0x20 | 8 | `dyldBaseAddress` | 0x2fe00000 (matches on-disk dyld `__TEXT` vmaddr — cross-check ✓) |
| 0x28 | 8 | `codeSignatureOffset` | **0xea37000** |
| 0x30 | 8 | `codeSignatureSize` | **0** → "runs to end of file" (comment in `dyld_cache_format.h`); here EOF−0xea37000 = 0x124d5e, and the embedded superblob's own length is exactly 0x124d5e ✓ |
| 0x38 | 8 | `slideInfoOffset` | **0xcaa3000** |
| 0x40 | 8 | `slideInfoSize` | **0x63000** |
| 0x48 | 8 | `localSymbolsOffset` | **0xcb06000** |
| 0x50 | 8 | `localSymbolsSize` | **0x1f30fdf** |

**Fields that do NOT exist on iOS 6**: `uuid`, `cacheType`, `branchPoolsOffset/Count`, `accelerateInfoAddr/Size`, `imagesTextOffset/Count`, `sharedRegionStart/Size`, `maxSlide`, `dylibsImageArrayAddr`, `subCacheArrayOffset/Count`, `objcOpts`, `dynamicDataOffset`. First tag where `cacheType`/`branchPools*`/`accelerateInfo*`/`imagesText*` appear = dyld-421.2; `sharedRegionStart`/`maxSlide`/`dylibsTrie*` = dyld-852.2; `subCacheArray*` = dyld-940; `objcOpts`/`dynamicData*`/`cacheAtlas*` = dyld-1042.1. For iOS 6, `sharedRegionStart` must be derived from `mapping[0].address`, not read from the header.

### 5.3 Mappings (`[measured]`), slide info, code signature

```
mapping[0] addr=0x30000000 size=0x09518000 fileOff=0x00000000 maxProt=5 initProt=5   # r-x  (TEXT; contains header, mapping/image arrays, image paths)
mapping[1] addr=0x39518000 size=0x01aa6000 fileOff=0x09518000 maxProt=3 initProt=3   # rw-
mapping[2] addr=0x3afbe000 size=0x01b48000 fileOff=0x0afbe000 maxProt=1 initProt=1   # r--  (LINKEDIT)
```
Note the file layout *after* the mappings: `mapping[2]` ends at file offset 0xcb06000, which is exactly `localSymbolsOffset` — so **both the local-symbols blob and the code signature lie outside every mapping** (unmapped file data), which is why a runtime cache needs only the three mappings but an offline tool must read the file tail directly.
* Shared region base/VAs: 0x30000000..0x3cb06000, i.e. inside the ARM shared region `SHARED_REGION_BASE_ARM = 0x30000000`, `SHARED_REGION_SIZE_ARM = 0x10000000` `[src] xnu osfmk/mach/shared_region.h`; `[measured]` the three mapping addresses.
* **Slide info** = old (v1) format at `slideInfoOffset`, `[measured]`: `version 1`, `toc_offset 0x18`, `toc_count 0x1aa6` (= 6822 = `mapping[1].size / 4096` exactly ✓ — the toc has one `uint16` per 4 KB page of the writable region), `entries_offset 0x3580` (page-aligned after the toc: `0x18 + 0x1aa6*2` = 0x3564 → 0x3580), `entries_count 0xbec`, `entries_size 0x80` (128 = bits per entry). Because there is **no per-segment slide info and no rebase opcodes in the images** (`LC_DYLD_INFO.rebase_off/size = 0` in every cache image), all rebasing of cache images must be done from this toc+entries table; the dylibs as laid out in the cache are unslid.
* **Code signature**: at `codeSignatureOffset` the bytes are `fa de 0c c0` = `CSMAGIC_EMBEDDED_SIGNATURE (0xfade0cc0)` **big-endian**, then BE `length = 1199454` (= 0x124D5E = exactly EOF−offset ✓), BE `count = 3` blob slots. So: a parser must take the signature length from the blob header, because the header's `codeSignatureSize` is 0 for this build.

### 5.4 Image array and what the images contain (`[measured]`, all 569 entries parsed)

* 569 `dyld_cache_image_info` (32 B each: `address,modTime,inode u64; pathFileOffset,pad u32`), paths are NUL-terminated strings at file offset `pathFileOffset` (they live in mapping 0). `[measured]` first: `/System/Library/BulletinBoardPlugins/CMASBBPlugin.bundle/CMASBBPlugin` @0x30007000; last: `/usr/lib/libz.1.2.5.dylib` @0x39386000. **0 of 569 are `/Applications/…`**; the rest are frameworks/PrivateFrameworks/bundles/plugins/`/usr/lib/*` (e.g. `*.bundle/*`, `*.lockbundle/*`, `/System/Library/BulletinBoardPlugins/*`). `/usr/lib/libSystem.dylib`, `/usr/lib/libpthread.dylib`, `/usr/lib/libc.dylib` are duplicate path entries at the **same address as `/usr/lib/libSystem.B.dylib`** (0x38893000) — i.e. cache-internal symlink aliases, and pthread symbols live in libSystem.B on iOS 6 (**there is no `libsystem_pthread.dylib`**; the `libsystem_*` set is `blocks, c, dnssd, info, kernel, m, network, notify, sandbox`).
* Load-command survey over all 569 images:

| Command | images containing it |
|---|---|
| `LC_SEGMENT` | 1703 total command instances (≈3 per image) |
| `LC_SYMTAB`, `LC_DYSYMTAB` | **569/569** |
| `LC_ID_DYLIB` | 569/569 |
| `LC_DYLD_INFO_ONLY` | 569/569 |
| `LC_UUID`, `LC_VERSION_MIN_IPHONEOS`, `LC_FUNCTION_STARTS` | 569/569 |
| `LC_LOAD_DYLIB` 4560 instances; `LC_REEXPORT_DYLIB` 279; `LC_LOAD_WEAK_DYLIB` 23 | |
| `LC_SOURCE_VERSION` 533; `LC_SUB_FRAMEWORK` 18; `LC_SUB_CLIENT` 1 | |
| `LC_ENCRYPTION_INFO`, `LC_CODE_SIGNATURE`, `LC_DATA_IN_CODE`, `LC_SEGMENT_SPLIT_INFO`, `LC_DYLIB_CODE_SIGN_DRS`, `LC_MAIN`, `LC_UNIXTHREAD` | **0** |
| `cpusubtype` / `filetype` | 9 (ARM_V7) × 569 / MH_DYLIB(6) × 569, all with `flags & 0x80000000` |

* So the "cache images are stripped" folklore needs precision for iOS 6: images **do** carry `LC_SYMTAB`/`LC_DYSYMTAB`, but the offsets are **cache-file absolute** and the string pool is **shared by all images**: `[measured]` e.g. `libsystem_kernel` `symoff 0xc1de07e nsyms 862 stroff 0xc42ee24 strsize 0x673558`; `UIKit` `symoff 0xbbde786 nsyms 30955` with the **same** `stroff/strsize`; `libdispatch` `symoff 0xc1cc7f2 nsyms 1219`. `stroff + strsize` (0xca6237c) stays inside mapping 2 (ends 0xcb06000).
* **Per-image `__LINKEDIT` in cache images is not trustworthy**: `[measured]` every image reports `vmaddr 0x3b046000, vmsize 0x40ce000, fileoff 0xb046000, filesize 0x40ce000` — i.e. the whole shared region with a size beyond EOF. Use `LC_DYLD_INFO_ONLY` + `LC_SYMTAB` offsets + the cache mappings instead.

### 5.5 Identities/versions read out of the cache (`[measured]`, `LC_ID_DYLIB current_version`)

| Image | `current_version` | Notes |
|---|---|---|
| `/usr/lib/system/libsystem_kernel.dylib` | **2107.7.55** | = XNU version in the kernelcache string ✓ |
| `/usr/lib/system/libsystem_c.dylib` | 849.29.0 | |
| `/usr/lib/system/libdispatch.dylib` | 244.33.0 | |
| `/System/Library/Frameworks/UIKit.framework/UIKit` | 2380.17.0 | `LC_VERSION_MIN_IPHONEOS 6.1.0` |
| `/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation` | (in cache; see `[measured]` MobileSafari load list: `cur 793.0.0 compat 150.0.0`) | |
| `/System/Library/Frameworks/Foundation.framework/Foundation` | 993.0.0 (per MobileSafari's `LC_LOAD_DYLIB`) | |
| `/usr/lib/libobjc.A.dylib` | 228.0.0 | |

### 5.6 Local symbols blob (per-image local symbols)

`[measured]` at `localSymbolsOffset = 0xcb06000`, per `dyld_cache_local_symbols_info` (24 B, `dyld_cache_format.h`):

| Field | Value | Check |
|---|---|---|
| `entriesOffset` | 0x18 | = `sizeof(info)` ✓ |
| `entriesCount` | 0x207 = **519** (≤ 569 images) | |
| `nlistOffset` | 0x186c | = `0x18 + 519*12` **exactly** ✓ (`dyld_cache_local_symbols_entry` = 12 B) |
| `nlistCount` | 0x92fb2 = **602,034** | |
| `stringsOffset` | 0x6e54c4 | `UNVERIFIED:` this is 0x100 **less** than `nlistOffset + nlistCount*12` (0x6e55c4) — read the fields, do not derive `stringsOffset` |
| `stringsSize` | 0x184bb1b | `stringsOffset+stringsSize` = 0x1f300df, 0x100 short of `localSymbolsSize` (0x1f30fdf) — same 0x100 puzzle |

Per-image lookup: `dyld_cache_local_symbols_entry{ uint32 dylibOffset; uint32 nlistStartIndex; uint32 nlistCount; }`; `dylibOffset` is the **cache-file offset of that image's Mach-O header** = `image.address − mapping[k].address + mapping[k].fileOffset`. Apple's own extractor matches exactly like that (`dsc_extractor.cpp` `entries[i].dylibOffset() == textOffsetInCache`). Rationale `[src] https://github.com/apple-oss-distributions/dyld/blob/main/doc/CacheLayout.md`: on embedded devices the locals are moved to an **unmapped** area (present in the file, absent at runtime); locally-built/macOS caches keep them mapped. Consequence: `LC_SYMTAB` in the cache gives you exports+imports and (for `n_sect()==1`) filename-ish leftovers, but **true function-level local names for a system dylib come from this blob**.

### 5.7 Offline extraction tooling

| Tool | Can do | iOS 6 armv7? |
|---|---|---|
| Apple `dsc_extractor` (`dyld` `launch-cache/dsc_extractor.cpp`, modern `other-tools/dsc_extractor.cpp`) | extract **all** images to a directory, rebuilding standalone Mach-Os (clears `MH_DYLIB_IN_CACHE`, re-lays segment file offsets, merges the unmapped local nlists back in front of exports/imports) | yes; builds on Linux in principle (C++), no blocks needed at that tag; shipped only as Xcode's `dsc_extractor.bundle` for dlopen on macOS |
| Apple `dsc_iterator.h` (`dyld_shared_cache_iterate`) | enumerate images+segments (path, machHeader, uuid, per-segment offsets) | yes (enumeration primitive) |
| `dyld_shared_cache_util` | `-list -info -linkedit -map -slide_info -dependents` (no extraction) | yes |
| blacktop `ipsw` (installed) | `ipsw dyld info <cache>`, `ipsw dyld image <cache>`, `ipsw dyld extract <cache> <dylib>` / `--all`, `a2s/a2o/disass/objc/…`; `ipsw extract --dyld <ipsw>` (needs to mount the DMG in place → root, and fails on the encrypted DMG) | **not verified here** — modern `pkg/dyld` targets the newer header (expects longer headers/subcaches in places). Use the layout in §5.2/§5.6 if it refuses |
| `com/patchers: decache` (phoenix3200) | "nearly perfectly extract dylibs from iOS ≤ 6 cache" | purpose-built for this era |
| `jtool2 -extract`, `DyldExtractor`, `keith/dyld-shared-cache-extractor` | extraction wrappers | mostly iOS 8+ (chained fixups) |
| Custom (~200 lines, what I did) | read header via `mappingOffset`, walk mappings/images, `addr→fileOffset` via mappings, parse Mach-O load commands in place | **works** — all §5.2–§5.6 data came from it |

### 5.8 LLVM 23 status (checked on this host, LLVM 23.1.3)

* **No dyld-cache support in `llvm::object`.** `[measured]`: `ls /usr/include/llvm/Object/` has no `DyldCache.h`; `grep -rl DyldCache /usr/include/llvm*/ /usr/lib/llvm21/include/llvm/` = no matches; up to date upstream (release/23.x and main) also has no `llvm/include/llvm/Object/DyldCache.h` (`UNVERIFIED:` only via raw URL 404s — the local header tree is authoritative for this host).
* `llvm-objdump`'s only dyld-related switch is `--dyld-info` (prints `LC_DYLD_INFO` bind/rebase, requires `--macho`); `llvm-readobj` has nothing for caches. Feeding it a cache file yields `is not an object file` / `The file was not recognized as a valid object file` — the container magic is not a Mach-O magic, so `MachOObjectFile` rejects it.
* The only dyld-cache-ish code in the toolchain is in **LLDB** (`ObjectFileMachO` shared-cache support; `liblldb.so.23.1` contains "was found in a shared cache"), not a reusable library API.
* Practical plan: materialize images yourself (or port `dsc_extractor`), then hand plain Mach-O files to `llvm::object::MachOObjectFile` / `llvm-objdump` / the MC layer for re-emission.

---

## 6. FairPlay / encryption of Mach-O payloads

### 6.1 `LC_ENCRYPTION_INFO` (0x21)

```c
struct encryption_info_command {   /* 32-bit; 16 bytes */
  uint32_t cmd;        /* 0x21 */
  uint32_t cmdsize;    /* 16 */
  uint32_t cryptoff;   /* file offset of the first encrypted byte (usually 0x1000 or the __TEXT fileoff) */
  uint32_t cryptsize;  /* bytes of __TEXT that are encrypted (page multiple, ends inside __TEXT) */
  uint32_t cryptid;    /* 0 = not encrypted (plaintext), 1 = FairPlay-encrypted, 2+ = other */
};
```
(`LC_ENCRYPTION_INFO_64` = 0x2c is the 64-bit variant; irrelevant here.)

Semantics for a recompiler: the bytes in `[cryptoff, cryptoff+cryptsize)` on disk are **not the real code** — they are the FairPlay-encrypted `__TEXT`; a static recompiler would produce garbage from them. `cryptsize` is page-rounded and typically leaves the tail of `__TEXT` (e.g. `__cstring`/`__const` pages that hold no code) in the clear (`UNVERIFIED:` exact extent — no `cryptid=1` sample exists locally); loaders run by the kernel with `cryptid=1` cause the kernel's AppleMobileFileIntegrity/FairPlay path to map the decrypted pages from the device's keys (the classic "decrypt on page fault into the shared text"), which is exactly what a dumper harvests.

### 6.2 Which local artifacts are encrypted (`[measured]`)

| Artifact | Encrypted? | Evidence |
|---|---|---|
| `048-2634-005.dmg` (rootfs) | **yes**, container-level: `encrcdsa` v2 (AES-128-CBC + HMAC-SHA1-derived IVs) | `head -c 8` = `encrcdsa`; §3.4; keys published → offline decryption works |
| `kernelcache.release.k93` | **yes**, IMG3 DATA = AES-256-CBC | KBAG tags present; decrypted successfully with published IV/key |
| `048-2516/048-2679-005.dmg` (ramdisks) | **yes** (IMG3 `rdsk`, KBAG) | decrypted successfully with published IV/key |
| iBoot/iBEC/iBSS/LLB/DeviceTree/logos | **yes** (IMG3, KBAG+CERT) | `[measured]` all are `3gmI` with `TYPE,DATA`(+KBAG/CERT) |
| System apps on the decrypted rootfs (`MobileSafari`, `SpringBoard`, `MobileMail`, `dyld`) | **no FairPlay encryption at all** — the command is *absent* | `[measured]` Mach-O load-command dumps show no `LC_ENCRYPTION_INFO` (0x21) in any of them |
| System dylibs in the shared cache (569 images) | **no** — `LC_ENCRYPTION_INFO` absent in 569/569 | §5.4 |
| **App Store / third-party binaries** | **yes** (`cryptid=1`) — but **none are present in these artifacts** (a restore IPSW contains only Apple content). Any `.ipa`/App Store Mach-O is a separate acquisition, and its `__TEXT` is device-bound | — |

### 6.3 What decryption actually requires

* FairPlay `cryptid=1` bytes are decrypted **by the device**, per page, using the device's UID key + the per-app keys handed to the kernel by the App Store stack. Offline there is no key material in the IPSW, no key in the app bundle, and no server-side key derivation available.
* Consequently: obtaining plaintext App Store code requires **(a)** a jailbroken armv7 device with the app installed and a dump tool (classic `dumpdecrypted.dylib` + `DYLD_INSERT_LIBRARIES`, or a kernel-assisted dumper using the `cryptid`-clearing trick), then **(b)** copying the dumped file off the device; or obtaining a pre-decrypted copy from elsewhere.
* What a decrypted binary looks like: identical size and layout, but `LC_ENCRYPTION_INFO.cryptid` is set to **0** and `[cryptoff, cryptoff+cryptsize)` now contains real armv7 code. `codesign`-style signatures remain attached (`LC_CODE_SIGNATURE`) but become stale/invalid; the loader must ignore the signature when recompiling.
* Dumper precedent: the well-known trick is to `mmap`/read the target's `__TEXT` from another process after pointing at the kernel's decrypted pages, or to `ptrace`+`vm_read` them; tools commonly flip `cryptid` in the header after dumping so that static tools see plaintext.

### 6.4 Why static recompilation of an App Store binary is impossible without this step

For a `cryptid=1` image, `[cryptoff, cryptoff+cryptsize)` is ciphertext whose plaintext is only reachable with device-held keys. There is no mathematical shortcut: the ciphertext is the same on every copy of that app, and the key is per-device-app pair; therefore *any* pipeline step that disassembles or lifts `__TEXT` must run after a device-assisted dump. (Apple's own dyld never sees the encrypted bytes; the kernel decrypts on fault.)

---

## 7. User address-space layout on iOS 6 (armv7)

| Region | Range | Evidence |
|---|---|---|
| Main executable `__PAGEZERO` | 0x00000000 – 0x00001000 (**4 KB**) | `[measured]` MobileSafari/SpringBoard/MobileMail `LC_SEGMENT __PAGEZERO vmsize 0x1000, maxprot 0` |
| Main executable `__TEXT` | vmaddr **0x1000** (`fileoff 0`, `vmsize` = `filesize`) | `[measured]` `__TEXT vmaddr 0x1000` in all three local apps |
| `dyld` | **0x2fe00000** (`__TEXT` 0x2fe00000/0x21000; `__DATA` 0x2fe21000; `__LINKEDIT` 0x2fe4b000) | `[measured]` on-disk `/usr/lib/dyld`; also `dyld_cache_header.dyldBaseAddress = 0x2fe00000` |
| Shared cache ("shared region") | **0x30000000 – 0x3cb06000** in practice; region reserved 0x30000000 size 0x10000000 | `[measured]` mapping[0..2] addresses; `[src] xnu osfmk/mach/shared_region.h` (`SHARED_REGION_BASE_ARM 0x30000000`, `SHARED_REGION_SIZE_ARM 0x10000000`) |
| Main-thread stack | grows down from just **below 0x30000000** | `UNVERIFIED:` exact base; the placement below the shared region is the layout documented for 32-bit iOS (crash-log stacks land at ~0x2ff…/0x2f…), and `mapping[0]` starts at 0x30000000 with dyld occupying 0x2fe00000–0x2fe4b000. Default main-thread stack size (usually quoted as 1 MB, secondary threads 512 KB) `UNVERIFIED:` |
| Kernel | **0x80000000 – 0xFFFFFFFF**: kernel text 0x80001000+, kexts prelinked from 0x80394000, exception vectors at 0xFFFF0000 | `[measured]` decompressed kernelcache segments (`__TEXT 0x80001000`, `__PRELINK_TEXT 0x80394000`, `__PRELINK_INFO 0x80d04000`); `[src] osfmk/arm/arm_vm_init.c` ("Special case write protection for the mapping of ExceptionVectorsBase (EVB) at 0xFFFF0000"), `osfmk/arm/locore.s`'
| Page size | 4 KB for user mappings; HFS volume block 8192 B | `[measured]` `__PAGEZERO` 0x1000, `__TEXT`/segment sizes 4 KB multiples, slide-info `toc_count = __DATA size/4096` ✓; HFSX volume header `blockSize 8192` |

Version-specific notes: the shared-region base (0x30000000) and dyld base (0x2fe00000) are the same for armv7 iOS 5 and 6; iOS 7 moved dyld down (0x1fe00000-era) and iOS 8 changed the cache layout. Do not carry these constants past iOS 6 without re-measuring.

---

## 8. Syscall / trap entry conventions on 32-bit ARM Darwin (iOS 6.1)

### 8.1 Trap instruction and number register

* Instruction: **`svc #0x80`** — `SWI_SYSCALL = 0x80` `[src] <mach/arm/vm_param.h>` (iOS 6.1 SDK) = `osfmk/mach/arm/vm_param.h`; userspace stubs are generated by `osfmk/mach/arm/syscall_sw.h` (`swi #SWI_SYSCALL`).
  `[measured]` the device's own `libsystem_kernel.dylib` (`__TEXT` from the cache, file offset 0x94b4000, size 0x17000) contains **389** occurrences of the ARM-mode encoding `svc #0x80` (`80 00 00 ef`) and 0 Thumb `svc #0x80` (`80 df`) — matching the 389 found independently in the iPhoneOS 6.1 SDK slice.
* **Syscall number in `r12`** (`ip`), arguments in `r0–r6` **and `r8`** (r7 is skipped: it is the frame pointer) `[src] osfmk/arm/locore.s` `fleh_swi` ("System call number is stored in r12. System call arguments are stored in r0 to r6 and r8 (we skip r7)").
* The kernel never validates the immediate; `svc #0x80` is an ABI convention, not a requirement (`fleh_swi` does not read the instruction word; it only panics on a kernel-mode SWI) `[src] osfmk/arm/locore.s`.

### 8.2 Classes are encoded in the *sign* of r12 (no x86-style class byte)

| r12 | Meaning |
|---|---|
| `0` | **indirect syscall**: number in **r0**, args from r1; `[measured]` `_syscall`/`___syscall` (same address) in the device binary: `mov ip, sp; push {r4,r5,r6,r8}; ldm ip,{r4,r5,r6}; mov ip,#0; svc #0x80` |
| positive (1..~443) | **BSD/unix syscall**, index into `sysent[]`; handler `unix_syscall(state, thread, uthread, proc)` |
| negative (other than specials) | **Mach trap**, index = `-r12` into `mach_trap_table[]` (`MACH_TRAP_TABLE_COUNT = 128`) — no `mach_call_munger` on arm32; optional `mach_trap_arg_munge32` runs, then `bx` into the trap |
| `-3` | `mach_absolute_time` fast path (stackless, `ml_get_timebase`) — **not** a table slot |
| `0x80000000` | machine-dependent group, selector in **r3** (0 icache_invalidate, 1 dcache_flush, 2/3 thread_set/get_cthread) `[measured]` `_sys_icache_invalidate`/`_sys_dcache_flush` set `mov r3,#0/1; mov r12,#-2147483648` |

### 8.3 Numbers verified against the device binary (`[measured]`)

Mach traps (r12 = −index; the stub's `mvn ip,#N` yields −(N+1)):

| Trap | index | stub in device libsystem_kernel | instruction |
|---|---|---|---|
| `mach_reply_port` | 26 | 0x394b4e70 | `mvn ip,#0x19; svc #0x80` |
| `thread_self_trap` | 27 | 0x394b4e7c | `mvn ip,#0x1a` |
| `task_self_trap` | 28 | 0x394b4e88 | `mvn ip,#0x1b` |
| `host_self_trap` | 29 | 0x394b4e94 | `mvn ip,#0x1c` |
| **`mach_msg_trap`** | **31** | 0x394b4ea0 | `mov ip,sp; push {r4,r5,r6,r8}; ldm ip,{r4,r5,r6}; mvn ip,#0x1e; svc #0x80` |
| **`mach_msg_overwrite_trap`** | **32** | 0x394b4ebc | `mvn ip,#0x1f`, 9 args (r4,r5,r6,r8 loaded) |
| `semaphore_signal_trap` | 33 | 0x394b4ed8 | `mvn ip,#0x20` |
| **`semaphore_wait_trap`** | **36** | 0x394b4efc | `mvn ip,#0x23` |
| `semaphore_timedwait_trap` | 38 | 0x394b4f14 | `mvn ip,#0x25` |
| `swtch_pri` | 59 | 0x394b4f9c | `mvn ip,#0x3a` |
| `swtch` | 60 | 0x394b4fa8 | `mvn ip,#0x3b` |
| **`thread_switch`** | **61** | `_syscall_thread_switch` @0x394b4fb4 | `mvn ip,#0x3c; svc #0x80` (the exported `_thread_switch` @0x394b7a38 is a Thumb shim that branches here) |
| `clock_sleep_trap` | 62 | `_clock_sleep_trap` @0x394b4fcc | `mvn ip,#0x3d; svc #0x80` |
| `mach_timebase_info` | 89 | `_mach_timebase_info` @0x394b4fdc | `mvn ip,#0x58` |
| `mach_wait_until` | 90 | `_mach_wait_until` @0x394b4fe8 | `mvn ip,#0x59` |
| `mk_timer_create/destroy/arm/cancel` | 91/92/93/94 | `_mk_timer_*` @0x394b4ff4… | `mvn ip,#0x5a…#0x5d` |
| **`mach_absolute_time`** | **not a table slot** (userspace r12 = **−3**) | in `libsystem_c` @**0x3940e240**, not `libsystem_kernel` (`[src]` iOS 6.1 SDK; device byte check `mvn ip,#2; svc #0x80; bx lr`) | Corroboration: no user-readable hardware counter is used anywhere in the 569 cache images (no `CNTVCT`/`CNTPCT`/`CNTFRQ` instruction encodings — peer cross-check), so on A5 time queries always go through the `−3` trap and the kernel's timebase (`ml_get_timebase` → `mach_timebase_info` trap 89 for conversion) |
| `_kernelrpc_mach_vm_allocate/deallocate/protect_trap`, `_kernelrpc_mach_port_allocate/destroy/deallocate/mod_refs/move_member/insert_right/insert_member/extract_member_trap` | 10, 12, 14, 16, 17, 18, 19, 20, 21, 22, 23 | @0x394b4dc8…0x394b4e68 | verified stubs, e.g. `_kernelrpc_mach_vm_allocate_trap` = `mvn ip,#9` |
| `task_name_for_pid`, `task_for_pid`, `pid_for_task` | 44, 45, 46 | @0x394b4f30… | |
| `macx_swapon/swapoff/triggers/backing_store_suspend/backing_store_recovery` | 48, 49, 51, 52, 53 | @0x394b4f60… | |

**Negative-r12 census over the device `libsystem_kernel` (`[measured]`: 389 `svc #0x80` sites byte-scanned, each attributed to the `mvn/mov ip,#imm` immediately preceding it and to the nearest preceding symbol):** the complete set of mach-trap indexes used in userspace is
`{10, 12, 14, 16–23, 26–29, 31–39, 44–46, 48, 49, 51–53, 59–62, 89–94}`.
Indexes `1–9, 11, 13, 15, 24, 25, 30, 40–43, 47, 50, 54–58, 63–88, 95+` have **no** userspace stub, which is exactly the shape expected from `mach_trap_table`'s reserved/`kern_invalid` holes; `r12 = −3` likewise has no `libsystem_kernel` stub (it lives in `libsystem_c` and is special-cased before the table lookup). Source check of the holes (`[src] https://github.com/apple-oss-distributions/xnu/blob/xnu-2050.18.24/osfmk/kern/syscall_sw.c`, `mach_trap_table[]`): lines 149–156 give `44 task_name_for_pid, 45 task_for_pid, 46 pid_for_task, 47 kern_invalid, 48 macx_swapon, 49 macx_swapoff, 50 kern_invalid, 51 macx_triggers` (52/53 = `macx_backing_store_suspend/recovery`), and lines 195–200 give `89 mach_timebase_info_trap, 90 mach_wait_until_trap, 91 mk_timer_create_trap, 92 mk_timer_destroy_trap, 93 mk_timer_arm_trap, 94 mk_timer_cancel_trap` — i.e. exactly the present/absent pattern the census found (88 is also `kern_invalid`). Identified by symbol: `-10 __kernelrpc_mach_vm_allocate_trap`, `-12 …_vm_deallocate_trap`, `-14 …_vm_protect_trap`, `-16 …_port_allocate_trap`, `-17 …_port_destroy_trap`, `-18 …_port_deallocate_trap`, `-19 …_port_mod_refs_trap`, `-20 …_port_move_member_trap`, `-21 …_port_insert_right_trap`, `-22 …_port_insert_member_trap`, `-23 …_port_extract_member_trap`, `-26 _mach_reply_port`, `-27 _thread_self_trap`, `-28 _task_self_trap`, `-29 _host_self_trap`, `-31 _mach_msg_trap`, `-32 _mach_msg_overwrite_trap`, `-33..-39 _semaphore_*`, `-44 _task_name_for_pid`, `-45 _task_for_pid`, `-46 _pid_for_task`, `-48..-53 _macx_*`, `-59 _swtch_pri`, `-60 _swtch`, `-61 _syscall_thread_switch`, `-62 _clock_sleep_trap`, `-89 _mach_timebase_info`, `-90 _mach_wait_until`, `-91..-94 _mk_timer_*`.

BSD syscalls (device stubs, `[measured]`; all match `bsd/kern/syscalls.master` order). Where a row cites `sys/syscall.h` instead of an instruction, the device stub is a lazy-pointer shim whose `svc` body lives elsewhere (`_ptrace`, `_mmap`) or it is not in the exported symbol table (`___thread_selfid`); every other row above was read straight out of the device stub body:

| Name | # | device stub |
|---|---|---|
| `syscall` (indirect) | 0 | `_syscall` 0x394c5bc4 `mov ip,#0` |
| `exit` / `fork` | 1 / 2 | (not present as stubs in this image) |
| `read` | 3 | `_read` 0x394b6568 `mov ip,#3; svc #0x80; blo …` |
| `write` | 4 | `_write` 0x394b67a0 `mov ip,#4` |
| `open` | 5 | `_open` 0x394c4de8 `mov ip,#5` |
| `getpid` | 20 | `_getpid` 0x394c45f4 `mov ip,#0x14` |
| `ptrace` | 26 | `_ptrace` 0x394c53b8 (indirect/lazy stub) |
| `mmap` | 197 | `_mmap` 0x394b5ee8 |
| `sysctl` (`__sysctl`) | 202 | `___sysctl` 0x394c5bf8 `mov ip,#0xca` |
| `issetugid` | 327 | `_issetugid` 0x394b63b4 `mov ip,#0x100; orr ip,ip,#0x47; svc #0x80` (= **0x147**) |
| `__semwait_signal` | 334 (0x14e) | `___semwait_signal` 0x394c568c `mov ip,#0x100; orr ip,ip,#0x4e` |
| `__semwait_signal_nocancel` | 423 (0x1a7) | `___semwait_signal_nocancel` 0x394c56c4 `mov ip,#0x100; orr ip,ip,#0xa7` |
| `read_nocancel` / `write_nocancel` | 396 / 397 | `_read$NOCANCEL` 0x394c5434 `mov ip,#0x18c` / `_write$NOCANCEL` 0x394c5ddc `mov ip,#0x100; orr ip,ip,#0x8d` |
| `thread_selfid` | **372** (BSD, returns 64-bit tid in r0:r1) | `___thread_selfid` `[src]` iOS 6.1 SDK `sys/syscall.h`; cross-checked in the same cache by peer agent `Armv7Abi.MachSync` |
| `sigaction` | 46 | `[src] iOS 6.1 SDK sys/syscall.h` (census sees `___sigaction` at r12=46) |

There are no "3800-range" kernelrpc syscalls on iOS 6; `_kernelrpc_mach_vm_*`/`_kernelrpc_mach_port_*` are **Mach traps 10–25**.

### 8.4 Return convention and wrapper entry points

* **Error = carry flag set + errno in r0.** `[measured]` every BSD stub: `svc #0x80; blo <success: bx lr>` and a fall-through lazy tail call to **`cerror`** on C=1 (`cerror` and `cerror_nocancel` are *inside libsystem_kernel*). Kernel side: `arm_prepare_u32_syscall_return()` sets `PSR_CF` and r0=errno on error and clears the flag on success `[src] bsd/dev/arm/systemcalls.c` ("ARM runtime will call cerror if the carry bit is set after a system call"). Mach traps have no carry protocol: `kern_return_t` in r0.
  Symmetric for re-entry emulation: `ERESTART` rewinds `pc` by 4 to re-execute the `svc`.
* Entry points: `Lswi_vector` → `fleh_swi` (exception vector table at 0xFFFF0000); BSD path `fleh_swi_unix` → `bl unix_syscall`; Mach path `fleh_swi_mach` → `mach_trap_table[-r12]`; return via `thread_syscall_return`/`fleh_swi_exit` → `thread_exception_return` `[src] osfmk/arm/locore.s`, `bsd/dev/arm/systemcalls.c`.
* Kernel-side sources: XNU 2107.7.55 (the version this device runs) is **not published**; the arm32 files referenced above are from the closest open-source arm32 tree (`xnu-4570.71.2`, `osfmk/arm/locore.s`, `bsd/dev/arm/systemcalls.c`) and the machine-independent tables from `xnu-2050.48.11` (`osfmk/kern/syscall_sw.c`, `bsd/kern/syscalls.master`). `[measured]` every user-visible number in §8.3 was independently confirmed in the device or iPhoneOS 6.1 SDK binaries.

---

## 9. Top 10 "verify before relying on this"

1. **dyld cache header length**: this cache is an 88-byte header (`mappingOffset = 0x58`) with **no `uuid`**; dyld-239.3's published header is 0x68 with `uuid` at 0x58. Parse via `mappingOffset`/`imagesOffset`, never `sizeof(struct)`.
2. **`codeSignatureSize == 0`** in this cache: it means "to EOF" (`codeSignatureOffset = 0xea37000`); take the real length from the embedded `0xfade0cc0` superblob's BE length field.
3. **Local symbols**: the mapped `LC_SYMTAB` of cache images shares one string pool (0x673558 bytes) and cache-absolute offsets; per-image local function names come from the unmapped `localSymbols` blob. Re-derive `stringsOffset` never; read it. And check the 0x100-byte discrepancy (`UNVERIFIED:`).
4. **HFS+ compression / Linux driver**: mounted files can read as 0 bytes/size 0 while real data exists (`/usr/lib/dyld` 196,064 B; `MobileSafari` 1,128,816 B). Validate any extraction with `7z l` sizes, not `stat`.
5. **No on-disk copies of cached dylibs**: `/usr/lib/libSystem.B.dylib`, `/System/Library/Frameworks/*/CoreFoundation` etc. do not exist on the rootfs — build the loader's dylib search around the cache image list (569 paths), not the VFS listing.
6. **armv7s scope**: only iPhone 5 and iPad 4 in iOS 6. iPad mini 1/iPad 2/iPad 3 are A5/A5X armv7; and armv7s differs from armv7 only by IDIV + VFPv4 FMA + FP16 convert + SIMDv2.
7. **`LC_MAIN` vs `LC_UNIXTHREAD`**: iOS 6 *apps* use `LC_MAIN` (`entryoff` = file offset relative to `__TEXT.fileoff`, `stacksize 0` here); `LC_UNIXTHREAD` is the kernel's (`pc 0x800860c8`). Do not implement a UnixThread-only entry path.
8. **`__PAGEZERO` = 4 KB** (not the macOS 4 GB): a loader that hard-codes a big page zero will mis-map `__TEXT` at 0x1000.
9. **Cache images' `__LINKEDIT` segment sizes are placeholders** (0x40ce000 for every image, exceeding EOF) and rebase info is *not* in the images (`LC_DYLD_INFO.rebase_off/size = 0`) — slid pointers must be reconstructed from the cache slide-info toc/entries (v1 format) or by using the cache's own unslid VAs.
10. **FairPlay**: no local artifact contains a `cryptid=1` payload, and the lynchpin of any App-Store-binary path (device UID-keyed decryption) is not offline-obtainable. Treat "run a real third-party iOS 6 app" as device-gated. Also `UNVERIFIED:` main-thread stack base, Swift/LPAE, `SCTLR.A` (unaligned policy in this kernel), and the `complzss` header field semantics.

---

## Appendix A — acquisition answers (condensed)

* **Is the rootfs key for iPad2,1 6.1.3 public?** Yes: `70f5ac054bf50a522fd39071f6acbd92954804599c1507b881d7d9c4026005e3867bfab0` from `https://theapplewiki.com/wiki/Keys:BrightonMaps_10B329_(iPad2,1)`, a **72-hex** string = AES-128 key (first 32 hex) ‖ HMAC-SHA1 key (last 40 hex); algorithm in §3.4; verified by decrypting + mounting the volume. The key is *not* UID-derived (it is a static per-build key that the community extracted), so no device is needed.
* **Other armv7 iOS 6 device/build pairs with public keys** (same `/wiki/Keys:<Codename>_<Build>_(<Device>)` scheme, all armv7 unless noted): `iPad2,1` 6.1.3/10B329; `iPad3,1` 6.1.3/10B329 (A5X); `iPhone3,1` 6.1.3/10B329 (A4); `iPhone4,1` 6.1.3/10B329 (A5); `iPod5,1` 6.1.3/10B329 (A5); `iPhone2,1` 6.1.6/10B500 (A4, newest iOS 6 build); `iPod4,1` 6.1.5/10B400 and 6.1.6/10B500. armv7s substitutes if ever needed: `iPhone5,1` 6.0/10A405, `iPad3,4` 6.0/10A407. Third-party verification status of the wiki values is per-page (community-extracted); the iPad2,1 6.1.3 rootfs key and all iPad2,1 IMG3 IVs I tested worked.
* **Restore ramdisk**: encrypted inside IMG3, key public (see §3.5); contents = minimal toolset + framework stubs, **no shared cache**.
* **Kernelcache unwrap**: see §3.5 (IMG3 → AES-256-CBC → `complzss` at data offset 0x180 → LZSS → 13,987,783-byte Mach-O kernel). `ipsw kernel dec/kexts/syscall/mach` cannot parse iOS 6 IMG3; `img3tool`/`xpwntool`/`redsn0w` are macOS-era and not needed — ~60 lines of Python (or `blacktop/lzss` + any AES library) suffice, as demonstrated.
* **Are the kernel's Mach/BSD tables extractable offline?** Yes in principle (the decompressed kernel exports 4072 symbols and carries `LC_SYMTAB`/`LC_DYSYMTAB`), but the tables themselves are **not** exported symbols: `[measured]` grepping the 4072 exports for `mach_trap_table`, `sysent`, `nsysent`, `unix_syscall`, `kern_invalid`, `mach_call_munger` finds none, and a structural scan of `__DATA` for the "run of identical `kern_invalid` pointers" pattern found only an unrelated 33-entry run at 0x802d2434. A custom parser would have to (a) locate the tables by code references from `fleh_swi`/`unix_syscall` in `__TEXT`, or (b) recover them from `__PRELINK_INFO`. **Practical alternative used here:** read the trap numbers from userspace stubs (§8.3) and the BSD numbers from the SDK's `sys/syscall.h` — they are authoritative and were cross-checked against the device.
