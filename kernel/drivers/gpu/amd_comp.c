/* AMD Renoir GPU compositor, M4a: a frame of many draws in one submission, with fixed shaders that take their parameters in user
 * SGPRs (see include/amd_comp.h). The command stream comes from the same pieces as the 3D tests (amd_3d_internal.h: head, draws,
 * tail), so every register value and where it comes from is in amd_3d.c.
 *
 * GFX9 shader rule kept in every shader here: at most ONE scalar operand (an SGPR, a literal, or the VCC that V_CNDMASK reads) per
 * VALU instruction (Mesa aco_validate.cpp, const_bus_limit 1); an SGPR needed next to VCC is copied into a VGPR first. */
#include "amd_comp.h"
#include "amd_3d.h"
#include "amd_3d_isa.h"
#include "amd_3d_internal.h"
#include "amd_3d_regs.h"
#include "amd_native.h"
#include "gfx.h"

#define SGPR(n) ((uint32_t)(n)) /* source operand code of SGPR n */

/* ---- the fixed shaders ---- */

/* The rectangle from s0..s3 (x0, y0, x1, y1 as floats), a triangle strip of 4 vertices: bit 0 of the vertex number picks x1, bit 1
 * picks y1. Window space position (pixels), like the 3D tests' vertex shader. */
uint32_t amd_comp_build_vs(uint32_t *code)
{
    uint32_t n = 0;
    code[n++] = a3d_vop2(A3D_OP2_AND_B32, 1, 0, A3D_SRC_INT(1));                 /* v_and_b32 v1, 1, v0        */
    code[n++] = a3d_vopc(A3D_OPC_CMP_NE_U32, 1, A3D_SRC_ZERO);                   /* v_cmp_ne_u32 vcc, 0, v1    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 2, SGPR(2));                           /* v_mov_b32 v2, s2  (x1)     */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 7, SGPR(0));                           /* v_mov_b32 v7, s0  (x0)     */
    code[n++] = a3d_vop2(A3D_OP2_CNDMASK_B32, 3, 2, A3D_SRC_VGPR(7));            /* v_cndmask_b32 v3, v7, v2, vcc */
    code[n++] = a3d_vop2(A3D_OP2_LSHRREV_B32, 1, 0, A3D_SRC_INT(1));             /* v_lshrrev_b32 v1, 1, v0    */
    code[n++] = a3d_vop2(A3D_OP2_AND_B32, 1, 1, A3D_SRC_INT(1));                 /* v_and_b32 v1, 1, v1        */
    code[n++] = a3d_vopc(A3D_OPC_CMP_NE_U32, 1, A3D_SRC_ZERO);                   /* v_cmp_ne_u32 vcc, 0, v1    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 2, SGPR(3));                           /* v_mov_b32 v2, s3  (y1)     */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 7, SGPR(1));                           /* v_mov_b32 v7, s1  (y0)     */
    code[n++] = a3d_vop2(A3D_OP2_CNDMASK_B32, 4, 2, A3D_SRC_VGPR(7));            /* v_cndmask_b32 v4, v7, v2, vcc */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 5, A3D_SRC_ZERO);                      /* v_mov_b32 v5, 0.0   (z)    */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 6, A3D_SRC_ONE_F);                     /* v_mov_b32 v6, 1.0   (w)    */
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_PARAM0, 0, 0, 0);                   /* exp param0 v3, v4, v5, v6 (one is required) */
    code[n++] = a3d_exp1(3, 4, 5, 6);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_POS0, 0, 1, 0);                     /* exp pos0 v3, v4, v5, v6 done */
    code[n++] = a3d_exp1(3, 4, 5, 6);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* A solid colour: s0 = (r, g), s1 = (b, a) as halves (FP16_ABGR export, RB+). */
uint32_t amd_comp_build_ps_fill(uint32_t *code)
{
    uint32_t n = 0;
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 0, SGPR(0));                           /* v_mov_b32 v0, s0           */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 1, SGPR(1));                           /* v_mov_b32 v1, s1           */
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v0, v1 compr done vm */
    code[n++] = a3d_exp1(0, 1, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* A surface: texel = pixel position + (s8, s9) (integers, wrap-around arithmetic), read with IMAGE_LOAD through the resource in
 * s0..s7; the alpha is the opacity in s10 (the surface has no alpha of its own). */
uint32_t amd_comp_build_ps_image(uint32_t *code)
{
    uint32_t n = 0;
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 4, A3D_SRC_VGPR(2));               /* v_cvt_u32_f32 v4, v2 (x)   */
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 5, A3D_SRC_VGPR(3));               /* v_cvt_u32_f32 v5, v3 (y)   */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 4, 4, SGPR(8));                        /* v_add_u32 v4, s8, v4       */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 5, 5, SGPR(9));                        /* v_add_u32 v5, s9, v5       */
    code[n++] = a3d_mimg_0(A3D_MIMG_IMAGE_LOAD, 0xF);                            /* image_load v[6:9], v[4:5], s[0:7] dmask:0xf */
    code[n++] = a3d_mimg_1(4, 6, 0, 0);
    code[n++] = A3D_S_WAITCNT_VM0;                                               /* s_waitcnt vmcnt(0)         */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 9, SGPR(10));                          /* v_mov_b32 v9, s10 (alpha)  */
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 10);                      /* v_cvt_pkrtz_f16_f32 v10, v6, v7 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(6), A3D_SRC_VGPR(7), 0);
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 11);                      /* v_cvt_pkrtz_f16_f32 v11, v8, v9 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(8), A3D_SRC_VGPR(9), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v10, v11 compr done vm */
    code[n++] = a3d_exp1(10, 11, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* The screen's brightness (the dim setting): an image read like amd_comp_build_ps_image's and its colour multiplied by s10
 * (0 .. 1, a float). One SGPR per instruction (the constant bus). */
uint32_t amd_comp_build_ps_dim(uint32_t *code)
{
    uint32_t n = 0, c;
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 4, A3D_SRC_VGPR(2));               /* v_cvt_u32_f32 v4, v2 (x)   */
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 5, A3D_SRC_VGPR(3));               /* v_cvt_u32_f32 v5, v3 (y)   */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 4, 4, SGPR(8));                        /* v_add_u32 v4, s8, v4       */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 5, 5, SGPR(9));                        /* v_add_u32 v5, s9, v5       */
    code[n++] = a3d_mimg_0(A3D_MIMG_IMAGE_LOAD, 0xF);                            /* image_load v[6:9], v[4:5], s[0:7] dmask:0xf */
    code[n++] = a3d_mimg_1(4, 6, 0, 0);
    code[n++] = A3D_S_WAITCNT_VM0;                                               /* s_waitcnt vmcnt(0)         */
    for (c = 0; c < 3; c++)
        code[n++] = a3d_vop2(A3D_OP2_MUL_F32, 6 + c, 6 + c, SGPR(10));           /* v6+c = s10 * v6+c          */
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 10);                      /* v_cvt_pkrtz_f16_f32 v10, v6, v7 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(6), A3D_SRC_VGPR(7), 0);
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 11);                      /* v_cvt_pkrtz_f16_f32 v11, v8, v9 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(8), A3D_SRC_VGPR(9), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v10, v11 compr done vm */
    code[n++] = a3d_exp1(10, 11, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* A packed layer (gfx_layer_pack): the same load as the image, but the texel keeps its own alpha (the 4th byte: how much of what is
 * below shows through), for blend mode 2 (ONE / SRC_ALPHA). */
uint32_t amd_comp_build_ps_layer(uint32_t *code)
{
    uint32_t n = 0;
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 4, A3D_SRC_VGPR(2));               /* v_cvt_u32_f32 v4, v2 (x)   */
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 5, A3D_SRC_VGPR(3));               /* v_cvt_u32_f32 v5, v3 (y)   */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 4, 4, SGPR(8));                        /* v_add_u32 v4, s8, v4       */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 5, 5, SGPR(9));                        /* v_add_u32 v5, s9, v5       */
    code[n++] = a3d_mimg_0(A3D_MIMG_IMAGE_LOAD, 0xF);                            /* image_load v[6:9], v[4:5], s[0:7] dmask:0xf */
    code[n++] = a3d_mimg_1(4, 6, 0, 0);
    code[n++] = A3D_S_WAITCNT_VM0;                                               /* s_waitcnt vmcnt(0)         */
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 10);                      /* v_cvt_pkrtz_f16_f32 v10, v6, v7 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(6), A3D_SRC_VGPR(7), 0);
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 11);                      /* v_cvt_pkrtz_f16_f32 v11, v8, v9 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(8), A3D_SRC_VGPR(9), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v10, v11 compr done vm */
    code[n++] = a3d_exp1(10, 11, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* The window being dragged, finished in one pass: the layer (s0..s7, texel = pixel + (s8, s9)) and the picture below it, which the CPU
 * put s10 rows further down in the same surface. out = layer colour + below * layer alpha (what blend mode 2 gives over the screen), so
 * the screen gets the final pixel in one write and never shows the picture without the layer. The second load uses its own address
 * VGPRs (v14, v15). */
uint32_t amd_comp_build_ps_under(uint32_t *code)
{
    uint32_t n = 0, c;
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 4, A3D_SRC_VGPR(2));               /* v_cvt_u32_f32 v4, v2 (x)   */
    code[n++] = a3d_vop1(A3D_OP1_CVT_U32_F32, 5, A3D_SRC_VGPR(3));               /* v_cvt_u32_f32 v5, v3 (y)   */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 4, 4, SGPR(8));                        /* v_add_u32 v4, s8, v4       */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 5, 5, SGPR(9));                        /* v_add_u32 v5, s9, v5       */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 14, A3D_SRC_VGPR(4));                  /* v_mov_b32 v14, v4          */
    code[n++] = a3d_vop2(A3D_OP2_ADD_U32, 15, 5, SGPR(10));                      /* v_add_u32 v15, s10, v5     */
    code[n++] = a3d_mimg_0(A3D_MIMG_IMAGE_LOAD, 0xF);                            /* image_load v[6:9], v[4:5], s[0:7] (layer) */
    code[n++] = a3d_mimg_1(4, 6, 0, 0);
    code[n++] = a3d_mimg_0(A3D_MIMG_IMAGE_LOAD, 0xF);                            /* image_load v[10:13], v[14:15], s[0:7] (below) */
    code[n++] = a3d_mimg_1(14, 10, 0, 0);
    code[n++] = A3D_S_WAITCNT_VM0;                                               /* s_waitcnt vmcnt(0)         */
    for (c = 0; c < 3; c++) {
        code[n++] = a3d_vop2(A3D_OP2_MUL_F32, 10 + c, 10 + c, A3D_SRC_VGPR(9));  /* v10+c = alpha * below      */
        code[n++] = a3d_vop2(A3D_OP2_ADD_F32, 6 + c, 10 + c, A3D_SRC_VGPR(6 + c)); /* v6+c = layer + v10+c     */
    }
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 16);                      /* v_cvt_pkrtz_f16_f32 v16, v6, v7 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(6), A3D_SRC_VGPR(7), 0);
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 17);                      /* v_cvt_pkrtz_f16_f32 v17, v8, v9 */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(8), A3D_SRC_VGPR(9), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v16, v17 compr done vm */
    code[n++] = a3d_exp1(16, 17, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* One axis of the soft shadow: vout = clamp(min(pos - lo, hi - pos) * (1 / r), 0, 1); lo / hi / 1/r in SGPRs, v5 is scratch. */
static void shadow_axis(uint32_t *code, uint32_t *n, uint32_t vpos, uint32_t vout, uint32_t s_lo, uint32_t s_hi)
{
    code[(*n)++] = a3d_vop2(A3D_OP2_SUBREV_F32, vout, vpos, SGPR(s_lo));         /* vout = pos - lo            */
    code[(*n)++] = a3d_vop2(A3D_OP2_SUB_F32, 5, vpos, SGPR(s_hi));               /* v5 = hi - pos              */
    code[(*n)++] = a3d_vop2(A3D_OP2_MIN_F32, vout, 5, A3D_SRC_VGPR(vout));       /* vout = min(vout, v5)       */
    code[(*n)++] = a3d_vop2(A3D_OP2_MUL_F32, vout, vout, SGPR(4));               /* vout *= 1 / r              */
    code[(*n)++] = a3d_vop2(A3D_OP2_MIN_F32, vout, vout, A3D_SRC_ONE_F);         /* vout = min(1.0, vout)      */
    code[(*n)++] = a3d_vop2(A3D_OP2_MAX_F32, vout, vout, A3D_SRC_ZERO);          /* vout = max(0.0, vout)      */
}

/* A soft black shadow: alpha = s5 * fx * fy (the same formula as the M3 shadow shader, which amd_3d_shadow_alpha() checks). */
uint32_t amd_comp_build_ps_shadow(uint32_t *code)
{
    uint32_t n = 0;
    shadow_axis(code, &n, 2, 4, 0, 2);                                           /* v4 = fx                    */
    shadow_axis(code, &n, 3, 6, 1, 3);                                           /* v6 = fy                    */
    code[n++] = a3d_vop2(A3D_OP2_MUL_F32, 4, 6, A3D_SRC_VGPR(4));                /* v4 = fx * fy               */
    code[n++] = a3d_vop2(A3D_OP2_MUL_F32, 4, 4, SGPR(5));                        /* v4 *= alpha                */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 0, A3D_SRC_ZERO);                      /* v0 = (r, g) = (0, 0)       */
    code[n++] = a3d_vop1(A3D_OP1_MOV_B32, 5, A3D_SRC_ZERO);                      /* v5 = b = 0.0               */
    code[n++] = a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 1);                       /* v1 = (b, alpha) as halves  */
    code[n++] = a3d_vop3a_1(A3D_SRC_VGPR(5), A3D_SRC_VGPR(4), 0);
    code[n++] = a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1);                     /* exp mrt0 v0, v1 compr done vm */
    code[n++] = a3d_exp1(0, 1, 0, 0);
    code[n++] = A3D_S_ENDPGM;
    return n;
}

