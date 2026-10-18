# LLVM 23 tooling reference for `ios6-recomp`

Reference for (a) the lifter (ARMv7/Thumb-2 guest code -> host x86-64 code) and (b) the ORC/JITLink
runtime. Everything marked **VERIFIED** was checked on this machine against the installed LLVM
23.1.3 / Capstone 5.0.9 / QEMU 11.1.2 and the exact command is given. Anything else is marked
`UNVERIFIED:`.

Environment probed for this document:

```
lsb/OS:      Arch Linux x86-64 (linux 7.2.10-arch1-1)
llvm:        23.1.3-2 (llvm), 23.1.3-2 (llvm-libs), 23.1.3-2 (clang), /usr/bin/llvm-config
capstone:    5.0.9-1 (lib + cstool), python-capstone 5.0.9-1.1 (module reports 5.0.7)
qemu:        qemu-full 11.1.2-1 (/usr/bin/qemu-arm, qemu-arm-static)
cmake:       4.4.4 (default linker on this box: mold)
```

---

## 1. The local LLVM 23.1.3 install

### 1.1 `llvm-config` output (VERIFIED, verbatim)

```
$ llvm-config --includedir --libdir --version --has-rtti --cxxflags --bindir --prefix --shared-mode
/usr/include
/usr/lib
23.1.3
YES
-I/usr/include -std=c++17  -D_GNU_SOURCE -D_GLIBCXX_USE_CXX11_ABI=1 -D__STDC_CONSTANT_MACROS \
  -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -fno-exceptions
/usr/bin
/usr
shared
```

* Headers: `/usr/include/llvm/**` and `/usr/include/llvm-c/**` (a full, complete include tree —
  there is no `libLLVM`-only "dev headers missing" situation on this box).
* Libraries: shared `libLLVM.so -> libLLVM.so.23.1` (172,657,792 bytes) in `/usr/lib`.
  `--shared-mode` = `shared`. `nm -D` shows 1102 exported `_ZN4llvm3orc*` symbols, i.e. the whole
  ORC/JITLink stack is inside the dylib (VERIFIED).
* `/usr/lib/llvm21/` also exists on this machine but it is leftovers from `clang21`/`lld21`
  packages (`pacman -Qo /usr/lib/llvm21` -> clang21, compiler-rt21, lld21); it contains **no**
  `bin/llvm-config` and no LLVM headers (`/usr/lib/llvm21/include` has only `clang`, `clang-c`,
  `clang-tidy`, `lld`). Not a usable LLVM 23 install; ignore it.

### 1.2 Build configuration (VERIFIED from `/usr/lib/cmake/llvm/LLVMConfig.cmake`)

| Variable | Value | Consequence |
|---|---|---|
| `LLVM_VERSION_MAJOR/MINOR/PATCH` | 23 / 1 / 3 | — |
| `LLVM_ENABLE_RTTI` | `ON` | consumers must **not** compile with `-fno-rtti` |
| `LLVM_ENABLE_EH` | `OFF` | LLVM never throws; exceptions must not cross LLVM frames |
| `LLVM_ENABLE_ASSERTIONS` | `OFF` | bad `MCInst` operand indices etc. are UB, not aborts |
| `LLVM_ENABLE_THREADS` | `ON` | `-lpthread` needed by consumers using ORC threading |
| `LLVM_LINK_LLVM_DYLIB` | `ON` | link the `LLVM` dylib target, not per-component archives |
| `LLVM_TARGETS_TO_BUILD` | `AArch64;AMDGPU;ARM;AVR;BPF;Hexagon;Lanai;LoongArch;Mips;MSP430;NVPTX;PowerPC;RISCV;Sparc;SPIRV;SystemZ;VE;WebAssembly;X86;XCore` | `arm`, `armeb`, `thumb`, `thumbeb`, `x86`, `x86-64` all present |
| `LLVM_DEFAULT_TARGET_TRIPLE` / `LLVM_HOST_TRIPLE` | `x86_64-pc-linux-gnu` | JIT target = host x86-64 ELF |
| `LLVM_INCLUDE_DIRS` / `LLVM_LIBRARY_DIRS` | `/usr/include` / `/usr/lib` | — |
| `LLVM_DEFINITIONS` (line 371) | `-D_GNU_SOURCE -D_GLIBCXX_USE_CXX11_ABI=1 -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS` | apply via `target_compile_definitions` |

C++ standard: LLVM 23 headers are consumed with **C++17** here (that is what `llvm-config
--cxxflags` emits, and what the probe in §1.6 compiles with). `LLVMConfig.cmake` does **not** set
`CMAKE_CXX_STANDARD`/`CXX_STANDARD` for consumers (`grep -n "CXX_STANDARD" /usr/lib/cmake/llvm/*.cmake`
finds nothing) — set it yourself. Compatibility is `major.minor`-exact, see §1.5.

RTTI/EH policy for our code: build with `-frtti` (the default) and do not let exceptions escape
into LLVM calls (`LLVM_ENABLE_EH=OFF` means LLVM's own code is not exception-safe; a throw through
it is UB). We may use exceptions inside our own code paths. Since LLVM is a shared dylib with
`RTTI=ON`, `dynamic_cast`/`typeid` across the boundary is fine (`RTTIExtends<...>` in ORC relies
on it).

### 1.3 ORC / JITLink headers actually present (VERIFIED)

`/usr/include/llvm/ExecutionEngine/Orc/` contains 73 entries. Present and relevant to us:

```
AbsoluteSymbols.h            BacktraceTools.h           COFF.h / COFFPlatform.h
CallSPSViaEPC.h              CallViaEPC.h               CompileOnDemandLayer.h
CompileUtils.h               Core.h                     CoreContainers.h
DylibManager.h               EPCDynamicLibrarySearchGenerator.h
EPCGenericDylibManager.h     EPCGenericJITLinkMemoryManager.h   EPCGenericMemoryAccess.h
EPCGenericRTDyldMemoryManager.h   EPCIndirectionUtils.h
ExecutionUtils.h             ExecutorProcessControl.h    ExecutorResolutionGenerator.h
IRCompileLayer.h             IRPartitionLayer.h          IRTransformLayer.h
InProcessEPC.h               IndirectionUtils.h          JITLinkRedirectableSymbolManager.h
JITLinkReentryTrampolines.h  JITTargetMachineBuilder.h   LLJIT.h
Layer.h                      LazyObjectLinkingLayer.h    LazyReexports.h
LinkGraphLayer.h             LinkGraphLinkingLayer.h     LoadLinkableFile.h
LookupAndRecordAddrs.h       MachO.h   MachOBuilder.h    MachOPlatform.h
Mangling.h                   MapperJITLinkMemoryManager.h  MaterializationUnit.h
MemoryAccess.h               MemoryMapper.h              ObjectFileInterface.h
ObjectLinkingLayer.h         ObjectTransformLayer.h      OrcABISupport.h
RTDyldObjectLinkingLayer.h   ReOptimizeLayer.h           RedirectionManager.h
SectCreate.h                 SelfExecutorProcessControl.h  Shared/  SimpleRemoteEPC.h
SimpleRemoteMemoryMapper.h   SpeculateAnalyses.h         Speculation.h
SymbolLookupSet.h            SymbolStringPool.h          TargetProcess/   TaskDispatch.h
ThreadSafeModule.h           UnwindInfoRegistrationPlugin.h  WaitingOnGraph.h
WaitingOnGraphOpReplay.h
```

Names that do **not** exist (grep of the whole `/usr/include/llvm` tree returns nothing):

* `llvm/ExecutionEngine/Orc/EPCDefaultMemoryMappings.h` — and no symbol by that name exists in
  `libLLVM.so.23.1`. See §2.9.
* `llvm/ExecutionEngine/Orc/EPCDefaultMemoryMappings` class, `LegacyDynamicLibrarySearchGenerator`,
  `orc::Legacy*`, `llvm/ExecutionEngine/Orc/Legacy.h`, `OrcMCJITReplacement.h`, `TargetProcessControl.h`.
* The string `Legacy` does not occur **at all** in `ExecutionEngine/Orc/**` (VERIFIED:
  `grep -rn Legacy /usr/include/llvm/ExecutionEngine/Orc/` -> 0 hits).

JITLink backends present (`/usr/include/llvm/ExecutionEngine/JITLink/`): `ELF.h`, `ELF_aarch32.h`,
`ELF_aarch64.h`, `ELF_x86_64.h`, `MachO.h`, `MachO_arm64.h`, `MachO_x86_64.h`, `COFF*`, `aarch32.h`,
`x86_64.h`, `aarch64.h`, … **There is no ARMv7 Mach-O JITLink backend** (only `MachO_arm64` and
`MachO_x86_64`). Irrelevant for us as long as the JIT target is the host (x86-64 ELF), but it means
JITLink can never be asked to link guest-form Mach-O/ARM objects — do not design for that.

ARM target headers are **not** installed: `/usr/include/llvm/Target/ARM/` does not exist and no
`*.td` files for ARM are shipped (only `llvm/Target/{Target.td,GenericOpcodes.td,...}`,
`llvm/CodeGen/*.td`, `llvm/IR/*.td`). `/usr/include/llvm/TargetParser/ARMTargetParser.h`,
`ARMTargetParser.def`, `TargetParserDef.inc` **are** installed. Consequence: TableGen against
`ARM.td` requires a checkout of the matching llvm-project source tree (§5.2), but the runtime MC
APIs give you everything you need without it (§5.1).

### 1.4 The second LLVM on this box (important hazard, VERIFIED)

`/usr/local/lib/cmake/llvm/LLVMConfig.cmake` exists and reports **`LLVM_PACKAGE_VERSION 24.0.0git`,
`LLVM_ENABLE_RTTI OFF`**, static-only component archives (`/usr/local/lib/libLLVM*.a`) and
**`/usr/local/include/llvm` does not exist**. CMake searches `/usr/local` before `/usr` on Linux, so
a naive `find_package(LLVM CONFIG REQUIRED)` picks the broken 24.0.0git tree, and the build fails at
"cannot find `llvm/Support/...`". Always pin the correct install:

```cmake
set(LLVM_DIR /usr/lib/cmake/llvm)          # or rely on the 23.1 version request below
find_package(LLVM 23.1 REQUIRED CONFIG)
```

### 1.5 `find_package` version semantics (VERIFIED)

`/usr/lib/cmake/llvm/LLVMConfigVersion.cmake` (complete file, 447 bytes) requires the **major.minor**
to match exactly and the patch to be >= requested:

```cmake
set(PACKAGE_VERSION "23.1.3")
if("23.1" VERSION_EQUAL "${PACKAGE_FIND_VERSION_MAJOR}.${PACKAGE_FIND_VERSION_MINOR}"
   AND NOT "3" VERSION_LESS "${PACKAGE_FIND_VERSION_PATCH}")
  set(PACKAGE_VERSION_COMPATIBLE 1)
  ...
```

Therefore:

* `find_package(LLVM 23 REQUIRED CONFIG)` **fails** ("not compatible with requested version 23").
* `find_package(LLVM 23.1 REQUIRED CONFIG)` succeeds and (because 24.0.0git is a different
  major.minor) reliably selects `/usr/lib/cmake/llvm`, i.e. it also dodges the `/usr/local` trap.

### 1.6 Working CMake incantation (VERIFIED — this exact file configures, builds and runs)

```cmake
cmake_minimum_required(VERSION 3.20)
project(ios6recomp C CXX)          # C is REQUIRED, see note below

find_package(LLVM 23.1 REQUIRED CONFIG)
message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} @ ${LLVM_DIR}")   # -> "LLVM 23.1.3 @ /usr/lib/cmake/llvm"

add_executable(app main.cpp)
target_include_directories(app PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(app PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(app PRIVATE LLVM)     # exported SHARED target, LLVMExports.cmake:348
set_target_properties(app PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
```

Notes, all observed:

* `project(... C CXX)`: `LLVMConfig.cmake` calls `find_package(LibEdit)` which runs
  `check_include_file()` (a **C** check); with only CXX enabled configure dies with
  `CMake Error at .../CheckIncludeFile.cmake:157 (try_compile): try_compile() works only for enabled
  languages. Currently these are: CXX`.
* **Do not** use `llvm_map_components_to_libnames()`. It returns `LLVMARMDisassembler;LLVMARMDesc;…`
  but the distro ships only a handful of component archives (`libLLVMDemangle.a`,
  `libLLVMSupport.a`, `libLLVMTableGen.a`, `libLLVMHTTP.a`, `libLLVMTesting*.a`); the per-target
  archives are absent, so the link fails with
  `mold: fatal: library not found: LLVMARMDisassembler` (VERIFIED).
  `add_library(LLVM SHARED IMPORTED)` at `LLVMExports.cmake:348` is the intended entry point
  (`llvm-config --libs armdisassembler …` likewise prints `-lLLVM-23`).
* `find_package(LLVM CONFIG)` (no version) is not enough on this box — see §1.4.

### 1.7 The probe program used to verify §5 (compiles + runs against the above CMake)

`main.cpp` — verified to print the ARM opcode table size (4531), decode ARM and Thumb, name
registers, and resolve branch targets:

