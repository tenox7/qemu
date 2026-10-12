/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha CPU -- instruction decode helpers
 */

#ifndef _ALPHA_INSTMAP_H
#define _ALPHA_INSTMAP_H

#include "qemu/bitops.h"

#define DEFINE_INSN_HELPER(name, shift, nb)                             \
    static inline uint32_t glue(get_, name)(uint32_t opc)               \
    {                                                                   \
        return extract32(opc, shift, nb);                               \
    }

DEFINE_INSN_HELPER(opc, 26, 6)
DEFINE_INSN_HELPER(ra, 21, 5)
DEFINE_INSN_HELPER(rb, 16, 5)
DEFINE_INSN_HELPER(rc, 0, 5)
DEFINE_INSN_HELPER(fn11, 5, 11)

#undef DEFINE_INSN_HELPER

/*
 * Symbolic register names.
 */
enum {
    IR_V0   = 0,
    IR_T0   = 1,
    IR_T1   = 2,
    IR_T2   = 3,
    IR_T3   = 4,
    IR_T4   = 5,
    IR_T5   = 6,
    IR_T6   = 7,
    IR_T7   = 8,
    IR_S0   = 9,
    IR_S1   = 10,
    IR_S2   = 11,
    IR_S3   = 12,
    IR_S4   = 13,
    IR_S5   = 14,
    IR_S6   = 15,
    IR_FP   = IR_S6,
    IR_A0   = 16,
    IR_A1   = 17,
    IR_A2   = 18,
    IR_A3   = 19,
    IR_A4   = 20,
    IR_A5   = 21,
    IR_T8   = 22,
    IR_T9   = 23,
    IR_T10  = 24,
    IR_T11  = 25,
    IR_RA   = 26,
    IR_T12  = 27,
    IR_PV   = IR_T12,
    IR_AT   = 28,
    IR_GP   = 29,
    IR_SP   = 30,
    IR_ZERO = 31,
};

enum {
    IR_R0   = 0,
    IR_R1   = 1,
    IR_R2   = 2,
    IR_R3   = 3,
    IR_R4   = 4,
    IR_R5   = 5,
    IR_R6   = 6,
    IR_R7   = 7,
    IR_R8   = 8,
    IR_R9   = 9,
    IR_R10  = 10,
    IR_R11  = 11,
    IR_R12  = 12,
    IR_R13  = 13,
    IR_R14  = 14,
    IR_R15  = 15,
    IR_R16  = 16,
    IR_R17  = 17,
    IR_R18  = 18,
    IR_R19  = 19,
    IR_R20  = 20,
    IR_R21  = 21,
    IR_R22  = 22,
    IR_R23  = 23,
    IR_R24  = 24,
    IR_R25  = 25,
    IR_R26  = 26,
    IR_R27  = 27,
    IR_R28  = 28,
    IR_R29  = 29,
    IR_R30  = 30,
    IR_R31  = 31,
};

