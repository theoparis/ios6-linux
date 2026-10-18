#include "ios6/disasm.h"
#include <string.h>
#include <stdio.h>

int disasm_init(disassembler_t *dis) {
    if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &dis->cs_arm) != CS_ERR_OK) {
        return -1;
    }
    cs_option(dis->cs_arm, CS_OPT_DETAIL, CS_OPT_ON);

    if (cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &dis->cs_thumb) != CS_ERR_OK) {
        cs_close(&dis->cs_arm);
        return -1;
    }
    cs_option(dis->cs_thumb, CS_OPT_DETAIL, CS_OPT_ON);

    return 0;
}

void disasm_destroy(disassembler_t *dis) {
    if (dis->cs_arm) cs_close(&dis->cs_arm);
    if (dis->cs_thumb) cs_close(&dis->cs_thumb);
}

static arm_cc arm_cc_from_number(uint8_t n) {
    switch (n) {
        case 0: return ARM_CC_EQ;
        case 1: return ARM_CC_NE;
        case 2: return ARM_CC_HS;
        case 3: return ARM_CC_LO;
        case 4: return ARM_CC_MI;
        case 5: return ARM_CC_PL;
        case 6: return ARM_CC_VS;
        case 7: return ARM_CC_VC;
        case 8: return ARM_CC_HI;
        case 9: return ARM_CC_LS;
        case 10: return ARM_CC_GE;
        case 11: return ARM_CC_LT;
        case 12: return ARM_CC_GT;
        case 13: return ARM_CC_LE;
        default: return ARM_CC_AL;
    }
}

int it_block_length(uint8_t mask) {
    if (mask == 0) return 0;
    return 4 - __builtin_ctz(mask);
}

arm_cc decode_it_condition(uint8_t firstcond, uint8_t mask, int index) {
    if (index == 0) return arm_cc_from_number(firstcond);
    bool odd = (firstcond & 1) != 0;
    bool bit = ((mask >> (5 - (index + 1))) & 1) == 1;
    bool invert = odd ? !bit : bit;
    uint8_t cond_num = invert ? (firstcond ^ 1) : firstcond;
    return arm_cc_from_number(cond_num);
}

arm_cc invert_arm_cc(arm_cc cc) {
    switch (cc) {
        case ARM_CC_EQ: return ARM_CC_NE;
        case ARM_CC_NE: return ARM_CC_EQ;
        case ARM_CC_HS: return ARM_CC_LO;
        case ARM_CC_LO: return ARM_CC_HS;
        case ARM_CC_MI: return ARM_CC_PL;
        case ARM_CC_PL: return ARM_CC_MI;
        case ARM_CC_VS: return ARM_CC_VC;
        case ARM_CC_VC: return ARM_CC_VS;
        case ARM_CC_HI: return ARM_CC_LS;
        case ARM_CC_LS: return ARM_CC_HI;
        case ARM_CC_GE: return ARM_CC_LT;
        case ARM_CC_LT: return ARM_CC_GE;
        case ARM_CC_GT: return ARM_CC_LE;
        case ARM_CC_LE: return ARM_CC_GT;
        default: return cc;
    }
}

static const char *cc_suffixes(arm_cc cc, const char **alt) {
    *alt = NULL;
    switch (cc) {
        case ARM_CC_EQ: return "eq";
        case ARM_CC_NE: return "ne";
        case ARM_CC_HS: *alt = "cs"; return "hs";
        case ARM_CC_LO: *alt = "cc"; return "lo";
        case ARM_CC_MI: return "mi";
        case ARM_CC_PL: return "pl";
        case ARM_CC_VS: return "vs";
        case ARM_CC_VC: return "vc";
        case ARM_CC_HI: return "hi";
        case ARM_CC_LS: return "ls";
        case ARM_CC_GE: return "ge";
        case ARM_CC_LT: return "lt";
        case ARM_CC_GT: return "gt";
        case ARM_CC_LE: return "le";
        default: return NULL;
    }
}

void strip_cc_suffix(char *base_mn, arm_cc cc) {
    const char *alt;
    const char *suf = cc_suffixes(cc, &alt);
    if (!suf) return;
    size_t len = strlen(base_mn);
    if (len <= 2) return;
    const char *cands[2] = { suf, alt };
    for (int i = 0; i < 2; i++) {
        if (!cands[i]) continue;
        if (strcmp(base_mn + len - 2, cands[i]) != 0) continue;
        char tmp[32];
        memcpy(tmp, base_mn, len - 2);
        tmp[len - 2] = '\0';
        // Leave branches alone: they are handled via their embedded condition.
        // Branches keep their suffix: capstone reports the true cc in insn->cc, and the
        // codegen predicates on it. Stripping "blne" -> "bl" would make the call unconditional.
        if (tmp[0] == 'b' && (tmp[1] == '\0' || tmp[1] == 'l' || tmp[1] == 'x')) return;
        strcpy(base_mn, tmp);
        return;
    }
}

uint32_t get_pc_value(uint32_t insn_addr, arm_mode_t mode) {
    return insn_addr + (mode == ARM_MODE_ARM ? 8 : 4);
}