```cpp
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"          // MCOperand lives here (there is no llvm/MC/MCOperand.h)
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <memory>
using namespace llvm;

int main() {
  InitializeAllTargetInfos(); InitializeAllTargetMCs(); InitializeAllDisassemblers();

  // MUST outlive MCAsmInfo/MCContext: MCAsmInfo stores `const MCTargetOptions &` (MCAsmInfo.h:439)
  static MCTargetOptions MCOpts;

  Triple TT("armv7s-apple-ios6.1.3");
  std::string Err;
  const Target *T = TargetRegistry::lookupTarget(TT, Err);

  std::unique_ptr<MCRegisterInfo> MRI(T->createMCRegInfo(TT));
  std::unique_ptr<MCAsmInfo>      MAI(T->createMCAsmInfo(*MRI, TT, MCOpts));
  std::unique_ptr<MCSubtargetInfo> STI(T->createMCSubtargetInfo(TT, /*CPU=*/"swift", /*FS=*/""));
  std::unique_ptr<MCInstrInfo>    MII(T->createMCInstrInfo());   // 4531 opcodes for ARM
  MCContext Ctx(TT, *MAI, *MRI, *STI);
  std::unique_ptr<MCDisassembler> Dis(T->createMCDisassembler(*STI, Ctx));
  std::unique_ptr<MCInstrAnalysis> MIA(T->createMCInstrAnalysis(MII.get()));
  // ... Dis->getInstruction(Inst, Size, Bytes, Address, nulls()); MII->getName(Inst.getOpcode()); ...
}
```

Two traps this probe found the hard way (both VERIFIED by A/B test):

1. Passing a **temporary** `MCTargetOptions()` to `createMCAsmInfo` is a dangling reference
   (`MCAsmInfo.h:439` `const MCTargetOptions &TargetOptions;`). The surviving MCContext constructor
   then copies `TO.AsSecureLogFile` out of freed memory and the process dies with
   `terminate called after throwing an instance of 'std::bad_alloc'` **inside the MCContext
   constructor**. Keep one `MCTargetOptions` alive for the whole lift session.
2. `MCRegisterInfo` has `const char *getName(MCRegister)` — **not** `getRegName()` (that accessor no
   longer exists). `MCOperand` is declared in `llvm/MC/MCInst.h`; `llvm/MC/MCOperand.h` is gone.

---

## 2. ORC in LLVM 23 — exact classes for a lazy, one-function-per-basic-block JIT

There are three viable stances:

* **S1 (recommended):** our own `MaterializationUnit` + a `DefinitionGenerator`, on top of an
  `LLJIT` (JITLink `ObjectLinkingLayer`) — for lazily materializing *one guest basic block at a
  time*, resolved by guest address.
* **S2:** `LLLazyJIT`, which adds `IRPartitionLayer` + `CompileOnDemandLayer` and gives *function*-at-a-time
  laziness over an LLVM `Module`. This is the closest stock thing to "one guest basic block per
  function": we would put one host function per guest block into a module and let COD split it.
* **S3:** `ExecutionSession` + hand-assembled layers (`IRCompileLayer` -> `ObjectTransformLayer` ->
  `ObjectLinkingLayer`). Maximum control, most code.

### 2.1 Session and dylibs

```cpp
// llvm/ExecutionEngine/Orc/ExecutorProcessControl.h:32
class llvm::orc::ExecutorProcessControl { ... };

// llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h:24 / :38
class llvm::orc::SelfExecutorProcessControl : public ExecutorProcessControl {
  static Expected<std::unique_ptr<SelfExecutorProcessControl>>
  Create(std::shared_ptr<SymbolStringPool> SSP = nullptr,           // default: new pool
         std::unique_ptr<TaskDispatcher> D = nullptr);              // default: new dispatcher
  // createDefaultMemoryManager() -> jitlink::InProcessMemoryManager (header comment, line 33)
  Expected<std::unique_ptr<jitlink::JITLinkMemoryManager>> createDefaultMemoryManager() override;
  Expected<std::unique_ptr<DylibManager>>                  createDefaultDylibMgr() override;
  Expected<std::unique_ptr<MemoryAccess>>                  createDefaultMemoryAccess() override;
  Expected<int32_t> runAsMain(ExecutorAddr, ArrayRef<std::string>) override;
};

// llvm/ExecutionEngine/Orc/Core.h:1131
class llvm::orc::ExecutionSession {
  ExecutionSession(std::unique_ptr<ExecutorProcessControl> EPC);        // Core.h:1159
  Error endSession();                                                   // Core.h:1167 (MUST be called)
  ExecutorProcessControl &getExecutorProcessControl();                  // Core.h:1171
  const Triple &getTargetTriple() const;                                // from EPC
  size_t getPageSize() const;
  std::shared_ptr<SymbolStringPool> getSymbolStringPool();              // Core.h:1180
  SymbolStringPtr intern(StringRef SymName);                            // Core.h:1185
  JITDylib &getBootstrapJITDylib();                                     // Core.h:1192
  JITDylib &createBareJITDylib(std::string Name);                       // Core.h:1236
  Expected<JITDylib &> createJITDylib(std::string Name);                // Core.h:1247
  ExecutionSession &setErrorReporter(ErrorReporter ReportError);         // Core.h:1274
  Expected<SymbolMap> lookup(const JITDylibSearchOrder &, SymbolLookupSet, SymbolLookupFlags); // :1327
  Expected<ExecutorSymbolDef> lookup(ArrayRef<JITDylib *>, StringRef, SymbolLookupFlags);      // :1351
  Expected<SymbolFlagsMap> lookupFlags(LookupKind, ...);                // Core.h:1292
  void lookup(LookupKind, const JITDylibSearchOrder&, SymbolLookupSet, ...); // async, Core.h:1315
};
```

`SymbolMap = DenseMap<SymbolStringPtr, ExecutorSymbolDef>`, `SymbolFlagsMap = DenseMap<SymbolStringPtr, JITSymbolFlags>`
(`CoreContainers.h:37,40`). `SymbolLookupSet` is a vector-backed set of `(SymbolStringPtr, SymbolLookupFlags)`
with `SymbolLookupFlags::{RequiredSymbol, WeaklyReferencedSymbol}` (`SymbolLookupSet.h`).

### 2.2 Lazy definition — `MaterializationUnit`

```cpp
// llvm/ExecutionEngine/Orc/MaterializationUnit.h:33
class llvm::orc::MaterializationUnit {
  struct Interface { SymbolFlagsMap SymbolFlags; SymbolStringPtr InitSymbol; };
  MaterializationUnit(Interface I);
  virtual StringRef getName() const = 0;
  virtual void materialize(std::unique_ptr<MaterializationResponsibility> R) = 0;
  virtual void discard(const JITDylib &JD, const SymbolStringPtr &Name) = 0;  // private virtual
  const SymbolFlagsMap &getSymbols() const;
};

// llvm/ExecutionEngine/Orc/Core.h:369
class llvm::orc::MaterializationResponsibility {
  SymbolNameSet getRequestedSymbols() const;                     // Core.h:414
  Error defineMaterializing(SymbolFlagsMap SymbolFlags);         // Core.h:463
  Error notifyResolved(const SymbolMap &Symbols);                // Core.h:429
  Error notifyEmitted(ArrayRef<SymbolDependenceGroup> DepGroups); // Core.h:450
  void failMaterialization();                                    // Core.h:470
  Error replace(std::unique_ptr<MaterializationUnit> MU);        // Core.h:477 (re-emit yourself)
};
```

Ready-made MUs:

* `AbsoluteSymbolsMaterializationUnit(SymbolMap)` + helper `absoluteSymbols(SymbolMap)` —
  `Orc/AbsoluteSymbols.h:29,52`. All symbols become ready immediately; use for constants, guest
  globals with host addresses, function-pointer tables, and "block id -> address" entries.
* `BasicObjectLayerMaterializationUnit` — `Orc/Layer.h:176`:
  `static Expected<std::unique_ptr<BasicObjectLayerMaterializationUnit>> Create(ObjectLayer&, std::unique_ptr<MemoryBuffer>)`
  plus the direct-construction ctors. Use it when the lifter emits a relocatable object per block.
  `ObjectLayer::add(ResourceTrackerSP RT, std::unique_ptr<MemoryBuffer> O)` (Layer.h:146) is the
  usual entry point; `ObjectLayer::add(JITDylib&, ...)` overload exists too.
* `BasicIRLayerMaterializationUnit` (`Layer.h:121`) for `ThreadSafeModule` inputs; `IRMaterializationUnit`
  (`Layer.h:32`) maps `SymbolNameToDefinitionMap`.

Define into a dylib:

```cpp
// llvm/ExecutionEngine/Orc/Core.h:841/855 (templates), implemented at Core.h:1500+
Error JITDylib::define(std::unique_ptr<MaterializationUnitType> &&MU, ResourceTrackerSP RT = nullptr);
Error JITDylib::define(std::unique_ptr<MaterializationUnitType> &MU,  ResourceTrackerSP RT = nullptr);
ResourceTrackerSP JITDylib::createResourceTracker();       // Core.h:743
ResourceTrackerSP JITDylib::getDefaultResourceTracker();   // Core.h:737
Error JITDylib::remove(const SymbolNameSet &Names);        // Core.h:871
GeneratorT &JITDylib::addGenerator(std::unique_ptr<GeneratorT> DefGenerator);  // Core.h:755
void JITDylib::removeGenerator(DefinitionGenerator &G);    // Core.h:764
void JITDylib::setLinkOrder(JITDylibSearchOrder, bool LinkAgainstThisJITDylibFirst = true); // :787
```

`ResourceTracker` matters: definitions added against a non-default RT can be removed later
(`ResourceTracker::remove()`), which for us means "drop this block's code" — the address is then
*freed*, so do not use removal for code addresses that guest code can still branch to (§2.6).

### 2.3 On-demand generation — `DefinitionGenerator`

This is the mechanism that lets you avoid putting a million symbols into a `JITDylib` up front: a
generator is consulted *during lookup*, for the specific unresolved symbol set, and may define just
those symbols then (`Orc/Core.h:652`):

```cpp
class llvm::orc::DefinitionGenerator {
  virtual Error tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD,
                              JITDylibLookupFlags JDLookupFlags,
                              const SymbolLookupSet &LookupSet) = 0;
};
// LookupState::continueLookup(Error Err)  -> Core.h:639 (for generators that define asynchronously)
```

A generator for guest addresses is ~40 lines: for each requested `SymbolStringPtr`, parse the guest
address out of the name, build the block's `MaterializationUnit`, call
`JD.define(std::move(MU), RT)`, then return the current `LookupState` unchanged (the JD continues
the lookup after your new definitions land). Precedents in-tree: `EPCDynamicLibrarySearchGenerator`,
`ExecutorResolutionGenerator`, `StaticLibraryDefinitionGenerator`.

### 2.4 `LLJIT` / `LLLazyJIT` and what they give you by default

```cpp
// llvm/ExecutionEngine/Orc/LLJIT.h:44
class llvm::orc::LLJIT {
  ExecutionSession &getExecutionSession();        // :98
  const Triple &getTargetTriple() const;          // :101
  const DataLayout &getDataLayout() const;        // :104
  JITDylib &getMainJITDylib();                    // :107
  JITDylibSP getProcessSymbolsJITDylib();         // :113
  std::string mangle(StringRef) const;            // :241
  SymbolStringPtr mangleAndIntern(StringRef) const;  // :247
  Error addIRModule(JITDylib&, ThreadSafeModule);    // :144
  Error addObjectFile(JITDylib&, std::unique_ptr<MemoryBuffer>);  // :156
  Expected<ExecutorAddr> lookup(JITDylib&, StringRef UnmangledName);   // :187
  Expected<ExecutorAddr> lookupLinkerMangled(JITDylib&, StringRef);    // :171
  ObjectLayer &getObjLinkingLayer();              // :234
  ObjectTransformLayer &getObjTransformLayer();   // :237
  IRTransformLayer &getIRTransformLayer();        // :240
  IRCompileLayer &getIRCompileLayer();            // :243
  jitlink::JITLinkMemoryManager &getMemoryManager();   // :229
  DylibManager &getDylibMgr();                    // :223
  Error initialize(JITDylib&); Error deinitialize(JITDylib&);   // :196/:204
};

// llvm/ExecutionEngine/Orc/LLJIT.h:289
class llvm::orc::LLLazyJIT : public LLJIT {
  void setPartitionFunction(IRPartitionLayer::PartitionFunction);   // :295
  CompileOnDemandLayer &getCompileOnDemandLayer();                  // :302
  Error addLazyIRModule(JITDylib&, ThreadSafeModule);               // :306
};
```

Builder knobs that matter to us (`LLJITBuilderState`, `LLJIT.h:322`; setters `LLJITBuilderSetters`
at `:363`):

```cpp
LLJITBuilder().setJITTargetMachineBuilder(JTMB)      // triple/CPU/features/code model
             .setDataLayout(DL)
             .setMemoryManagerCreator(LLJITBuilderState::MemoryManagerCreator)   // §2.8 (LLJIT.h:422)
             .setObjectLinkingLayerCreator(LLJITBuilderState::ObjectLinkingLayerCreator)
             .setCompileFunctionCreator(LLJITBuilderState::CompileFunctionCreator)
             .setProcessSymbolsJITDylibSetup(ProcessSymbolsJITDylibSetupFunction) // host libc etc.
             .setPlatformSetUp(PlatformSetupFunction)
             .setNotifyCreatedCallback(NotifyCreatedFunction)  // attach generators, inject symbols
             .setNumCompileThreads(unsigned)                   // 0 = compile on the calling thread
             .setSupportConcurrentCompilation(std::optional<bool>)
             .create();
// LLLazyJITBuilder adds (LLJIT.h:567+):
             .setLazyCompileFailureAddr(ExecutorAddr)
             .setLazyCallthroughManager(std::unique_ptr<LazyCallThroughManager>)
             .setIndirectStubsManagerBuilder(IndirectStubsManagerBuilderFunction)
```

Defaults for our triple (x86_64 Linux/ELF), VERIFIED by reading `LLJIT.cpp` at tag 23.1.3:

