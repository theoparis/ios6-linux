#include "ios6/c_api.h"
#include "ios6/codegen.h"
#include "ios6/disasm.h"
#include <lightning.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    disassembler_t dis;
    jit_state_t *_jit;
} c_jit_t;

c_jit_engine_t c_jit_create(void) {
    c_jit_t *j = (c_jit_t *)calloc(1, sizeof(c_jit_t));
    if (!j) return NULL;

    if (disasm_init(&j->dis) != 0) {
        free(j);
        return NULL;
    }

    init_jit(NULL);
    j->_jit = jit_new_state();
    if (!j->_jit) {
        disasm_destroy(&j->dis);
        free(j);
        return NULL;
    }

    return (c_jit_engine_t)j;
}

void c_jit_destroy(c_jit_engine_t engine) {
    if (!engine) return;
    c_jit_t *j = (c_jit_t *)engine;
    if (j->_jit) {
        jit_state_t *_jit = j->_jit;
        jit_destroy_state();
        finish_jit();
    }
    disasm_destroy(&j->dis);
    free(j);
}

c_block_fn_t c_jit_compile_block(c_jit_engine_t engine,
                                 const uint8_t *code,
                                 size_t code_len,
                                 uint32_t pc,
                                 int is_thumb,
                                 uint32_t slide) {
    if (!engine || !code || code_len == 0) return NULL;
    c_jit_t *j = (c_jit_t *)engine;

    arm_mode_t mode = is_thumb ? ARM_MODE_THUMB : ARM_MODE_ARM;
    basic_block_t bb;
    if (disasm_decode_block(&j->dis, code, code_len, pc, mode, &bb) != 0) {
        return NULL;
    }

    return (c_block_fn_t)codegen_compile_block(j->_jit, &bb, slide);
}