/* ---- building a frame ---- */

void amd_comp_begin(AmdCompFrame *f, uint32_t *ib, uint32_t cap, uint64_t rt_mc, uint32_t pitch_px, uint32_t width, uint32_t height,
                    int bgr, uint64_t shader_mc)
{
    Ib b = { ib, 0, cap, 0 };
    f->ib = ib; f->cap = cap;
    f->rt_mc = rt_mc; f->pitch_px = pitch_px; f->width = width; f->height = height; f->bgr = bgr;
    f->vs_mc = shader_mc + AMD_COMP_VS_OFF;
    f->ps_fill_mc = shader_mc + AMD_COMP_PS_FILL_OFF;
    f->ps_image_mc = shader_mc + AMD_COMP_PS_IMAGE_OFF;
    f->ps_shadow_mc = shader_mc + AMD_COMP_PS_SHADOW_OFF;
    f->ps_layer_mc = shader_mc + AMD_COMP_PS_LAYER_OFF;
    f->ps_under_mc = shader_mc + AMD_COMP_PS_UNDER_OFF;
    f->ps_dim_mc = shader_mc + AMD_COMP_PS_DIM_OFF;
    f->draws = 0;
    a3d_emit_head(&b, rt_mc, pitch_px, width, height, bgr);
    f->n = b.n; f->over = b.over;
}

/* One draw of the rectangle with the given pixel shader and its user SGPRs; the vertex shader gets the rectangle as floats. */
static void comp_draw(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, int blend, uint64_t ps_mc, int ps_pos,
                      uint32_t ps_vgprs, const uint32_t *ps_user, uint32_t ps_user_n)
{
    Ib b = { f->ib, f->n, f->cap, f->over };
    uint32_t vs_user[4];
    A3dDraw d;
    if (x1 > f->width) x1 = f->width;
    if (y1 > f->height) y1 = f->height;
    if (x0 >= x1 || y0 >= y1)
        return;
    vs_user[0] = a3d_f32_bits(x0); vs_user[1] = a3d_f32_bits(y0); vs_user[2] = a3d_f32_bits(x1); vs_user[3] = a3d_f32_bits(y1);
    d.x0 = x0; d.y0 = y0; d.x1 = x1; d.y1 = y1;
    d.blend = blend;
    d.ps_mc = ps_mc; d.ps_pos = ps_pos;
    d.ps_vgprs = ps_vgprs;
    d.ps_sgprs = 2;                 /* 24 SGPRs: up to 11 user SGPRs + the system ones + VCC */
    d.ps_user_n = ps_user_n; d.ps_user = ps_user;
    d.vs_mc = f->vs_mc; d.vs_vgprs = 1; d.vs_user_n = 4; d.vs_user = vs_user;
    a3d_emit_draw(&b, &d);
    f->n = b.n; f->over = b.over;
    f->draws++;
}

void amd_comp_fill(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t u[2];
    u[0] = a3d_f16_unorm8(r) | (a3d_f16_unorm8(g) << 16);
    u[1] = a3d_f16_unorm8(b) | (a3d_f16_unorm8(255) << 16);
    comp_draw(f, x0, y0, x1, y1, 0, f->ps_fill_mc, 0, 0, u, 2);
}

void amd_comp_image(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                    uint32_t opacity, int bgr)
{
    Amd3dJob t = { 0 };
    uint32_t u[11];
    t.tex_mc = s->mc; t.tex_w = s->w; t.tex_h = s->h; t.tex_pitch_px = s->pitch_px; t.bgr = bgr;
    amd_3d_build_tex_desc(u, &t);
    u[8] = sx - x0;                 /* texel = pixel + (sx - x0, sy - y0), as wrap-around integers */
    u[9] = sy - y0;
    u[10] = opacity >= 255 ? 0x3F800000u : a3d_f32_ratio(opacity, 255);
    comp_draw(f, x0, y0, x1, y1, opacity < 255, f->ps_image_mc, 1, 2, u, 11);
}

void amd_comp_dim(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                  uint32_t scale, int bgr)
{
    Amd3dJob t = { 0 };
    uint32_t u[11];
    t.tex_mc = s->mc; t.tex_w = s->w; t.tex_h = s->h; t.tex_pitch_px = s->pitch_px; t.bgr = bgr;
    amd_3d_build_tex_desc(u, &t);
    u[8] = sx - x0;
    u[9] = sy - y0;
    u[10] = scale >= 256 ? 0x3F800000u : a3d_f32_ratio(scale, 256);
    comp_draw(f, x0, y0, x1, y1, 0, f->ps_dim_mc, 1, 2, u, 11);
}

void amd_comp_shadow(AmdCompFrame *f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t r, uint32_t alpha)
{
    uint32_t u[6];
    u[0] = a3d_f32_bits(x0); u[1] = a3d_f32_bits(y0); u[2] = a3d_f32_bits(x1); u[3] = a3d_f32_bits(y1);
    u[4] = a3d_f32_ratio(1, r ? r : 1);
    u[5] = a3d_f32_ratio(alpha, 255);
    comp_draw(f, x0, y0, x1, y1, 1, f->ps_shadow_mc, 1, 1, u, 6);
}

/* The float bits of a whole number that may be below 0 (a shadow's box can start left of or above the screen). */
static uint32_t f32_of_int(int v) { return v < 0 ? (a3d_f32_bits((uint32_t)-v) | 0x80000000u) : a3d_f32_bits((uint32_t)v); }

void amd_comp_shadow_part(AmdCompFrame *f, int bx0, int by0, int bx1, int by1, uint32_t r, uint32_t alpha, uint32_t x0, uint32_t y0,
                          uint32_t x1, uint32_t y1)
{
    uint32_t u[6];
    u[0] = f32_of_int(bx0); u[1] = f32_of_int(by0); u[2] = f32_of_int(bx1); u[3] = f32_of_int(by1);
    u[4] = a3d_f32_ratio(1, r ? r : 1);
    u[5] = a3d_f32_ratio(alpha, 255);
    comp_draw(f, x0, y0, x1, y1, 1, f->ps_shadow_mc, 1, 1, u, 6);
}

/* SQ_SEL_W (Mesa gfx9.json SQ_SEL_XYZW01): the image's 4th byte as alpha, where amd_3d_build_tex_desc() gives 1. */
#define E_SQ_SEL_W 7u

void amd_comp_layer(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                    int bgr)
{
    Amd3dJob t = { 0 };
    uint32_t u[10];
    t.tex_mc = s->mc; t.tex_w = s->w; t.tex_h = s->h; t.tex_pitch_px = s->pitch_px; t.bgr = bgr;
    amd_3d_build_tex_desc(u, &t);
    u[3] = (u[3] & ~(7u << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_W)) | (E_SQ_SEL_W << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_W);
    u[8] = sx - x0;
    u[9] = sy - y0;
    comp_draw(f, x0, y0, x1, y1, 2, f->ps_layer_mc, 1, 2, u, 10);
}

