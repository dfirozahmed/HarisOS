/* Encoders for the few GCN5 ("Vega") shader instructions the 3D test needs, and the small number conversions it uses.
 * Every bit layout is from the AMD "Vega" Instruction Set Architecture Reference Guide, chapter 13 (Microcode Formats):
 *   VOP2   [8:0] SRC0, [16:9] VSRC1, [24:17] VDST, [30:25] OP, [31] = 0
 *   VOP1   [8:0] SRC0, [16:9] OP, [24:17] VDST, [31:25] = 0111111
 *   VOPC   [8:0] SRC0, [16:9] VSRC1, [24:17] OP, [31:25] = 0111110
 *   SOPP   [15:0] SIMM16, [22:16] OP, [31:23] = 101111111
 *   EXP    first dword: [3:0] EN, [9:4] TARGET, [10] COMPR, [11] DONE, [12] VM, [31:26] = 110001;
 *          second dword: [7:0] VSRC0, [15:8] VSRC1, [23:16] VSRC2, [31:24] VSRC3
 * Opcode numbers: VOP2 V_CNDMASK_B32 0, V_LSHRREV_B32 16, V_AND_B32 19; VOP1 V_MOV_B32 1; VOPC V_CMP_NE_U32 205; SOPP S_ENDPGM 1.
 * SRC operand codes: 0..101 SGPR, 128 = 0, 129..192 = integer 1..64, 242 = 1.0, 255 = a 32-bit literal follows the instruction,
 * 256.. = VGPR 0.. */
#ifndef HARIS_AMD_3D_ISA_H
#define HARIS_AMD_3D_ISA_H

#include "gpu_types.h"

#define A3D_SRC_VGPR(n) (256u + (n))
#define A3D_SRC_ZERO 128u
#define A3D_SRC_INT(n) (128u + (n)) /* 1 .. 64 */
#define A3D_SRC_ONE_F 242u
#define A3D_SRC_LITERAL 255u

#define A3D_OP2_CNDMASK_B32 0u
#define A3D_OP2_LSHRREV_B32 16u
#define A3D_OP2_AND_B32 19u
#define A3D_OP1_MOV_B32 1u
#define A3D_OPC_CMP_NE_U32 205u

#define A3D_EXP_TARGET_MRT0 0u
#define A3D_EXP_TARGET_POS0 12u
#define A3D_EXP_TARGET_PARAM0 32u

#define A3D_S_ENDPGM 0xBF810000u

static inline uint32_t a3d_vop2(uint32_t op, uint32_t vdst, uint32_t vsrc1, uint32_t src0)
{
    return (op << 25) | (vdst << 17) | (vsrc1 << 9) | src0;
}

static inline uint32_t a3d_vop1(uint32_t op, uint32_t vdst, uint32_t src0)
{
    return 0x7E000000u | (vdst << 17) | (op << 9) | src0;
}

static inline uint32_t a3d_vopc(uint32_t op, uint32_t vsrc1, uint32_t src0)
{
    return 0x7C000000u | (op << 17) | (vsrc1 << 9) | src0;
}

static inline uint32_t a3d_exp0(uint32_t en, uint32_t target, uint32_t compr, uint32_t done, uint32_t vm)
{
    return 0xC4000000u | (vm << 12) | (done << 11) | (compr << 10) | (target << 4) | en;
}

static inline uint32_t a3d_exp1(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3)
{
    return v0 | (v1 << 8) | (v2 << 16) | (v3 << 24);
}

/* IEEE-754 single precision bits of a small unsigned integer (exact up to 2^24). */
static inline uint32_t a3d_f32_bits(uint32_t v)
{
    uint32_t p = 0;
    if (v == 0)
        return 0;
    while ((v >> (p + 1)) != 0)
        p++;
    uint32_t mant = p <= 23 ? (v << (23 - p)) : (v >> (p - 23));
    return ((p + 127u) << 23) | (mant & 0x7FFFFFu);
}

/* IEEE-754 single precision bits of num / den (num, den < 2^20; truncated). 0 if either is 0. */
static inline uint32_t a3d_f32_ratio(uint32_t num, uint32_t den)
{
    uint64_t q;
    uint32_t p = 0, mant;
    if (!num || !den)
        return 0;
    q = ((uint64_t)num << 40) / den;
    while ((q >> (p + 1)) != 0)
        p++;
    mant = p >= 23 ? (uint32_t)(q >> (p - 23)) : (uint32_t)(q << (23 - p));
    return ((p + 127u - 40u) << 23) | (mant & 0x7FFFFFu);
}

/* IEEE-754 half precision bits of v / 255 (v = 0 .. 255), rounded toward zero like the V_CVT_PKRTZ_F16_F32 instruction. */
static inline uint32_t a3d_f16_unorm8(uint32_t v)
{
    if (v == 0)
        return 0;
    if (v >= 255)
        return 0x3C00; /* 1.0 */
    uint32_t x = (v << 16) / 255u; /* 16.16 fixed point, below 1.0 */
    uint32_t h = 0;
    while ((x >> (h + 1)) != 0)
        h++;
    uint32_t mant = h >= 10 ? (x >> (h - 10)) : (x << (10 - h));
    return ((h - 1u) << 10) | (mant & 0x3FFu); /* value 2^(h-16) .. : half exponent field = (h - 16) + 15 */
}