* Object linking layer: the builder's `prepareForConstruction()` auto-configures JITLink
  (`LLJIT.cpp:798-838`): for `Triple::x86_64` with non-COFF object format it sets
  `CreateObjectLinkingLayer = [](ES, MemMgr){ return std::make_unique<ObjectLinkingLayer>(ES, MemMgr); }`
  and forces `CodeModel::Small` + `Reloc::PIC_`. The `RTDyldObjectLinkingLayer` fallback at
  `LLJIT.cpp:961-967` only triggers when auto-configuration does not apply — the stale comment on
  `setObjectLinkingLayerCreator` ("will construct an RTDyldObjectLinkingLayer") is wrong for Linux.
* Memory manager: `ExecutorProcessControl::createDefaultMemoryManager()` ->
  `jitlink::InProcessMemoryManager`.
* Process symbols: `EPCDynamicLibrarySearchGenerator::GetForTargetProcess(ES, dylibMgr)`
  (`LLJIT.cpp:846`) — this is how JIT'd code calls into host libc/libm without an ORC runtime.
  Note it now takes a `DylibManager&`: `EPCDynamicLibrarySearchGenerator(ExecutionSession&,
  DylibManager&, tpctypes::DylibHandle, SymbolPredicate, AddAbsoluteSymbolsFn)` +
  `Load(...)`, `GetForTargetProcess(...)` (`Orc/EPCDynamicLibrarySearchGenerator.h:41` (handle ctor), `:55` (null-resolving ctor), `:65` (`Load`), `:72` (`GetForTargetProcess`)).

### 2.5 Lazy per-symbol materialization: `CompileOnDemandLayer` vs. our own MU

* `LLLazyJIT` + `CompileOnDemandLayer` (`Orc/CompileOnDemandLayer.h`, `LLJIT.h:302`) is stock, works,
  and is built on `IndirectStubsManager` + `LazyCallThroughManager` + `IRPartitionLayer`. The stub
  mechanism is legacy-ish (`OrcABISupport.h` implements stubs only for OrcGenericABI, OrcAArch64,
  OrcX86_64_SysV/Win32, OrcI386, OrcMips32/64, OrcRiscv64 — **no 32-bit ARM**; fine, the JIT target
  is the host).
* For "one guest basic block = one host function", S1 with `BasicObjectLayerMaterializationUnit`
  (or our own MU that hands a `MemoryBuffer` to `ObjectLinkingLayer`) is a better fit than COD,
  because a guest block is not an LLVM function until we make it one, and because COD's partition
  function is designed around IR functions in a module.
* `LazyObjectLinkingLayer.h` + `ReOptimizeLayer.h`/`JITLinkReentryTrampolines.h` are the modern
  (LLVM 20+) lazy-emission paths: `LazyObjectLinkingLayer` links `LinkGraph`s lazily, and
  `ReOptimizeLayer` recompiles a function with profile data, using a `RedirectableSymbolManager`
  for the version swap. `ReOptimizeLayer` is exactly the "patch the entry point in place" pattern we
  need for guest-block patching, see §2.6.

### 2.6 Redirecting calls between blocks (verified API surface)

Two generations, both present:

*(a) Modern, JITLink-based — `RedirectableSymbolManager`.* The symbol's address is a stable
indirection; the definition behind it is patched without changing the address.

```cpp
// Orc/RedirectionManager.h:24, :45
class llvm::orc::RedirectionManager {
  virtual Error redirect(JITDylib &JD, const SymbolMap &NewDests) = 0;
  Error redirect(JITDylib &JD, SymbolStringPtr Symbol, ExecutorSymbolDef NewDest);
};
class llvm::orc::RedirectableSymbolManager : public RedirectionManager {
  Error createRedirectableSymbols(ResourceTrackerSP RT, SymbolMap InitialDests);
  Error createRedirectableSymbol(SymbolStringPtr, ExecutorSymbolDef InitialDest);
  virtual void emitRedirectableSymbols(std::unique_ptr<MaterializationResponsibility>, SymbolMap) = 0;
};
class llvm::orc::RedirectableMaterializationUnit : public MaterializationUnit { ... };  // RedirectionManager.h:67

// Orc/JITLinkRedirectableSymbolManager.h:27
class llvm::orc::JITLinkRedirectableSymbolManager : public RedirectableSymbolManager {
  static Expected<std::unique_ptr<RedirectableSymbolManager>>
  Create(ObjectLinkingLayer &ObjLinkingLayer, MemoryAccess &MemAccess);
  Error redirect(JITDylib &JD, const SymbolMap &NewDests) override;
};
// Orc/JITLinkReentryTrampolines.h:64 — lazy re-exports on top of JITLink
Expected<std::unique_ptr<LazyReexportsManager>>
createJITLinkLazyReexportsManager(ObjectLinkingLayer&, RedirectableSymbolManager&, JITDylib &PlatformJD,
                                  LazyReexportsManager::Listener *L = nullptr);
```

*(b) Legacy stubs — `IndirectStubsManager`.* Each stub is a fixed address with a mutable pointer
slot (`updatePointer`) and a resolver trampoline:

```cpp
// Orc/IndirectionUtils.h:283
class llvm::orc::IndirectStubsManager : public RedirectableSymbolManager {
  using StubInitsMap = StringMap<std::pair<ExecutorAddr, JITSymbolFlags>>;
  virtual Error createStub(StringRef StubName, ExecutorAddr StubAddr, JITSymbolFlags) = 0;
  virtual Error createStubs(const StubInitsMap &) = 0;
  virtual ExecutorSymbolDef findStub(StringRef, bool ExportedStubsOnly) = 0;
  virtual ExecutorSymbolDef findPointer(StringRef) = 0;
  virtual Error updatePointer(StringRef, ExecutorAddr NewAddr) = 0;
};
// Orc/IndirectionUtils.h:490
std::function<std::unique_ptr<IndirectStubsManager>> createLocalIndirectStubsManagerBuilder(const Triple &T);
// Orc/LazyReexports.h:135, :171
Expected<std::unique_ptr<LazyCallThroughManager>>
createLocalLazyCallThroughManager(const Triple &T, ExecutionSession &ES, ExecutorAddr ErrorHandlerAddr);
std::unique_ptr<MaterializationUnit> lazyReexports(LazyCallThroughManager&, IndirectStubsManager&,
                                                  JITDylib&, SymbolAliasMap);
```

Use (a) for "block X now lives at address Y after recompilation/patching" and (b) only if you
actually want pre-allocated stub slots for indirect calls from JIT code into the dispatcher.

### 2.7 Symbol names, interning, mangling

```cpp
// Orc/SymbolStringPool.h:38
class llvm::orc::SymbolStringPool { SymbolStringPtr intern(StringRef S); void clearDeadEntries(); };
// refcounted StringMap; SymbolStringPtr is a refcounted handle (thread-safe atomic refcount)
// Orc/Mangling.h:26
class llvm::orc::MangleAndInterner { MangleAndInterner(ExecutionSession&, const DataLayout&);
                                     SymbolStringPtr operator()(StringRef Name); };
```

`LLJIT::mangleAndIntern()` uses the target DataLayout's mangler (x86-64 ELF: no underscore prefix).
Use `SymbolStringPtr` for lookup *and* definition sets; converting `StringRef` each time re-interns
(hash + possible allocation).

### 2.8 Memory: who allocates, where, and with which protection

```cpp
// llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h:47
class llvm::jitlink::JITLinkMemoryManager {
  class FinalizedAlloc { orc::ExecutorAddr getAddress() const; orc::ExecutorAddr release(); };
  class InFlightAlloc {  // :~120
    using OnFinalizedFunction = unique_function<void(Expected<FinalizedAlloc>)>;
    virtual void abandon(OnAbandonedFunction) = 0;
    virtual void finalize(OnFinalizedFunction) = 0;
    Expected<FinalizedAlloc> finalize();          // blocking convenience
  };
  using AllocResult = Expected<std::unique_ptr<InFlightAlloc>>;
  using OnAllocatedFunction = unique_function<void(AllocResult)>;
  virtual void allocate(const JITLinkDylib *JD, LinkGraph &G, OnAllocatedFunction) = 0;  // :163
  AllocResult allocate(const JITLinkDylib *JD, LinkGraph &G);                            // blocking
  virtual void deallocate(std::vector<FinalizedAlloc>, OnDeallocatedFunction) = 0;
  Error deallocate(std::vector<FinalizedAlloc>);
};

class llvm::jitlink::BasicLayout {          // :214 — segments grouped by AllocGroup/protection
  BasicLayout(LinkGraph &G);
  Expected<ContiguousPageBasedLayoutSizes> getContiguousPageBasedLayoutSizes(uint64_t PageSize);
  iterator_range<SegmentMap::iterator> segments();
  Error apply();
};
class llvm::jitlink::SimpleSegmentAlloc {   // :299 — one-section LinkGraph per segment
  static Expected<SimpleSegmentAlloc> Create(JITLinkMemoryManager&, std::shared_ptr<orc::SymbolStringPool>,
                                            Triple, const JITLinkDylib*, SegmentMap);
  SegmentInfo getSegInfo(orc::AllocGroup);   // { ExecutorAddr Addr; MutableArrayRef<char> WorkingMem; }
  Expected<FinalizedAlloc> finalize();
};
class llvm::jitlink::InProcessMemoryManager : public JITLinkMemoryManager {   // :370
  static Expected<std::unique_ptr<InProcessMemoryManager>> Create();  // host page size
  InProcessMemoryManager(uint64_t PageSize);
};
```

What `InProcessMemoryManager` actually does (VERIFIED by reading
`llvm/lib/ExecutionEngine/JITLink/JITLinkMemoryManager.cpp` at tag 23.1.3 — identical to `main`
except for an unrelated `LinkGraph` ctor change; upstream file:
<https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/llvm/lib/ExecutionEngine/JITLink/JITLinkMemoryManager.cpp>):

1. `BasicLayout BL(G)`; `SegsSizes = BL.getContiguousPageBasedLayoutSizes(PageSize)`.
2. **One** contiguous slab:
   `sys::Memory::allocateMappedMemory(SegsSizes->total(), /*NearBlock=*/nullptr, MF_READ|MF_WRITE, EC)`,
   then `memset(slab, 0, allocatedSize)`.
3. The slab is split into a standard part and a finalization part (`FinalizeSegsMem`).
4. `InFlightAlloc::finalize()` -> `applyProtections()`: per segment
   `sys::Memory::protectMappedMemory(MemoryBlock(Seg.WorkingMem, alignTo(ContentSize+ZeroFillSize, PageSize)), Prot)`
   with `Prot = toSysMemoryProtectionFlags(AG.getMemProt())`; if `Prot & MF_EXEC` then
   `sys::Memory::InvalidateInstructionCache(base, size)`.
5. `deallocate()` frees the slab and runs dealloc actions.

There is **no address control**: the OS picks the address. `sys::Memory` (`/usr/include/llvm/Support/Memory.h:56-129`)
exposes only `allocateMappedMemory(size_t NumBytes, const MemoryBlock *NearBlock, unsigned Flags, std::error_code&)`,
`releaseMappedMemory`, `protectMappedMemory`, `InvalidateInstructionCache`; `NearBlock` is a *hint*
and there is **no `MAP_FIXED` equivalent** in the LLVM API (VERIFIED).

`MapperJITLinkMemoryManager` + `MemoryMapper` (present since LLVM 16, still here) give a
reserve/initialize/deinitialize/release lifecycle that is friendlier for fixed ranges:

```cpp
// Orc/MemoryMapper.h:31
class llvm::orc::MemoryMapper {
  struct AllocInfo { ExecutorAddr MappingBase; std::vector<SegInfo> Segments; shared::AllocActions Actions; };
  virtual unsigned getPageSize() = 0;
  virtual void reserve(size_t NumBytes, OnReservedFunction) = 0;                  // -> ExecutorAddrRange
  virtual char *prepare(jitlink::LinkGraph &G, ExecutorAddr Addr, size_t ContentSize) = 0;
  virtual void initialize(AllocInfo&, OnInitializedFunction) = 0;                 // -> ExecutorAddr
  virtual void deinitialize(ArrayRef<ExecutorAddr>, OnDeinitializedFunction) = 0;
  virtual void release(ArrayRef<ExecutorAddr>, OnReleasedFunction) = 0;
};
class llvm::orc::InProcessMemoryMapper : public MemoryMapper {
  static Expected<std::unique_ptr<InProcessMemoryMapper>> Create();
};
class llvm::orc::MapperJITLinkMemoryManager : public jitlink::JITLinkMemoryManager { // MapperJITLinkMemoryManager.h:23
  MapperJITLinkMemoryManager(size_t ReservationGranularity, std::unique_ptr<MemoryMapper>);
  template <class MemoryMapperType, class... Args>
  static Expected<std::unique_ptr<MapperJITLinkMemoryManager>> CreateWithMapper(size_t, Args&&...);
};
```

`InProcessMemoryMapper::reserve()` calls `sys::Memory::allocateMappedMemory(NumBytes, nullptr, MF_READ|MF_WRITE, EC)`
too (VERIFIED, `MemoryMapper.cpp:43-47`) — i.e. still an OS-chosen address. `SharedMemoryMapper`
(used for remote/out-of-process executors) uses `mmap(nullptr, ..., MAP_SHARED, shm_fd, 0)`.

**Conclusion for (f) "control which address range JIT'd code is allocated in":** you must write
your own `JITLinkMemoryManager` (or your own `MemoryMapper` + `MapperJITLinkMemoryManager`) that
`mmap`s one big arena with the address you want and then satisfies `allocate()` from it. Skeleton
(exact virtuals to implement):