void amd_comp_layer_under(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t below_dy, uint32_t x0, uint32_t y0,
                          uint32_t x1, uint32_t y1, int bgr)
{
    Amd3dJob t = { 0 };
    uint32_t u[11];
    t.tex_mc = s->mc; t.tex_w = s->w; t.tex_h = s->h; t.tex_pitch_px = s->pitch_px; t.bgr = bgr;
    amd_3d_build_tex_desc(u, &t);
    u[3] = (u[3] & ~(7u << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_W)) | (E_SQ_SEL_W << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_W);
    u[8] = sx - x0;
    u[9] = sy - y0;
    u[10] = below_dy;
    comp_draw(f, x0, y0, x1, y1, 0, f->ps_under_mc, 1, 4, u, 11);   /* v0 .. v17: 20 VGPRs */
}

uint32_t amd_comp_end(AmdCompFrame *f, uint64_t sync_mc)
{
    Ib b = { f->ib, f->n, f->cap, f->over };
    a3d_emit_tail(&b, sync_mc);
    f->n = b.n; f->over = b.over;
    return f->over ? 0 : f->n;
}

/* ---- the VRAM pool (blocks kept in address order, so neighbours are next to each other in b[]) ---- */
void amd_vpool_init(AmdVPool *p, uint32_t off, uint32_t size)
{
    uint64_t lo = ((uint64_t)off + AMD_VPOOL_GRAIN - 1u) & ~(uint64_t)(AMD_VPOOL_GRAIN - 1u);
    uint64_t hi = ((uint64_t)off + size) & ~(uint64_t)(AMD_VPOOL_GRAIN - 1u);
    if (lo == 0) lo = AMD_VPOOL_GRAIN;  /* offset 0 means "no room" */
    p->n = 0;
    if (hi > lo && hi <= 0x100000000ull) {
        p->b[0].off = (uint32_t)lo; p->b[0].size = (uint32_t)(hi - lo); p->b[0].used = 0;
        p->n = 1;
    }
}

uint32_t amd_vpool_alloc(AmdVPool *p, uint32_t bytes)
{
    uint32_t need, i, k;
    if (!bytes || bytes > 0xFFFFFFFFu - (AMD_VPOOL_GRAIN - 1u)) return 0;
    need = (bytes + AMD_VPOOL_GRAIN - 1u) & ~(AMD_VPOOL_GRAIN - 1u);
    for (i = 0; i < p->n; i++) {
        AmdVBlock *b = &p->b[i];
        if (b->used || b->size < need) continue;
        if (b->size > need && p->n < AMD_VPOOL_BLOCKS) {    /* split: the rest stays free, right after it */
            for (k = p->n; k > i + 1u; k--) p->b[k] = p->b[k - 1u];
            p->b[i + 1u].off = b->off + need; p->b[i + 1u].size = b->size - need; p->b[i + 1u].used = 0;
            p->n++;
            b->size = need;
        }                                                   /* (no entry left to split with: the whole block is given) */
        b->used = 1;
        return b->off;
    }
    return 0;
}

int amd_vpool_free(AmdVPool *p, uint32_t off)
{
    uint32_t i, k;
    for (i = 0; i < p->n && !(p->b[i].off == off && p->b[i].used); i++) {}
    if (i == p->n) return 0;
    p->b[i].used = 0;
    if (i + 1u < p->n && !p->b[i + 1u].used) {               /* merge with the next block */
        p->b[i].size += p->b[i + 1u].size;
        for (k = i + 1u; k + 1u < p->n; k++) p->b[k] = p->b[k + 1u];
        p->n--;
    }
    if (i > 0 && !p->b[i - 1u].used) {                       /* and with the one before */
        p->b[i - 1u].size += p->b[i].size;
        for (k = i; k + 1u < p->n; k++) p->b[k] = p->b[k + 1u];
        p->n--;
    }
    return 1;
}

uint32_t amd_vpool_free_bytes(const AmdVPool *p)
{
    uint32_t i, s = 0;
    for (i = 0; i < p->n; i++)
        if (!p->b[i].used) s += p->b[i].size;
    return s;
}

/* ---- running it, and the M4a test ---- */
#define VRAM_COMP_SHADER_OFF 0x0B004000u   /* 4 KiB: the four shaders at AMD_COMP_*_OFF */
#define VRAM_COMP_SYNC_OFF 0x0B300040u     /* 4 bytes for the end-of-frame sync */
#define VRAM_COMP_SYNC2_OFF 0x0B300044u    /* the same for a second frame in one submission (the scene compositor) */
/* Below 256 MiB: the CPU reaches VRAM only through BAR0, and on this laptop BAR0 covers the first 256 MiB of the 512 MiB (reads at
 * 256 MiB and up gave 0xFFFFFFFF, 2026-10-29). 208 MiB = after the PSP TMR (192 MiB, 4 MiB). */
#define VRAM_COMP_RT_OFF 0x0D000000u       /* the 512 x 256 offscreen target (512 KiB) */
#define VRAM_COMP_SURF_OFF 0x0D100000u     /* the test windows, 256 KiB apart */
#define COMP_RT_W 512u
#define COMP_RT_H 256u

static const char *status = "compositor test not run";
static uint32_t val[AMD_COMP_VALUES];
const char *amd_comp_status(void) { return status; }
uint32_t amd_comp_value(int what) { return (what >= 0 && what < AMD_COMP_VALUES) ? val[what] : 0; }

static unsigned long long comp_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* Writes the seven shaders to VRAM, each padded with zeros (the shader fetch reads ahead). */
static void upload_shaders(void)
{
    static const uint32_t off[7] = { AMD_COMP_VS_OFF, AMD_COMP_PS_FILL_OFF, AMD_COMP_PS_IMAGE_OFF, AMD_COMP_PS_SHADOW_OFF,
                                     AMD_COMP_PS_LAYER_OFF, AMD_COMP_PS_UNDER_OFF, AMD_COMP_PS_DIM_OFF };
    uint32_t code[AMD_COMP_SHADER_MAX], k, i, n;
    for (k = 0; k < 7; k++) {
        n = k == 0 ? amd_comp_build_vs(code) : k == 1 ? amd_comp_build_ps_fill(code) : k == 2 ? amd_comp_build_ps_image(code)
          : k == 3 ? amd_comp_build_ps_shadow(code) : k == 4 ? amd_comp_build_ps_layer(code) : k == 5 ? amd_comp_build_ps_under(code) : amd_comp_build_ps_dim(code);
        for (i = 0; i < 0x200u / 4u; i++)
            amd_gpu_vram_write32(VRAM_COMP_SHADER_OFF + off[k] + 4u * i, i < n ? code[i] : 0u);
    }
}

/* One submission for a frame: the default context state stream (the Linux clear-state table, what a3d_default_state() sends on its
 * own) and then the frame, one after the other in the frame IB. Plain SET_CONTEXT_REG packets and a CONTEXT_CONTROL, so in one IB they
 * do what two IBs did, with one ring submission and one fence wait instead of two. Returns the fence number, 0 = not submitted. */
static uint32_t frame_submit(const uint32_t *ib, uint32_t n)
{
    static uint32_t def[AMD_3D_DEFAULT_IB_MAX];
    static uint32_t nd;
    uint32_t k;
    if (!nd) nd = amd_3d_build_default_ib(def, AMD_3D_DEFAULT_IB_MAX);
    if (!nd || !n || nd + n > amd_gpu_frame_ib_max_dwords()) return 0;
    for (k = 0; k < nd; k++)
        amd_gpu_frame_ib_write32(k, def[k]);
    for (k = 0; k < n; k++)
        amd_gpu_frame_ib_write32(nd + k, ib[k]);
    /* the frames end by waiting until the sync word holds the value their colour-buffer flush writes: it must not be there already
     * from an earlier submission, or the wait ends at once (and a second frame would read what the first one has not finished) */
    amd_gpu_vram_write32(VRAM_COMP_SYNC_OFF, 0);
    amd_gpu_vram_write32(VRAM_COMP_SYNC2_OFF, 0);
    amd_gpu_wc_flush();                 /* the IB, and whatever the CPU wrote that the frame reads */
    return amd_gpu_submit_ib(amd_gpu_frame_ib_mc(), nd + n);
}

/* The test scene, in scene coordinates (0, 0) .. (480, 224); the caller places it at (ox, oy). Three windows painted in order, each
 * after its shadow (offset 6, 8; r 12; alpha 150; the shadow rectangle is the window moved by the offset and grown by r / 2). */
#define SCENE_W 480u
#define SCENE_H 224u
static const struct { uint32_t x0, y0, x1, y1, opacity; } win[3] = {
    { 24, 24, 184, 124, 255 },   /* 160 x 100 */
    { 134, 74, 314, 194, 255 },  /* 180 x 120, over the first one and its shadow */
    { 284, 14, 444, 114, 160 },  /* 160 x 100, partly transparent, over the second one */
};
#define SHADOW_DX 6u
#define SHADOW_DY 8u
#define SHADOW_R 12u
#define SHADOW_A 150u
#define BG_R 0x30u
#define BG_G 0x50u
#define BG_B 0x70u

static uint32_t rgb_px(uint32_t r, uint32_t g, uint32_t b, int bgr) { return bgr ? ((r << 16) | (g << 8) | b) : ((b << 16) | (g << 8) | r); }

/* Pixel (x, y) of window k's picture: a 16-row title bar, a 1-pixel darker border, and a body with a gradient. */
static uint32_t win_texel(uint32_t k, uint32_t x, uint32_t y, int bgr)
{
    uint32_t w = win[k].x1 - win[k].x0, h = win[k].y1 - win[k].y0;
    if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
        return rgb_px(0x18, 0x18, 0x20, bgr);
    if (y < 16)
        return rgb_px(k == 0 ? 0x40u : k == 1 ? 0xC0u : 0x40u, k == 1 ? 0x60u : 0x90u, k == 2 ? 0x30u : 0xE0u, bgr);
    return rgb_px(0xF0u - (x * 0x40u) / w, 0xF0u - (y * 0x60u) / h, 0xE8u - k * 0x30u, bgr);
}

static void shadow_rect(uint32_t k, uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1)
{
    *x0 = win[k].x0 + SHADOW_DX - SHADOW_R / 2u; *y0 = win[k].y0 + SHADOW_DY - SHADOW_R / 2u;
    *x1 = win[k].x1 + SHADOW_DX + SHADOW_R / 2u; *y1 = win[k].y1 + SHADOW_DY + SHADOW_R / 2u;
}

