#ifndef IOS6_CODEGEN_H
#define IOS6_CODEGEN_H

#include "ios6/context.h"
#include "ios6/disasm.h"
#include <lightning.h>

// Compiles a disassembled basic block into native host machine code using GNU Lightning.
// Returns the compiled function pointer: void (*)(uint32_t *gtc)
jit_block_fn codegen_compile_block(jit_state_t *_jit, const basic_block_t *bb, uint32_t slide);

#endif /* IOS6_CODEGEN_H */
