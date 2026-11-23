#ifndef IOS6_LLVMJIT_H
#define IOS6_LLVMJIT_H

#include "ios6/context.h"
#include "ios6/disasm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LLVM + TPDE block JIT.
 *
 * Each disassembled basic block is lifted to an LLVM IR function
 *   void ios6_block_<n>(uint32_t *gtc)
 * (semantics identical to the guest-translation contract in ios6/context.h)
 * and compiled to native code by TPDE-LLVM (fast LLVM back-end), mapped into
 * the process for execution.
 */

typedef struct llvmjit_engine llvmjit_engine_t;

llvmjit_engine_t *llvmjit_create(void);
void llvmjit_destroy(llvmjit_engine_t *engine);

/* Lifts and compiles one basic block; the returned function pointer stays
 * valid until llvmjit_destroy(). Returns NULL on failure. */
jit_block_fn llvmjit_compile_block(llvmjit_engine_t *engine, const basic_block_t *bb);

#ifdef __cplusplus
}
#endif

#endif /* IOS6_LLVMJIT_H */