/* What the scene should be at scene pixel (x, y): the background, then every shadow and window "over" it in the painting order. */
static uint32_t scene_px(uint32_t x, uint32_t y, int bgr)
{
    uint32_t v = rgb_px(BG_R, BG_G, BG_B, bgr), k;
    for (k = 0; k < 3; k++) {
        Amd3dJob s = { 0 };
        shadow_rect(k, &s.x0, &s.y0, &s.x1, &s.y1);
        s.shadow_r = SHADOW_R; s.a = SHADOW_A;
        if (x >= s.x0 && x < s.x1 && y >= s.y0 && y < s.y1)
            v = amd_3d_blend_px(0, v, amd_3d_shadow_alpha(&s, x, y));
        if (x >= win[k].x0 && x < win[k].x1 && y >= win[k].y0 && y < win[k].y1) {
            uint32_t t = win_texel(k, x - win[k].x0, y - win[k].y0, bgr);
            v = win[k].opacity >= 255 ? t : amd_3d_blend_px(t, v, win[k].opacity);
        }
    }
    return v & 0x00FFFFFFu;
}

static int near_n(uint32_t a, uint32_t b, int tol)
{
    int s;
    for (s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 0xFFu) - (int)((b >> s) & 0xFFu);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}
static int near3(uint32_t a, uint32_t b) { return near_n(a, b, 3); }

static void keep_fault_registers(void)
{
    amd_gpu_fault_snapshot();
    val[AMD_COMP_V_FAULT_GC] = amd_native_fault_gc_status(); val[AMD_COMP_V_FAULT_GC + 1] = amd_native_fault_gc_lo();
    val[AMD_COMP_V_FAULT_GC + 2] = amd_native_fault_gc_hi();
}

/* Composites the scene at (ox, oy) of the target (VRAM offset base_off, GPU address rt_mc) in ONE submission through the frame IB and
 * checks it, with a margin of 8 pixels around it that must stay as the CPU wrote it. */
static int comp_run(uint32_t base_off, uint64_t rt_mc, uint32_t pitch_px, uint32_t width, uint32_t height, int bgr, uint32_t ox, uint32_t oy)
{
    static uint32_t ib[4096];
    static int shaders_up;
    AmdSurface surf[3];
    AmdCompFrame f;
    uint32_t k, x, y, n, seq, cap;
    uint32_t cx0 = ox >= 8u ? ox - 8u : 0, cy0 = oy >= 8u ? oy - 8u : 0;
    uint32_t cx1 = ox + SCENE_W + 8u > width ? width : ox + SCENE_W + 8u, cy1 = oy + SCENE_H + 8u > height ? height : oy + SCENE_H + 8u;
    uint64_t shader_mc = amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u), sync_mc = amd_native_vram_mc_addr(VRAM_COMP_SYNC_OFF, 4u);
    uint64_t frame_mc = amd_gpu_frame_ib_mc();
    unsigned long long t0;
    const uint32_t pattern = 0x00112233u;

    for (k = 0; k < AMD_COMP_VALUES; k++) val[k] = 0;
    if (!amd_gpu_ring_running()) { status = "compositor test skipped: the persistent ring is not running"; return 0; }
    if (!shader_mc || !sync_mc || !rt_mc || !frame_mc) { status = "compositor test skipped: no MC address"; return 0; }
    if (ox + SCENE_W > width || oy + SCENE_H > height) { status = "compositor test skipped: the target is too small"; return 0; }

    /* the CPU must reach the target and the surfaces (BAR0 may be smaller than the VRAM): a word written must read back */
    amd_gpu_vram_write32(base_off, 0x5AA5C33Cu);
    amd_gpu_vram_write32(VRAM_COMP_SURF_OFF + 2u * 0x40000u, 0xC33C5AA5u);
    amd_gpu_wc_flush();
    if (amd_gpu_vram_read32(base_off) != 0x5AA5C33Cu || amd_gpu_vram_read32(VRAM_COMP_SURF_OFF + 2u * 0x40000u) != 0xC33C5AA5u) {
        status = "compositor test skipped: the CPU cannot read back what it wrote to the target / surfaces (outside BAR0?)";
        return 0;
    }

    if (!shaders_up) { upload_shaders(); shaders_up = 1; }
    for (k = 0; k < 3; k++) {               /* the windows' pictures */
        surf[k].off = VRAM_COMP_SURF_OFF + k * 0x40000u;
        surf[k].w = win[k].x1 - win[k].x0; surf[k].h = win[k].y1 - win[k].y0; surf[k].pitch_px = 256;
        surf[k].mc = amd_native_vram_mc_addr(surf[k].off, surf[k].pitch_px * surf[k].h * 4u);
        if (!surf[k].mc) { status = "compositor test skipped: no MC address for a surface"; return 0; }
        for (y = 0; y < surf[k].h; y++)
            for (x = 0; x < surf[k].w; x++)
                amd_gpu_vram_write32(surf[k].off + (y * surf[k].pitch_px + x) * 4u, win_texel(k, x, y, bgr));
    }
    for (y = cy0; y < cy1; y++)              /* what must stay around the scene (and is painted over inside it) */
        for (x = cx0; x < cx1; x++)
            amd_gpu_vram_write32(base_off + (y * pitch_px + x) * 4u, pattern);
    amd_gpu_vram_write32(VRAM_COMP_SYNC_OFF, 0);
    amd_gpu_wc_flush();

    /* the frame: background, then per window its shadow and the window */
    cap = amd_gpu_frame_ib_max_dwords() < 4096u ? amd_gpu_frame_ib_max_dwords() : 4096u;
    amd_comp_begin(&f, ib, cap, rt_mc, pitch_px, width, height, bgr, shader_mc);
    amd_comp_fill(&f, ox, oy, ox + SCENE_W, oy + SCENE_H, BG_R, BG_G, BG_B);
    for (k = 0; k < 3; k++) {
        uint32_t sx0, sy0, sx1, sy1;
        shadow_rect(k, &sx0, &sy0, &sx1, &sy1);
        amd_comp_shadow(&f, ox + sx0, oy + sy0, ox + sx1, oy + sy1, SHADOW_R, SHADOW_A);
        amd_comp_image(&f, &surf[k], 0, 0, ox + win[k].x0, oy + win[k].y0, ox + win[k].x1, oy + win[k].y1, win[k].opacity, bgr);
    }
    n = amd_comp_end(&f, sync_mc);
    val[AMD_COMP_V_DWORDS] = n; val[AMD_COMP_V_DRAWS] = f.draws;
    if (!n) { status = "compositor test skipped: the frame does not fit"; return 0; }

    if (a3d_default_state()) { status = "compositor test FAILED: the default state stream did not run"; return 0; }
    for (k = 0; k < n; k++)
        amd_gpu_frame_ib_write32(k, ib[k]);
    amd_gpu_wc_flush();
    amd_gpu_fault_clear();
    t0 = comp_tsc();
    seq = amd_gpu_submit_ib(frame_mc, n);
    if (!seq) { status = "compositor test FAILED: the ring did not take the frame"; return 0; }
    if (!amd_gpu_fence_wait(seq, 8000000u)) {
        keep_fault_registers();
        val[AMD_COMP_V_GRBM] = amd_gpu_reg_read32(0x2004u);               /* GRBM_STATUS: which blocks are still busy */
        status = "compositor test FAILED: fence timeout (the GPU did not finish the frame)";
        return 0;
    }
    val[AMD_COMP_V_CYCLES] = (uint32_t)(comp_tsc() - t0);

    for (y = cy0; y < cy1; y++)
        for (x = cx0; x < cx1; x++) {
            uint32_t v = amd_gpu_vram_read32(base_off + (y * pitch_px + x) * 4u) & 0x00FFFFFFu;
            if (x >= ox && x < ox + SCENE_W && y >= oy && y < oy + SCENE_H) {
                uint32_t want = scene_px(x - ox, y - oy, bgr);
                val[AMD_COMP_V_IN_TOTAL]++;
                if (near3(v, want)) val[AMD_COMP_V_IN_GOOD]++;
                else if (!val[AMD_COMP_V_FIRST_IN]) {
                    val[AMD_COMP_V_FIRST_IN] = v | 0x01000000u; val[AMD_COMP_V_FIRST_IN + 1] = want;
                    val[AMD_COMP_V_FIRST_IN + 2] = (x - ox) | ((y - oy) << 16);
                }
            } else {
                val[AMD_COMP_V_OUT_TOTAL]++;
                if (v != (pattern & 0x00FFFFFFu)) val[AMD_COMP_V_OUT_BAD]++;
            }
        }
    if (val[AMD_COMP_V_IN_GOOD] == val[AMD_COMP_V_IN_TOTAL] && !val[AMD_COMP_V_OUT_BAD]) {
        status = "compositor PASS: background, 3 shadows and 3 windows (one 63% opaque) in one submission, every pixel right";
        return 1;
    }
    keep_fault_registers();
    status = val[AMD_COMP_V_IN_GOOD] ? "compositor FAILED: some pixels are wrong" : "compositor FAILED: nothing of the scene is there";
    return 0;
}

int amd_comp_test_offscreen(void)
{
    uint64_t rt_mc = amd_native_vram_mc_addr(VRAM_COMP_RT_OFF, COMP_RT_W * COMP_RT_H * 4u);
    return comp_run(VRAM_COMP_RT_OFF, rt_mc, COMP_RT_W, COMP_RT_W, COMP_RT_H, 1, 16, 16);
}

/* On the screen: the scene in the top right corner, 20 pixels in. */
int amd_comp_test_screen(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr)
{
    uint64_t off64;
    if (!pitch_bytes || (pitch_bytes & 3u) || width < SCENE_W + 40u || height < SCENE_H + 40u || fb_phys < vram_cpu) {
        status = "compositor screen test skipped: bad framebuffer info"; return 0; }
    off64 = fb_phys - vram_cpu;
    if (off64 + (uint64_t)pitch_bytes * height > (uint64_t)VRAM_COMP_SHADER_OFF) {
        status = "compositor screen test skipped: the framebuffer reaches the test memory"; return 0; }
    return comp_run((uint32_t)off64, amd_native_vram_mc_addr((uint32_t)off64, pitch_bytes * height), pitch_bytes / 4u, width, height, bgr,
                    width - SCENE_W - 20u, 20u);
}

/* ---- M4b: surfaces in the pool, uploads ---- */
/* 16 MiB .. 127 MiB: after the screen (the firmware's frame buffer at VRAM 0, 8 MiB at 1920 x 1080), before the GART page
 * directory (127 MiB); nothing else of HarisOS is there. The scene compositor needs room: a screen-size back buffer and
 * wallpaper, and every window. */