```cpp
class ArenaMemMgr : public llvm::jitlink::JITLinkMemoryManager {
  void allocate(const llvm::jitlink::JITLinkDylib *JD, llvm::jitlink::LinkGraph &G,
                OnAllocatedFunction OnAllocated) override {
    llvm::jitlink::BasicLayout BL(G);
    auto Segs = BL.getContiguousPageBasedLayoutSizes(PageSize);       // per-protection sizes
    // bump-allocate Segs->total() inside our pre-mmap'd arena (or mmap(MAP_FIXED) here),
    // set Seg.Segment.Addr / WorkingMem for each segment, then:
    if (auto Err = BL.apply()) { OnAllocated(std::move(Err)); return; }
    OnAllocated(std::make_unique<InFlightAllocImpl>(*this, std::move(BL)/*, arena range */));
  }
  void deallocate(std::vector<FinalizedAlloc>, OnDeallocatedFunction) override { /* return to arena */ }
};
// InFlightAllocImpl::finalize(): copy WorkingMem -> executor memory, protect (mprotect R-X for code),
//   run BL.graphAllocActions(), then OnFinalized(FinalizedAlloc(arenaBaseOrHandle));
// InFlightAllocImpl::abandon(): release the range.
```

Hooked in via `LLJITBuilder().setMemoryManagerCreator(...)` (`LLJIT.h:411`) or
`setObjectLinkingLayerCreator(...)` if you build the layer yourself, or directly on
`ObjectLinkingLayer(ExecutionSession&, JITLinkMemoryManager&)` (`Orc/ObjectLinkingLayer.h:55,63`).

### 2.9 `EPCDefaultMemoryMappings` — does not exist

`grep -rn EPCDefaultMemoryMappings /usr/include/llvm` -> nothing;
`nm -D --defined-only /usr/lib/libLLVM.so.23.1 | grep DefaultMemoryMappings` -> nothing; and the
header does not exist in the release trees I probed (`llvm/include/llvm/ExecutionEngine/Orc/`
listing at tags llvmorg-15.0.7, -17.0.1, -19.1.0, -23.1.3, and both `Orc/` and
`Orc/TargetProcess/` paths). The actual classes are `orc::MemoryMapper` /
`orc::InProcessMemoryMapper` / `orc::MapperJITLinkMemoryManager` /
`orc::EPCGenericJITLinkMemoryManager` / `jitlink::InProcessMemoryManager`.
`UNVERIFIED:` the name may come from a pre-15 rename or from a design doc; it is not part of any
LLVM 15+ public API and must not be planned for.

### 2.10 Object persistence ((e) "persist compiled objects to disk and reload")

`ObjectTransformLayer` (`Orc/ObjectTransformLayer.h`) sits between `IRCompileLayer` and the
`ObjectLayer`; `LLJIT::getObjTransformLayer().setTransform(fn)` sees every compiled
`MemoryBuffer` before linking — the natural place to write `.o` files and to substitute a cached
buffer. `LLJIT::getIRCompileLayer()` exposes the compiler if you prefer to intercept earlier.

The in-tree canonical example is `llvm/examples/OrcV2Examples/LLJITWithObjectCache/LLJITWithObjectCache.cpp`
(VERIFIED present at 23.1.3), which uses `llvm::ObjectCache`:

```cpp
class MyObjectCache : public llvm::ObjectCache {
  void notifyObjectCompiled(const Module *M, MemoryBufferRef ObjBuffer) override;  // write to disk
  std::unique_ptr<MemoryBuffer> getObject(const Module *M) override;               // read from disk
};
LLJITBuilder().setCompileFunctionCreator(
  [&](JITTargetMachineBuilder JTMB) -> Expected<std::unique_ptr<IRCompileLayer::IRCompiler>> {
    auto TM = JTMB.createTargetMachine();
    if (!TM) return TM.takeError();
    return std::make_unique<TMOwningSimpleCompiler>(std::move(*TM), &ObjCache);
  }).create();
```

Caveats for our use: the cache key in that example is `Module::getIdentifier()`, i.e. you must give
each guest block's module a stable identifier that encodes the guest address **and the lifter
version** (lift results change when the lifter changes — version the cache key or you will execute
stale translations).

---

## 3. What is deprecated/removed relative to older tutorials

### 3.1 Still present and usable in 23.1.3 (VERIFIED by grep/list)

| API | Header | Status |
|---|---|---|
| `orc::RTDyldObjectLinkingLayer` | `Orc/RTDyldObjectLinkingLayer.h:31` | present, **not** annotated deprecated (zero `LLVM_DEPRECATED`/`DEPRECATED` occurrences in `ExecutionEngine/Orc/**`) |
| `orc::CompileOnDemandLayer` | `Orc/CompileOnDemandLayer.h` | present |
| `orc::LLLazyJIT`, `LLLazyJITBuilder` | `Orc/LLJIT.h:289,600` | present |
| `orc::IndirectStubsManager`, `LocalIndirectStubsManager`, `createLocalIndirectStubsManagerBuilder` | `Orc/IndirectionUtils.h:283,379,490` | present |
| `orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(char GlobalPrefix, SymbolPredicate)` | `Orc/ExecutionUtils.h:217,247` | present (LLVM 10-era tutorial API still compiles) |
| `orc::EPCDynamicLibrarySearchGenerator` | `Orc/EPCDynamicLibrarySearchGenerator.h` | present; **now requires a `DylibManager&`** — this is the signature change that breaks older code |
| `orc::JITTargetMachineBuilder::detectHost()` | `Orc/JITTargetMachineBuilder.h:52` | present |
| `orc::ThreadSafeContext`, `ThreadSafeModule` | `Orc/ThreadSafeModule.h:29,70` | present |
| `MCJIT`/`EngineBuilder` | `ExecutionEngine/MCJIT.h`, `ExecutionEngine.h` | present; `ExecutionEngine.h:237,347,357,366,409` carry "deprecated" comments |

### 3.2 Gone (will not compile)

* OrcV1 / `orc::Legacy*`: no occurrence of `Legacy` in `ExecutionEngine/Orc/**`; no
  `Orc/Legacy.h`, no `DynamicLibrarySearchGenerator` subclasses with a `Legacy` prefix, no
  `LegacyDynamicLibrarySearchGenerator`. Any snippet using `orc::LegacyLookupFn`,
  `orc::LegacyJITDylib`, `orc::IndirectStubsManager` *V1* plumbing or `OrcMCJITReplacement` is dead.
* `orc::TargetProcessControl` (the class): the header and class are gone; the live name is
  `orc::ExecutorProcessControl`. Only the shared ABI types file kept the old name
  (`Orc/Shared/TargetProcessControlTypes.h`, still `tpctypes::DylibHandle` etc.).
  `EPCDefaultMemoryMappings` — see §2.9.
* `RTDyldObjectLinkingLayer::RegisterJITEventListener` (and the old `RuntimeDyld`-era listener
  plumbing): use `ObjectLinkingLayer::addPlugin(std::shared_ptr<Plugin>)`
  (`LinkGraphLinkingLayer.h:93`) with a plugin deriving from `ObjectLinkingLayer::Plugin`. In-tree
  plugins: `Orc/Debugging/ELFDebugObjectPlugin.h` (`ELFDebugObjectPlugin`),
  `Orc/Debugging/DebuggerSupportPlugin.h`, `Orc/Debugging/PerfSupportPlugin.h`,
  `Orc/Debugging/VTuneSupportPlugin.h`, and `Orc/EHFrameRegistrationPlugin.h`
  (`EHFrameRegistrationPlugin`).
* `LLJIT`'s old `LegacyDynamicLibrarySearchGenerator` setup pattern (LLVM <=13 tutorials): the
  `Legacy*` variant is gone entirely; the non-legacy `DynamicLibrarySearchGenerator`
  (`Orc/ExecutionUtils.h:217`) is still there and still compiles, but the EPC-era way to wire the
  main/process dylib is `setProcessSymbolsJITDylibSetup` +
  `EPCDynamicLibrarySearchGenerator::GetForTargetProcess`.
* The "compile IR with an object cache" idiom moved: `LLJITWithObjectCache` uses
  `TMOwningSimpleCompiler(std::unique_ptr<TargetMachine>, ObjectCache * = nullptr)`
  (`Orc/CompileUtils.h:69`) because `JITTargetMachineBuilder::createTargetMachine()` hands you a
  `unique_ptr<TargetMachine>`. The non-owning `SimpleCompiler(TargetMachine&, ObjectCache * = nullptr)`
  (`CompileUtils.h:42`) still exists and still takes a cache. What is gone is the old
  `EngineBuilder`/`RTDyld` "compile this module" entry point and `LLJIT`'s former
  `DynamicLibrarySearchGenerator`-based main-dylib setup.

### 3.3 `MCTargetOptions`/`MCContext` are unchanged but hostile to known "tutorial" code

See §1.7 trap 1: `MCAsmInfo` stores a reference to the `MCTargetOptions`; the common
`createMCAsmInfo(*MRI, TT, MCTargetOptions())` idiom crashes in `MCContext`'s ctor.

### 3.4 Canonical up-to-date examples (VERIFIED listing of tag llvmorg-23.1.3, `llvm/examples/OrcV2Examples/`)

```
CMakeLists.txt  ExampleModules.h  LLJITDumpObjects/  LLJITRemovableCode/
LLJITWithCustomObjectLinkingLayer/  LLJITWithExecutorProcessControl/  LLJITWithGDBRegistrationListener/
LLJITWithInitializers/  LLJITWithLazyReexports/  LLJITWithObjectCache/
LLJITWithObjectLinkingLayerPlugin/  LLJITWithOptimizingIRTransform/  LLJITWithRemoteDebugging/
LLJITWithSymbolAliases/  LLJITWithThinLTOSummaries/  OrcV2CBindingsAddObjectFile/
OrcV2CBindingsBasicUsage/  OrcV2CBindingsDumpObjects/  OrcV2CBindingsIRTransforms/
OrcV2CBindingsLazy/  OrcV2CBindingsMCJITLikeMemoryManager/  OrcV2CBindingsRemovableCode/
OrcV2CBindingsVeryLazy/
```

Most relevant for us: `LLJITWithLazyReexports` (stubs + `createLocalLazyCallThroughManager` +
`lazyReexports(...)`, `getIRTransformLayer().setTransform(...)`), `LLJITWithObjectCache` (persistence,
§2.10), `LLJITWithCustomObjectLinkingLayer` (custom memory manager/layer wiring),
`LLJITWithExecutorProcessControl` (explicit EPC), `LLJITWithInitializers` (platform/JITDylib init),
`LLJITRemovableCode` / `OrcV2CBindingsRemovableCode` (`ResourceTracker` removal semantics — read
before using `ResourceTracker::remove()` for block invalidation), `LLJITWithRemoteDebugging`.

Prose docs: <https://llvm.org/docs/ORCv2.html> (fetched: no mention of `EPCDefaultMemoryMappings`,
`MemoryMapper` or `InProcessMemoryManager`; it still describes `LLLazyJIT`). LLVM 23 release notes
(`llvm/docs/ReleaseNotes.md` at tag 23.1.3) contain **no** ORC/EPC API changes; the only JIT item is
"JITLink gained an ELF backend for Hexagon". Practical reading: everything you learned by reading
LLVM 20/21 code still applies in 23.

---

## 4. Guest address -> symbol mapping, and lookup cost

### 4.1 Do not create one ORC symbol per guest address up front

Data structures involved (VERIFIED, `Orc/Core.h:987` etc.):

```cpp
using SymbolTable = DenseMap<SymbolStringPtr, SymbolTableEntry>;   // JITDylib::SymbolTable (Core.h:987)
class SymbolTableEntry { ExecutorAddr Addr(8B); JITSymbolFlags Flags(2B); uint8_t State:7, MaterializerAttached:1; };
// JITSymbolFlags = two uint8_t (ExecutionEngine/JITSymbol.h:77-78)
//                                           -> 16 B  (8 + 2 + 1 + 5 padding)
// DenseMap bucket = key(8B) + value(16B) = 24B, and DenseMap keeps >= 1/8 slots empty/tombstoned
// SymbolStringPool: StringMap<RefCountType> (atomic<size_t> value) + StringMapEntry header (~16B) + chars
```

Budget per lazily-defined symbol: ~24 B / 0.875 ≈ 28 B of table, ~32-40 B of pooled name
(StringMap entry header + atomic refcount + name bytes + alignment), plus whatever your own
`std::map<guest_addr, BlockInfo>` costs. **Order of magnitude: 60-100 B per symbol, so 1e6 symbols
is 60-100 MB and 1e6 interned strings** — survivable but pure waste for code that is never executed.
Lookup cost per miss is: intern the name (StringMap hash + possible alloc), DenseMap probe, walk the
JITDylib link order, build/attach an `AsynchronousSymbolQuery`, run generators, then materialize.
The dominant costs are the query bookkeeping and the materialization, not the map probe; measure
before optimizing (`UNVERIFIED:` no ns figures measured here; use `perf` on the real workload).

Recommended shapes, in order of preference:

1. **No symbol per block at all.** Reserve one host arena and make host address a pure function of
   guest address: `host = arena_base + guest_va` (or `arena_base + (guest_va - image_base)`), with
   the guest image mapped at its Mach-O `vmaddr`. Intra-image branches/calls then become
   *constant* host addresses in the lifted code — no lookup, no interning, no table. This also gives
   you stable addresses for free (requirement (c)). Guard the arena with real virtual memory
   reservations so that a stray guest address faults rather than aliases another region.
   ORC symbols are then only needed for (i) the entry point(s), (ii) guest imports resolved to host
   symbols (libSystem -> our shim library, `EPCDynamicLibrarySearchGenerator` + a custom
   `DefinitionGenerator` for our own guest-API shims), and (iii) rebound/patched blocks.
