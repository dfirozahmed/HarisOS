/* Host test of the AMD Renoir 3D pipeline test's command stream and shader code (kernel/drivers/gpu/amd_3d.c, compiled unchanged).
 * No GPU: the command stream is walked packet by packet, the shader dwords are decoded back, and the encoders are compared with
 * encodings that are published (the Mesa ACO assembler test, the ISA guide). */
#include <stdio.h>
#include "amd_3d.h"
#include "amd_3d_isa.h"
#include "amd_3d_regs.h"
#include "amd_comp.h"

/* the GPU side is not there: stand-ins so that amd_3d.c links */
uint64_t amd_native_vram_mc_addr(uint32_t offset, uint32_t bytes) { (void)offset; (void)bytes; return 0; }
int amd_gpu_ring_running(void) { return 0; }
void amd_gpu_vram_write32(uint32_t off, uint32_t v) { (void)off; (void)v; }
uint32_t amd_gpu_vram_read32(uint32_t off) { (void)off; return 0; }
void amd_gpu_ib_write32(uint32_t dw, uint32_t v) { (void)dw; (void)v; }
void amd_gpu_ib_flush(void) {}
uint64_t amd_gpu_ib_va(void) { return 0; }
void amd_gpu_wc_flush(void) {}
void amd_gpu_fault_snapshot(void) {}
uint32_t amd_native_fault_gc_status(void) { return 0; }
uint32_t amd_native_fault_gc_lo(void) { return 0; }
uint32_t amd_native_fault_gc_hi(void) { return 0; }
uint32_t amd_native_fault_mm_status(void) { return 0; }
uint32_t amd_gpu_submit_ib(uint64_t va, uint32_t dw) { (void)va; (void)dw; return 0; }
int amd_gpu_fence_wait(uint32_t seq, uint32_t loops) { (void)seq; (void)loops; return 0; }
uint32_t amd_gpu_reg_read32(uint32_t dword_addr) { (void)dword_addr; return 0; }
uint32_t amd_gpu_submit_raw(const uint32_t *dw, uint32_t n) { (void)dw; (void)n; return 0; }
void amd_gpu_fault_clear(void) {}
uint64_t amd_gpu_frame_ib_va(void) { return 0; }
uint32_t amd_gpu_frame_ib_max_dwords(void) { return 32768; }
void amd_gpu_frame_ib_write32(uint32_t dw, uint32_t v) { (void)dw; (void)v; }
uint64_t amd_gpu_frame_ib_mc(void) { return 0; }
void amd_gpu_frame_ib_ptes(uint32_t out[7]) { (void)out; }

static int ok = 1;

/* GFX9 constant bus (Mesa aco_validate.cpp, const_bus_limit = 1): walks shader code instruction by instruction; returns how many
 * instructions read VCC (VOP2 V_CNDMASK_B32 0, V_ADDC_CO_U32 0x1C, V_SUBB 0x1D, V_SUBBREV 0x1E) together with a literal or an SGPR,
 * or ~0u if an encoding is not known to the walker. */
static uint32_t const_bus_bad(const uint32_t *code, uint32_t cn)
{
    uint32_t k = 0, bad = 0;
    while (k < cn) {
        uint32_t w = code[k], top6 = w >> 26, src0 = w & 0x1FFu;
        if (!(w >> 31)) {                                                                   /* VOP2 */
            uint32_t op = (w >> 25) & 0x3Fu;
            if ((op == 0 || op == 0x1C || op == 0x1D || op == 0x1E) && (src0 == 255 || src0 < 128)) bad++;
            k += src0 == 255 ? 2 : 1;
        } else if ((w & 0xFE000000u) == 0x7E000000u || (w & 0xFE000000u) == 0x7C000000u) { /* VOP1, VOPC */
            k += src0 == 255 ? 2 : 1;
        } else if (top6 == 0x31 || top6 == 0x34 || top6 == 0x3C || top6 == 0x37) {          /* EXP, VOP3, MIMG, GLOBAL */
            k += 2;
        } else if ((w >> 23) == 0x17Fu) {                                                   /* SOPP */
            k += 1;
        } else {
            return ~0u;
        }
    }
    return bad;
}

static void check(int cond, const char *name)
{
    printf("%s: %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond)
        ok = 0;
}

/* the body of the first packet of this opcode that sets `reg`: returns the values, *count_out = how many */
static const uint32_t *find_set(const uint32_t *ib, uint32_t n, uint32_t opcode, uint32_t reg, uint32_t *count_out)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t h = ib[i], body = ((h >> 16) & 0x3FFF) + 1;
        if ((h >> 30) == 3 && ((h >> 8) & 0xFF) == opcode && ib[i + 1] == reg) {
            *count_out = body - 1;
            return &ib[i + 2];
        }
        i += 1 + body;
    }
    return 0;
}

/* the half float of v / 255, converted back to a number 0 .. 255 (rounded to nearest) */
static uint32_t half_to_u8(uint32_t h)
{
    uint32_t e = (h >> 10) & 31, m = h & 0x3FF;
    unsigned long long mant, num;
    if (h == 0)
        return 0;
    mant = 1024u + m;                       /* value = mant * 2^(e - 15 - 10) */
    num = mant * 255ULL;                    /* 255 * value = num * 2^(e - 25) */
    if (e >= 25)
        return 255;
    return (uint32_t)((num + (1ULL << (24 - e))) >> (25 - e)); /* rounded */
}