#define VRAM_COMP_POOL_OFF 0x01000000u
#define VRAM_COMP_POOL_BYTES 0x06F00000u
static AmdVPool pool;
static int pool_up;

int amd_surface_alloc(AmdSurface *s, uint32_t w, uint32_t h)
{
    uint32_t pitch = (w + 63u) & ~63u, off;
    s->mc = 0; s->off = 0; s->w = 0; s->h = 0; s->pitch_px = 0;
    if (!w || !h || w > 16384u || h > 16384u) return 0;
    if (!pool_up) { amd_vpool_init(&pool, VRAM_COMP_POOL_OFF, VRAM_COMP_POOL_BYTES); pool_up = 1; }
    off = amd_vpool_alloc(&pool, pitch * h * 4u);
    if (!off) return 0;
    s->mc = amd_native_vram_mc_addr(off, pitch * h * 4u);
    if (!s->mc) { amd_vpool_free(&pool, off); return 0; }
    s->off = off; s->w = w; s->h = h; s->pitch_px = pitch;
    return 1;
}

void amd_surface_free(AmdSurface *s)
{
    if (s->off) amd_vpool_free(&pool, s->off);
    s->mc = 0; s->off = 0; s->w = 0; s->h = 0; s->pitch_px = 0;
}

void amd_surface_upload(const AmdSurface *s, const uint32_t *src, uint32_t src_pitch_px, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r;
    if (!s->off || x >= s->w || y >= s->h) return;
    if (w > s->w - x) w = s->w - x;
    if (h > s->h - y) h = s->h - y;
    for (r = 0; r < h; r++)
        amd_gpu_vram_write_span(s->off + ((y + r) * s->pitch_px + x) * 4u, src + (uint64_t)r * src_pitch_px, w);
}

static uint32_t uval[AMD_COMP_U_VALUES];
uint32_t amd_comp_upload_value(int what) { return (what >= 0 && what < AMD_COMP_U_VALUES) ? uval[what] : 0; }

int amd_comp_upload_test(void)
{
    static uint32_t pic[512u * 512u];   /* 1 MiB in RAM */
    AmdSurface s;
    uint32_t x, y, k, bad = 0;
    unsigned long long t0;
    for (k = 0; k < AMD_COMP_U_VALUES; k++) uval[k] = 0;
    if (!amd_gpu_ring_running()) { status = "upload test skipped: the persistent ring is not running"; return 0; }
    if (!amd_surface_alloc(&s, 512, 512)) { status = "upload test skipped: no surface"; return 0; }
    for (y = 0; y < 512u; y++)
        for (x = 0; x < 512u; x++)
            pic[y * 512u + x] = (x * 0x01010101u) ^ (y << 12) ^ 0x00A5005Au;
    uval[AMD_COMP_U_OFF] = s.off;
    uval[AMD_COMP_U_BYTES] = 512u * 512u * 4u;
    t0 = comp_tsc();
    amd_surface_upload(&s, pic, 512, 0, 0, 512, 512);
    amd_gpu_wc_flush();
    uval[AMD_COMP_U_CYCLES] = (uint32_t)(comp_tsc() - t0);
    for (y = 0; y < 512u; y++)
        for (x = 0; x < 512u; x++)
            if (amd_gpu_vram_read32(s.off + (y * s.pitch_px + x) * 4u) != pic[y * 512u + x]) bad++;
    uval[AMD_COMP_U_BAD] = bad;
    amd_surface_free(&s);
    uval[AMD_COMP_U_POOL_FREE] = amd_vpool_free_bytes(&pool);
    status = bad ? "upload FAILED: words read back wrong" : "upload PASS: 1 MiB RAM -> VRAM surface, every word right";
    return !bad;
}

/* ---- M4b-2: the window being dragged, pasted by the GPU (the gfx overlay driver, see include/gfx.h) ---- */
static struct
{
    int up;
    uint64_t rt_mc;
    uint32_t pitch_px, width, height;
    int bgr;
    AmdSurface s;                       /* the last uploaded layer, and the picture below it in rows lh .. 2 lh - 1 */
    uint32_t lh;                        /* the layer's height */
} ov;
static uint32_t oval[AMD_COMP_OV_VALUES];
uint32_t amd_comp_overlay_value(int what) { return (what >= 0 && what < AMD_COMP_OV_VALUES) ? oval[what] : 0; }

int amd_comp_overlay_init(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr)
{
    uint64_t off64;
    ov.up = 0;
    if (!amd_gpu_ring_running() || !pitch_bytes || (pitch_bytes & 3u) || !width || !height || fb_phys < vram_cpu) return 0;
    off64 = fb_phys - vram_cpu;
    if (off64 + (uint64_t)pitch_bytes * height > (uint64_t)VRAM_COMP_SHADER_OFF) return 0;
    ov.rt_mc = amd_native_vram_mc_addr((uint32_t)off64, pitch_bytes * height);
    if (!ov.rt_mc || !amd_gpu_frame_ib_mc() || !amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u)) return 0;
    ov.pitch_px = pitch_bytes / 4u; ov.width = width; ov.height = height; ov.bgr = bgr;
    upload_shaders();
    amd_gpu_wc_flush();
    ov.up = 1;
    return 1;
}

int amd_comp_overlay_upload(const void *layer, int lw, int lh)
{
    unsigned long long t0 = comp_tsc();
    if (!ov.up || !layer || lw <= 0 || lh <= 0) return -1;
    if (ov.s.off && (ov.s.w != (uint32_t)lw || ov.lh != (uint32_t)lh))
        amd_surface_free(&ov.s);
    /* one surface twice as high: the layer in rows 0 .. lh-1, the picture below it (written each frame by gfx_present) under it */
    if (!ov.s.off && !amd_surface_alloc(&ov.s, (uint32_t)lw, 2u * (uint32_t)lh)) { oval[AMD_COMP_OV_FAILS]++; return -1; }
    ov.lh = (uint32_t)lh;
    amd_surface_upload(&ov.s, (const uint32_t *)layer, (uint32_t)lw, 0, 0, (uint32_t)lw, (uint32_t)lh);
    amd_gpu_wc_flush();
    oval[AMD_COMP_OV_UPLOADS]++;
    oval[AMD_COMP_OV_UPLOAD_CYCLES] = (uint32_t)(comp_tsc() - t0);
    return 0;
}

volatile unsigned int *amd_comp_overlay_below(int *pitch)
{
    if (!ov.up || !ov.s.off) return 0;
    *pitch = (int)ov.s.pitch_px;
    return (volatile unsigned int *)amd_gpu_vram_ptr(ov.s.off + ov.lh * ov.s.pitch_px * 4u);
}

int amd_comp_overlay_draw(int x, int y, int x0, int y0, int x1, int y1)
{
    static uint32_t ib[1024];
    AmdCompFrame f;
    uint32_t n, seq;
    unsigned long long t0 = comp_tsc();
    if (!ov.up || !ov.s.off || !amd_gpu_ring_running()) { oval[AMD_COMP_OV_FAILS]++; return -1; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)ov.width) x1 = (int)ov.width;
    if (y1 > (int)ov.height) y1 = (int)ov.height;
    if (x0 < x) x0 = x;
    if (y0 < y) y0 = y;
    if (x1 > x + (int)ov.s.w) x1 = x + (int)ov.s.w;
    if (y1 > y + (int)ov.lh) y1 = y + (int)ov.lh;
    if (x0 >= x1 || y0 >= y1) return 0;  /* nothing of the layer in the box */

    amd_comp_begin(&f, ib, 1024, ov.rt_mc, ov.pitch_px, ov.width, ov.height, ov.bgr,
                   amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u));
    amd_comp_layer_under(&f, &ov.s, (uint32_t)(x0 - x), (uint32_t)(y0 - y), ov.lh, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1,
                         (uint32_t)y1, ov.bgr);
    n = amd_comp_end(&f, amd_native_vram_mc_addr(VRAM_COMP_SYNC_OFF, 4u));
    if (!n || n > amd_gpu_frame_ib_max_dwords()) { oval[AMD_COMP_OV_FAILS]++; return -1; }
    seq = frame_submit(ib, n);          /* default state + frame, and the picture below the layer the CPU just wrote is flushed */
    if (!seq || !amd_gpu_fence_wait(seq, 2000000u)) { oval[AMD_COMP_OV_FAILS]++; return -1; }
    oval[AMD_COMP_OV_DRAWS]++;
    oval[AMD_COMP_OV_DRAW_CYCLES] = (uint32_t)(comp_tsc() - t0);
    return 0;
}

/* What blend mode 2 should give for one pixel: colour + below * t / 255, per byte (t = the layer's 4th byte). */
uint32_t amd_comp_layer_px(uint32_t layer, uint32_t below)
{
    uint32_t t = layer >> 24, v = 0, s;
    for (s = 0; s < 24; s += 8) {
        uint32_t c = ((layer >> s) & 0xFFu) + (((below >> s) & 0xFFu) * t + 127u) / 255u;
        v |= (c > 255u ? 255u : c) << s;
    }
    return v;
}

/* The 96 x 64 test layer: opaque inside, a fading edge 16 pixels wide, a corner nothing was drawn in; packed as gfx_layer_pack() does. */
static void test_layer(uint32_t *lay)
{
    uint32_t x, y;
    for (y = 0; y < 64u; y++)
        for (x = 0; x < 96u; x++) {
            uint32_t e = x < y ? x : y, t = e >= 16u ? 0u : 255u - e * 16u;   /* see-through at the edge, opaque inside */
            uint32_t c = (x * 2u) | ((y * 3u) << 8) | (0x30u << 16);
            if (x >= 80u && y < 16u) { t = 255u; c = 0; }                       /* a corner nothing was drawn in */
            c = (((c & 0xFFu) * (255u - t) / 255u)) | ((((c >> 8) & 0xFFu) * (255u - t) / 255u) << 8) |
                ((((c >> 16) & 0xFFu) * (255u - t) / 255u) << 16);              /* premultiplied */
            lay[y * 96u + x] = c | (t << 24);
        }
}

/* The M4b-2 check before the overlay is used: a 96 x 64 layer (opaque middle, a fading edge, a see-through corner) pasted with blend
 * mode 2 onto a pattern in the offscreen buffer; every pixel within 2 of amd_comp_layer_px(). 1 = right. */
