cmake_minimum_required(VERSION 3.25)
project(ios6_jit_c C CXX)

# Host toolchain: clang (required before project()).
if (NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif ()
if (NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif ()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -D_GNU_SOURCE
    -DHAVE_MMAP=1
    -DHAVE_FFSL=1
    -DHAVE_ISNAN=1
    -DHAVE_ISINF=1
)

include_directories(c-src/include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAPSTONE REQUIRED capstone)

# LLVM 23 (needed by TPDE-LLVM)
find_package(LLVM 23.1 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

# TPDE (LLVM back-end + JIT mapper); built as part of this build.
set(TPDE_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(TPDE_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_COVERAGE OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_EH OFF CACHE BOOL "" FORCE)
set(TPDE_TARGETS "x86_64" CACHE STRING "" FORCE)
set(TPDE_LINK_LLVM_STATIC OFF CACHE BOOL "" FORCE)
add_subdirectory(submodules/tpde)

# Core sources
add_library(ios6_jit_core STATIC
    c-src/src/guestmem/arena.c
    c-src/src/jit/disasm.c
    c-src/src/jitllvm/lifter.cpp
    c-src/src/jit/neon.c
    c-src/src/jit/runner.c
    c-src/src/jit/c_api.c
    c-src/src/macho/loader.c
    c-src/src/macho/exports.c
    c-src/src/macho/shared_cache.c
    c-src/src/runtime/syscall.c
    c-src/src/runtime/hooks.c
)

target_link_libraries(ios6_jit_core PUBLIC
    ${CAPSTONE_LIBRARIES}
    tpde_llvm
    LLVM
    dl
    m
    pthread
)
target_include_directories(ios6_jit_core PUBLIC ${CAPSTONE_INCLUDE_DIRS})

# Main CLI executable
add_executable(ios6_recomp
    c-src/src/main.c
)
target_link_libraries(ios6_recomp PRIVATE ios6_jit_core)

# Test binary
add_executable(test_jit_exec
    c-src/tests/test_jit_exec.c
)
target_link_libraries(test_jit_exec PRIVATE ios6_jit_core)

# Differential-test helper (driven by tools/difftest/difftest.py)
add_executable(difftest_jit
    c-src/tests/difftest_jit.c
)
target_link_libraries(difftest_jit PRIVATE ios6_jit_core)

enable_testing()
add_test(NAME test_jit_exec COMMAND test_jit_exec)
