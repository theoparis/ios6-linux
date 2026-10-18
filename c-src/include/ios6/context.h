#ifndef IOS6_CONTEXT_H
#define IOS6_CONTEXT_H

#include <stdint.h>
#include <stdbool.h>

#define GTC_NUM_FIELDS 18

/* Field offsets in uint32_t words */
#define GTC_R0            0
#define GTC_R1            1
#define GTC_R2            2
#define GTC_R3            3
#define GTC_R4            4
#define GTC_R5            5
#define GTC_R6            6
#define GTC_R7            7
#define GTC_R8            8
#define GTC_R9            9
#define GTC_R10           10
#define GTC_R11           11
#define GTC_R12           12
#define GTC_SP            13
#define GTC_LR            14
#define GTC_FIELD_CPSR    15
#define GTC_FIELD_NEXT_PC 16
#define GTC_FIELD_TRAP_RESUME 17

/* CPSR bit positions */
#define CPSR_N (1u << 31)
#define CPSR_Z (1u << 30)
#define CPSR_C (1u << 29)
#define CPSR_V (1u << 28)

#define SYSCALL_TRAP_PC 0xffffff00u

typedef enum {
    ARM_MODE_ARM = 0,
    ARM_MODE_THUMB = 1
} arm_mode_t;

typedef void (*jit_block_fn)(uint32_t *gtc);

#endif /* IOS6_CONTEXT_H */