int amd_comp_layer_test(void)
{
    static uint32_t lay[96u * 64u];
    AmdSurface s;
    AmdCompFrame f;
    static uint32_t ib[1024];
    uint32_t x, y, n, seq, bad = 0, k;
    const uint32_t ox = 40, oy = 30, pattern = 0x00406080u;
    uint64_t rt_mc = amd_native_vram_mc_addr(VRAM_COMP_RT_OFF, COMP_RT_W * COMP_RT_H * 4u);
    for (k = 0; k < AMD_COMP_VALUES; k++) val[k] = 0;
    if (!amd_gpu_ring_running() || !rt_mc) { status = "layer test skipped: no ring / no MC address"; return 0; }
    test_layer(lay);
    upload_shaders();
    if (!amd_surface_alloc(&s, 96, 64)) { status = "layer test skipped: no surface"; return 0; }
    amd_surface_upload(&s, lay, 96, 0, 0, 96, 64);
    for (y = 0; y < 64u + 2u * oy; y++)
        for (x = 0; x < 96u + 2u * ox; x++)
            amd_gpu_vram_write32(VRAM_COMP_RT_OFF + (y * COMP_RT_W + x) * 4u, pattern);
    amd_gpu_wc_flush();
    amd_comp_begin(&f, ib, 1024, rt_mc, COMP_RT_W, COMP_RT_W, COMP_RT_H, 1, amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u));
    amd_comp_layer(&f, &s, 0, 0, ox, oy, ox + 96u, oy + 64u, 1);
    n = amd_comp_end(&f, amd_native_vram_mc_addr(VRAM_COMP_SYNC_OFF, 4u));
    if (!n || a3d_default_state()) { amd_surface_free(&s); status = "layer test FAILED: the frame / default state"; return 0; }
    for (k = 0; k < n; k++)
        amd_gpu_frame_ib_write32(k, ib[k]);
    amd_gpu_wc_flush();
    amd_gpu_fault_clear();
    seq = amd_gpu_submit_ib(amd_gpu_frame_ib_mc(), n);
    if (!seq || !amd_gpu_fence_wait(seq, 8000000u)) {
        keep_fault_registers();
        val[AMD_COMP_V_GRBM] = amd_gpu_reg_read32(0x2004u);
        amd_surface_free(&s);
        status = "layer test FAILED: fence timeout";
        return 0;
    }
    for (y = 0; y < 64u + 2u * oy; y++)
        for (x = 0; x < 96u + 2u * ox; x++) {
            uint32_t v = amd_gpu_vram_read32(VRAM_COMP_RT_OFF + (y * COMP_RT_W + x) * 4u) & 0x00FFFFFFu, want = pattern;
            int in = x >= ox && x < ox + 96u && y >= oy && y < oy + 64u;
            if (in) want = amd_comp_layer_px(lay[(y - oy) * 96u + (x - ox)], pattern);
            if (in) val[AMD_COMP_V_IN_TOTAL]++; else val[AMD_COMP_V_OUT_TOTAL]++;
            if (in ? near_n(v, want, 2) : v == pattern) { if (in) val[AMD_COMP_V_IN_GOOD]++; continue; }
            if (!in) val[AMD_COMP_V_OUT_BAD]++;
            if (!bad++) {
                val[AMD_COMP_V_FIRST_IN] = v | 0x01000000u; val[AMD_COMP_V_FIRST_IN + 1] = want;
                val[AMD_COMP_V_FIRST_IN + 2] = x | (y << 16);
            }
        }
    amd_surface_free(&s);
    val[AMD_COMP_V_DWORDS] = n; val[AMD_COMP_V_DRAWS] = f.draws;
    if (bad) { keep_fault_registers(); status = "layer FAILED: blend mode 2 (ONE / SRC_ALPHA) pixels are wrong"; return 0; }
    status = "layer PASS: a premultiplied layer pasted with blend mode 2, every pixel right";
    return 1;
}

/* The window drag as the overlay does it, as a test: 8 rounds of "the CPU writes the picture below the layer fast (one row at a time,
 * like gfx_present()) into the rows under the layer, the GPU writes layer + below in one pass (amd_comp_layer_under), the CPU reads it
 * back", each round a different picture, so an old pixel shows. Around the layer the target must keep what the CPU wrote there.
 * Returns the number of wrong pixels (0 = right), ~0u = could not run; amd_comp_value(AMD_COMP_V_IN_TOTAL) = pixels checked. */
uint32_t amd_comp_drag_test(void)
{
    static uint32_t lay[96u * 64u], row[176u];
    static uint32_t ib[1024];
    AmdSurface s;
    AmdCompFrame f;
    uint32_t x, y, n = 0, seq, k, round, bad = 0;
    unsigned long long t0;
    const uint32_t ox = 40, oy = 30, w = 96u + 2u * 40u, h = 64u + 2u * 30u, around = 0x00C0A080u;
    uint64_t rt_mc = amd_native_vram_mc_addr(VRAM_COMP_RT_OFF, COMP_RT_W * COMP_RT_H * 4u);
    for (k = 0; k < AMD_COMP_VALUES; k++) val[k] = 0;
    if (!amd_gpu_ring_running() || !rt_mc) { status = "drag test skipped: no ring / no MC address"; return ~0u; }
    test_layer(lay);
    upload_shaders();
    if (!amd_surface_alloc(&s, 96, 128)) { status = "drag test skipped: no surface"; return ~0u; }
    amd_surface_upload(&s, lay, 96, 0, 0, 96, 64);
    for (round = 0; round < 8u; round++) {
        uint32_t pattern = ((0x00102030u * (round + 1u)) ^ (round & 1u ? 0x00FF00FFu : 0u)) & 0x00FFFFFFu;
        for (x = 0; x < w; x++) row[x] = around;
        for (y = 0; y < h; y++)                                           /* the screen: the old frame */
            amd_gpu_vram_write_span(VRAM_COMP_RT_OFF + y * COMP_RT_W * 4u, row, w);
        for (x = 0; x < 96u; x++) row[x] = pattern;
        for (y = 0; y < 64u; y++)                                         /* the picture below the layer, fast */
            amd_gpu_vram_write_span(s.off + (64u + y) * s.pitch_px * 4u, row, 96u);
        __asm__ volatile("sfence" ::: "memory");
        amd_comp_begin(&f, ib, 1024, rt_mc, COMP_RT_W, COMP_RT_W, COMP_RT_H, 1, amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u));
        amd_comp_layer_under(&f, &s, 0, 0, 64u, ox, oy, ox + 96u, oy + 64u, 1);
        n = amd_comp_end(&f, amd_native_vram_mc_addr(VRAM_COMP_SYNC_OFF, 4u));
        if (!n) { bad = ~0u; status = "drag test FAILED: the frame does not fit"; break; }
        amd_gpu_fault_clear();
        t0 = comp_tsc();
        seq = frame_submit(ib, n);          /* default state + frame in one submission, as the overlay does it */
        if (!seq || !amd_gpu_fence_wait(seq, 8000000u)) {
            keep_fault_registers();
            val[AMD_COMP_V_GRBM] = amd_gpu_reg_read32(0x2004u);
            bad = ~0u; status = "drag test FAILED: fence timeout"; break;
        }
        val[AMD_COMP_V_CYCLES] = (uint32_t)(comp_tsc() - t0);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t v = amd_gpu_vram_read32(VRAM_COMP_RT_OFF + (y * COMP_RT_W + x) * 4u) & 0x00FFFFFFu, want = around;
                int in = x >= ox && x < ox + 96u && y >= oy && y < oy + 64u;
                if (in) want = amd_comp_layer_px(lay[(y - oy) * 96u + (x - ox)], pattern);
                val[AMD_COMP_V_IN_TOTAL]++;
                if (in ? near_n(v, want, 2) : v == want) { val[AMD_COMP_V_IN_GOOD]++; continue; }
                if (!bad++) {
                    val[AMD_COMP_V_FIRST_IN] = v | 0x01000000u; val[AMD_COMP_V_FIRST_IN + 1] = want;
                    val[AMD_COMP_V_FIRST_IN + 2] = x | (y << 16) | (round << 28);
                }
            }
    }
    val[AMD_COMP_V_DWORDS] = n; val[AMD_COMP_V_DRAWS] = 1;
    amd_surface_free(&s);
    if (bad == 0) status = "drag PASS: 8 rounds of layer + picture below written in one pass, every pixel right";
    else if (bad != ~0u) { keep_fault_registers(); status = "drag FAILED: pixels are wrong"; }
    return bad;
}

/* ---- M4c: the scene compositor (the gfx compositor driver, see include/gfx.h) ----
 * Surfaces by id (index + 1). A compose puts the items together in a screen-size back buffer in VRAM (frame 1), then copies the
 * changed rectangles from it onto the screen (frame 2): the screen gets each pixel once, finished. Both frames, after the default
 * state stream, go in one submission (frame_submit). */
#define SC_MAX 64
#define SC_IB_MAX 24000u
static AmdSurface sc_surf[SC_MAX];
static struct
{
    int up;
    uint64_t fb_mc;
    uint32_t fb_pitch_px, width, height;
    int bgr;
    AmdSurface bb;                      /* the back buffer */
    uint32_t dim;                       /* the screen's brightness, 0 .. 256 (256: full); 0 means "not set": full */
} sc;
static uint32_t scval[AMD_COMP_SC_VALUES];
static AmdSceneSums sums; /* since the last amd_comp_scene_sums() */
void amd_comp_scene_sums(AmdSceneSums *out)
{
    *out = sums;
    sums = (AmdSceneSums){ 0 };
}
uint32_t amd_comp_scene_value(int what) { return (what >= 0 && what < AMD_COMP_SC_VALUES) ? scval[what] : 0; }

int amd_comp_surface_new(int w, int h)
{
    for (int i = 0; i < SC_MAX; i++)
        if (!sc_surf[i].off)
            return amd_surface_alloc(&sc_surf[i], (uint32_t)w, (uint32_t)h) ? i + 1 : 0;
    return 0;
}

void amd_comp_surface_free(int id)
{
    if (id >= 1 && id <= SC_MAX)
        amd_surface_free(&sc_surf[id - 1]);
}

int amd_comp_surface_upload(int id, const void *px, int pitch, int x, int y, int w, int h, int opaque)
{
    static uint32_t row[16384];
    const AmdSurface *s;
    if (id < 1 || id > SC_MAX || !px || w <= 0 || h <= 0 || x < 0 || y < 0) return -1;
    s = &sc_surf[id - 1];
    if (!s->off || (uint32_t)(x + w) > s->w || (uint32_t)(y + h) > s->h || w > 16384) return -1;
    for (int r = 0; r < h; r++) {
        const uint32_t *src = (const uint32_t *)px + (long long)r * pitch;
        uint32_t off = s->off + ((uint32_t)(y + r) * s->pitch_px + (uint32_t)x) * 4u;
        if (opaque) {                   /* in a layer the top byte is how much shows through: 0 */
            for (int k = 0; k < w; k++) row[k] = src[k] & 0x00FFFFFFu;
            amd_gpu_vram_write_span(off, row, (uint32_t)w);
        } else
            amd_gpu_vram_write_span(off, src, (uint32_t)w);
    }
    scval[AMD_COMP_SC_UPLOADS]++;
    sums.upload_bytes += (uint64_t)w * (uint64_t)h * 4u;
    return 0;
}