2. **One symbol per *region*, lazily materialized.** Symbol name encodes the region base
   (e.g. `g.<guest_hex>`), one `MaterializationUnit` per 4-64 KiB region that emits all blocks in
   that region. 1e4-1e5 symbols instead of 1e6.
3. **`DefinitionGenerator` per address.** Zero table entries until a lookup happens: the generator
   parses the guest address out of the requested name, defines that block, and the JD re-resolves.
   This is the only shape where the symbol-table cost is proportional to *touched* code.
4. Symbols for *all* addresses: only if the guest does data-driven dispatch through a real
   symbol table (ObjC `methodForSelector`, `dlsym`), and even then generate them on demand in a
   generator rather than up front.

Naming: keep names short and fixed-width so interning is cheap and parseable, e.g. `g$00001000` for
code at `0x1000`, `d$0000c000` for a data global, `s$<lib>!<name>` for import shims. Avoid
`std::to_string(addr)` style with variable length if you are generating millions
(`to_chars` into a fixed buffer, then one `intern`).

`AbsoluteSymbolsMaterializationUnit`/`absoluteSymbols()` is the tool for "this name is just a
constant address" (one MU can define many entries, so batch them).

### 4.2 Related knobs worth knowing

* `ExecutionSession::getPageSize()` / `ExecutorProcessControl::getPageSize()` — required to size the
  arena and to compute `BasicLayout` sizes.
* `WaitingOnGraph` (`Orc/WaitingOnGraph.h`, `WaitingOnGraphOpReplay.h`) is the LLVM 20+ replacement
  for the old dependency-tracking data structures; if you instrument ORC, know that these exist.
* `orc::SectCreate.h`, `orc::MachOBuilder.h`, `orc::MachO.h`, `orc::MachOPlatform.h` are present but
  target the *Apple-host* ORC runtime path; on Linux we neither need them nor can we use them for
  guest-side Mach-O.

---

## 5. Lifting path options

### 5.1 `MCInst` from `MCDisassembler` for ARM (VERIFIED working)

There is **no** `llvm::createARMDisassembler` in the installed public headers — that symbol lives
only in the LLVM source tree at `llvm/lib/Target/ARM/Disassembler/ARMDisassembler.h` (target-private).
The public route is the target registry, and it works (probe in §1.7):

```cpp
Triple TT("armv7s-apple-ios6.1.3");                 // also "thumbv7s-apple-ios6.1.3"
// VERIFIED: getArchName()=="armv7s", getSubArch()==26 (Triple::ARMSubArch_v7s), getOSName()=="ios6.1.3"
std::string Err;
const Target *T = TargetRegistry::lookupTarget(TT, Err);          // MC/TargetRegistry.h:519 area
MRI = T->createMCRegInfo(TT);                                     // MC/TargetRegistry.h:447
MAI = T->createMCAsmInfo(*MRI, TT, MCOpts);                       // MC/TargetRegistry.h:410
STI = T->createMCSubtargetInfo(TT, "swift", "");                  // MC/TargetRegistry.h:462
MII = T->createMCInstrInfo();                                     // MC/TargetRegistry.h:432
MCContext Ctx(TT, *MAI, *MRI, *STI);                              // MC/MCContext.h:379
Dis = T->createMCDisassembler(*STI, Ctx);                         // MC/TargetRegistry.h:519
MIA = T->createMCInstrAnalysis(MII);                              // MC/TargetRegistry.h:440
auto S = Dis->getInstruction(Inst, Size, Bytes, Address, nulls());// MC/MCDisassembler/MCDisassembler.h:135
```

* `MCDisassembler::DecodeStatus { Fail = 0, SoftFail = 1, Success = 3 }`
  (`MCDisassembler.h:109`). `Size` is always written (bytes consumed even on `Fail`); loop with
  `Off += Size ? Size : <min instruction length>`.
* CPU/features: **use `createMCSubtargetInfo(TT, /*CPU=*/"swift", /*FS=*/"")`; do not rely on the
  `armv7s` triple alone.** VERIFIED feature matrix (same behaviour in the assembler and in the
  disassembler — `sdiv r0,r1,r2` / `vfma.f32` / `vcvtb.f32.f16` vs the bytes `11 f2 10 e7` /
  `81 0a a0 ee` / `40 0a b2 ee`):

  | subtarget config for `-triple=armv7s-apple-ios6.1.3` | `sdiv` (ARM mode) | `vfma.f32` | `vcvtb.f32.f16` |
  |---|---|---|---|
  | bare triple | reject | reject | reject |
  | `-mattr=+hwdiv` | reject | reject | reject |
  | `-mattr=+vfp4` | reject | **accept** | **accept** |
  | `-mattr=+hwdiv-arm` | **accept** | reject | reject |
  | `-mattr=+vfp4,+hwdiv-arm` | **accept** | **accept** | **accept** |
  | `-mcpu=swift` | **accept** | **accept** | **accept** |

  Notes: the divide features are **strictly mode-scoped** — `hwdiv-arm` gates ARM-mode `sdiv`/`udiv`
  and `hwdiv` gates the Thumb forms; each fails on the other mode with the confusing
  `invalid instruction, any one of the following would fix this` error. (Verified in both
  directions: I measured ARM mode, and `Armv7Abi` independently reproduced the Thumb mirror image —
  `+hwdiv` accepts Thumb `sdiv r0,r1`, `+hwdiv-arm` does not.) So a mode-switching lifter needs
  **both**, and `-mcpu=swift` supplies both; there is no single feature name that covers both modes.
  `+fp16` is *not* needed in 23.1.3 — the half-float conversion instructions ride on `+vfp4`; and
  `+v7s` is **not** a valid `-mattr` feature name (`'+v7s' is not a recognized feature for this
  target (ignoring feature)`), i.e. the arch-extension string in
  `TargetParser/ARMTargetParser.def:213` is not usable through `-mattr`. In the *disassembler* the
  failure mode is silent compared to the assembler: `getInstruction()` returns `Fail`/`SoftFail`
  ("invalid instruction encoding" in `llvm-mc -disassemble`), so a mis-configured `MCSubtargetInfo`
  makes the lifter classify valid A6 code as an illegal instruction instead of crashing loudly.
  The A6/`armv7s` ("Swift") part has VFPv4 with fused multiply-add, FP16 conversions, integer
  divide and NEON; clang gets this right only because `-target armv7s-apple-ios…` implies
  `-target-cpu swift`. A peer measured the clang-side feature diff and the 2012 `ProcSwift` TableGen
  definition: see `docs/research/apple-armv7-abi.md` §2 (independently reproduced; the codegen
  consequences — `vmla` vs `vmul`+`vadd`, inlined `sdiv`/`udiv` — are covered there).
* Opcode identity: `MCInstrInfo::getName(unsigned Opcode)` (`MC/MCInstrInfo.h:96`) returns the
  TableGen name as a `StringRef` at **runtime** — `getNumOpcodes()` == 4531 for ARM here, and
  `getName()` gives `PHI`, `ADDrr`, `LDR_PRE_IMM`, `VADDS`, `Bcc`, `tB`, `tMOVSr`, `t2MRC`, … This
  means you can build a sparse opcode->handler table keyed by name (or by the numeric opcode with a
  name lookup for diagnostics) **without shipping or generating any ARM `.td`-derived headers**.
  Numeric opcode values are stable within one LLVM release but are *not* an ABI; do not persist them
  (persist names).
* Register identity: `MCRegisterInfo::getName(MCRegister)` (`MC/MCRegisterInfo.h:522`) — VERIFIED
  `74 -> "R0"`, `77 -> "R3"`, `87 -> "S0"`, `getProgramCounter() == 15`.
* Operand conventions observed for ARM (VERIFIED with `llvm-mc --show-inst` and the probe):
  * `ADDrr` 818: `R0 R1 R2 #14 <none>` — the last two operands are the **condition code**
    (`ARMCC::AL == 14`) and the predication-CPSR register (usually "no register").
  * `Bcc` 853 (`fd ff ff ea` at 0x1000): `#-12 #14 <none>`; `isBranch=1 isConditionalBranch=1
    isTerminator=1`; `evaluateBranch` -> `0x0ffc`.
  * `LDR_PRE_IMM` 992 (`04 00 b1 e5`): `R0 R1 R1 #4 #14 <none>` — pre-indexed writeback is encoded
    in the *opcode*, and the base register appears twice (load dest + writeback dest).
  * `VADDS` 2184 (`81 0a 30 ee`): `S0 S1 S2 #14 <none>`; VFP/NEON registers come back as `S0/S1/...`.
  * Thumb `tLSRri` (`tLSRri ... R1 CPSR R0 #10 #14 <none>`): the **flag-setting** operand is an
    explicit `CPSR` register operand; `tMOVSr` has none. `tB` is `[br=1 cond=0 uncond=1 term=1]`.
  * `BLX` register form (`33 ff 2f e1`) is `isCall=1` and does **not** resolve via `evaluateBranch`.
  All of this means: the lifter needs a per-`MCInstrDesc` table of "which operand is the condition /
  which is the writeback / which is the S-flag", and for ARM the condition lives in the MCInst even
  for non-branch instructions (predication!). Treat `#14` as AL and the trailing predication register
  as unused when it is `<none>`.

### 5.2 TableGen: opcode enums and operand tables

* `llvm-tblgen --help` (23.1.3) offers, among ~50 actions: `-print-enums` (with `--class=<name>`),
  `-gen-instr-info` (`--instr-info-expand-mi-operand-info`), `-gen-disassembler`
  (`--specialize-decoders-per-bitwidth`, `--use-fn-table-in-decode-to-mcinst`, …),
  `-gen-asm-matcher`, `-gen-asm-writer`, `-gen-emitter`, `-gen-register-info`, `-gen-subtarget`,
  `-gen-dag-isel`, `-gen-global-isel`, `-gen-instr-docs`, `-gen-arm-target-def`, …
* **`-gen-instr-semantics` does not exist.** It is not in `llvm-tblgen --help` for 23.1.3 and there
  is no reference to such an action in LLVM's TableGen docs; any article claiming LLVM ships an
  instruction-semantics backend is wrong (`UNVERIFIED:` it may exist in some fork; not upstream, not
  in any release I can check). Do not plan around it.
* The ARM `.td` files (`ARM.td`, `ARMInstrInfo.td`, `ARMInstrThumb2.td`, `ARMInstrVFP.td`,
  `ARMInstrNEON.td`, `ARMScheduleSwift.td`, …) **are not installed** by the distro package. To run
  TableGen you need the matching source tree, e.g.
  `git clone --depth 1 --branch llvmorg-23.1.3 https://github.com/llvm/llvm-project` and then
  `llvm-tblgen -I llvm/lib/Target/ARM -I llvm/include -print-enums -class=Instruction llvm/lib/Target/ARM/ARM.td`.
  Practical guidance: prefer §5.1's runtime `MCInstrInfo`/`MCInstrDesc` over generating our own
  headers, because it is version-matched to the linked dylib by construction. Generate tables only
  for things the runtime API does not expose (e.g. a bulk operand-semantics annotation).
* The runtime `MCInstrDesc` surface you actually want (`MC/MCInstrDesc.h`):
  `getNumOperands()` (:238), `operands()` -> `const MCOperandInfo *`, `hasDefOfPhysReg(Inst, Reg, MRI)`,
  `getOpcode()`, `TSFlags` (:216; ARM uses `ARMII::FormMask`/`ARMII::ThumbFrm` from the target-private
  `llvm/lib/Target/ARM/MCTargetDesc/ARMBaseInfo.h` (not installed) to distinguish ARM/Thumb forms), and the classification bits (`MCInstrDesc.h:270-330`): `isPseudo`, `isReturn`,
  `isCall`, `isBarrier`, `isTerminator`, `isBranch`, `isIndirectBranch`, `isConditionalBranch`,
  `isUnconditionalBranch`. Operand kinds are `MCOI::OperandType` (`MCInstrDesc.h:59`):
  `OPERAND_UNKNOWN=0`, `OPERAND_IMMEDIATE=1`, …, `OPERAND_PCREL=4`.

### 5.3 Control-flow recovery: `MCInstrAnalysis`

```cpp
// MC/MCInstrAnalysis.h:31
class llvm::MCInstrAnalysis {
  virtual bool isBranch(const MCInst&) const;                 // default: MCInstrDesc::isBranch
  virtual bool isConditionalBranch(const MCInst&) const;
  virtual bool isUnconditionalBranch(const MCInst&) const;
  virtual bool isIndirectBranch(const MCInst&) const;
  virtual bool isCall(const MCInst&) const;
  virtual bool isReturn(const MCInst&) const;
  virtual bool isTerminator(const MCInst&) const;
  virtual bool mayAffectControlFlow(const MCInst&, const MCRegisterInfo&) const;
  virtual bool evaluateBranch(const MCInst &Inst, uint64_t Addr, uint64_t Size, uint64_t &Target) const;
  virtual std::optional<uint64_t> evaluateMemoryOperandAddress(const MCInst&, const MCSubtargetInfo*, uint64_t Addr, uint64_t Size) const;
  virtual void updateState(const MCInst&, const MCSubtargetInfo*, uint64_t Addr);  // pc-relative chains
  virtual void resetState();
};
```

ARM implementation (source-verified at tag 23.1.3:
`llvm/lib/Target/ARM/MCTargetDesc/ARMMCTargetDesc.cpp:181` (free function) and `:~394-424` (`ARMMCInstrAnalysis`)):

