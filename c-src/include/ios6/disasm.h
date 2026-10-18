#ifndef IOS6_DISASM_H
#define IOS6_DISASM_H

#include "ios6/context.h"
#include <capstone/capstone.h>
#include <stdbool.h>

#define MAX_BLOCK_INSNS 512

typedef struct {
    uint32_t address;
    uint32_t size;
    uint32_t id;
    arm_cc cc;
    bool update_flags;
    bool writeback;
    bool is_terminator;
    char mnemonic[32];
    char op_str[160];
    uint8_t bytes[8];
    cs_arm arm_detail;
} disassembled_insn_t;

typedef struct {
    uint32_t start_address;
    arm_mode_t mode;
    size_t insn_count;
    disassembled_insn_t insns[MAX_BLOCK_INSNS];
} basic_block_t;

typedef struct {
    csh cs_arm;
    csh cs_thumb;
} disassembler_t;

int disasm_init(disassembler_t *dis);
void disasm_destroy(disassembler_t *dis);

int disasm_decode_block(disassembler_t *dis, const uint8_t *code, size_t code_len,
                       uint32_t start_address, arm_mode_t mode, basic_block_t *bb);

arm_cc decode_it_condition(uint8_t firstcond, uint8_t mask, int index);
int it_block_length(uint8_t mask);
arm_cc invert_arm_cc(arm_cc cc);
/* Strip a trailing condition-code suffix (e.g. "popeq" -> "pop") for non-branch insns. */
void strip_cc_suffix(char *base_mn, arm_cc cc);
uint32_t get_pc_value(uint32_t insn_addr, arm_mode_t mode);

#endif /* IOS6_DISASM_H */