/* ---- for the textured rectangle (M2) ----
 * VOP3A  dword0: [7:0] VDST, [15] CLMP, [25:16] OP, [31:26] = 110100;  dword1: [8:0] SRC0, [17:9] SRC1, [26:18] SRC2, [28:27] OMOD, [31:29] NEG
 *        (V_CVT_PKRTZ_F16_F32 is VOP3-only on GFX9: opcode 0x296 = 662, Mesa aco_opcodes.py "gfx8=0x296")
 * MIMG   dword0: [11:8] DMASK, [12] UNRM, [13] GLC, [14] DA, [15] A16, [16] TFE, [17] LWE, [24:18] OP[6:0], [0] OP[7], [25] SLC,
 *        [31:26] = 111100;  dword1: [7:0] VADDR, [15:8] VDATA, [20:16] SRSRC (SGPR number / 4), [25:21] SSAMP (SGPR number / 4), [31] D16
 * S_WAITCNT (SOPP op 12): simm16 [3:0] vmcount, [6:4] expcount, [11:8] lgkmcount, [15:14] vmcount upper bits */
#define A3D_OP1_CVT_U32_F32 7u
/* float VOP2 (Mesa aco_opcodes.py, gfx8 column): V_SUB_F32 2 (S0 - S1), V_SUBREV_F32 3 (S1 - S0), V_MUL_F32 5, V_MIN_F32 0xA, V_MAX_F32 0xB */
#define A3D_OP2_SUB_F32 2u
#define A3D_OP2_SUBREV_F32 3u
#define A3D_OP2_MUL_F32 5u
#define A3D_OP2_MIN_F32 0xAu
#define A3D_OP2_MAX_F32 0xBu
#define A3D_OP2_ADD_F32 1u    /* gfx8 column */
#define A3D_OP2_ADD_U32 0x34u /* GFX9 only: D = S0 + S1, no carry, VCC untouched (Mesa aco_opcodes.py gfx9=0x34) */
#define A3D_OP2_SUBREV_CO_U32 27u /* D = S1 - S0, carry to VCC (VOP2 opcode 0x1B on GFX9) */
#define A3D_OP3_CVT_PKRTZ_F16_F32 662u
#define A3D_MIMG_IMAGE_LOAD 0u
#define A3D_S_WAITCNT_VM0 0xBF8C0F70u /* s_waitcnt vmcnt(0): expcnt and lgkmcnt left at their maximum (7, 15) */

static inline uint32_t a3d_vop3a_0(uint32_t op, uint32_t vdst)
{
    return 0xD0000000u | (op << 16) | vdst;
}

static inline uint32_t a3d_vop3a_1(uint32_t src0, uint32_t src1, uint32_t src2)
{
    return src0 | (src1 << 9) | (src2 << 18);
}

static inline uint32_t a3d_mimg_0(uint32_t op, uint32_t dmask)
{
    return 0xF0000000u | ((op & 0x80u) >> 7) | ((op & 0x7Fu) << 18) | (dmask << 8);
}

static inline uint32_t a3d_mimg_1(uint32_t vaddr, uint32_t vdata, uint32_t srsrc_sgpr, uint32_t ssamp_sgpr)
{
    return vaddr | (vdata << 8) | ((srsrc_sgpr >> 2) << 16) | ((ssamp_sgpr >> 2) << 21);
}

/* ---- for the diagnostic store from the vertex shader ----
 * GLOBAL (FLAT format, GFX9, Mesa aco_assembler.cpp emit_flatlike_instruction):
 *   dword0: [12:0] OFFSET (signed), [13] LDS, [15:14] SEG (2 = global), [16] GLC, [17] SLC, [24:18] OP, [31:26] = 110111
 *   dword1: [7:0] ADDR (VGPR pair, 64-bit address), [15:8] DATA (VGPR), [22:16] SADDR (0x7F = none), [23] NV, [31:24] VDST
 * Opcode numbers (Mesa aco_opcodes.py, the gfx8 column, which GFX9 uses): GLOBAL_STORE_DWORD 0x1C, GLOBAL_STORE_DWORDX4 0x1F;
 * VOP2 V_LSHLREV_B32 0x12 (D = S1 << S0), V_OR_B32 0x14, V_ADD_CO_U32 0x19 (carry to VCC), V_ADDC_CO_U32 0x1C (D = S0 + S1 + VCC).
 * GFX9 VALU rule (Mesa aco_validate.cpp, const_bus_limit = 1): at most ONE scalar operand per instruction, counting SGPRs, literals
 * and the VCC that V_CNDMASK_B32 / V_ADDC_CO_U32 read. So those two take only VGPRs or inline constants besides VCC. */
#define A3D_OP2_LSHLREV_B32 0x12u
#define A3D_OP2_OR_B32 0x14u
#define A3D_OP2_ADD_CO_U32 0x19u
#define A3D_OP2_ADDC_CO_U32 0x1Cu
#define A3D_GLOBAL_STORE_DWORD 0x1Cu
#define A3D_GLOBAL_STORE_DWORDX4 0x1Fu

static inline uint32_t a3d_global_0(uint32_t op, uint32_t offset)
{
    return 0xDC000000u | (op << 18) | (2u << 14) | (offset & 0x1FFFu);
}

static inline uint32_t a3d_global_1(uint32_t addr_vgpr, uint32_t data_vgpr)
{
    return addr_vgpr | (data_vgpr << 8) | (0x7Fu << 16);
}

#endif
