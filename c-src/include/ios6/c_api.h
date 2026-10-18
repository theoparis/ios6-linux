#ifndef IOS6_C_API_H
#define IOS6_C_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *c_jit_engine_t;
typedef void (*c_block_fn_t)(uint32_t *gtc);

c_jit_engine_t c_jit_create(void);
void c_jit_destroy(c_jit_engine_t engine);

c_block_fn_t c_jit_compile_block(c_jit_engine_t engine,
                                 const uint8_t *code,
                                 size_t code_len,
                                 uint32_t pc,
                                 int is_thumb,
                                 uint32_t slide);

#ifdef __cplusplus
}
#endif

#endif /* IOS6_C_API_H */