```cpp
uint64_t ARM_MC::evaluateBranchTarget(const MCInstrDesc &InstDesc, uint64_t Addr, int64_t Imm) {
  uint64_t Offset = ((InstDesc.TSFlags & ARMII::FormMask) == ARMII::ThumbFrm) ? 4 : 8;
  if (InstDesc.getOpcode() == ARM::tBLXi) Addr &= ~0x3;      // Thumb BLX(i) aligning to ARM code
  return Addr + Imm + Offset;
}
// ARMMCInstrAnalysis::evaluateBranch: scan operands, take the one whose MCOI type is OPERAND_PCREL,
// compute Target = Addr + Imm + (ThumbFrm ? 4 : 8). Bcc with cc == ARMCC::AL counts as unconditional.
// ARMMCInstrAnalysis::isUnconditionalBranch / isConditionalBranch special-case ARM::Bcc + ARMCC::AL.
```

Consequences for the lifter:

* For `B`/`Bcc`/`BL`/`tB`/`tBL`/`t2B`/`t2BL`, the MCInst operand is the **raw PC-relative byte
  offset**; use `evaluateBranch` (or the two-line formula) to get the target. VERIFIED numerically:
  probe at `0x1000` with `#-12` -> `0x0ffc`; `tB #258` at `0x1006` -> `0x110c`; `tB #-1182` at
  `0x100a` -> `0x0b70`.
* `evaluateBranch` returns **false** for register/table branches (`BX`, `BLX reg`, `tBX`, `tBB`,
  `tBH`, `LDR pc, [..]`). Those need your own handling: indirect target set + `isIndirectBranch`,
  and for `tBB`/`tBH` you must materialize the jump table read.
* `LLVM's MC prInting of branch operands is inconsistent across tools` (VERIFIED): `llvm-objdump`
  prints the resolved absolute target (`b 0x0 <__text>  @ imm = #-0xc`), `llvm-mc -disassemble`
  prints the **raw** immediate (`b #-12`) because `PrintBranchImmAsAddress` is off in that path.
  Do not use `llvm-mc -disassemble` text as a target-address oracle.
* `MCInstrAnalysis::updateState` exists for architectures with multi-instruction PC-relative
  sequences (RISCV auipc/jalr, AArch64 adrp/ldr); ARM's implementation is the default no-op, so no
  state machine is needed for ARM.
* `MCDisassembler` has no built-in "next instruction" walk for ARM Thumb IT state in *your* code:
  the IT state lives in `MCSubtargetInfo` and the decoder updates it, so **reuse one
  `MCDisassembler`/`MCSubtargetInfo` instance for a linear Thumb sweep** and reset it at every new
  entry point (`MCSubtargetInfo::setFeatureBits`/`ToggleFeature` or simply re-create the object).
  The platform must also implement it at runtime (predicated execution is per-instruction state),
  and the guest/host mode switch (ARM<->Thumb) is triggered by the low bit of `BX`/`BLX` targets.

### 5.4 Is `llvm-mctoll` usable? (source-verified from microsoft/llvm-mctoll, master)

* Raises binaries **AOT to LLVM IR**; it is not a lazy JIT and not per-basic-block.
* Supported: `x86_64-linux` and `arm-linux` **ELF only**, verified in its README feature table
  (VarArgs/FuncProto/StackFrame/JumpTables/SharedLibs all marked; C++ **not**).