static bool check_terminator(const char *base_mn, const char *mnemonic, const char *op_str) {
    if (strcmp(base_mn, "b") == 0 || strcmp(base_mn, "bx") == 0 ||
        strcmp(base_mn, "bl") == 0 || strcmp(base_mn, "blx") == 0 ||
        strcmp(base_mn, "bxj") == 0 || strcmp(base_mn, "cbz") == 0 ||
        strcmp(base_mn, "cbnz") == 0 || strcmp(base_mn, "tbb") == 0 ||
        strcmp(base_mn, "tbh") == 0 || strcmp(base_mn, "svc") == 0) {
        return true;
    }

    const char *cond_branches[] = {
        "beq", "bne", "bgt", "blt", "bge", "ble", "bhs", "bcs",
        "blo", "bcc", "bmi", "bpl", "bvs", "bvc", "bhi", "bls"
    };
    for (size_t i = 0; i < sizeof(cond_branches)/sizeof(cond_branches[0]); i++) {
        if (strcmp(base_mn, cond_branches[i]) == 0) return true;
    }

    if (strcmp(base_mn, "pop") == 0 && strstr(op_str, "pc") != NULL) return true;
    if ((strcmp(base_mn, "mov") == 0 || strcmp(base_mn, "movs") == 0) &&
        (strncmp(op_str, "pc,", 3) == 0 || strncmp(op_str, "pc ,", 4) == 0)) return true;
    if (strcmp(base_mn, "ldr") == 0 &&
        (strncmp(op_str, "pc,", 3) == 0 || strncmp(op_str, "pc ,", 4) == 0)) return true;
    if ((strcmp(base_mn, "ldm") == 0 || strcmp(base_mn, "ldmia") == 0 || strcmp(base_mn, "ldmfd") == 0) &&
        strstr(op_str, "pc") != NULL) return true;

    // Predicated bx, bl, blx
    const char *conds[] = {
        "eq", "ne", "hs", "cs", "lo", "cc", "mi", "pl",
        "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"
    };
    size_t mlen = strlen(base_mn);
    for (size_t i = 0; i < 16; i++) {
        size_t clen = strlen(conds[i]);
        if (mlen > clen && strcmp(base_mn + mlen - clen, conds[i]) == 0) {
            char prefix[16];
            size_t plen = mlen - clen;
            if (plen < sizeof(prefix)) {
                strncpy(prefix, base_mn, plen);
                prefix[plen] = '\0';
                if (strcmp(prefix, "bx") == 0 || strcmp(prefix, "bl") == 0 ||
                    strcmp(prefix, "blx") == 0 || strcmp(prefix, "bxj") == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

int disasm_decode_block(disassembler_t *dis, const uint8_t *code, size_t code_len,
                       uint32_t start_address, arm_mode_t mode, basic_block_t *bb) {
    bb->start_address = start_address;
    bb->mode = mode;
    bb->insn_count = 0;

    csh cs = (mode == ARM_MODE_ARM) ? dis->cs_arm : dis->cs_thumb;
    const uint8_t *cur_code = code;
    size_t cur_len = code_len;
    uint64_t cur_addr = start_address;

    cs_insn *insn = cs_malloc(cs);
    if (!insn) return -1;

    arm_cc it_queue[4];
    int it_len = 0;
    int it_idx = 0;

    while (cur_len > 0 && bb->insn_count < MAX_BLOCK_INSNS) {
        if (!cs_disasm_iter(cs, &cur_code, &cur_len, &cur_addr, insn)) {
            break;
        }

        disassembled_insn_t *d = &bb->insns[bb->insn_count++];
        d->address = (uint32_t)insn->address;
        d->size = insn->size;
        d->id = insn->id;
        strncpy(d->mnemonic, insn->mnemonic, sizeof(d->mnemonic) - 1);
        d->mnemonic[sizeof(d->mnemonic) - 1] = '\0';
        strncpy(d->op_str, insn->op_str, sizeof(d->op_str) - 1);
        d->op_str[sizeof(d->op_str) - 1] = '\0';
        size_t bsz = insn->size < sizeof(d->bytes) ? insn->size : sizeof(d->bytes);
        memcpy(d->bytes, insn->bytes, bsz);

        if (insn->detail && insn->detail->arm.op_count > 0) {
            d->arm_detail = insn->detail->arm;
            d->cc = insn->detail->arm.cc;
            d->update_flags = insn->detail->arm.update_flags;
            d->writeback = insn->detail->arm.writeback;
        } else {
            memset(&d->arm_detail, 0, sizeof(d->arm_detail));
            d->cc = ARM_CC_AL;
            d->update_flags = false;
            d->writeback = false;
        }

        // IT block tracking
        char base_mn[32];
        strncpy(base_mn, d->mnemonic, sizeof(base_mn) - 1);
        base_mn[sizeof(base_mn) - 1] = '\0';
        char *dot = strchr(base_mn, '.');
        if (dot) *dot = '\0';

        if (mode == ARM_MODE_THUMB && (strncmp(base_mn, "it", 2) == 0)) {
            if (d->size >= 2) {
                uint8_t firstcond = (d->bytes[0] >> 4) & 0xf;
                uint8_t mask = d->bytes[0] & 0xf;
                it_len = it_block_length(mask);
                it_idx = 0;
                for (int i = 0; i < it_len; i++) {
                    it_queue[i] = decode_it_condition(firstcond, mask, i);
                }
            }
        } else if (it_idx < it_len) {
            d->cc = it_queue[it_idx++];
        }

        strip_cc_suffix(base_mn, d->cc);
        d->is_terminator = check_terminator(base_mn, d->mnemonic, d->op_str);
        if (d->is_terminator) {
            break;
        }
    }

    cs_free(insn, 1);
    return bb->insn_count > 0 ? 0 : -1;
}