/* [x0,x1) x [y0,y1) cut down to the item and to w x h; 1 when something is left */
static int cut(int *x0, int *y0, int *x1, int *y1, const GfxCompItem *it, uint32_t w, uint32_t h)
{
    if (*x0 < it->x) *x0 = it->x;
    if (*y0 < it->y) *y0 = it->y;
    if (*x1 > it->x + it->w) *x1 = it->x + it->w;
    if (*y1 > it->y + it->h) *y1 = it->y + it->h;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > (int)w) *x1 = (int)w;
    if (*y1 > (int)h) *y1 = (int)h;
    return *x0 < *x1 && *y0 < *y1;
}

/* The compose into a target (the screen, or the test buffer): rt_mc, its pitch and size. */
static int compose_to(uint64_t rt_mc, uint32_t rt_pitch_px, uint32_t w, uint32_t h, const GfxCompItem *items, int n,
                      const int *rects, int nr)
{
    static uint32_t ib[SC_IB_MAX];
    AmdCompFrame f;
    uint32_t na, nb, seq;
    uint64_t shader_mc = amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u), sync_mc = amd_native_vram_mc_addr(VRAM_COMP_SYNC_OFF, 4u);
    unsigned long long t0 = comp_tsc();
    if (!sc.up || !amd_gpu_ring_running() || w > sc.bb.w || h > sc.bb.h) return -1;

    amd_comp_begin(&f, ib, SC_IB_MAX, sc.bb.mc, sc.bb.pitch_px, w, h, sc.bgr, shader_mc);   /* 1: the scene into the back buffer */
    for (int r = 0; r < nr; r++)
        for (int i = 0; i < n; i++) {
            const GfxCompItem *it = &items[i];
            int x0 = rects[4 * r], y0 = rects[4 * r + 1], x1 = rects[4 * r + 2], y1 = rects[4 * r + 3];
            const AmdSurface *s;
            if (it->layer == 2) /* a soft shadow: no surface; `surface` is its blur */
            {
                if (cut(&x0, &y0, &x1, &y1, it, w, h))
                    amd_comp_shadow_part(&f, it->x, it->y, it->x + it->w, it->y + it->h, (uint32_t)it->surface, (uint32_t)it->alpha,
                                         (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
                continue;
            }
            if (it->surface < 1 || it->surface > SC_MAX || !sc_surf[it->surface - 1].off) return -1;
            if (!cut(&x0, &y0, &x1, &y1, it, w, h)) continue;
            s = &sc_surf[it->surface - 1];
            if (it->layer)
                amd_comp_layer(&f, s, (uint32_t)(x0 - it->x), (uint32_t)(y0 - it->y), (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1,
                               sc.bgr);
            else
                amd_comp_image(&f, s, (uint32_t)(x0 - it->x), (uint32_t)(y0 - it->y), (uint32_t)x0, (uint32_t)y0, (uint32_t)x1,
                               (uint32_t)y1, 255, sc.bgr);
        }
    scval[AMD_COMP_SC_DRAWS] = f.draws;
    na = amd_comp_end(&f, sync_mc);
    if (!na) return -1;
    amd_comp_begin(&f, ib + na, SC_IB_MAX - na, rt_mc, rt_pitch_px, w, h, sc.bgr, shader_mc);   /* 2: the changed parts onto the target */
    for (int r = 0; r < nr; r++) {
        GfxCompItem all = { 0, 0, 0, (int)w, (int)h, 0, 0 };
        int x0 = rects[4 * r], y0 = rects[4 * r + 1], x1 = rects[4 * r + 2], y1 = rects[4 * r + 3];
        if (!cut(&x0, &y0, &x1, &y1, &all, w, h))
            continue;
        if (sc.dim && sc.dim < 256) /* dimmed: the colour scaled on the way to the screen (the back buffer keeps the true one) */
            amd_comp_dim(&f, &sc.bb, (uint32_t)x0, (uint32_t)y0, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1, sc.dim, sc.bgr);
        else
            amd_comp_image(&f, &sc.bb, (uint32_t)x0, (uint32_t)y0, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1, 255, sc.bgr);
    }
    nb = amd_comp_end(&f, amd_native_vram_mc_addr(VRAM_COMP_SYNC2_OFF, 4u));   /* its own sync word */
    if (!nb) return -1;
    seq = frame_submit(ib, na + nb);
    if (!seq || !amd_gpu_fence_wait(seq, 2000000u)) return -1;
    scval[AMD_COMP_SC_FRAMES]++;
    scval[AMD_COMP_SC_DWORDS] = na + nb;
    scval[AMD_COMP_SC_CYCLES] = (uint32_t)(comp_tsc() - t0);
    {
        uint64_t c = comp_tsc() - t0;
        sums.composes++;
        sums.cycles += c;
        if (c > sums.cycles_max) sums.cycles_max = c;
        sums.draws += scval[AMD_COMP_SC_DRAWS];
        for (int r = 0; r < nr; r++)
            sums.pixels += (uint64_t)(rects[4 * r + 2] - rects[4 * r]) * (uint64_t)(rects[4 * r + 3] - rects[4 * r + 1]);
    }
    return 0;
}

void amd_comp_set_dim(int scale) { sc.dim = scale < 0 ? 0 : scale > 256 ? 256 : (uint32_t)scale; }

int amd_comp_compose(const GfxCompItem *items, int n, const int *rects, int nr)
{
    int rc = compose_to(sc.fb_mc, sc.fb_pitch_px, sc.width, sc.height, items, n, rects, nr);
    if (rc) scval[AMD_COMP_SC_FAILS]++;
    return rc;
}

int amd_comp_scene_init(uint64_t fb_phys, uint32_t pitch_bytes, uint32_t width, uint32_t height, uint64_t vram_cpu, int bgr)
{
    uint64_t off64;
    sc.up = 0;
    if (!amd_gpu_ring_running() || !pitch_bytes || (pitch_bytes & 3u) || !width || !height || fb_phys < vram_cpu) return 0;
    off64 = fb_phys - vram_cpu;
    if (off64 + (uint64_t)pitch_bytes * height > (uint64_t)VRAM_COMP_POOL_OFF) return 0;   /* the screen must end before the pool */
    sc.fb_mc = amd_native_vram_mc_addr((uint32_t)off64, pitch_bytes * height);
    if (!sc.fb_mc || !amd_gpu_frame_ib_mc() || !amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u)) return 0;
    if (!sc.bb.off && !amd_surface_alloc(&sc.bb, width, height)) return 0;
    sc.fb_pitch_px = pitch_bytes / 4u; sc.width = width; sc.height = height; sc.bgr = bgr;
    upload_shaders();
    amd_gpu_wc_flush();
    sc.up = 1;
    return 1;
}

/* What the scene test should show at (x, y) without the layer. */
static uint32_t sc_wall(uint32_t x, uint32_t y) { return ((x * 3u) & 0xFFu) | (((y * 2u) & 0xFFu) << 8) | (0x40u << 16); }

/* The scene test's shadow: box (24, 18) .. (152, 108), fading over 12 pixels, 150 at its darkest (partly under the layer, partly
 * not, and cut by both rectangles). */
#define SC_SH_X0 24
#define SC_SH_Y0 18
#define SC_SH_X1 152
#define SC_SH_Y1 108
#define SC_SH_R 12u
#define SC_SH_A 150u

/* What the scene test should show at (x, y): wallpaper, the shadow over it, the layer over both. *in: 1 where shadow or layer is. */
static uint32_t sc_want(const uint32_t *lay, uint32_t x, uint32_t y, int *in)
{
    uint32_t v = sc_wall(x, y);
    *in = 0;
    if ((int)x >= SC_SH_X0 && (int)x < SC_SH_X1 && (int)y >= SC_SH_Y0 && (int)y < SC_SH_Y1) {
        Amd3dJob s = { 0 };
        s.x0 = SC_SH_X0; s.y0 = SC_SH_Y0; s.x1 = SC_SH_X1; s.y1 = SC_SH_Y1; s.shadow_r = SC_SH_R; s.a = SC_SH_A;
        v = amd_3d_blend_px(0, v, amd_3d_shadow_alpha(&s, x, y));
        *in = 1;
    }
    if (x >= 40u && x < 136u && y >= 30u && y < 94u) {
        v = amd_comp_layer_px(lay[(y - 30u) * 96u + (x - 40u)], v);
        *in = 1;
    }
    return v & 0x00FFFFFFu;
}

/* M4c check before the compositor is used: a 176 x 124 "wallpaper", a soft shadow and the test layer at (40, 30) put together in two rectangles
 * (the top 60 rows, then the rest) into the offscreen buffer, through the back buffer, every pixel checked. 1 = right. */
int amd_comp_scene_test(void)
{
    static uint32_t lay[96u * 64u], wall[176u * 124u];
    GfxCompItem items[3];
    const int rects[8] = { 0, 0, 176, 60, 0, 60, 176, 124 };
    uint32_t x, y, k, bad = 0;
    int ws, ls;
    uint64_t rt_mc = amd_native_vram_mc_addr(VRAM_COMP_RT_OFF, COMP_RT_W * COMP_RT_H * 4u);
    for (k = 0; k < AMD_COMP_VALUES; k++) val[k] = 0;
    if (!sc.up || !rt_mc) { status = "scene test skipped: the compositor is not set up"; return 0; }
    test_layer(lay);
    for (y = 0; y < 124u; y++)
        for (x = 0; x < 176u; x++) wall[y * 176u + x] = sc_wall(x, y);
    ws = amd_comp_surface_new(176, 124);
    ls = amd_comp_surface_new(96, 64);
    if (!ws || !ls) { amd_comp_surface_free(ws); amd_comp_surface_free(ls); status = "scene test skipped: no surfaces"; return 0; }
    if (amd_comp_surface_upload(ws, wall, 176, 0, 0, 176, 124, 1) || amd_comp_surface_upload(ls, lay, 96, 0, 0, 96, 64, 0)) {
        amd_comp_surface_free(ws); amd_comp_surface_free(ls); status = "scene test FAILED: upload"; return 0;
    }
    items[0] = (GfxCompItem){ ws, 0, 0, 176, 124, 0, 0 };
    items[1] = (GfxCompItem){ (int)SC_SH_R, SC_SH_X0, SC_SH_Y0, SC_SH_X1 - SC_SH_X0, SC_SH_Y1 - SC_SH_Y0, 2, (int)SC_SH_A };
    items[2] = (GfxCompItem){ ls, 40, 30, 96, 64, 1, 0 };
    amd_gpu_fault_clear();
    if (compose_to(rt_mc, COMP_RT_W, 176, 124, items, 3, rects, 2) != 0) {
        keep_fault_registers();
        val[AMD_COMP_V_GRBM] = amd_gpu_reg_read32(0x2004u);
        amd_comp_surface_free(ws); amd_comp_surface_free(ls);
        status = "scene test FAILED: the compose did not finish";
        return 0;
    }
    for (y = 0; y < 124u; y++)
        for (x = 0; x < 176u; x++) {
            int in;
            uint32_t v = amd_gpu_vram_read32(VRAM_COMP_RT_OFF + (y * COMP_RT_W + x) * 4u) & 0x00FFFFFFu, want = sc_want(lay, x, y, &in);
            val[AMD_COMP_V_IN_TOTAL]++;
            if (in ? near_n(v, want, 3) : v == want) { val[AMD_COMP_V_IN_GOOD]++; continue; }
            if (!bad++) {
                val[AMD_COMP_V_FIRST_IN] = v | 0x01000000u; val[AMD_COMP_V_FIRST_IN + 1] = want;
                val[AMD_COMP_V_FIRST_IN + 2] = x | (y << 16);
            }
        }
    val[AMD_COMP_V_DRAWS] = scval[AMD_COMP_SC_DRAWS]; val[AMD_COMP_V_DWORDS] = scval[AMD_COMP_SC_DWORDS];
    val[AMD_COMP_V_CYCLES] = scval[AMD_COMP_SC_CYCLES];
    /* the same scene with the screen at half brightness: every byte of the copy halved (within 2) */
    uint32_t dbad = 0;
    if (!bad) {
        sc.dim = 128;
        int rc = compose_to(rt_mc, COMP_RT_W, 176, 124, items, 3, rects, 2);
        sc.dim = 0;
        if (rc != 0) dbad = 1;
        for (y = 0; y < 124u && !dbad; y++)
            for (x = 0; x < 176u; x++) {
                int in;
                uint32_t v = amd_gpu_vram_read32(VRAM_COMP_RT_OFF + (y * COMP_RT_W + x) * 4u) & 0x00FFFFFFu, want = sc_want(lay, x, y, &in), half = 0;
                for (uint32_t s = 0; s < 24; s += 8) half |= (((want >> s) & 0xFFu) * 128u / 256u) << s;
                if (!near_n(v, half, 3) && !dbad++) {
                    val[AMD_COMP_V_FIRST_IN] = v | 0x02000000u; val[AMD_COMP_V_FIRST_IN + 1] = half;   /* 0x02...: in the dimmed round */
                    val[AMD_COMP_V_FIRST_IN + 2] = x | (y << 16);
                }
            }
    }
    amd_comp_surface_free(ws);
    amd_comp_surface_free(ls);
    scval[AMD_COMP_SC_FRAMES] = scval[AMD_COMP_SC_UPLOADS] = 0;
    { AmdSceneSums z; amd_comp_scene_sums(&z); }   /* the desktop's reports start from here */
    if (bad) { status = "scene FAILED: pixels are wrong"; return 0; }
    if (dbad) { status = "scene FAILED: the copy at half brightness is wrong"; return 0; }
    status = "scene PASS: wallpaper + shadow + layer through the back buffer in 2 rectangles, and at half brightness, every pixel right";
    return 1;
}

/* ---- M4c: rows moved inside a surface (a window's list scrolled) ----
 * A copy that keeps all four bytes: the packed layer's 4th byte (how much shows through) must survive. The layer shader (texel
 * alpha) with no blending. */
static void comp_copy(AmdCompFrame *f, const AmdSurface *s, uint32_t sx, uint32_t sy, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    Amd3dJob t = { 0 };
    uint32_t u[10];
    t.tex_mc = s->mc; t.tex_w = s->w; t.tex_h = s->h; t.tex_pitch_px = s->pitch_px; t.bgr = sc.bgr;
    amd_3d_build_tex_desc(u, &t);
    u[3] = (u[3] & ~(7u << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_W)) | (E_SQ_SEL_W << F3D_SQ_IMG_RSRC_WORD3__DST_SEL_W);
    u[8] = sx - x0;
    u[9] = sy - y0;
    comp_draw(f, x0, y0, x1, y1, 0, f->ps_layer_mc, 1, 2, u, 10);
}

/* The rows of [x, x+w) x [y, y+h) of a surface move by dy inside that rectangle. Source and destination overlap, so through the
 * back buffer (free between composes: a compose fills every rectangle it copies to the screen first): frame 1 copies the rows
 * that stay into it, frame 2 copies them back, moved. One submission. The rows that came into view keep old pixels (the window
 * draws them). */
int amd_comp_surface_scroll(int id, int x, int y, int w, int h, int dy)
{
    static uint32_t ib[1024];
    AmdCompFrame f;
    const AmdSurface *s;
    uint32_t na, nb, seq;
    int n = h - (dy < 0 ? -dy : dy), sy, ty;
    uint64_t shader_mc = amd_native_vram_mc_addr(VRAM_COMP_SHADER_OFF, 0x1000u);
    if (!sc.up || !amd_gpu_ring_running() || id < 1 || id > SC_MAX || !sc_surf[id - 1].off || x < 0 || y < 0 || w <= 0 || h <= 0)
        return -1;
    s = &sc_surf[id - 1];
    if ((uint32_t)(x + w) > s->w || (uint32_t)(y + h) > s->h || (uint32_t)(x + w) > sc.bb.w || (uint32_t)(y + h) > sc.bb.h)
        return -1;
    if (dy == 0) return 0;
    if (n <= 0) return 0;               /* everything scrolled out: nothing stays */
    sy = dy > 0 ? y : y - dy;           /* the rows that stay ... */
    ty = sy + dy;                       /* ... and where they go */
    amd_comp_begin(&f, ib, 1024, sc.bb.mc, sc.bb.pitch_px, sc.bb.w, sc.bb.h, sc.bgr, shader_mc);
    comp_copy(&f, s, (uint32_t)x, (uint32_t)sy, (uint32_t)x, (uint32_t)sy, (uint32_t)(x + w), (uint32_t)(sy + n));
    na = amd_comp_end(&f, amd_native_vram_mc_addr(VRAM_COMP_SYNC_OFF, 4u));
    if (!na) return -1;
    amd_comp_begin(&f, ib + na, 1024 - na, s->mc, s->pitch_px, s->w, s->h, sc.bgr, shader_mc);
    comp_copy(&f, &sc.bb, (uint32_t)x, (uint32_t)sy, (uint32_t)x, (uint32_t)ty, (uint32_t)(x + w), (uint32_t)(ty + n));
    nb = amd_comp_end(&f, amd_native_vram_mc_addr(VRAM_COMP_SYNC2_OFF, 4u));
    if (!nb) return -1;
    seq = frame_submit(ib, na + nb);
    if (!seq || !amd_gpu_fence_wait(seq, 2000000u)) return -1;
    scval[AMD_COMP_SC_SCROLLS]++;
    return 0;
}

/* M4c check before scrolling is used: the test layer (every 4th byte different) scrolled up by 10 and then down by 7 inside a
 * rectangle of it; every word read back, all four bytes, inside and outside the rectangle. 1 = right. */
int amd_comp_scroll_test(void)
{
    static uint32_t lay[96u * 64u], want[96u * 64u];
    const int rx = 8, ry = 4, rw = 80, rh = 50;
    const int dys[2] = { -10, 7 };
    uint32_t x, y, k, bad = 0;
    int id;
    for (k = 0; k < AMD_COMP_VALUES; k++) val[k] = 0;
    if (!sc.up) { status = "scroll test skipped: the compositor is not set up"; return 0; }
    test_layer(lay);
    id = amd_comp_surface_new(96, 64);
    if (!id || amd_comp_surface_upload(id, lay, 96, 0, 0, 96, 64, 0)) {
        amd_comp_surface_free(id); status = "scroll test skipped: no surface"; return 0;
    }
    for (k = 0; k < 96u * 64u; k++) want[k] = lay[k];
    for (int r = 0; r < 2; r++) {
        int dy = dys[r], n = rh - (dy < 0 ? -dy : dy), sy = dy > 0 ? ry : ry - dy;
        static uint32_t tmp[96u * 64u];
        for (k = 0; k < 96u * 64u; k++) tmp[k] = want[k];
        for (y = 0; y < (uint32_t)n; y++)               /* what the CPU says the move gives */
            for (x = (uint32_t)rx; x < (uint32_t)(rx + rw); x++)
                want[(sy + dy + (int)y) * 96 + (int)x] = tmp[(sy + (int)y) * 96 + (int)x];
        amd_gpu_fault_clear();
        if (amd_comp_surface_scroll(id, rx, ry, rw, rh, dy) != 0) {
            keep_fault_registers();
            val[AMD_COMP_V_GRBM] = amd_gpu_reg_read32(0x2004u);
            amd_comp_surface_free(id);
            status = "scroll test FAILED: the move did not finish";
            return 0;
        }
    }
    for (y = 0; y < 64u; y++)
        for (x = 0; x < 96u; x++) {
            uint32_t v = amd_gpu_vram_read32(sc_surf[id - 1].off + (y * sc_surf[id - 1].pitch_px + x) * 4u);
            val[AMD_COMP_V_IN_TOTAL]++;
            if (v == want[y * 96u + x]) { val[AMD_COMP_V_IN_GOOD]++; continue; }
            if (!bad++) {
                val[AMD_COMP_V_FIRST_IN] = v; val[AMD_COMP_V_FIRST_IN + 1] = want[y * 96u + x];
                val[AMD_COMP_V_FIRST_IN + 2] = x | (y << 16);
            }
        }
    amd_comp_surface_free(id);
    scval[AMD_COMP_SC_SCROLLS] = 0;
    if (bad) { status = "scroll FAILED: words are wrong (all four bytes must survive)"; return 0; }
    status = "scroll PASS: rows moved up and down inside a surface, every word right (all four bytes)";
    return 1;
}
