cmake_minimum_required(VERSION 3.16)
project(ios6_jit_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -D_GNU_SOURCE
    -DHAVE_MMAP=1
    -DHAVE_FFSL=1
    -DHAVE_ISNAN=1
    -DHAVE_ISINF=1
)

include_directories(
    c-src/include
    submodules/lightning/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAPSTONE REQUIRED capstone)

# Lightning core sources
set(LIGHTNING_SRCS
    submodules/lightning/lib/lightning.c
    submodules/lightning/lib/jit_memory.c
    submodules/lightning/lib/jit_size.c
    submodules/lightning/lib/jit_note.c
    submodules/lightning/lib/jit_print.c
    submodules/lightning/lib/jit_disasm.c
)

# Project library
add_library(ios6_jit_core STATIC
    ${LIGHTNING_SRCS}
    c-src/src/guestmem/arena.c
    c-src/src/jit/disasm.c
    c-src/src/jit/codegen.c
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
