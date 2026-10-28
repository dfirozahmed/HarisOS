/* Host test of the AMD Renoir 3D pipeline test's command stream and shader code (kernel/drivers/gpu/amd_3d.c, compiled unchanged).
 * No GPU: the command stream is walked packet by packet, the shader dwords are decoded back, and the encoders are compared with
 * encodings that are published (the Mesa ACO assembler test, the ISA guide). */
#include <stdio.h>
#include "amd_3d.h"
#include "amd_3d_isa.h"
#include "amd_3d_regs.h"

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

static int ok = 1;

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
                     0x4100000000ULL + 0x0E000000ULL, 0x4100000000ULL + 0x0E000100ULL, 0, 0, 0, 0, 0, 0, 0 };
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


    /* with a marker address the stream writes a marker just before and just after the draw */
    {
        static uint32_t mib[AMD_3D_IB_MAX];
        Amd3dJob mj = job;
        uint32_t k = 0, nm, pos[3], np = 0, wd = 0, tail = 0, tail_ops[3] = { 0, 0, 0 };
        mj.marker_mc = 0x4100000000ULL + 0x0B300000ULL;
        nm = amd_3d_build_ib(mib, AMD_3D_IB_MAX, &mj);
        while (k < nm) {
            uint32_t h = mib[k], body = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
            if (op == 0x37) { wd++; if (np < 3) pos[np++] = op; }
            if (op == 0x2D) { if (np < 3) pos[np++] = op; }
            if ((op == 0x46 || op == 0x49 || op == 0x3C) && tail < 3) tail_ops[tail++] = op;
            k += 1 + body;
        }
        /* 5 + 5 dwords of markers, then EVENT_WRITE (2) + RELEASE_MEM (8) + WAIT_REG_MEM (7) */
        check(nm == n + 10 + 17 && wd == 2 && np == 3 && pos[0] == 0x37 && pos[1] == 0x2D && pos[2] == 0x37,
              "marker: WRITE_DATA before and after the DRAW_INDEX_AUTO (5 dwords each)");
        check(mib[nm - 17 - 5 + 4] == 0x3D3D0002u && mib[nm - 17 - 5 + 2] == (uint32_t)(mj.marker_mc + 4u), "marker after the draw: 0x3D3D0002 at marker + 4");
        check(tail == 3 && tail_ops[0] == 0x46 && tail_ops[1] == 0x49 && tail_ops[2] == 0x3C &&
              mib[nm - 16] == 46u && (mib[nm - 14] & 0x3Fu) == 45u && mib[nm - 10] == 0x3D3D0003u &&
              mib[nm - 5] == (uint32_t)(mj.marker_mc + 8u) && mib[nm - 3] == 0x3D3D0003u,
              "the colour buffer is flushed (CB_META event, RELEASE_MEM CB_DATA_TS) and waited for at marker + 8");
    }

    /* the register numbers themselves, against values known from Mesa radeonsi (R_028C60 / 4 - 0xA000, R_00B020 / 4 - 0xB000 / 4, ...) */
    check(R3D_CB_COLOR0_BASE == 0x318 && R3D_SPI_SHADER_PGM_LO_PS == 0x008 && R3D_SPI_SHADER_PGM_LO_VS == 0x048 &&
          R3D_VGT_PRIMITIVE_TYPE == 0x242 && R3D_IA_MULTI_VGT_PARAM == 0x258,
          "register offsets match Mesa R_028C60 / R_00B020 / R_00B120 / R_030908 / R_030960");

    printf("\nAMD 3D TEST %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
