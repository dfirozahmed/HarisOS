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

#endif