int main(void)
{
    static uint32_t ib[AMD_3D_IB_MAX], vs[AMD_3D_VS_MAX], ps[AMD_3D_PS_MAX];
    Amd3dJob job = { 0x4100000000ULL + 0x0E100000ULL, 256, 256, 64, 1, 32, 16, 224, 48, 255, 128, 0, 255,
                     0x4100000000ULL + 0x0E000000ULL, 0x4100000000ULL + 0x0E000100ULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t i, n, cnt;
    const uint32_t *p;

    /* encoders against published encodings */
    check(A3D_S_ENDPGM == 0xBF810000u, "s_endpgm = 0xBF810000");
    check(a3d_vop1(A3D_OP1_MOV_B32, 1, A3D_SRC_VGPR(0)) == 0x7E020300u, "v_mov_b32 v1, v0 = 0x7E020300");
    /* SDWA form: SRC0 = 249 (SDWA), the VSRC1 field holds 0x80 (the constant 0 of the SDWA word), OP = 201 (V_CMP_LT_U32) */
    check(a3d_vopc(201, 0x80, 249) == 0x7D9300F9u, "VOPC layout: v_cmp_lt_u32_sdwa vcc, 0, 0 = 0x7D9300F9 (Mesa test_assembler.cpp)");
    check(a3d_exp0(0xF, A3D_EXP_TARGET_POS0, 0, 1, 0) == 0xC40008CFu, "exp pos0 done, EN=0xF");
    check(a3d_exp0(0xF, A3D_EXP_TARGET_MRT0, 1, 1, 1) == 0xC4001C0Fu, "exp mrt0 compr done vm, EN=0xF");
    check(a3d_f32_bits(0) == 0 && a3d_f32_bits(1) == 0x3F800000u && a3d_f32_bits(2) == 0x40000000u, "float bits of 0, 1, 2");
    check(a3d_f32_bits(255) == 0x437F0000u && a3d_f32_bits(1080) == 0x44870000u, "float bits of 255, 1080");
    check(a3d_f16_unorm8(0) == 0 && a3d_f16_unorm8(255) == 0x3C00 && a3d_f16_unorm8(128) == 0x3804 && a3d_f16_unorm8(64) == 0x3404,
          "half float bits of 0, 255, 128, 64 (/255)");
    {
        int bad = 0;
        for (i = 0; i < 256; i++)
            if (half_to_u8(a3d_f16_unorm8(i)) != i)
                bad++;
        check(bad == 0, "the half float of v/255 converts back to v for all 256 values");
    }

    /* shaders */
    n = amd_3d_build_vs(vs, &job);
    check(n > 0 && n <= AMD_3D_VS_MAX && vs[n - 1] == A3D_S_ENDPGM, "vertex shader fits and ends with s_endpgm");
    {
        int pos_done = 0, params = 0;
        for (i = 0; i < n; i++)
            if ((vs[i] >> 26) == 0x31) { /* EXP */
                uint32_t target = (vs[i] >> 4) & 0x3F, done = (vs[i] >> 11) & 1;
                if (target == 12 && done) pos_done++;
                if (target >= 32) params++;
                i++; /* its second dword */
            }
        check(pos_done == 1 && params == 1, "vertex shader: one position export with DONE, one parameter export");
    }
    {
        int found[4] = {0, 0, 0, 0};
        for (i = 0; i < n; i++) {
            if (vs[i] == a3d_f32_bits(job.x0)) found[0]++;
            if (vs[i] == a3d_f32_bits(job.x1)) found[1]++;
            if (vs[i] == a3d_f32_bits(job.y0)) found[2]++;
            if (vs[i] == a3d_f32_bits(job.y1)) found[3]++;
        }
        check(found[0] && found[1] && found[2] && found[3], "vertex shader holds the four rectangle coordinates as literals");
    }
    n = amd_3d_build_ps(ps, &job);
    check(n > 0 && n <= AMD_3D_PS_MAX && ps[n - 1] == A3D_S_ENDPGM, "pixel shader fits and ends with s_endpgm");
    check(ps[1] == (a3d_f16_unorm8(255) | (a3d_f16_unorm8(128) << 16)) && ps[3] == (a3d_f16_unorm8(0) | (a3d_f16_unorm8(255) << 16)),
          "pixel shader: (R,G) and (B,A) as packed half floats");
    check(ps[4] == 0xC4001C0Fu && ps[5] == 0x00000100u, "pixel shader: exp mrt0 v0, v1 compr done vm");

    /* command stream */
    n = amd_3d_build_ib(ib, AMD_3D_IB_MAX, &job);
    printf("command stream: %u dwords\n", n);
    check(n > 0 && n < 300, "command stream fits (below 300 dwords of 1024)");
    check(amd_3d_build_ib(ib, 20, &job) == 0, "a too small buffer is refused, not overrun");
    {
        uint32_t k = 0;
        int packets = 0, bad = 0, draws = 0, sets = 0;
        while (k < n) {
            uint32_t h = ib[k], body = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
            if ((h >> 30) != 3 || k + 1 + body > n) { bad++; break; }
            packets++;
            if (op == 0x2D) draws++;
            if (op == 0x69) { /* SET_CONTEXT_REG: offset + values stay inside the context register range (0x400 dwords) */
                sets++;
                if (ib[k + 1] + (body - 1) > 0x400) bad++;
            }
            if (op == 0x76 && ib[k + 1] + (body - 1) > 0x400) bad++; /* SET_SH_REG: 0x2C00 .. 0x3000 */
            if (op == 0x79 && ib[k + 1] + (body - 1) > 0x2000) bad++; /* SET_UCONFIG_REG: 0xC000 .. 0xE000 */
            k += 1 + body;
        }
        check(k == n && !bad, "every packet header is type 3, fits, and every register offset is in range");
        check(draws == 1 && ((ib[n - 3] >> 8) & 0xFF) == 0x2D, "ends with one DRAW_INDEX_AUTO");
        printf("%d packets, %d SET_CONTEXT_REG\n", packets, sets);
    }

    /* the default context state: the whole Linux clear-state table, as SET_CONTEXT_REG packets */
    {
        static uint32_t dib[AMD_3D_DEFAULT_IB_MAX];
        uint32_t k = 0, regs = 0, bad = 0, nd = amd_3d_build_default_ib(dib, AMD_3D_DEFAULT_IB_MAX);
        int clear_state_in_draw = 0;
        printf("default state stream: %u dwords\n", nd);
        check(nd > 0 && nd < 1024, "default state stream fits in the 1024 dword IB page");
        check(amd_3d_build_default_ib(dib, 100) == 0, "a too small buffer for the default state is refused");
        while (k < nd) {
            uint32_t h = dib[k], body = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
            if ((h >> 30) != 3 || k + 1 + body > nd) { bad++; break; }
            if (op == 0x69) {
                regs += body - 1;
                if (dib[k + 1] + (body - 1) > 0x400) bad++;
            }
            k += 1 + body;
        }
        check(k == nd && !bad && regs == 879, "default state: well-formed packets, 879 context registers (212+282+4+157+2+1+66+155)");
        p = find_set(dib, nd, 0x69, 0x2FA, &cnt); /* PA_CL_GB_VERT_CLIP_ADJ: inside the table extent that starts at 0x2F5 */
        check(find_set(dib, nd, 0x69, 0x2F5, &cnt) && cnt == 155, "the extent that holds the guard band registers (0xA2F5 ..) is there");
        {
            const uint32_t *e = find_set(dib, nd, 0x69, 0x2F5, &cnt);
            check(e && e[4] == 5 && e[5] == 0x3F800000u && e[6] == 0x3F800000u && e[7] == 0x3F800000u && e[8] == 0x3F800000u,
                  "PA_SU_VTX_CNTL = 5 and PA_CL_GB_{VERT,HORZ}_{CLIP,DISC}_ADJ = 1.0 (a guard band of 0 clips every triangle)");
        }
        for (k = 0; k + 1 < n; ) {
            uint32_t h = ib[k], body = ((h >> 16) & 0x3FFF) + 1;
            if (((h >> 8) & 0xFF) == 0x12) clear_state_in_draw = 1;
            k += 1 + body;
        }
        check(!clear_state_in_draw, "the draw stream has no CLEAR_STATE (it would undo the default state)");
    }

    p = find_set(ib, n, 0x69, R3D_CB_COLOR0_BASE, &cnt);
    check(p && cnt == 15, "colour buffer: 15 registers from CB_COLOR0_BASE (BASE, BASE_EXT, ATTRIB2, VIEW, INFO, ATTRIB, DCC_CONTROL, ...)");
    if (p) {
        check(p[0] == (uint32_t)(job.rt_mc >> 8) && p[1] == (uint32_t)(job.rt_mc >> 40), "CB_COLOR0_BASE / BASE_EXT: the buffer address");
        check(((p[4] >> F3D_CB_COLOR0_INFO__FORMAT) & 0x3F) == 10 && ((p[4] >> F3D_CB_COLOR0_INFO__COMP_SWAP) & 3) == 1,
              "CB_COLOR0_INFO: format 8_8_8_8, swap ALT (B,G,R order)");
        check(((p[5] >> F3D_CB_COLOR0_ATTRIB__COLOR_SW_MODE) & 0x1F) == 0, "CB_COLOR0_ATTRIB: linear surface");
        check(((p[2] >> F3D_CB_COLOR0_ATTRIB2__MIP0_WIDTH) & 0x3FFF) == 255 && ((p[2] >> F3D_CB_COLOR0_ATTRIB2__MIP0_HEIGHT) & 0x3FFF) == 63,
              "CB_COLOR0_ATTRIB2: width - 1, height - 1");
    }
    p = find_set(ib, n, 0x69, R3D_CB_MRT0_EPITCH, &cnt);
    check(p && cnt == 1 && p[0] == 255, "CB_MRT0_EPITCH = pitch in pixels - 1");
    p = find_set(ib, n, 0x76, R3D_SPI_SHADER_PGM_LO_VS, &cnt);
    check(p && cnt == 1 && p[0] == (uint32_t)(job.vs_mc >> 8), "SPI_SHADER_PGM_LO_VS = vertex shader address >> 8");
    p = find_set(ib, n, 0x76, R3D_SPI_SHADER_PGM_LO_PS, &cnt);
    check(p && cnt == 1 && p[0] == (uint32_t)(job.ps_mc >> 8), "SPI_SHADER_PGM_LO_PS = pixel shader address >> 8");
    p = find_set(ib, n, 0x69, R3D_SPI_PS_INPUT_ENA, &cnt);
    check(p && cnt == 2 && (p[0] & (1u << F3D_SPI_PS_INPUT_ENA__PERSP_CENTER_ENA)) && p[0] == p[1],
          "SPI_PS_INPUT_ENA / ADDR: at least one perspective input (else the GPU hangs)");
    p = find_set(ib, n, 0x69, R3D_PA_CL_VTE_CNTL, &cnt);
    check(p && p[0] == ((1u << F3D_PA_CL_VTE_CNTL__VTX_XY_FMT) | (1u << F3D_PA_CL_VTE_CNTL__VTX_Z_FMT)), "PA_CL_VTE_CNTL: window space position");
    p = find_set(ib, n, 0x79, R3D_VGT_PRIMITIVE_TYPE, &cnt);
    check(p && p[0] == 6, "VGT_PRIMITIVE_TYPE = triangle strip");
    p = find_set(ib, n, 0x69, R3D_PA_SC_VPORT_SCISSOR_0_TL, &cnt);
    check(p && cnt == 2 && ((p[0] >> F3D_PA_SC_VPORT_SCISSOR_0_TL__TL_X) & 0x7FFF) == 32 && ((p[1] >> F3D_PA_SC_VPORT_SCISSOR_0_BR__BR_X) & 0x7FFF) == 224,
          "viewport scissor = the rectangle");


    /* clipping / guard band for window space positions (Mesa ac_compute_guardband, 16.8 fixed point, the viewport = the buffer) */
    check(a3d_f32_ratio(65536, 256) == 0x43800000u && a3d_f32_ratio(65536, 64) == 0x44800000u && a3d_f32_ratio(3, 2) == 0x3FC00000u,
          "float bits of 65536/256 = 256.0, 65536/64 = 1024.0, 3/2 = 1.5");
    p = find_set(ib, n, 0x69, R3D_PA_SU_VTX_CNTL, &cnt);
    check(p && cnt == 5 && p[0] == 0x2D && p[1] == 0x44800000u && p[2] == 0x3F800000u && p[3] == 0x43800000u && p[4] == 0x3F800000u,
          "PA_SU_VTX_CNTL = 0x2D (pixel centre, round to even, 16.8) and the guard band of a 256 x 64 buffer: clip 1024 / 256, discard 1.0");
    p = find_set(ib, n, 0x69, R3D_PA_SU_HARDWARE_SCREEN_OFFSET, &cnt);
    check(p && p[0] == (8u | (2u << 16)), "PA_SU_HARDWARE_SCREEN_OFFSET = (128 >> 4, 32 >> 4)");
    p = find_set(ib, n, 0x69, R3D_PA_CL_CLIP_CNTL, &cnt);
    check(p && (p[0] & (1u << 16)), "PA_CL_CLIP_CNTL: CLIP_DISABLE (window space positions)");
    p = find_set(ib, n, 0x69, R3D_PA_SC_MODE_CNTL_0, &cnt);
    check(p && (p[0] & 2u), "PA_SC_MODE_CNTL_0: VPORT_SCISSOR_ENABLE");


    /* M2: the textured rectangle's pixel shader and image resource */
    {
        Amd3dJob tj = job;
        uint32_t td[8], tps[AMD_3D_PS_MAX], tn, k;
        tj.textured = 1; tj.tex_mc = 0x4100000000ULL + 0x0B200000ULL; tj.tex_off = 0x0B200000u;
        tj.tex_w = 192; tj.tex_h = 32; tj.tex_pitch_px = 256;
        tn = amd_3d_build_ps(tps, &tj);
        check(tn > 0 && tn <= AMD_3D_PS_MAX && tps[tn - 1] == A3D_S_ENDPGM, "textured pixel shader fits and ends with s_endpgm");
        check(a3d_mimg_0(0, 0xF) == 0xF0000F00u && a3d_mimg_1(4, 6, 0, 0) == 0x00000604u, "IMAGE_LOAD dmask 0xF v[6:9] <- v[4:5], s[0:7]: 0xF0000F00 0x00000604");
        check(A3D_S_WAITCNT_VM0 == 0xBF8C0F70u, "s_waitcnt vmcnt(0) = 0xBF8C0F70");
        check(a3d_vop3a_0(A3D_OP3_CVT_PKRTZ_F16_F32, 10) == 0xD296000Au && a3d_vop3a_1(256 + 6, 256 + 7, 0) == 0x00020F06u,
              "v_cvt_pkrtz_f16_f32 v10, v6, v7 = 0xD296000A 0x00020F06 (opcode 0x296)");
        {
            int has_load = 0, has_wait = 0, lits = 0;
            for (k = 0; k < tn; k++) {
                if (tps[k] == 0xF0000F00u) has_load++;
                if (tps[k] == A3D_S_WAITCNT_VM0) has_wait++;
                if (tps[k] == tj.x0 || tps[k] == tj.y0) lits++;
            }
            check(has_load == 1 && has_wait == 1 && lits >= 2, "textured pixel shader: one image_load, one s_waitcnt, the rectangle corner as literals");
        }
        amd_3d_build_tex_desc(td, &tj);
        check(td[0] == (uint32_t)(tj.tex_mc >> 8), "T# word 0: the image address >> 8");
        check(((td[1] >> F3D_SQ_IMG_RSRC_WORD1__DATA_FORMAT) & 0x3F) == 10 && ((td[1] >> F3D_SQ_IMG_RSRC_WORD1__NUM_FORMAT) & 0xF) == 0,
              "T# word 1: data format 8_8_8_8, number format UNORM");
        check(((td[2] >> F3D_SQ_IMG_RSRC_WORD2__WIDTH) & 0x3FFF) == 191 && ((td[2] >> F3D_SQ_IMG_RSRC_WORD2__HEIGHT) & 0x3FFF) == 31,
              "T# word 2: width - 1, height - 1");
        check(((td[3] >> F3D_SQ_IMG_RSRC_WORD3__TYPE) & 0xF) == 9 && ((td[3] >> F3D_SQ_IMG_RSRC_WORD3__SW_MODE) & 0x1F) == 0 &&
              (td[3] & 7) == 6 && ((td[3] >> 6) & 7) == 4,
              "T# word 3: 2D image, linear, destination select R <- component Z, B <- component X (B,G,R,X memory order)");
        check(((td[4] >> F3D_SQ_IMG_RSRC_WORD4__PITCH) & 0xFFFF) == 255, "T# word 4: pitch - 1 (in texels)");
        check(amd_3d_texel(&tj, 0, 0) == 0x00000000u && ((amd_3d_texel(&tj, 191, 31) >> 16) & 0xFF) == 255 && ((amd_3d_texel(&tj, 191, 31) >> 8) & 0xFF) == 255,
              "the test image: texel (0,0) black, texel (191,31) has full red and green");
    }


    /* with a sync address the stream ends, after the draw, with the colour buffer sync (Mesa SYNC_AND_INV_CB): EVENT_WRITE CB_META (2),
     * RELEASE_MEM FLUSH_AND_INV_CB_DATA_TS writing 0x3D3D0003 there (8), WAIT_REG_MEM for it (7) */
    {
        static uint32_t mib[AMD_3D_IB_MAX];
        Amd3dJob mj = job;
        uint32_t k = 0, nm, tail = 0, tail_ops[3] = { 0, 0, 0 }, draw_at = 0;
        mj.sync_mc = 0x4100000000ULL + 0x0B300000ULL;
        nm = amd_3d_build_ib(mib, AMD_3D_IB_MAX, &mj);
        while (k < nm) {
            uint32_t h = mib[k], body = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
            if (op == 0x2D) draw_at = k;
            if ((op == 0x46 || op == 0x49 || op == 0x3C) && tail < 3) tail_ops[tail++] = op;
            k += 1 + body;
        }
        check(nm == n + 17 && draw_at == nm - 17 - 3 && tail == 3 && tail_ops[0] == 0x46 && tail_ops[1] == 0x49 && tail_ops[2] == 0x3C &&
              mib[nm - 16] == 46u && (mib[nm - 14] & 0x3Fu) == 45u && mib[nm - 10] == 0x3D3D0003u &&
              mib[nm - 5] == (uint32_t)mj.sync_mc && mib[nm - 3] == 0x3D3D0003u,
              "after the draw the colour buffer is flushed (CB_META event, RELEASE_MEM CB_DATA_TS) and waited for at sync_mc");
    }

    /* the register numbers themselves, against values known from Mesa radeonsi (R_028C60 / 4 - 0xA000, R_00B020 / 4 - 0xB000 / 4, ...) */
    check(R3D_CB_COLOR0_BASE == 0x318 && R3D_SPI_SHADER_PGM_LO_PS == 0x008 && R3D_SPI_SHADER_PGM_LO_VS == 0x048 &&
          R3D_VGT_PRIMITIVE_TYPE == 0x242 && R3D_IA_MULTI_VGT_PARAM == 0x258,
          "register offsets match Mesa R_028C60 / R_00B020 / R_00B120 / R_030908 / R_030960");

    /* GFX9 constant bus (Mesa aco_validate.cpp, const_bus_limit = 1): walk every shader instruction by instruction; an instruction
     * that reads VCC implicitly (VOP2 V_CNDMASK_B32 0, V_ADDC_CO_U32 0x1C, V_SUBB_CO_U32 0x1D, V_SUBBREV_CO_U32 0x1E) must not also
     * read a literal (src0 255) or an SGPR (src0 0 .. 127). The old vertex shader broke this with "v_cndmask_b32 v3, x0, v2, vcc". */
    {
        static uint32_t code[AMD_3D_VS_MAX + AMD_3D_PS_MAX];
        Amd3dJob tj = job;
        int which, bad = 0, walked = 1;
        for (which = 0; which < 5; which++) {
            uint32_t cn, k = 0;
            tj.textured = which == 2;
            tj.shadow_r = which == 3 ? 8u : 0u;
            tj.blur_r = which == 4 ? 1u : 0u;
            if (which == 4) { tj.textured = 1; tj.tex_w = 194; tj.tex_h = 34; tj.tex_pitch_px = 256; }
            cn = which == 0 ? amd_3d_build_vs(code, &tj) : amd_3d_build_ps(code, &tj);
            while (k < cn) {
                uint32_t w = code[k], top6 = w >> 26, src0 = w & 0x1FFu;
                if (!(w >> 31)) {                                       /* VOP2 */
                    uint32_t op = (w >> 25) & 0x3Fu;
                    if ((op == 0 || op == 0x1C || op == 0x1D || op == 0x1E) && (src0 == 255 || src0 < 128)) bad++;
                    k += src0 == 255 ? 2 : 1;
                } else if ((w & 0xFE000000u) == 0x7E000000u || (w & 0xFE000000u) == 0x7C000000u) {  /* VOP1, VOPC */
                    k += src0 == 255 ? 2 : 1;
                } else if (top6 == 0x31 || top6 == 0x34 || top6 == 0x3C || top6 == 0x37) {          /* EXP, VOP3, MIMG, GLOBAL */
                    k += 2;
                } else if ((w >> 23) == 0x17Fu) {                                                   /* SOPP */
                    k += 1;
                } else {
                    walked = 0; break;                                  /* an encoding this walker does not know */
                }
            }
        }
        check(walked && bad == 0, "no shader instruction reads VCC together with a literal or an SGPR (GFX9 constant bus limit 1)");
    }

    /* the diagnostic global stores of the vertex shader, against encodings worked out from Mesa aco_assembler.cpp (GFX9):
     * global_store_dwordx4 v[8:9], v[3:6], off  = 0xDC7C8000 0x007F0308; global_store_dword v[8:9], v0, off offset:16 = 0xDC708010 0x007F0008 */
    check(a3d_global_0(A3D_GLOBAL_STORE_DWORDX4, 0) == 0xDC7C8000u && a3d_global_1(8, 3) == 0x007F0308u,
          "global_store_dwordx4 v[8:9], v[3:6], off = 0xDC7C8000 0x007F0308");
    check(a3d_global_0(A3D_GLOBAL_STORE_DWORD, 16) == 0xDC708010u && a3d_global_1(8, 0) == 0x007F0008u,
          "global_store_dword v[8:9], v0, off offset:16 = 0xDC708010 0x007F0008");
    check(a3d_vop2(A3D_OP2_LSHLREV_B32, 7, 7, A3D_SRC_INT(5)) == 0x240E0E85u, "v_lshlrev_b32 v7, 5, v7 = 0x240E0E85");
    check(a3d_vop2(A3D_OP2_ADD_CO_U32, 8, 7, A3D_SRC_LITERAL) == 0x32100EFFu, "v_add_co_u32 v8, vcc, literal, v7 = 0x32100EFF");
    check(a3d_vop2(A3D_OP2_ADDC_CO_U32, 9, 10, A3D_SRC_LITERAL) == 0x381214FFu, "v_addc_co_u32 v9, vcc, literal, v10, vcc = 0x381214FF");

    /* M3 alpha blending: the CB / SX blend registers as radeonsi sets them for SRC_ALPHA / INV_SRC_ALPHA / ADD (values: Mesa gfx9.json) */
    {
        static uint32_t bib[AMD_3D_IB_MAX];
        Amd3dJob bj = job;
        uint32_t bn, cnt = 0;
        const uint32_t *bc, *bo, *bx;
        bj.blend = 1; bj.a = 128;
        bn = amd_3d_build_ib(bib, AMD_3D_IB_MAX, &bj);
        bc = find_set(bib, bn, 0x69, R3D_CB_BLEND0_CONTROL, &cnt);
        check(bc && bc[0] == ((1u << 30) | (0u << 5) | (4u << 0) | (5u << 8)),
              "blend: CB_BLEND0_CONTROL = ENABLE, COMB_DST_PLUS_SRC, SRCBLEND SRC_ALPHA (4), DESTBLEND ONE_MINUS_SRC_ALPHA (5)");
        bo = find_set(bib, bn, 0x69, R3D_SX_MRT0_BLEND_OPT, &cnt);
        check(bo && bo[0] == (4u | (5u << 4) | (1u << 8) | (4u << 16) | (5u << 20) | (1u << 24)),
              "blend: SX_MRT0_BLEND_OPT = PRESERVE_A1_IGNORE_A0 / PRESERVE_A0_IGNORE_A1 / OPT_COMB_ADD for colour and alpha");
        bx = find_set(bib, bn, 0x69, R3D_SX_PS_DOWNCONVERT, &cnt);
        check(bx && cnt >= 3 && bx[2] == 2u, "SX_BLEND_OPT_CONTROL = MRT0_ALPHA_OPT_DISABLE (the target keeps no alpha)");
        bj.blend = 0;
        bn = amd_3d_build_ib(bib, AMD_3D_IB_MAX, &bj);
        bc = find_set(bib, bn, 0x69, R3D_CB_BLEND0_CONTROL, &cnt);
        check(bc && bc[0] == 0, "no blend: CB_BLEND0_CONTROL = 0");
        check(amd_3d_blend_px(0x00FF8000u, 0x00204060u, 128) == 0x00906030u && amd_3d_blend_px(0x00FFFFFFu, 0x00000000u, 255) == 0x00FFFFFFu &&
              amd_3d_blend_px(0x00FFFFFFu, 0x00123456u, 0) == 0x00123456u, "blend_px: 0xFF8000 over 0x204060 at 128 = 0x906030; a 255 / 0");
    }

    /* M3 soft shadow: the integer alpha the check uses, against the shader's formula done in float on the host, for every pixel */
    {
        Amd3dJob sj = job;
        uint32_t x, y, worst = 0;
        static uint32_t sps[AMD_3D_PS_MAX];
        sj.x0 = 24; sj.y0 = 8; sj.x1 = 232; sj.y1 = 56; sj.a = 160; sj.blend = 1; sj.shadow_r = 8;
        for (y = sj.y0; y < sj.y1; y++)
            for (x = sj.x0; x < sj.x1; x++) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f, fx, fy, al;
                uint32_t want, got = amd_3d_shadow_alpha(&sj, x, y);
                fx = px - (float)sj.x0 < (float)sj.x1 - px ? px - (float)sj.x0 : (float)sj.x1 - px;
                fy = py - (float)sj.y0 < (float)sj.y1 - py ? py - (float)sj.y0 : (float)sj.y1 - py;
                fx /= 8.0f; fy /= 8.0f;
                if (fx > 1.0f) fx = 1.0f;
                if (fy > 1.0f) fy = 1.0f;
                al = 160.0f * fx * fy;
                want = (uint32_t)(al + 0.5f);
                if ((got > want ? got - want : want - got) > worst) worst = got > want ? got - want : want - got;
            }
        check(worst <= 1, "shadow: integer alpha within 1 of the shader's float formula for every pixel");
        check(amd_3d_shadow_alpha(&sj, 128, 32) == 160 && amd_3d_shadow_alpha(&sj, 24, 32) == 10 && amd_3d_shadow_alpha(&sj, 24, 8) == 1,
              "shadow alpha: 160 in the middle, 10 at the left edge, 1 in the corner");
        check(amd_3d_build_ps(sps, &sj) <= AMD_3D_PS_MAX, "shadow pixel shader fits");
    }

    /* M3 blur: the expected value inside one checker cell, and across a cell edge (3 texels of one colour, 6 of the other per 3 x 3) */
    {
        Amd3dJob rj = job;
        static uint32_t rps[AMD_3D_PS_MAX];
        uint32_t rn;
        rj.x0 = 32; rj.y0 = 16; rj.x1 = 224; rj.y1 = 48; rj.bgr = 1;
        rj.textured = 1; rj.blur_r = 1; rj.tex_w = 194; rj.tex_h = 34; rj.tex_pitch_px = 256;
        check(amd_3d_blur_px(&rj, 33, 17) == 0x002060E0u, "blur: inside one checker cell = the cell colour 0x2060E0");
        check(amd_3d_blur_px(&rj, 35, 17) == 0x00A07560u, "blur: across a cell edge = (3 x 0x2060E0 + 6 x 0xE08020) / 9 = 0xA07560");
        rn = amd_3d_build_ps(rps, &rj);
        check(rn > 0 && rn <= AMD_3D_PS_MAX && rps[rn - 1] == A3D_S_ENDPGM, "blur pixel shader fits and ends with s_endpgm");
    }

    /* M4 compositor: the four fixed shaders keep the constant bus rule and fit; a frame of fill + shadow + image is walked */
    {
        static uint32_t code[AMD_COMP_SHADER_MAX], fib[4096];
        uint32_t cn, bad = 0, fits = 1, k = 0, nf, draws = 0, vs_ud = 0, vs_rect_ok = 1, ps_ud_n[8], nps = 0, blends[8], nbl = 0;
        uint32_t img_s8 = 0, img_s10 = 0, sync_tail = 0;
        AmdCompFrame f;
        AmdSurface s = { 0x4100000000ULL + 0x10100000ULL, 0x10100000u, 160, 100, 256 };
        cn = amd_comp_build_vs(code);        bad += const_bus_bad(code, cn); fits &= cn <= AMD_COMP_SHADER_MAX && cn * 4u <= 0x200u;
        cn = amd_comp_build_ps_fill(code);   bad += const_bus_bad(code, cn); fits &= cn <= AMD_COMP_SHADER_MAX && cn * 4u <= 0x200u;
        cn = amd_comp_build_ps_image(code);  bad += const_bus_bad(code, cn); fits &= cn <= AMD_COMP_SHADER_MAX && cn * 4u <= 0x200u;
        cn = amd_comp_build_ps_shadow(code); bad += const_bus_bad(code, cn); fits &= cn <= AMD_COMP_SHADER_MAX && cn * 4u <= 0x200u;
        check(bad == 0 && fits, "compositor shaders: no VCC + literal / SGPR, each fits in its 512-byte slot");
        check(a3d_vop2(A3D_OP2_ADD_U32, 4, 4, 8) == 0x68080808u, "v_add_u32 v4, s8, v4 = 0x68080808 (GFX9 VOP2 0x34)");

        amd_comp_begin(&f, fib, 4096, 0x4100000000ULL + 0x10000000ULL, 512, 512, 256, 1, 0x4100000000ULL + 0x0B004000ULL);
        amd_comp_fill(&f, 16, 16, 496, 240, 0x30, 0x50, 0x70);
        amd_comp_shadow(&f, 34, 26, 196, 138, 12, 150);
        amd_comp_image(&f, &s, 0, 0, 40, 40, 200, 140, 160, 1);
        nf = amd_comp_end(&f, 0x4100000000ULL + 0x0B300040ULL);
        while (k < nf) {
            uint32_t h = fib[k], body = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
            if (op == 0x2D) draws++;
            if (op == 0x76 && fib[k + 1] == R3D_SPI_SHADER_USER_DATA_VS_0) {
                vs_ud++;
                if (body != 5) vs_rect_ok = 0;
            }
            if (op == 0x76 && fib[k + 1] == R3D_SPI_SHADER_USER_DATA_PS_0 && nps < 8) {
                ps_ud_n[nps++] = body - 1;
                if (body - 1 == 11) { img_s8 = fib[k + 2 + 8]; img_s10 = fib[k + 2 + 10]; }
            }
            if (op == 0x69 && fib[k + 1] == R3D_CB_BLEND0_CONTROL && nbl < 8) blends[nbl++] = fib[k + 2];
            if (op == 0x3C) sync_tail = 1;
            k += 1 + body;
        }
        check(nf > 0 && k == nf && f.draws == 3 && draws == 3, "compositor frame: whole packets, 3 draws (fill, shadow, image) in one stream");
        check(vs_ud == 3 && vs_rect_ok && nps == 3 && ps_ud_n[0] == 2 && ps_ud_n[1] == 6 && ps_ud_n[2] == 11,
              "compositor frame: the rectangle in 4 VS user SGPRs per draw; 2 / 6 / 11 PS user SGPRs for fill / shadow / image");
        check(img_s8 == (uint32_t)(0u - 40u) && img_s10 == a3d_f32_ratio(160, 255),
              "compositor image: s8 = surface x - screen x (-40), s10 = opacity 160 / 255");
        check(nbl == 3 && blends[0] == 0 && blends[1] != 0 && blends[2] != 0 && sync_tail,
              "compositor frame: no blending for the fill, blending for the shadow and the transparent image; the end sync is there");
    }

    /* PA_SC_RASTER_CONFIG / _1: outside the clear-state table, so the draw stream itself writes both (0, as Mesa's GFX9 default) */
    {
        static uint32_t rib[AMD_3D_IB_MAX];
        uint32_t rn = amd_3d_build_ib(rib, AMD_3D_IB_MAX, &job), cnt = 0;
        const uint32_t *rc = find_set(rib, rn, 0x69, R3D_PA_SC_RASTER_CONFIG, &cnt);
        check(R3D_PA_SC_RASTER_CONFIG == 0x0D4 && R3D_PA_SC_RASTER_CONFIG_1 == 0x0D5 && rc && cnt >= 2 && rc[0] == 0 && rc[1] == 0,
              "PA_SC_RASTER_CONFIG and _1 (0x0D4, 0x0D5, not in the clear-state table) are written as 0");
    }

    /* the ring start-up packets of Linux gfx_v9_0_cp_gfx_start(): order, and small enough for the 1024-dword ring with the fence */
    {
        static uint32_t rs[1100];
        uint32_t nr = amd_3d_build_ring_start(rs, 1100), k = 0, ops[8], no = 0, last_op = 0, nctx = 0;
        while (k < nr) {
            uint32_t h = rs[k], body = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
            if (op == 0x69) nctx++;
            else if (no < 8) ops[no++] = op;
            last_op = op;
            k += 1 + body;
        }
        printf("ring start dwords: %u\n", nr);
        check(nr > 0 && k == nr && nr + 8 <= 1023, "ring start: whole packets, fits in the 1024-dword ring with the 8-dword fence");
        check(no == 6 && ops[0] == 0x4A && ops[1] == 0x28 && ops[2] == 0x4A && ops[3] == 0x12 && ops[4] == 0x11 && ops[5] == 0x79 &&
              last_op == 0x79 && nctx > 0,
              "ring start: PREAMBLE_CNTL, CONTEXT_CONTROL, SET_CONTEXT_REG..., PREAMBLE_CNTL, CLEAR_STATE, SET_BASE, SET_UCONFIG_REG");
        check(rs[1] == (2u << 28) && rs[nr - 2] == 0x20000243u && rs[nr - 1] == 0,
              "ring start: BEGIN_CLEAR_STATE first, VGT_INDEX_TYPE = 0 with INDEX_TYPE last");
    }

    printf("\nAMD 3D TEST %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
