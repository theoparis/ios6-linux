#ifndef IOS6_CODEGEN_H
#define IOS6_CODEGEN_H

#include "ios6/context.h"
#include "ios6/disasm.h"
#include "ios6/llvmjit.h"

// Compiles a disassembled basic block into native host machine code using the
// LLVM + TPDE back-end. Returns the compiled function pointer:
// void (*)(uint32_t *gtc). The engine must outlive all returned pointers.
typedef struct llvmjit_engine llvmjit_engine_t;

llvmjit_engine_t *codegen_engine_create(void);
void codegen_engine_destroy(llvmjit_engine_t *engine);
jit_block_fn codegen_compile_block(llvmjit_engine_t *engine, const basic_block_t *bb,
                                   uint32_t slide);

#endif /* IOS6_CODEGEN_H */