* Known issues (README): SIMD (SSE/AVX/**Neon**) cannot be raised; testing is mostly on
  clang-compiled Linux binaries.
* Build model: it is an in-tree LLVM tool (`llvm/tools/llvm-mctoll`), and it pins a *tested* LLVM
  commit in `llvm-project-git-commit-to-use.txt` — the pinned commit is
  `8dfdcc7b7bf66834a761bd8de445840ef68e4d1a` (Nov 17 2022, i.e. LLVM 16-era). It will not build
  against the system LLVM 23 without porting.
* Last upstream activity: commit feed shows the newest commit at **2024-06-18**
  (<https://github.com/microsoft/llvm-mctoll/commits/master.atom>).
* Verdict for `ios6-recomp`: **not reusable as-is** (Mach-O/iOS/armv7s unsupported, no SIMD, no
  laziness, stale LLVM pin), but its Arm32 raising code is a useful cross-check for your own
  lifter's instruction coverage (`UNVERIFIED:` I did not read its ARM lifter sources).

### 5.5 `llvm::object` Mach-O support for iOS 6 armv7/armv7s

Headers: `llvm/Object/MachO.h` (`llvm::object::MachOObjectFile`), `llvm/Object/MachOUniversal.h`
(`MachOUniversalBinary`), plus `MachOUniversalWriter.h`, `TapiFile.h`, `TapiUniversal.h`,
`BinaryFormat/MachO.h` (constants), `Object/RelocationResolver.h`, `Orc/MachO*.h` (platform support
for Apple hosts — not applicable here).

What you get:

* Full parsing of 32-bit Mach-O (also FAT/universal and 64-bit, and chained fixups):
  `MachOObjectFile` gives load commands, sections (`section_begin/end`), symbols, relocations
  (`relocation_iterator`, `section_rel_begin`), `getDyldInfo*`, exports trie, `MachORebaseEntry`,
  `MachOBindEntry`, `MachOChainedFixupEntry`, `MachOAbstractFixupEntry`, `DiceRef` (Apple's "DICE").
* Arch mapping: `static Triple::ArchType MachOObjectFile::getArch(uint32_t CPUType, uint32_t CPUSubType)`
  (`MachO.h:753`) and `static Triple getArchTriple(uint32_t CPUType, uint32_t CPUSubType,
  const char **McpuDefault = nullptr, const char **ArchFlag = nullptr)` (`MachO.h:754`);
  `MachOUniversalBinary::ObjectForArch::getCPUType()/getCPUSubType()` (`MachOUniversal.h:60+`).
* Constants: `MachO::CPU_TYPE_ARM = 12`, `MachO::CPU_SUBTYPE_ARM_V7 = 9`,
  `MachO::CPU_SUBTYPE_ARM_V7S = 11`, `CPU_SUBTYPE_ARM_ALL = 0` (`BinaryFormat/MachO.h:1626,1684-1699`).
  **armv7s is handled** — VERIFIED end-to-end: built a Mach-O object with
  `llvm-mc -filetype=obj -triple=armv7s-apple-ios6.1.3`; `file(1)` says `Mach-O armv7s object`;
  `llvm-readobj --file-headers` prints `Format: Mach-O arm / Arch: arm / CpuSubType: CPU_SUBTYPE_ARM_V7S (0xB)`;
  `llvm-objdump -d` disassembles it as ARM. `Triple("armv7s-apple-ios6.1.3")` round-trips with
  `getArchName() == "armv7s"` and `getSubArch() == Triple::ARMSubArch_v7s` (26).
* ARM-specific relocation interpretation exists for Mach-O: `llvm/lib/Target/ARM/MCTargetDesc/ARMMachORelocationInfo.cpp`
  (`createARMMachORelocationInfo`, wired by `createARMMCRelocationInfo` when `isOSBinFormatMachO()`),
  and `ARMMachObjectWriter.cpp` on the writer side.

What it does **not** give you:

* No relocation *application* to a memory image (that's `RuntimeDyld`/JITLink for host objects, or
  your own code for guest images). You must implement Mach-O ARM relocations (ARM_RELOC_VANILLA,
  ARM_RELOC_BR24, ARM_RELOC_HALF/_SECTDIFF, ARM_RELOC_PB_LA_PTR, ARM_THUMB_RELOC_BR22, …) yourself.
* No dyld, no shared-cache, no `LC_DYLD_INFO` execution, no ObjC runtime metadata handling, no
  code-signing/entitlements handling, no decryption of IPSW-encrypted payloads.
* No Mach-O *writing* for ARM in JITLink (only `MachO_arm64`/`MachO_x86_64` exist, see §1.3) — you do
  not need it if the JIT target is the host.
* `MachOObjectFile` is a reader over a `MemoryBuffer`; the iOS 6 kernel/userland artifacts must be
  extracted and decrypted before you get here (`UNVERIFIED`: exact IPSW extraction flow — that's the
  platform/prior-art tracks' problem, and `/home/theo/src/ios6-recomp/ipsw` is a regular file, not a
  directory, in this checkout).

---

## 6. Capstone: coverage, detail API, accuracy, and whether it can be the only decoder

Installed: **Capstone 5.0.9** (`/usr/include/capstone/capstone.h`, `arm.h`, `libcapstone.so`,
`cstool`), python binding `python-capstone 5.0.9-1.1` (module reports `5.0.7`). License: BSD-3-Clause.
Arch bits compiled in: `x86 arm arm64 mips ppc sparc sysz xcore m68k tms320c64x m680x evm wasm
mos65xx bpf riscv sh tricore` (from `cstool -v`).

Version context: the 5.0 line is the current stable; the `next` (6.0) line is where ARM was
re-synced ("ARM and PPC updated to LLVM 16", auto-sync reproducibility, ARM alias support, operand
access info, "several improvements to the API of the ARM/AArch64/PPC modules" — from
`ChangeLog` on branch `next`). 5.0.x ARM fixes visible in the 5.0 ChangeLog include
"ARM/AArch64: Add post-index to instruction info (#1937)", "correct register accesses for ARM's
sxtb/uxtb (#1968)", "Update python bindings to fix ARM in detail mode (#1961)". Practical point:
**5.0.x ARM decode tables are older than 6.0's and are not the LLVM-16 auto-synced ones.**

### 6.1 `cs_arm` detail API (VERIFIED from `/usr/include/capstone/arm.h`)

```c
typedef struct cs_arm_op {                 // arm.h:407
  int vector_index;                        // -1 if irrelevant
  struct { arm_shifter type; unsigned int value; } shift;
  arm_op_type type;                        // ARM_OP_REG / ARM_OP_IMM / ARM_OP_MEM / ARM_OP_FP / ARM_OP_CIMM / ARM_OP_PIMM / ARM_OP_SETEND / ARM_OP_SYSREG
  union { int reg; int32_t imm; double fp; arm_op_mem mem; arm_setend_type setend; };
  bool subtracted;  uint8_t access;  int8_t neon_lane;
} cs_arm_op;                               // arm_op_mem { base, index, scale, disp, lshift }

typedef struct cs_arm {                    // arm.h:440
  bool usermode; int vector_size; arm_vectordata_type vector_data;
  arm_cpsmode_type cps_mode; arm_cpsflag_type cps_flag;
  arm_cc cc;                               // condition for this insn (arm.h:33, ARM_CC_INVALID=0, EQ=1, ..., AL=15)
  bool update_flags;                       // S bit
  bool writeback; bool post_index;          // post_index only meaningful when writeback
  arm_mem_barrier mem_barrier;
  uint8_t op_count; cs_arm_op operands[36];
} cs_arm;
```

* `cc`, `update_flags`, `writeback`, `post_index` are exactly the fields a lifter needs for
  predication, flag writes, and addressing-mode side effects. VERIFIED with `cstool -d`:
  `ldr r0,[r1,#4]!` -> `Write-back: True`; `ldm r0!,{r1,r2,r3}` -> `Write-back: True`;
  `it eq` + `movs r0,#0` -> first insn `ID: 67 (it)`, `Code condition: 1`, second `Code condition: 1`
  with `Update-flags: True`.
* Groups (`arm.h:940+`): `ARM_GRP_JUMP` (=`CS_GRP_JUMP`), `ARM_GRP_CALL`, `ARM_GRP_INT`,
  `ARM_GRP_PRIVILEGE`, `ARM_GRP_BRANCH_RELATIVE`, plus feature groups (NEON, V4T, …, CRYPTO,
  DATABARRIER, DIVIDE, FPARMV8, MULTPRO, T2EXTRACTPACK, THUMB2DSP, TRUSTZONE).
  VERIFIED: `b #8` gets `branch_relative arm jump`; `svc` gets `int`; `push.w` gets `thumb2` etc.
  `ARM_GRP_BRANCH_RELATIVE` is reliable in 5.0.9 in my sampling; `CS_GRP_JUMP/BRANCH/CALL` are
  reliable for `b/bl/bx/blx/cbz/cbnz/tbb`, and `pop {..,pc}` is in `ARM_GRP_RET`-equivalent
  (`pop` writes PC; the changelog entry "#2216 Add RET group to pop instructions which write to the
  PC" indicates this was fixed on `next`, so verify it for the version you pin).
  `UNVERIFIED:` exact `CS_GRP_*` sets for 5.0.9's `tbb/tbh`, `blxns`, conditional-branch aliases —
  check with `cstool -d` on your own corpus before relying on groups for CFG recovery.
* IT blocks: Capstone keeps IT state inside the `cs_handle` across `cs_disasm_iter`/`cs_disasm`
  calls (VERIFIED: decoding `08 bf 00 20` as one buffer yields `it eq` then `moveq r0, #0`, and
  `08 bf 01 20 02 21` yields `moveq`/`movs` with the condition applied only to the first). It does
  **not** expose an explicit ITSTATE object; the applied condition is in each insn's `cc`. There is
  **no** mode auto-switch: `CS_MODE_ARM` vs `CS_MODE_THUMB` is fixed per handle. `BL`/`BLX` targets
  are not used to flip the mode; your driver must do that.
* `svc` immediate: `ARM_OP_IMM` on the first operand of `ARM_INS_SVC` (VERIFIED `svc #0x80` from
  `80 00 00 ef` / `80 df`).
* Capstone's branch *operand* is the **absolute, PC-biased target computed from the address you pass
  to `cs_disasm`** (VERIFIED: bytes `00 00 00 ea` at 0 -> `b #8`; `fd ff ff ea` at 0 ->
  `b #0xfffffffc`; `80 b9` at 0 -> `cbnz r0, #0x24`; `00 f0 00 f8` at 0 -> `bl #4`). The same
  instruction decoded at a different address yields a different immediate. LLVM's MCInst keeps the
  raw offset instead. Two consequences: (i) never mix the two conventions in one lifter, (ii) if you
  cache Capstone output keyed by encoding bytes you must also key by address (or re-derive the
  offset by subtracting the PC bias).

### 6.2 Measured agreement with LLVM's MC decoder (my own differential, VERIFIED)

Method: Python + `capstone` (5.0.7) vs `llvm-mc -disassemble -triple=armv7s-apple-ios6.1.3
-mcpu=swift` (23.1.3), 1500 random 4-byte words per mode; comparison unit = the *first* instruction
decoded from that 4-byte window (fresh state on both sides); mnemonic compared after stripping
`.w`/`.n` and mapping a small alias table.

ARM mode, run B (`-mcpu=swift`, 1500 words): 1293 identical mnemonics, 201 rejected by both,
**0 cases where LLVM decoded and Capstone failed**, 2 cases where Capstone decoded and LLVM rejected,
4 mnemonic disagreements. All six are listed here (bytes in memory order):

```
66 8b cd fd | llvm: (invalid)                        | cs: stc2l p11, c8, [sp, #0x198]
16 2a 48 fd | llvm: (invalid)                        | cs: stc2l p10, c2, [r8, #-0x58]
64 59 56 ce | llvm: cdpgt p9, #5, c5, c6, c4, #3     | cs: vnmlagt.f16 s11, s12, s9
46 c9 fd 0e | llvm: cdpeq p9, #15, c12, c13, c6, #2   | cs: vcvtreq.s32.f16 s25, s12
8b 69 86 7e | llvm: cdpvc p9, #8, c6, c6, c11, #4    | cs: vdivvc.f16 s12, s13, s22
0c d9 b2 be | llvm: cdplt p9, #11, c13, c2, c12, #0   | cs: vmovlt.f16 s26, #1.4e1
```

For contrast, run A used the bare `armv7s` triple (no `-mcpu=swift`, i.e. **my** feature-set mistake),
1200 words, and produced a *different* extra set — these vanish once the subtarget is right:

```
07 da e6 de | llvm: (invalid) | cs: vfmale.f32 s27, s12, s14        <- VFPv4-only
47 7a b2 4e | llvm: (invalid) | cs: vcvtbmi.f32.f16 s14, s14       <- FP16-only
cb bb b3 ae | llvm: (invalid) | cs: vcvttge.f16.f64 s22, d11       <- FP16-only
```

Interpretation (important): the residual run-B differences are **not** bit-level mis-decodes of
ordinary ARMv7 code. They are (a) ambiguity in the *undefined / coprocessor encoding space*
(`cdp<cc> p9…` vs Capstone's `v…<cc>.f16`, i.e. two different readings of the coproc 9/10/11 +
condition-field combinations), and (b) `stc2`/`ldc2` in the `cond=1111` unconditional space, which
Capstone accepts and LLVM rejects here. Both classes must be resolved by an explicit, documented
policy in the lifter, and they are exactly what the differential harness in §6.3 exists to pin down.
The general lesson stands: **Capstone has no subtarget/ISA-version gating** (run A shows how easily
that produces "valid" decodes of instructions the A6 cannot execute; run-B Thumb shows the same for
ARMv8-M `blxns`/`bxns`), while LLVM's MC decoder is gated by the `MCSubtargetInfo` feature set and
therefore rejects them.

Thumb mode (1500 random 4-byte windows): **0 both-reject**, 1432 identical mnemonics, 6 mnemonic
disagreements, and 62 apparent "LLVM decodes / Capstone rejects" cases that turned out to be an
artifact: in `-disassemble` mode `llvm-mc` resynchronizes at **byte** granularity (it emits
"invalid instruction encoding" warnings and then decodes a valid instruction from an *odd* offset),
while Capstone only ever decodes at halfword boundaries. E.g. bytes `d6 ea 21 59`: llvm-mc warns at
offset 0 and 3 and prints `movs r1, #234` (which is the halfword `ea 21` = `0x21EA` at offset 1);
Capstone yields nothing, which is the *more correct* behaviour for sequential Thumb decoding.
The 6 remaining Thumb disagreements include `blxns`/`bxns` (ARMv8-M) which Capstone accepts and LLVM
rejects for armv7s.

Formatting-only differences (do not treat as decoder bugs, but do normalize them in any diff
harness): hex vs decimal immediates (`#0x1a6` vs `#422`), register aliases (`ip`/`r12`, `sp`/`r13`,
`lr`/`r14`, `sl`/`r10`, `sb`/`r9`, `fp`/`r11`), ADR folding (`adr r0, #16` in LLVM-mc vs
`add r0, pc, #0x10` in Capstone), `movw r0, #0x1234` vs `movw r0, #4660`, and the branch-target
convention (§6.1).

Additionally, both decoders agree on the "permanently undefined" 32-bit Thumb pattern I tested
(`ff ff 70 47` -> `vqshl.u32 q10, q8, #31` in **both** — LLVM also decodes it; the 2-byte prefix
`ff ff` alone is rejected by both).

### 6.3 Is Capstone usable as the ONLY decoder?

My verdict: **yes for *coverage* of armv7s user-mode code, no for *correctness guarantees*.** It is
the only one of the two with a stable, documented, versioned C API and rich detail semantics, and in
my sampling it never failed on an instruction LLVM accepted (ARM), and never produced a wrong
mnemonic for a normal ARMv7-A/Thumb-2 instruction that LLVM decoded identically. But:

1. It has no ISA/subtarget gating, so it will decode instructions the A6 (armv7s: ARMv7-A +
   VFPv4 + NEON + hwdiv, no ARMv8) cannot execute. Those must be rejected by *your* feature table —
   otherwise an undefined-instruction trap in the guest turns into a silently "translated" block.
2. Both decoders disagree in the undefined/coprocessor space (`cdp` vs VFP/FP16 with `p9..p11`,
   `stc2/ldc2` in cond=1111). Neither is "right"; you must pick a policy and pin it (this is
   exactly the kind of thing that must be a *tested decision*, not an accident).
3. Capstone's API is address-sensitive for PC-relative operands and preserves IT/mode state inside a
   handle; naive use (per-instruction fresh handles, or reusing a handle across a mode switch) gives
   wrong results.
4. Decoder bugs are found by *differential* testing, not by reading changelogs. Capstone 5.0.9's ARM
   backend predates the LLVM-16 auto-sync in the 6.0 line, so treat the 5.0.x ARM decoder as
   "good but drifting".

Concrete cross-validation plan (do this before trusting either decoder):

1. **Build the oracle from LLVM**, since LLVM 23 is already a hard dependency: an offline tool that
   decodes a byte range with the §5.1 recipe (CPU `swift`, features `+v7s,+vfp4,+fp16,+neon,+hwdiv`)
   and emits `(offset, size, opcode_name, operand_kinds, operand_values, is_branch/is_call/
   is_terminator, evaluateBranch target)` as JSONL. Never compare *printed text*: compare these
   tuples, with operands normalized to `(kind, value)` and register names via
   `MCRegisterInfo::getName`.
2. **Same-address alignment**: run both decoders as *linear sweeps* over the same byte range, and
   compare only when both decoders advance by the same size; on mismatch, record
   `{bytes, llvm_tuple, cs_tuple}` and resynchronize at `offset + min(size_llvm, size_cs)` (or at the
   next 2-byte boundary for Thumb). Do not compare "first instruction in a 4-byte window" (that is
   what produced my 62 false positives).
3. **Corpora**: (a) the actual iOS 6 binaries you're lifting; (b) a fuzz corpus of ~1e6 random words
   per mode plus ~1e5 *biased* corpora (valid opcode prefix + random tail, all 16 condition codes,
   all `IT` masks, VFP/NEON register banks, LDM/STM register lists with `pc`); (c) a corpus of
   hand-assembled instruction/encoding round-trips from LLVM's own `test/MC/ARM/` tests.
4. **Triage policy**: classify every difference as (i) formatting/alias — normalize away;
   (ii) ISA-gating — Capstone accepts, LLVM rejects; decide per-instruction with a documented
   allow/deny list derived from the armv7s feature set (authority =
   `MCSubtargetInfo` feature bits, and the Swift CPU feature list in `ARMProcessors.td`);
   (iii) genuine decode disagreement on *defined* encodings — each one becomes a permanent
   regression test with a hand-checked answer from the ARM ARM.
5. **Freeze an allowlist**: the runtime lifter accepts an opcode only if it appears in the
   `(llvm_name, capstone_name)` mapping that the differential has proven identical, plus explicitly
   reviewed exceptions. Unknown/undefined encodings must go to a trap path (guest `SIGILL`
   semantics), never to a best-effort translation.
6. Since LLVM ships `llvm-mc`/`llvm-objdump` in this environment, the same oracle is available as a
   CLI for quick triage — but remember §6.2: `llvm-mc -disassemble` prints raw branch offsets and
   resyncs at byte granularity, so use `llvm-objdump -d --triple=...` on a thrown-together object, or
   the in-process oracle, when you need trustworthy offsets/targets.

---

## 7. Oracle options for instruction-level differential testing

Legend for "state dump": what you can observe *per instruction* (register file + flags + memory).

| Oracle | ARMv7 support | Per-instruction state | License | Status here |
|---|---|---|---|---|
| `qemu-arm` (user-mode) | full, incl. VFP/NEON | yes, via `-one-insn-per-tb -d in_asm,cpu,nochain` | GPL-2.0-only | installed (11.1.2), **recipe verified** |
| Unicorn 2.x | full (QEMU core) | yes, `UC_HOOK_CODE` + `uc_reg_read_batch`/`uc_context_save`/`uc_mem_read` | GPLv2 (+ LGPL2 for parts) | not installed; `extra/unicorn 2.1.4-2` |
| dynarmic (A32) | v3–v8 (A32 subset), AArch64 | yes: `Step()` + `Regs()/ExtRegs()/Cpsr()` or `UserCallbacks` | 0BSD | not packaged; upstream `azahar-emu/dynarmic` |
| `llvm-mc` + `lli` | disassembler yes, **no ARM execution** | n/a | Apache-2.0 WITH LLVM-exception | installed (`llc`/`lli` can't execute ARM on x86) |
| hand-written reference interpreter | whatever you implement | full by construction | yours | not yet |

### 7.1 `qemu-arm` — verified recipe

```sh
# Build an ARM Linux test binary with the *same* LLVM you use for lifting:
llvm-mc -filetype=obj -triple=armv7-linux-gnueabihf -o t.o t.s
ld.lld -o t -e _start -static t.o
# Full per-instruction trace: one instruction per translation block + register dump, no TB chaining
qemu-arm -one-insn-per-tb -d in_asm,cpu,nochain -D trace.log ./t
```

VERIFIED output shape (QEMU 11.1.2):

```
----------------
IN:
0x000200b8:  e2901000  adds     r1, r0, #0

R00=00000000 R01=00000000 ... R13=40800290 R14=00000000 R15=000200b8
PSR=00000010 ---- A usr32
----------------
IN:
0x000200bc:  e2502001  subs     r2, r0, #1
...
PSR=40000010 -Z-- A usr32
```

* `-d cpu` prints the register file and the PSR **with flag letters** (`-Z--`, `N---`, …), dumped
  *before* each TB executes; combined with `-one-insn-per-tb` (and `-d nochain`) this is exactly
  "full state after each instruction".
* Add `fpu` to include the FPU/SIMD registers, `mmu`/`page` for memory mapping events, `strace` for
  syscalls, `guest_errors`/`unimp` for unimplemented behaviour, `plugin` for TCG plugins.
* `-singlestep` was **removed** in this version (`qemu: unknown option 'singlestep'`) — use
  `-one-insn-per-tb` (VERIFIED).
* Memory state per instruction is not in `-d cpu`; get it from the TCG plugin API or by using the
  GDB stub (`-g 1234`) and a scripted client, or by comparing final memory images. For
  per-instruction memory diffs, Unicorn/dynarmic callbacks are easier.
* QEMU user-mode only runs Linux/ELF guests; to run iOS 6 Mach-O you must wrap the guest (load the
  Mach-O slices yourself with `MachOObjectFile`, map them at their `vmaddr`, implement
  `syscall`/`svc` via QEMU's `-strace`-observed ABI, or use `qemu-system-arm` with a stub kernel).
  `UNVERIFIED:` nobody has tried a Mach-O-in-qemu-arm-user wrapper here; plan to test the wrapper on
  a small armv7s Mach-O (§5.5) before betting on it.

### 7.2 Unicorn (API verified from `include/unicorn/unicorn.h` and `include/unicorn/arm.h`)

```c
uc_engine *uc;
uc_open(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_THUMB | UC_MODE_LITTLE_ENDIAN, &uc);  // arch id 1, mode bits
uc_mem_map(uc, base, size, UC_PROT_ALL);
uc_mem_write(uc, addr, buf, len);
uc_hook hh;
uc_hook_add(uc, &hh, UC_HOOK_CODE /*1<<2*/, cb, user, begin, end /*variadic*/);
uc_reg_read(uc, UC_ARM_REG_R0, &v);            // also uc_reg_read_batch(uc, regs, vals, n)
uc_mem_read(uc, addr, buf, size);
uc_context_save(uc, ctx);                      // whole-CPU snapshot, cheap
uc_emu_start(uc, begin, until, timeout, count);  // count = instructions to run
```

* ARM register ids: `UC_ARM_REG_R0..R15`, `UC_ARM_REG_CPSR`, `UC_ARM_REG_SPSR`, `UC_ARM_REG_S0..`,
  plus NEON/FPU ids (`uc_arm_reg` enum in `include/unicorn/arm.h`); `UC_ARM_REG_SP/LR/PC` aliases.
* Other hooks: `UC_HOOK_BLOCK` (1<<3), `UC_HOOK_MEM_READ/WRITE/FETCH`, `UC_HOOK_INSN` for `svc`.
  Stops at `until` or after `count` instructions, so the per-instruction loop is
  `uc_emu_start(uc, pc, 0, 0, 1)` or a `UC_HOOK_CODE` callback that snapshots
  `uc_context_save` + touched memory.
* Because it *is* QEMU's TCG core, its decode/execution matches `qemu-arm` (same upstream, and
  Unicorn's ARM backend is patched for embedding) — but it is a *separate* code base with its own
  release cadence, so do not assume bug-for-bug identity with your distro's `qemu-arm`.
* License: GPLv2/LGPL2 (verified from `unicorn-engine/unicorn` `COPYING` / `COPYING.LGPL2`).
  Use it as a **separate process / test-only binary**, never linked into a redistributed runtime if
  you want a permissive-licensed product.

### 7.3 dynarmic (A32) — verified API surface from `azahar-emu/dynarmic` (branch `master`)

* Not packaged on Arch (`pacman -Ss dynarmic` -> nothing); builds from source with CMake; upstream is
  `https://github.com/azahar-emu/dynarmic` (the maintainer's personal fork `wheremyfoodat/dynarmic`
  also exists; `merryhime/dynarmic` and `dynarmic/dynarmic` return 404 for me).