enum {
    OPC_PAL00   = 0x00, /* Pcd */
    OPC_OPC01   = 0x01, /* Res */
    OPC_OPC02   = 0x02, /* Res */
    OPC_OPC03   = 0x03, /* Res */
    OPC_OPC04   = 0x04, /* Res */
    OPC_OPC05   = 0x05, /* Res */
    OPC_OPC06   = 0x06, /* Res */
    OPC_OPC07   = 0x07, /* Res */
    OPC_LDA     = 0x08, /* Mem */
    OPC_LDAH    = 0x09, /* Mem */
    OPC_LDBU    = 0x0A, /* Mem */
    OPC_LDQ_U   = 0x0B, /* Mem */
    OPC_LDW_U   = 0x0C, /* Mem */
    OPC_STW     = 0x0D, /* Mem */
    OPC_STB     = 0x0E, /* Mem */
    OPC_STQ_U   = 0x0F, /* Mem */
    OPC_INTA    = 0x10, /* Opr */
    OPC_INTL    = 0x11, /* Opr */
    OPC_INTS    = 0x12, /* Opr */
    OPC_INTM    = 0x13, /* Opr */
    OPC_ITFP    = 0x14, /* FP */
    OPC_FLTV    = 0x15, /* FP (VAX) */
    OPC_FLTI    = 0x16, /* FP (IEEE) */
    OPC_FLTL    = 0x17, /* FP */
    OPC_MISC    = 0x18, /* Mfc */
    OPC_HW_MFPR = 0x19, /* Reserved for PALcode */
    OPC_JMP     = 0x1A, /* Mbr */
    OPC_HW_LD   = 0x1B, /* Reserved for PALcode */
    OPC_FPTI    = 0x1C, /* FP & Opr */
    OPC_HW_MTPR = 0x1D, /* Reserved for PALcode */
    OPC_HW_REI  = 0x1E, /* Reserved for PALcode */
    OPC_HW_ST   = 0x1F, /* Reserved for PALcode */
    OPC_LDF     = 0x20, /* Mem */
    OPC_LDG     = 0x21, /* Mem */
    OPC_LDS     = 0x22, /* Mem */
    OPC_LDT     = 0x23, /* Mem */
    OPC_STF     = 0x24, /* Mem */
    OPC_STG     = 0x25, /* Mem */
    OPC_STS     = 0x26, /* Mem */
    OPC_STT     = 0x27, /* Mem */
    OPC_LDL     = 0x28, /* Mem */
    OPC_LDQ     = 0x29, /* Mem */
    OPC_LDL_L   = 0x2A, /* Mem */
    OPC_LDQ_L   = 0x2B, /* Mem */
    OPC_STL     = 0x2C, /* Mem */
    OPC_STQ     = 0x2D, /* Mem */
    OPC_STL_C   = 0x2E, /* Mem */
    OPC_STQ_C   = 0x2F, /* Mem */
    OPC_BR      = 0x30, /* Bra */
    OPC_FBEQ    = 0x31, /* Bra (FP) */
    OPC_FBLT    = 0x32, /* Bra (FP) */
    OPC_FBLE    = 0x33, /* Bra (FP) */
    OPC_BSR     = 0x34, /* Mbr */
    OPC_FBNE    = 0x35, /* Bra (FP) */
    OPC_FBGE    = 0x36, /* Bra (FP) */
    OPC_FBGT    = 0x37, /* Bra (FP) */
    OPC_BLBC    = 0x38, /* Bra */
    OPC_BEQ     = 0x39, /* Bra */
    OPC_BLT     = 0x3A, /* Bra */
    OPC_BLE     = 0x3B, /* Bra */
    OPC_BLBS    = 0x3C, /* Bra */
    OPC_BNE     = 0x3D, /* Bra */
    OPC_BGE     = 0x3E, /* Bra */
    OPC_BGT     = 0x3F, /* Bra */
};

/*
 * FPU and integer operation definitions.
 */
#define QUAL_RM_C               0x000   /* round mode: chopped */
#define QUAL_RM_M               0x040   /* round mode: minus infinity */
#define QUAL_RM_N               0x080   /* round mode: nearest even */
#define QUAL_RM_D               0x0c0   /* round mode: dynamic */
#define QUAL_RM_MASK            0x0c0

#define QUAL_U                  0x100   /* underflow enable (fp output) */
#define QUAL_V                  0x100   /* overflow enable (int output) */
#define QUAL_S                  0x400   /* software completion enable */
#define QUAL_I                  0x200   /* inexact detection enable */
#define QUAL_MASK               0x700

/*
 * Modes for hw_ld and hw_st.
 */
#define EV4_HW_LDST_RWC         0b001       /* WrChk */
#define EV4_HW_LDST_ALT         0b010       /* Alt */
#define EV4_HW_LDST_PHY         0b100       /* Physical */
#define EV4_HW_LDST_LOCK        0b110       /* Lock */

#define EV5_HW_LDST_LOCK        0b00001     /* Lock */
#define EV5_HW_LDST_VPTE        0b00010     /* VPTE */
#define EV5_HW_LDST_WRTCK       0b00100     /* WrChk */
#define EV5_HW_LDST_ALT         0b01000     /* Alt */
#define EV5_HW_LDST_PHYS        0b10000     /* Physical */

#define EV6_HW_LD_PHYS          0b000       /* Physical */
#define EV6_HW_LD_PHYS_LOCK     0b001       /* Physical/Lock */
#define EV6_HW_LD_VPTE          0b010       /* Virtual/VPTE */
#define EV6_HW_LD_VIRT          0b100       /* Virtual */
#define EV6_HW_LD_VIRT_WCHK     0b101       /* Virtual/WrChk */
#define EV6_HW_LD_VIRT_ALT      0b110       /* Virtual/Alt */
#define EV6_HW_LD_VIRT_WALT     0b111       /* Virtual/WrChk/Alt */

#define EV6_HW_ST_PHYS          0b000       /* Physical */
#define EV6_HW_ST_PHYS_COND     0b001       /* Physical/Cond */
#define EV6_HW_ST_VIRT          0b010       /* Virtual */
#define EV6_HW_ST_VIRT_ALT      0b110       /* Virtual/Alt  */

#endif /* _ALPHA_INSTMAP_H */