* License: 0BSD ("Permission to use, copy, modify, and/or distribute this software for any purpose
  with or without fee is hereby granted." — `LICENSE.txt`). **This is the only permissively licensed
  ARM emulator in this list**, which matters if you ever want to ship it.
* Guest architectures supported (README): v3, v4, v4T, v5TE, v6K, v6T2, **v7A**, 32-bit v8, 64-bit v8;
  hosts: x86-64 and AArch64 only. `ArchVersion` selects the guest version
  (`src/dynarmic/interface/A32/arch_version.h`).
* State dump / stepping (`src/dynarmic/interface/A32/a32.h`):
  `HaltReason Jit::Run(); HaltReason Jit::Step(); std::array<u32,16>& Regs();
  std::array<u32,64>& ExtRegs(); u32 Cpsr() const; void SetCpsr(u32); u32 Fpscr();
  void SetFpscr(u32); void ClearCache(); void InvalidateCacheRange(u32 start, size_t len);
  void Reset(); void HaltExecution(HaltReason); void ClearHalt(HaltReason); bool IsExecuting();`
  — `Step()` plus reading `Regs()/Cpsr()/ExtRegs()` after each step is a direct per-instruction
  state dump.
* Hooks (`src/dynarmic/interface/A32/config.h`, `struct UserCallbacks : TranslateCallbacks`):
  `MemoryRead8/16/32/64`, `MemoryWrite8/16/32/64`, `MemoryWriteExclusive8/16/32/64`,
  `IsReadOnlyMemory`, `MemoryReadCode`, `InterpreterFallback(VAddr pc, size_t num_instructions)`,
  `CallSVC(uint32_t swi)`, `ExceptionRaised(VAddr pc, Exception)`,
  `InstructionSynchronizationBarrierRaised()`, `AddTicks(uint64_t)`, and the IR-level
  `PreCodeReadHook`/`PreCodeTranslationHook`. You supply the memory system, so per-instruction
  memory diffing is trivial (record writes in `MemoryWrite*`).
* Caveat: A32 in dynarmic is the classic JIT recompiler; instructions it cannot translate go through
  `InterpreterFallback`, so any trace you collect must check that callback to know what was
  interpreted vs translated.

### 7.4 `llvm-mc` + `lli`

* `lli` cannot execute ARM on an x86-64 host (no ARM target machine for the host ABI); `lli` is
  useless as an ARM oracle. It is useful only for testing *your host-side* lifted IR in isolation
  (`lli -jit-kind=orcjit`-era options have changed; `UNVERIFIED:` current `-jit-kind` spelling in 23).
* `llvm-mc` is an *encoder/decoder* oracle, not an execution oracle: `-show-encoding` (assemble,
  print bytes), `-disassemble` (decode bytes), `--show-inst` (dump `MCInst` with opcode number/name
  and operand kinds — very useful when writing the lifter's tables). Its two gotchas are in §5.3/§6.2
  (raw branch immediates in `-disassemble`; byte-granular resync).
* `llvm-mc` + `ld.lld` *are* how you build the small ARM Linux ELF binaries that `qemu-arm` executes
  (§7.1) — verified.

### 7.5 Hand-written reference interpreter

Needed eventually: it is the only oracle that can implement the *iOS* half of semantics (Mach-O
loading, `svc` => BSD syscall/XNU trap surface, ObjC/Foundation shims) and the only one that can be
shipped permissively. It is also the only oracle whose per-instruction state you fully control
(flags, memory ordering, unaligned-access behaviour). Treat it as a *third* opinion, not the first
one: write it *after* the lifter exists, driven by differential failures found with §7.1/§7.3 —
otherwise you spend months building a second ARM emulator instead of a recompiler.

### 7.6 What to compare (matters more than which oracle)

* Compare **traces**, not outcomes: after every guest instruction compare `(r0..r15, CPSR flags,
  FPSCR, the destination registers/memory the instruction wrote)`. Divergences in flag computation
  and in writeback/addressing-mode side effects are where lifters break; a "final result matches"
  test hides them for a long time.
* Bootstrap the trace on *block entries*, not at the entry point, and seed the state by fuzzing
  (`random registers`, `random flag combinations`, `random memory`) — ARM flag semantics for
  `ADC/SBC/RSC`, shift-carry (`r15`-relative shifts, `MOVS r0, r1, LSR #32`), and `LDM/STM` with
  `pc` in the list are the classic bug farms.
* Always run the oracle **twice** with different input states before accepting a candidate test
  case, otherwise you will "fix" the lifter to match an emulator bug.

---

## 8. Recommendation

### 8.1 Decoder

Use **LLVM's MC layer in-process as the primary decoder** (the §5.1 recipe, CPU `swift`, features
equivalent to `+v7s,+vfp4,+fp16,+neon,+hwdiv`), and **Capstone 5.0.9 as the second decoder in the
offline differential harness** plus as a fallback path behind a feature gate. Rationale: LLVM is
already a hard dependency (no new license, no new ABI), it is version-matched to the `MCInstrInfo`
tables you need for the operand semantics, it is subtarget-accurate for `armv7s`, and every
instruction it accepts is by construction an instruction the *host* LLVM backend can also emit for
the equivalent operation. Capstone's advantage (stable, documented C API with `cc`/`writeback`/
`post_update` detail) is not decisive for us, because we need per-operand semantics anyway and we
get them from `MCInstrDesc` + our own opcode table; and its lack of ISA gating is actively dangerous
(§6.2, §6.3).

### 8.2 Lifting representation

Per **guest basic block**:

1. Decode with one long-lived `MCDisassembler`/`MCSubtargetInfo` per mode (ARM/Thumb), rebuilding the
   IT/mode state at each block entry.
2. Lower to a small typed, SSA-ish IR of explicit "micro-ops" (each micro-op = one architectural
   effect: `Reg write`, `Mem load/store`, `Flag write`, `PC write`, `Trap`), keyed by `MCInstrDesc`
   classification and our per-opcode semantics table. Keep the guest PC of every micro-op.
3. Lower that IR to **one LLVM IR function per guest block**, named `g$<guest_hex>`, with the flags
   (`NZCV`) and the guest register file as explicit SSA values threaded through the block
   (`@nzcv`, `@r0..@r15` globals or a `%cpu` struct pointer argument). The function returns the
   successor block's guest address (or sets `%cpu.next_pc`), so the dispatcher can chain without a
   per-block call into C++.
4. Add `-O1`/`-O2` + `alwaysinline` on the flag helpers; do *not* try to be clever about
   optimizations that span blocks initially (see counter-argument 1).

### 8.3 ORC layering

* `LLJIT` (JITLink `ObjectLinkingLayer` on x86-64 ELF, CodeModel::Small + PIC by default — §2.4),
  with `setMemoryManagerCreator()` pointing at a **custom `JITLinkMemoryManager`** that serves a
  single reserved arena so that `host = f(guest)` (§4.1, §2.8).
* One `MaterializationUnit` per guest block, defined on demand by a custom `DefinitionGenerator`
  attached to the guest-code `JITDylib`; block-level laziness is ORC's job, not a
  `CompileOnDemandLayer`'s job.
* Stable block-entry addresses + `JITLinkRedirectableSymbolManager` (`createRedirectableSymbols`,
  `redirect`) for patching a block's entry to a recompiled version, and to implement the
  "self-modifying code" / hooking cases; keep `ResourceTracker`s per block so a block can be
  invalidated, but note that removal frees the address — only do that for blocks you know nobody can
  branch into anymore (or make every block entry a redirectable symbol so the address survives).
* Persist `.o` objects via `ObjectCache`/`TMOwningSimpleCompiler` (§2.10) with a cache key that
  includes the lifter version and guest-address/block hash.
* `EPCDynamicLibrarySearchGenerator::GetForTargetProcess` (or a custom generator) for host imports;
  a `DefinitionGenerator` for the guest-ABI shim library (libSystem/foundation entry points).

### 8.4 The three strongest counter-arguments against this recommendation

1. **LLVM MC gives you decoding and control-flow, not semantics — and its ARM surface is
   target-private and unstable.** `createARMDisassembler` is not even public (§5.1); the opcode
   *enum* is unstable across releases; operand layouts for ARM memory/writeback forms are
   "whatever the TableGen backend emitted" (`LDR_PRE_IMM`'s doubled base register, the trailing
   condition + predication-CPSR operands, the `CPSR` operand on Thumb flag-setting forms). You will
   hand-write ~all of the semantics table anyway, so Capstone's richer documented detail
   (`cc/update_flags/writeback/post_index`, operand `access`, IT handling) may cost you *less* total
   work — and Capstone's BSD-3 API is far more stable than LLVM's C++ ABI. Counter-counter: you have
   to vendor/pin whatever you use; LLVM's ABI is already pinned because the JIT needs it.
2. **A per-basic-block LLVM IR JIT has terrible per-unit overhead.** A guest block is often 2–10
   instructions; the cost of creating a `Module`/`Function`/`DIBuilder`-free context per block,
   running the pass pipeline with `-O2`, codegen, JITLink relocation and memory protection can dwarf
   the cost of interpreting the block (`UNVERIFIED:` measure it, but block-granular JITs usually end
   up compiling in *batches*, e.g. one module per region or per 100 blocks, or using a template/ASM
   threaded-code emitter with LLVM reserved for hot regions, like QEMU's TCG or V8's Sparkplug).
   Mitigation: cache aggressively, batch compilation of a region's blocks into one module, or emit
   host code directly from the micro-op IR (a "template JIT") and use LLVM only for the cold/hot
   tier. This is the strongest argument for reconsidering §8.2/§8.3 *granularity*, not the decoder.
3. **The arena/"host = f(guest)" address scheme fights the JIT's design and is the riskiest part of
   the plan.** LLVM provides no `MAP_FIXED` path (§2.8), so you must own the entire memory manager
   (allocation, both slabs, protections, i-cache invalidation, deallocation, and interaction with
   `ResourceTracker` removal), and you must keep the guest->host mapping *injective* while the guest
   image is also mapped (aliasing a guest data address with generated code is a silent
   memory-corruption generator). Add to that: JIT'd host code is x86-64 while the guest is ARM, so
   every indirect branch goes through a dispatcher anyway; the "pure offset" trick only saves you on
   direct intra-image branches. If the arena proves too hard, the fallback (per-block symbols in a
   `JITDylib`, lazily generated) costs the memory/time budget of §4.1, which for a boot-an-iOS-app
   workload (~1e4–1e6 blocks touched) is 1–100 MB and a hash lookup per block entry — acceptable.
   Counter-counter: the arena is also what makes block patching, self-modifying code, and
   "guest code address == host code address" debugging tractable; if you drop it, you must build a
   side table for `guest_va -> host_va` that every indirect lookup consults, at similar complexity.

Secondary risks worth naming: `armv7s` needs VFPv4+`sdiv` in the *subtarget* (§5.1) — get this wrong
and you reject valid code; the guest's `OT`/`IT` state and CPSR banking (user vs supervisor) are
runtime, not lift-time, concerns; and IPSW extraction/decryption (§5.5) is outside LLVM entirely.

### 8.5 Short checklist for the first implementation pass

1. `docs`-level decision recorded: decoder = LLVM MC, subtarget = `swift`; Capstone = differential
   oracle. Feature gate = armv7s/`swift` (VFPv3-D32 + VFPv4 with FMA, FP16 conversions, NEON,
   `hwdiv`/`hwdiv-arm`, no ARMv8, no ARMv8-M) — configure the `MCSubtargetInfo` with CPU `swift`
   and assert the resulting feature set in a unit test (§5.1).
2. Build the offline differential tool (§6.3) and freeze an opcode allowlist with a regression
   corpus **before** writing the lifter's semantics table.
3. Bring up `LLJIT` + custom arena memory manager + one `MaterializationUnit` per block +
   `DefinitionGenerator`; prove with a 10-block synthetic guest that addresses are stable across
   recompiles and that patching via `RedirectableSymbolManager` works.
4. Wire `qemu-arm -one-insn-per-tb -d in_asm,cpu,nochain` (`.s` -> `llvm-mc` -> `ld.lld` -> trace) as
   the block-level oracle for the first 100 lifter opcodes; promote dynarmic to the primary oracle
   only if you need permission-free shipping or per-write memory hooks.
