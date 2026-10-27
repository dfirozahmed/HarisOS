#include "amd_psp.h"
#include "amd_firmware.h"
#include "gpu_log.h"
#include "gpu_mmio.h"
#include "heap.h"

#define PSP_INFO 0x9A00u
#define PSP_MAGIC 0x50535046u
#define PSP_COUNT 5
#define PSP_TMR_TABLE_ADDR 24u
#define PSP_TMR_TABLE_SIZE 25u
#define PSP_CMD_TABLE_ADDR 26u
#define PSP_FENCE_TABLE_ADDR 27u
#define PSP_GFX_CMD_BUF_VERSION 1u
#define PSP_CMD_ID_SETUP_TMR 5u
#define PSP_CMD_ID_LOAD_IP_FW 6u
#define PSP_FW_TYPE_CP_PFP 2u
#define PSP_TMR_SIZE 0x00400000u
#define PSP_TMR_ALIGN 0x00100000u
#define PSP_TMR_LEGACY_PHYS 0u
#define PSP_CMD_SIZE 0x00000400u

/* Renoir MP0/PSP v12.0 mailbox offsets, matching Linux psp_v12_0.c. */
/* mp_12_0_0_offset.h uses MP0 register indices; Renoir MP0 base is 0x16000.
   Convert (base + register_index) * 4 into BAR byte offsets. */
#define C2PMSG33 0x00058184u /* (0x16000 + 0x61) * 4 */
#define C2PMSG35 0x0005818Cu /* (0x16000 + 0x63) * 4 */
#define C2PMSG36 0x00058190u /* (0x16000 + 0x64) * 4 */
#define C2PMSG64 0x00058200u /* (0x16000 + 0x80) * 4 */
#define C2PMSG67 0x0005820Cu /* (0x16000 + 0x83) * 4 */
#define C2PMSG69 0x00058214u /* (0x16000 + 0x85) * 4 */
#define C2PMSG70 0x00058218u /* (0x16000 + 0x86) * 4 */
#define C2PMSG71 0x0005821Cu /* (0x16000 + 0x87) * 4 */
#define C2PMSG81 0x00058244u /* (0x16000 + 0x91) * 4 */

#define PSP_READY 0x80000000u
#define PSP_BL_LOAD_SYSDRV 0x00010000u
#define PSP_BL_LOAD_SOSDRV 0x00020000u
#define PSP_RING_KM 2u /* PSP_RING_TYPE__KM in AMDGPU PSP interface: UM=1, KM=2 */
#define PSP_RING_RESP 0x80000000u
#define PSP_1M 0x00100000u
#define PSP_WAIT_LOOPS 1000000u

static const char *st="PSP payload package not staged";
static uint32_t sol;
static uint32_t last33;
static uint32_t last35;
static uint32_t last64;
static uint32_t last81;
static uint32_t ring_pre64;
static uint32_t ring_post64;
static uint32_t ring_post67;
static uint32_t ring_post69;
static uint32_t ring_post70;
static uint32_t ring_post71;
static uint32_t ring_result;
static uint32_t ring_attempted;
static uint32_t ring_addr;
static uint32_t ring_alloc_addr;
static uint32_t ring_cmd;
static uint32_t ring_post33;
static uint32_t ring_post35;
static uint32_t ring_post81;
static uint32_t ring_resp_code;
static uint32_t ring_stage;
static uint32_t tmr_addr;
static uint32_t tmr_size;
static uint32_t tmr_sys_addr;
/* Parameters actually placed in the SETUP_TMR command. The legacy path sets
   them from the UEFI table (system physical, hi=0); the VRAM path below sets
   a 64-bit VRAM MC address. */
static uint32_t tmr_cmd_lo, tmr_cmd_hi, tmr_cmd_size;
static const char *tmr_test_status = "TMR test not run";
static uint32_t tmr_test_resp, tmr_test_ran, tmr_test_wptr_before, tmr_test_wptr_after;
static uint32_t tmr_test_hi, tmr_test_lo;
static uint32_t cmd_addr;
static uint32_t fence_addr;
static uint32_t cmd_last_id;
static uint32_t cmd_last_fw_type;
static uint32_t cmd_last_fw_addr;
static uint32_t cmd_last_fw_size;
static uint32_t cmd_last_status;
static uint32_t cmd_last_fence;
static uint32_t cmd_attempted;
static uint32_t cmd_stage;
static uint32_t cmd_result;
static uint32_t active;
static uint32_t probe58000;
static uint32_t probe58004;
static uint32_t probe58180;
static uint32_t probe16000;
static uint32_t probe16000_61;
static uint32_t pbar0;
static uint32_t pbar1;
static uint16_t pcmd;
static uint32_t pbar2, pbar3, pbar4, pbar5;
static uint32_t selected_mmio;
static uint32_t cand2_81, cand5_81, cand2_35, cand5_35, cand2_64, cand5_64, cand2_58180, cand5_58180;

/* Renoir GC/GFXHUB v1.0 VM registers.
   The Renoir target exposes its register file through BAR5: the PSP
   C2PMSG sign-of-life probe and KM ring both succeeded there. BAR2 is the
   GOP framebuffer aperture on this machine, so it must not be used for
   GC register reads. gc_9_0 register indices are DWORD indices; the byte
   MMIO offset is (0x2000 + register_index) * 4. READ-ONLY probes only. */
#define RNO_GC_BASE_IDX                    0x2000u
#define RNO_GC_B(reg_idx)                  ((RNO_GC_BASE_IDX + (reg_idx)) << 2)
#define RNO_VM_L2_CNTL_OFF                 RNO_GC_B(0x0840u)
#define RNO_VM_L2_CNTL2_OFF                RNO_GC_B(0x0841u)
#define RNO_VM_L2_CNTL3_OFF                RNO_GC_B(0x0842u)
#define RNO_VM_L2_STATUS_OFF               RNO_GC_B(0x0843u)
#define RNO_VM_CONTEXT0_CNTL_OFF           RNO_GC_B(0x0880u)
#define RNO_VM_INVALIDATE_REQUEST_OFF      RNO_GC_B(0x08A3u)
#define RNO_VM_INVALIDATE_RESPONSE_OFF     RNO_GC_B(0x08B5u)
#define RNO_VM_CONTEXT0_FAULT_DEFAULT_OFF  RNO_GC_B(0x084Eu)
#define RNO_VM_CONTEXT0_PT_BASE_OFF        RNO_GC_B(0x08EBu)
#define RNO_VM_CONTEXT0_PT_START_OFF       RNO_GC_B(0x090Bu)
#define RNO_VM_CONTEXT0_PT_END_OFF         RNO_GC_B(0x092Bu)
#define RNO_MC_VM_SYSTEM_AP_LOW_OFF        RNO_GC_B(0x0985u)
#define RNO_MC_VM_SYSTEM_AP_HIGH_OFF       RNO_GC_B(0x0986u)
#define RNO_MC_VM_MX_L1_TLB_CNTL_OFF       RNO_GC_B(0x0987u)
static uint32_t vm_l2_cntl, vm_l2_cntl2, vm_l2_cntl3, vm_l2_status;
static uint32_t vm_ctx0_cntl, vm_ctx0_pt_base, vm_ctx0_pt_start, vm_ctx0_pt_end;
static uint32_t vm_ctx0_fault_default, vm_inv_req, vm_inv_resp;
static uint32_t vm_sys_ap_low, vm_sys_ap_high, vm_sys_ap_default, vm_l1_tlb_cntl;
static uint32_t vm_probe_ok, vm_probe_bar;
static const char *vm_status = "not run";

static uint32_t rd(volatile uint32_t *p);
static volatile uint32_t *regp(volatile uint8_t *b, uint32_t off);

static void renoir_gmc_readonly_preflight(uint32_t bar5)
{
    vm_l2_cntl=vm_l2_cntl2=vm_l2_cntl3=vm_l2_status=0;
    vm_ctx0_cntl=vm_ctx0_pt_base=vm_ctx0_pt_start=vm_ctx0_pt_end=0;
    vm_ctx0_fault_default=vm_inv_req=vm_inv_resp=0;
    vm_sys_ap_low=vm_sys_ap_high=vm_sys_ap_default=vm_l1_tlb_cntl=0;
    vm_probe_ok=0;
    vm_probe_bar=bar5 & 0xFFFFFFF0u;
    vm_status="BAR5 VM registers not read";
    if(!vm_probe_bar) return;
    volatile uint8_t *b=(volatile uint8_t*)gpu_mmio_ptr((uint64_t)vm_probe_bar);
    if(!b) { vm_status="BAR5 register aperture unavailable"; return; }
    vm_l2_cntl=rd(regp(b,RNO_VM_L2_CNTL_OFF));
    vm_l2_cntl2=rd(regp(b,RNO_VM_L2_CNTL2_OFF));
    vm_l2_cntl3=rd(regp(b,RNO_VM_L2_CNTL3_OFF));
    vm_l2_status=rd(regp(b,RNO_VM_L2_STATUS_OFF));
    vm_ctx0_cntl=rd(regp(b,RNO_VM_CONTEXT0_CNTL_OFF));
    vm_ctx0_pt_base=rd(regp(b,RNO_VM_CONTEXT0_PT_BASE_OFF));
    vm_ctx0_pt_start=rd(regp(b,RNO_VM_CONTEXT0_PT_START_OFF));
    vm_ctx0_pt_end=rd(regp(b,RNO_VM_CONTEXT0_PT_END_OFF));
    vm_ctx0_fault_default=rd(regp(b,RNO_VM_CONTEXT0_FAULT_DEFAULT_OFF));
    vm_inv_req=rd(regp(b,RNO_VM_INVALIDATE_REQUEST_OFF));
    vm_inv_resp=rd(regp(b,RNO_VM_INVALIDATE_RESPONSE_OFF));
    vm_sys_ap_low=rd(regp(b,RNO_MC_VM_SYSTEM_AP_LOW_OFF));
    vm_sys_ap_high=rd(regp(b,RNO_MC_VM_SYSTEM_AP_HIGH_OFF));
    vm_l1_tlb_cntl=rd(regp(b,RNO_MC_VM_MX_L1_TLB_CNTL_OFF));
    /* 0xFFFFFFFF on every VM register means the validated BAR5 register
       aperture is not exposing GC at these Renoir GFXHUB v1.0 offsets. */
    if(vm_l2_cntl==0xFFFFFFFFu && vm_ctx0_cntl==0xFFFFFFFFu &&
       vm_ctx0_pt_base==0xFFFFFFFFu && vm_sys_ap_low==0xFFFFFFFFu) {
        vm_status="BAR5 GFXHUB VM register reads are 0xFFFFFFFF";
        return;
    }
    vm_probe_ok=1;
    if(vm_ctx0_cntl & 1u)
        vm_status="VMID0 context enabled; page-table state read-only";
    else
        vm_status="VMID0 context disabled; GART/VM setup still required";
}

/* PSP v12 expects a GPU-visible system-memory ring. The kernel is identity-mapped
   in the low 32-bit physical space, so allocate from the kernel heap and align
   the returned address to 4 KiB. Keep the allocation for the lifetime of PSP. */
static uint8_t *km_ring;
static uint8_t *km_ring_alloc;
static uint32_t km_ring_addr;

static int alloc_km_ring(void){
    if(km_ring) return 1;
    km_ring_alloc=(uint8_t*)kmalloc(0x3000u);
    if(!km_ring_alloc) return 0;
    if((unsigned long long)km_ring_alloc+0x3000u>GPU_DMA_LIMIT){ kfree(km_ring_alloc); km_ring_alloc=0; return 0; }
    km_ring=(uint8_t*)(unsigned long long)(((uint32_t)(unsigned long long)km_ring_alloc+0xFFFu)&~0xFFFu);
    km_ring_addr=(uint32_t)(unsigned long long)km_ring;
    for(uint32_t i=0;i<0x1000u;i++) km_ring[i]=0;
    return (km_ring_addr & 0xFFFu)==0;
}

static uint32_t rd(volatile uint32_t *p){return *p;}
static void wr(volatile uint32_t *p,uint32_t v){*p=v; __asm__ volatile("" ::: "memory");}
static void pause_cpu(void){__asm__ volatile("pause");}

static volatile uint32_t *regp(volatile uint8_t *b, uint32_t off){
    return (volatile uint32_t *)(b + off);
}

static int wait_mask(volatile uint32_t *r, uint32_t mask, uint32_t value){
    for(uint32_t i=0;i<PSP_WAIT_LOOPS;i++){
        uint32_t v=rd(r);
        if((v & mask)==value) return 1;
        pause_cpu();
    }
    return 0;
}

static int wait_changed(volatile uint32_t *r, uint32_t old){
    for(uint32_t i=0;i<PSP_WAIT_LOOPS;i++){
        uint32_t v=rd(r);
        if(v!=old) return 1;
        pause_cpu();
    }
    return 0;
}

static int psp_table_ok(volatile uint32_t *t){
    /* The FirozOS loader left this table at physical 0x9A00. The HarisOS loader stages no PSP package, and that low
       page belongs to the SMP start-up code here, so it is not read: the answer is always "missing". */
    (void)t;
    return 0;
#if 0
    if(t[0]!=PSP_MAGIC || t[1]!=PSP_COUNT) return 0;
    for(int i=0;i<PSP_COUNT;i++){
        volatile uint32_t *e=t+4+i*4;
        if(e[2]!=1 || e[0]==0 || e[1]==0) return 0;
    }
    if(t[2]==0 || (t[2]&0xFFFFF)!=0) return 0;
    return 1;
#endif
}

/* PSP table indices: 0 stage1, 1 legacy BL, 2 A/B BL, 3 SYS_DRV, 4 sOS. */
static volatile uint8_t *payload_ptr(volatile uint32_t *t, int index, uint32_t *size){
    volatile uint32_t *e=t+4+index*4;
    *size=e[1];
    return (volatile uint8_t *)(unsigned long long)(uint32_t)e[0];
}

static int load_component(volatile uint8_t *b, volatile uint32_t *t,
                          int table_index, uint32_t command){
    uint32_t size=0;
    volatile uint8_t *src=payload_ptr(t,table_index,&size);
    uint32_t dst=t[2];
    if(!src || !size || size>PSP_1M || !dst) return 0;

    /* Linux psp_copy_fw() clears the 1 MiB private buffer then copies the
       signed component into it. The buffer address is passed in MiB units. */
    volatile uint8_t *d=(volatile uint8_t *)(unsigned long long)(uint32_t)dst;
    for(uint32_t i=0;i<PSP_1M;i++) d[i]=0;
    for(uint32_t i=0;i<size;i++) d[i]=src[i];

    wr(regp(b,C2PMSG36),dst>>20);
    wr(regp(b,C2PMSG35),command);
    return wait_mask(regp(b,C2PMSG35),PSP_READY,PSP_READY);
}

static void psp_delay_20ms(void){
    /* Linux psp_v12_0.c uses mdelay(20) after the KM ring command.
       Keep this local and polling-safe because FirozOS does not yet expose
       a generic graphics-driver delay API. */
    volatile uint32_t n=0;
    for(uint32_t ms=0; ms<20; ++ms){
        for(uint32_t i=0; i<50000u; ++i) n += i;
        __asm__ volatile("pause");
    }
    (void)n;
}

static uint32_t psp_table_value(volatile uint32_t *t, uint32_t idx){
    return t[idx];
}

static int psp_runtime_buffers(volatile uint32_t *t){
    tmr_addr = psp_table_value(t, PSP_TMR_TABLE_ADDR);
    tmr_size = psp_table_value(t, PSP_TMR_TABLE_SIZE);
    tmr_sys_addr = tmr_addr;
    cmd_addr = psp_table_value(t, PSP_CMD_TABLE_ADDR);
    fence_addr = psp_table_value(t, PSP_FENCE_TABLE_ADDR);
    if (!tmr_addr || !cmd_addr || !fence_addr) return 0;
    if ((tmr_addr & (PSP_TMR_ALIGN-1u)) != 0) return 0;
    if (tmr_size < PSP_TMR_SIZE) return 0;
    if ((tmr_addr & (PSP_TMR_SIZE-1u)) != 0) return 0;
    if ((cmd_addr & 0xFFFu) != 0 || (fence_addr & 0xFFFu) != 0) return 0;
    return 1;
}

static void clflush_range(volatile uint8_t *p, uint32_t size){
    for(uint32_t off=0; off<size; off+=64u)
        __asm__ volatile("clflush (%0)" : : "r"(p+off) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/* The GPCOM ring frame is exactly 64 bytes. Linux writes the command-buffer
   address, command-buffer size, fence address/value, then advances C2PMSG_67
   in DWORDs by 16. */
static int psp_submit_cmd(volatile uint8_t *b, uint32_t cmd_id,
                          uint32_t fw_addr, uint32_t fw_size, uint32_t fw_type,
                          uint32_t fence_value){
    volatile uint32_t *cmd=(volatile uint32_t *)(unsigned long long)(uint32_t)cmd_addr;
    volatile uint32_t *fence=(volatile uint32_t *)(unsigned long long)(uint32_t)fence_addr;
    volatile uint32_t *frame=(volatile uint32_t *)(unsigned long long)(uint32_t)km_ring_addr;
    uint32_t wptr, frame_index, next_wptr;

    if(!cmd_addr || !fence_addr || !km_ring_addr) return 0;
    for(uint32_t i=0;i<256u;i++) cmd[i]=0;
    *fence=0;
    clflush_range((volatile uint8_t *)fence, 64u);   /* make the 0 reach RAM so a later eviction cannot overwrite PSP's write */
    cmd[0]=PSP_CMD_SIZE;
    cmd[1]=PSP_GFX_CMD_BUF_VERSION;
    cmd[2]=cmd_id;
    if(cmd_id==PSP_CMD_ID_SETUP_TMR){
        cmd[7]=tmr_cmd_lo;
        cmd[8]=tmr_cmd_hi;
        cmd[9]=tmr_cmd_size;
        /* v71 diagnostic: FirozOS has no Linux-style GART mapping yet.
           Use the legacy physical TMR interpretation so the PSP can validate
           the actual system physical address without requiring a GPU VA map.
           This is deliberately limited to SETUP_TMR; no GFX engine is reset. */
        cmd[10]=0u; /* sriov_enabled=0, virt_phy_addr=0 */
        cmd[11]=0u;
        cmd[12]=0u;
    }else if(cmd_id==PSP_CMD_ID_LOAD_IP_FW){
        cmd[7]=fw_addr;
        cmd[8]=0;
        cmd[9]=fw_size;
        cmd[10]=fw_type;
    }else return 0;

    wptr=rd(regp(b,C2PMSG67));
    if((wptr & 0xFu)!=0u) return 0;
    frame_index=(wptr / 16u);
    if(frame_index >= 64u) frame_index=0u;
    frame=(volatile uint32_t *)((uint8_t *)(unsigned long long)(uint32_t)km_ring_addr + frame_index*64u);
    for(uint32_t i=0;i<16u;i++) frame[i]=0;
    frame[0]=cmd_addr;
    frame[1]=0;
    frame[2]=PSP_CMD_SIZE;
    frame[3]=fence_addr;
    frame[4]=0;
    frame[5]=fence_value;
    clflush_range((volatile uint8_t *)cmd, PSP_CMD_SIZE);
    clflush_range((volatile uint8_t *)frame, 64u);
    next_wptr=(wptr+16u)%1024u;
    wr(regp(b,C2PMSG67),next_wptr);
    for(uint32_t i=0;i<2000000u;i++){
        __asm__ volatile("clflush (%0)" : : "r"(fence) : "memory");
        __asm__ volatile("mfence" ::: "memory");
        if(rd(fence)==fence_value){
            clflush_range((volatile uint8_t *)cmd, PSP_CMD_SIZE);  /* re-read response from RAM */
            return 1;
        }
        pause_cpu();
    }
    return 0;
}

static int psp_setup_tmr_and_load_pfp(volatile uint8_t *b, volatile uint32_t *t){
    const AmdFirmwareEntry *pfp;
    uint32_t fw_addr, fw_size;
    cmd_attempted=1; cmd_stage=1; cmd_result=0; cmd_last_status=0;
    cmd_last_id=0; cmd_last_fw_type=0; cmd_last_fw_addr=0; cmd_last_fw_size=0; cmd_last_fence=0;
    if(!psp_runtime_buffers(t)) { cmd_stage=0xF0; cmd_result=0xFFFFFFF0u; return 0; }
    tmr_cmd_lo=tmr_addr; tmr_cmd_hi=0; tmr_cmd_size=tmr_size;
    cmd_stage=2;
    if(!psp_submit_cmd(b,PSP_CMD_ID_SETUP_TMR,0,0,0,1u)){
        cmd_stage=0xF1; cmd_result=0xFFFFFFF1u; return 0;
    }
    cmd_last_id=PSP_CMD_ID_SETUP_TMR; cmd_last_fence=1;
    volatile uint32_t *cmd=(volatile uint32_t *)(unsigned long long)(uint32_t)cmd_addr;
    cmd_last_status=cmd[216]; /* response.status at byte offset 864 */
    if(cmd_last_status!=0u){ cmd_stage=0xF2; cmd_result=cmd_last_status; return 0; }
    cmd_stage=3;
    pfp=amd_firmware_get(6); /* firmware table index 6 = renoir_pfp.bin */
    if(!pfp || !pfp->address || !pfp->size){ cmd_stage=0xF3; cmd_result=0xFFFFFFF3u; return 0; }
    fw_addr=pfp->address; fw_size=pfp->size;
    if((fw_addr & 0xFFFu)!=0u || fw_size==0u){ cmd_stage=0xF4; cmd_result=0xFFFFFFF4u; return 0; }
    cmd_last_fw_addr=fw_addr; cmd_last_fw_size=fw_size; cmd_last_fw_type=PSP_FW_TYPE_CP_PFP;
    cmd_stage=4;
    if(!psp_submit_cmd(b,PSP_CMD_ID_LOAD_IP_FW,fw_addr,fw_size,PSP_FW_TYPE_CP_PFP,2u)){
        cmd_stage=0xF5; cmd_result=0xFFFFFFF5u; return 0;
    }
    cmd_last_id=PSP_CMD_ID_LOAD_IP_FW; cmd_last_fence=2;
    cmd_last_status=cmd[216];
    cmd_last_fw_addr=cmd[218];
    if(cmd_last_status!=0u){ cmd_stage=0xF6; cmd_result=cmd_last_status; return 0; }
    cmd_stage=5; cmd_result=0; return 1;
}

static int create_km_ring(volatile uint8_t *b){
    ring_attempted=1;
    ring_stage=1;
    ring_pre64=rd(regp(b,C2PMSG64));
    ring_post64=ring_post67=ring_post69=ring_post70=ring_post71=ring_result=0;
    ring_addr=ring_alloc_addr=ring_cmd=ring_post33=ring_post35=ring_post81=ring_resp_code=0;
    ring_stage=2;
    if(!alloc_km_ring()){
        ring_stage=0xF0;
        ring_result=0xFFFFFFF0u;
        return 0;
    }
    ring_stage=3;
    uint32_t addr=km_ring_addr;
    ring_addr=addr;
    ring_alloc_addr=(uint32_t)(unsigned long long)km_ring_alloc;
    wr(regp(b,C2PMSG69),addr);
    wr(regp(b,C2PMSG70),0);
    wr(regp(b,C2PMSG71),0x1000);
    ring_post69=rd(regp(b,C2PMSG69));
    ring_post70=rd(regp(b,C2PMSG70));
    ring_post71=rd(regp(b,C2PMSG71));

    ring_stage=4;
    ring_cmd=(PSP_RING_KM<<16);
    wr(regp(b,C2PMSG64),ring_cmd);
    psp_delay_20ms();
    ring_stage=5;
    ring_post64=rd(regp(b,C2PMSG64));
    ring_post67=rd(regp(b,C2PMSG67));
    ring_post33=rd(regp(b,C2PMSG33));
    ring_post35=rd(regp(b,C2PMSG35));
    ring_post81=rd(regp(b,C2PMSG81));
    ring_resp_code=ring_post64 & 0xFFFFu;
    /* Linux defines GFX response bit31 and status in low 16 bits.
       A response flag alone is not success; 0x100 is a non-zero status. */
    if((ring_post64 & PSP_RING_RESP)!=PSP_RING_RESP || ring_resp_code!=0u){
        ring_stage=0xF2;
        ring_result=ring_post64;
        return 0;
    }
    if((ring_post64 & PSP_RING_RESP)!=PSP_RING_RESP){
        ring_stage=0xF1;
        ring_result=0xFFFFFFF1u;
        return 0;
    }
    ring_stage=6;
    ring_result=ring_post64;
    active=1;
    return 1;
}

uint32_t amd_psp_km_ring_addr(void){return km_ring_addr;}

int amd_psp_preflight(uint64_t mmio_base, uint32_t bar0, uint32_t bar1, uint32_t bar2, uint32_t bar3, uint32_t bar4, uint32_t bar5, uint16_t pci_command){
    (void)mmio_base;
    serial_print("PSP V69 BAR5-KM ENTRY\r\n");
    sol=last33=last35=last64=last81=0;
    active=0;
    ring_pre64=ring_post64=ring_post67=ring_post69=ring_post70=ring_post71=ring_result=0;
    ring_attempted=ring_stage=0;
    tmr_addr=tmr_size=tmr_sys_addr=cmd_addr=fence_addr=0;
    cmd_last_id=cmd_last_fw_type=cmd_last_fw_addr=cmd_last_fw_size=cmd_last_status=cmd_last_fence=0;
    cmd_attempted=cmd_stage=cmd_result=0;
    ring_addr=ring_alloc_addr=ring_cmd=ring_post33=ring_post35=ring_post81=ring_resp_code=0;
    pbar0=bar0; pbar1=bar1; pbar2=bar2; pbar3=bar3; pbar4=bar4; pbar5=bar5; pcmd=pci_command;
    selected_mmio=0; cand2_81=cand5_81=cand2_35=cand5_35=cand2_64=cand5_64=cand2_58180=cand5_58180=0;
    probe58000=probe58004=probe58180=probe16000=probe16000_61=0;
    vm_l2_cntl=vm_l2_cntl2=vm_l2_cntl3=vm_l2_status=0;
    vm_ctx0_cntl=vm_ctx0_pt_base=vm_ctx0_pt_start=vm_ctx0_pt_end=0;
    vm_ctx0_fault_default=vm_inv_req=vm_inv_resp=0;
    vm_sys_ap_low=vm_sys_ap_high=vm_sys_ap_default=vm_l1_tlb_cntl=0;
    vm_probe_ok=0; vm_probe_bar=0; vm_status="not run";

    if(!psp_table_ok((volatile uint32_t*)PSP_INFO)){
        st="PSP package staging record missing/incomplete";
        return 0;
    }

    /* V64: diagnostic-only. Never issue PSP mailbox writes. */
    volatile uint8_t *b2 = (bar2 && ((bar2 & 0x1u)==0)) ? gpu_mmio_ptr((uint64_t)(bar2 & 0xFFFFFFF0u)) : (volatile uint8_t*)0;
    volatile uint8_t *b5 = (bar5 && ((bar5 & 0x1u)==0)) ? gpu_mmio_ptr((uint64_t)(bar5 & 0xFFFFFFF0u)) : (volatile uint8_t*)0;

    cand2_81 = b2 ? rd(regp(b2,C2PMSG81)) : 0xFFFFFFFFu;
    cand2_35 = b2 ? rd(regp(b2,C2PMSG35)) : 0xFFFFFFFFu;
    cand2_64 = b2 ? rd(regp(b2,C2PMSG64)) : 0xFFFFFFFFu;
    cand2_58180 = b2 ? rd(regp(b2,0x00058180u)) : 0xFFFFFFFFu;
    cand5_81 = b5 ? rd(regp(b5,C2PMSG81)) : 0xFFFFFFFFu;
    cand5_35 = b5 ? rd(regp(b5,C2PMSG35)) : 0xFFFFFFFFu;
    cand5_64 = b5 ? rd(regp(b5,C2PMSG64)) : 0xFFFFFFFFu;
    cand5_58180 = b5 ? rd(regp(b5,0x00058180u)) : 0xFFFFFFFFu;

    /* Renoir PSP/MP0 is reached through the MMIO BAR used by the MP0
       aperture. On this machine BAR2 is also the E0000000 GOP mapping,
       so never select BAR2 merely because it is readable. A non-FF,
       non-zero C2PMSG_81 on BAR5 is the useful Renoir sOS sign-of-life
       evidence; Linux psp_v12_0 uses the same register as its test. */
    volatile uint8_t *b=(volatile uint8_t*)0;
    if(b5 && cand5_81 != 0xFFFFFFFFu && cand5_81 != 0u) {
        b=b5;
        selected_mmio=bar5 & 0xFFFFFFF0u;
    }

    if(!b){
        st="V65: BAR5 C2PMSG_81 did not provide an sOS sign-of-life value; no PSP write";
        return 0;
    }

    probe58000=rd(regp(b,0x00058000u));
    probe16000=rd(regp(b,0x00016000u));
    probe16000_61=rd(regp(b,0x00016000u + 0x61u*4u));
    probe58004=rd(regp(b,0x00058004u));
    probe58180=rd(regp(b,0x00058180u));
    last33=rd(regp(b,C2PMSG33));
    last35=rd(regp(b,C2PMSG35));
    last64=rd(regp(b,C2PMSG64));
    last81=rd(regp(b,C2PMSG81));
    sol=last81;

    /* BAR5 passed the sOS sign-of-life test. Perform only the Linux-matching
       Renoir v12 KM-ring initialization sequence; no firmware loading or
       reset is attempted here. */
    int ok = create_km_ring(b);
    if(!ok){ st="V72: BAR5 KM ring command failed"; return 0; }

    /* v72 deliberately stops here. Linux's Renoir PSP interface passes the
       TMR GPU virtual address to SETUP_TMR; FirozOS does not yet own a valid
       GFXHUB VMID0/GART mapping. Do not submit SETUP_TMR or LOAD_IP_FW until
       that mapping has been established and verified. */
    volatile uint32_t *t=(volatile uint32_t*)PSP_INFO;
    tmr_addr=psp_table_value(t,PSP_TMR_TABLE_ADDR);
    tmr_size=psp_table_value(t,PSP_TMR_TABLE_SIZE);
    tmr_sys_addr=tmr_addr;
    cmd_addr=psp_table_value(t,PSP_CMD_TABLE_ADDR);
    fence_addr=psp_table_value(t,PSP_FENCE_TABLE_ADDR);
    renoir_gmc_readonly_preflight(pbar5);
    st="V75: PSP KM ring ready; Renoir GFXHUB VM/GART register preflight is read-only";
    return 1;
}

uint32_t amd_psp_mailbox33(void){return last33;}
uint32_t amd_psp_sol(void){return sol;}
uint32_t amd_psp_mailbox35(void){return last35;}
uint32_t amd_psp_mailbox64(void){return last64;}
uint32_t amd_psp_mailbox81(void){return last81;}
uint32_t amd_psp_ring_pre64(void){return ring_pre64;}
uint32_t amd_psp_ring_post64(void){return ring_post64;}
uint32_t amd_psp_ring_post67(void){return ring_post67;}
uint32_t amd_psp_ring_post69(void){return ring_post69;}
uint32_t amd_psp_ring_post70(void){return ring_post70;}
uint32_t amd_psp_ring_post71(void){return ring_post71;}
uint32_t amd_psp_ring_result(void){return ring_result;}
uint32_t amd_psp_ring_addr(void){return ring_addr;}
uint32_t amd_psp_ring_alloc_addr(void){return ring_alloc_addr;}
uint32_t amd_psp_ring_cmd(void){return ring_cmd;}
uint32_t amd_psp_ring_post33(void){return ring_post33;}
uint32_t amd_psp_ring_post35(void){return ring_post35;}
uint32_t amd_psp_ring_post81(void){return ring_post81;}
uint32_t amd_psp_ring_resp_code(void){return ring_resp_code;}
uint32_t amd_psp_ring_attempted(void){return ring_attempted;}
uint32_t amd_psp_ring_stage(void){return ring_stage;}
int amd_psp_active(void){return active;}
uint32_t amd_psp_probe58000(void){return probe58000;}
uint32_t amd_psp_probe58004(void){return probe58004;}
uint32_t amd_psp_probe58180(void){return probe58180;}
uint32_t amd_psp_probe16000(void){return probe16000;}
uint32_t amd_psp_probe16000_61(void){return probe16000_61;}
uint32_t amd_psp_bar0(void){return pbar0;}
uint32_t amd_psp_bar1(void){return pbar1;}
uint16_t amd_psp_pci_command(void){return pcmd;}
uint32_t amd_psp_bar2(void){return pbar2;}
uint32_t amd_psp_bar3(void){return pbar3;}
uint32_t amd_psp_bar4(void){return pbar4;}
uint32_t amd_psp_bar5(void){return pbar5;}
uint32_t amd_psp_selected_mmio(void){return selected_mmio;}
uint32_t amd_psp_cand2_81(void){return cand2_81;}
uint32_t amd_psp_cand5_81(void){return cand5_81;}
uint32_t amd_psp_cand2_35(void){return cand2_35;}
uint32_t amd_psp_cand5_35(void){return cand5_35;}
uint32_t amd_psp_cand2_64(void){return cand2_64;}
uint32_t amd_psp_cand5_64(void){return cand5_64;}
uint32_t amd_psp_cand2_58180(void){return cand2_58180;}
uint32_t amd_psp_cand5_58180(void){return cand5_58180;}
uint32_t amd_psp_tmr_addr(void){return tmr_addr;}
uint32_t amd_psp_tmr_size(void){return tmr_size;}
uint32_t amd_psp_tmr_sys_addr(void){return tmr_sys_addr;}
uint32_t amd_psp_cmd_addr(void){return cmd_addr;}
uint32_t amd_psp_fence_addr(void){return fence_addr;}
uint32_t amd_psp_cmd_attempted(void){return cmd_attempted;}
uint32_t amd_psp_cmd_stage(void){return cmd_stage;}
uint32_t amd_psp_cmd_result(void){return cmd_result;}
uint32_t amd_psp_cmd_last_id(void){return cmd_last_id;}
uint32_t amd_psp_cmd_last_fw_type(void){return cmd_last_fw_type;}
uint32_t amd_psp_cmd_last_fw_addr(void){return cmd_last_fw_addr;}
uint32_t amd_psp_cmd_last_fw_size(void){return cmd_last_fw_size;}
uint32_t amd_psp_cmd_last_status(void){return cmd_last_status;}
uint32_t amd_psp_cmd_last_fence(void){return cmd_last_fence;}
const char *amd_psp_status(void){return st;}

uint32_t amd_psp_vm_l2_cntl(void){return vm_l2_cntl;}
uint32_t amd_psp_vm_l2_cntl2(void){return vm_l2_cntl2;}
uint32_t amd_psp_vm_l2_cntl3(void){return vm_l2_cntl3;}
uint32_t amd_psp_vm_l2_status(void){return vm_l2_status;}
uint32_t amd_psp_vm_ctx0_cntl(void){return vm_ctx0_cntl;}
uint32_t amd_psp_vm_ctx0_pt_base(void){return vm_ctx0_pt_base;}
uint32_t amd_psp_vm_ctx0_pt_start(void){return vm_ctx0_pt_start;}
uint32_t amd_psp_vm_ctx0_pt_end(void){return vm_ctx0_pt_end;}
uint32_t amd_psp_vm_ctx0_fault_default(void){return vm_ctx0_fault_default;}
uint32_t amd_psp_vm_inv_req(void){return vm_inv_req;}
uint32_t amd_psp_vm_inv_resp(void){return vm_inv_resp;}
uint32_t amd_psp_vm_sys_ap_low(void){return vm_sys_ap_low;}
uint32_t amd_psp_vm_sys_ap_high(void){return vm_sys_ap_high;}
uint32_t amd_psp_vm_sys_ap_default(void){return vm_sys_ap_default;}
uint32_t amd_psp_vm_l1_tlb_cntl(void){return vm_l1_tlb_cntl;}
uint32_t amd_psp_vm_probe_ok(void){return vm_probe_ok;}
uint32_t amd_psp_vm_probe_bar5(void){return vm_probe_bar;}
const char *amd_psp_vm_status(void){return vm_status;}

/* ---------------------------------------------------------------------
   Opt-in SETUP_TMR test (FIROZOS_PSP_TMR_TEST). Sends exactly ONE command:
   GFX_CMD_ID_SETUP_TMR with a TMR placed in VRAM, addressed by its MC
   address (what Linux passes). No firmware is loaded, no engine is reset.
   Result is only recorded; nothing is retried automatically.
   --------------------------------------------------------------------- */
int amd_psp_setup_tmr_vram(uint64_t tmr_mc_addr, uint32_t tmr_bytes)
{
    volatile uint8_t *b=(volatile uint8_t *)(unsigned long long)selected_mmio;
    volatile uint32_t *cmd;
    tmr_test_ran=1;
    if(!active || !km_ring_addr || !selected_mmio){
        tmr_test_status="TMR test skipped: PSP KM ring not ready"; return 0;
    }
    if(!cmd_addr || !fence_addr || (cmd_addr&0xFFFu) || (fence_addr&0xFFFu)){
        tmr_test_status="TMR test skipped: no valid cmd/fence buffer from loader"; return 0;
    }
    if(tmr_bytes < PSP_TMR_SIZE || (tmr_mc_addr & (PSP_TMR_ALIGN-1u))){
        tmr_test_status="TMR test skipped: TMR size/alignment invalid"; return 0;
    }
    tmr_cmd_lo=(uint32_t)tmr_mc_addr; tmr_cmd_hi=(uint32_t)(tmr_mc_addr>>32);
    tmr_cmd_size=tmr_bytes;
    tmr_test_lo=tmr_cmd_lo; tmr_test_hi=tmr_cmd_hi;
    tmr_test_wptr_before=rd(regp(b,C2PMSG67));
    cmd_attempted=1; cmd_stage=2; cmd_last_status=0;
    if(!psp_submit_cmd(b,PSP_CMD_ID_SETUP_TMR,0,0,0,1u)){
        tmr_test_wptr_after=rd(regp(b,C2PMSG67));
        tmr_test_status="TMR test: PSP did not complete (timeout, fence unchanged)";
        cmd_stage=0xF1; cmd_result=0xFFFFFFF1u;
        return 0;
    }
    tmr_test_wptr_after=rd(regp(b,C2PMSG67));
    cmd=(volatile uint32_t *)(unsigned long long)(uint32_t)cmd_addr;
    tmr_test_resp=cmd[216];
    cmd_last_id=PSP_CMD_ID_SETUP_TMR; cmd_last_fence=1; cmd_last_status=tmr_test_resp;
    if(tmr_test_resp!=0u){
        tmr_test_status="TMR test: PSP completed but REJECTED (see status)";
        cmd_stage=0xF2; cmd_result=tmr_test_resp;
        return 0;
    }
    tmr_test_status="TMR test: PSP accepted SETUP_TMR (status 0)";
    cmd_stage=3; cmd_result=0;
    return 1;
}
const char *amd_psp_tmr_test_status(void){return tmr_test_status;}
uint32_t amd_psp_tmr_test_resp(void){return tmr_test_resp;}
uint32_t amd_psp_tmr_test_ran(void){return tmr_test_ran;}
uint32_t amd_psp_tmr_test_lo(void){return tmr_test_lo;}
uint32_t amd_psp_tmr_test_hi(void){return tmr_test_hi;}
uint32_t amd_psp_tmr_test_wptr_before(void){return tmr_test_wptr_before;}
uint32_t amd_psp_tmr_test_wptr_after(void){return tmr_test_wptr_after;}

/* ---------------------------------------------------------------------
   Opt-in LOAD_IP_FW test (FIROZOS_PSP_FW_TEST): one firmware image.

   Linux does NOT hand the PSP the whole .bin file. It copies only the
   ucode body (file + header.ucode_array_offset_bytes, header.ucode_size_bytes
   long) into a 4 KiB-aligned buffer and passes that. Same here.
   common_firmware_header: +4 header_size_bytes, +20 ucode_size_bytes,
   +24 ucode_array_offset_bytes (matches amd_native.c header check).

   Uses physical (identity-mapped) addresses, which is what worked in the
   TMR test with GART off. Must be called after a successful SETUP_TMR.
   --------------------------------------------------------------------- */
static const char *fw_test_status = "FW load test not run";
static uint32_t fw_test_resp, fw_test_size, fw_test_addr, fw_test_type, fw_test_ran;

int amd_psp_load_fw_test(int fw_index, uint32_t fw_type)
{
    volatile uint8_t *b=(volatile uint8_t *)(unsigned long long)selected_mmio;
    const AmdFirmwareEntry *e;
    const uint8_t *file;
    uint32_t hdr_size, ucode_size, ucode_off;
    uint8_t *raw, *buf;
    volatile uint32_t *cmd;

    fw_test_ran=1; fw_test_type=fw_type;
    if(!active || !km_ring_addr || !selected_mmio || !cmd_addr || !fence_addr){
        fw_test_status="FW test skipped: PSP KM ring / buffers not ready"; return 0;
    }
    e=amd_firmware_get(fw_index);
    if(!e || !e->address || !e->size || !e->status){
        fw_test_status="FW test skipped: firmware image not staged"; return 0;
    }
    file=(const uint8_t *)(unsigned long long)(uint32_t)e->address;
    hdr_size  =*(const uint32_t *)(file+4);
    ucode_size=*(const uint32_t *)(file+20);
    ucode_off =*(const uint32_t *)(file+24);
    if(!ucode_size || ucode_size>0x400000u || ucode_off<hdr_size ||
       (uint64_t)ucode_off+ucode_size > e->size){
        fw_test_status="FW test skipped: firmware header inconsistent"; return 0;
    }
    raw=(uint8_t*)kmalloc(ucode_size+0x2000u);
    if(!raw){ fw_test_status="FW test skipped: out of memory"; return 0; }
    if((unsigned long long)raw+ucode_size+0x2000u>GPU_DMA_LIMIT){ kfree(raw); fw_test_status="FW test skipped: buffer above 4 GiB"; return 0; }
    buf=(uint8_t*)(unsigned long long)(((uint32_t)(unsigned long long)raw+0xFFFu)&~0xFFFu);
    for(uint32_t i=0;i<ucode_size;i++) buf[i]=file[ucode_off+i];
    clflush_range((volatile uint8_t *)buf, (ucode_size+63u)&~63u);

    fw_test_addr=(uint32_t)(unsigned long long)buf; fw_test_size=ucode_size;
    cmd_last_fw_addr=fw_test_addr; cmd_last_fw_size=ucode_size; cmd_last_fw_type=fw_type;
    if(!psp_submit_cmd(b,PSP_CMD_ID_LOAD_IP_FW,fw_test_addr,ucode_size,fw_type,2u)){
        fw_test_status="FW test: PSP did not complete (timeout, fence unchanged)"; return 0;
    }
    cmd=(volatile uint32_t *)(unsigned long long)(uint32_t)cmd_addr;
    fw_test_resp=cmd[216];
    cmd_last_id=PSP_CMD_ID_LOAD_IP_FW; cmd_last_fence=2; cmd_last_status=fw_test_resp;
    if(fw_test_resp!=0u){
        fw_test_status="FW test: PSP completed but REJECTED (see status)"; return 0;
    }
    fw_test_status="FW test: PSP accepted LOAD_IP_FW (status 0)";
    return 1;
}
const char *amd_psp_fw_test_status(void){return fw_test_status;}
uint32_t amd_psp_fw_test_resp(void){return fw_test_resp;}
uint32_t amd_psp_fw_test_size(void){return fw_test_size;}
uint32_t amd_psp_fw_test_addr(void){return fw_test_addr;}
uint32_t amd_psp_fw_test_type(void){return fw_test_type;}

/* ---------------------------------------------------------------------
   Opt-in batch: LOAD_IP_FW for every GFX/SDMA image in one run
   (FIROZOS_PSP_FW_BATCH). Each job is recorded separately; a rejected image
   does not stop the batch, a PSP timeout does (PSP may be stuck).

   Sizes/offsets follow Linux amdgpu_ucode_init_single_fw():
     CE/PFP/ME/RLC_G/SDMA0 : whole ucode body (ucode_size_bytes)
     MEC1/MEC2             : ucode_size_bytes - jt_size*4   (type CP_MEC)
     MEC1_JT/MEC2_JT       : jump table = ucode + jt_offset*4, jt_size*4 long
                             (types CP_MEC_ME1 / CP_MEC_ME2)
   gfx header v1.0: +32 feature_version, +36 jt_offset, +40 jt_size (dwords).
   FW type numbers come from Linux psp_gfx_if.h (from memory; only PFP=2 has
   been confirmed on this machine). A wrong type just gets REJECTED.
   --------------------------------------------------------------------- */
enum { FWJ_BODY=0, FWJ_MEC_MAIN=1, FWJ_JT=2 };
typedef struct { const char *name; int idx; uint32_t type; int kind;
                 uint32_t state, resp, size; } FwJob;
static FwJob fw_jobs[] = {
    {"CE",      2, 3,  FWJ_BODY,     0,0,0},
    {"PFP",     6, 2,  FWJ_BODY,     0,0,0},
    {"ME",      3, 1,  FWJ_BODY,     0,0,0},
    {"MEC1",    4, 4,  FWJ_MEC_MAIN, 0,0,0},
    {"MEC1_JT", 4, 5,  FWJ_JT,       0,0,0},
    {"MEC2",    5, 4,  FWJ_MEC_MAIN, 0,0,0},
    {"MEC2_JT", 5, 6,  FWJ_JT,       0,0,0},
    {"RLC_G",   7, 8,  FWJ_BODY,     0,0,0},
    {"SDMA0",   8, 9,  FWJ_BODY,     0,0,0},
};
#define FW_JOB_COUNT ((int)(sizeof(fw_jobs)/sizeof(fw_jobs[0])))
/* state: 0 not run, 1 accepted, 2 rejected, 3 timeout, 4 skipped (bad header/mem), 5 not run (batch aborted) */
static const char *fw_batch_status = "FW batch not run";
static uint32_t fw_batch_accepted;

/* Resolve a job to (source pointer, byte count). Returns 0 if the header is inconsistent. */
static int fw_job_resolve(const FwJob *j, const uint8_t **src, uint32_t *len)
{
    const AmdFirmwareEntry *e=amd_firmware_get(j->idx);
    const uint8_t *f; uint32_t hdr, usize, uoff, jtoff, jtsize;
    if(!e || !e->address || !e->size || !e->status) return 0;
    f=(const uint8_t *)(unsigned long long)(uint32_t)e->address;
    hdr=*(const uint32_t *)(f+4);
    usize=*(const uint32_t *)(f+20);
    uoff=*(const uint32_t *)(f+24);
    if(!usize || usize>0x400000u || uoff<hdr || (uint64_t)uoff+usize>e->size) return 0;
    if(j->kind==FWJ_BODY){ *src=f+uoff; *len=usize; return 1; }
    jtoff=*(const uint32_t *)(f+36);
    jtsize=*(const uint32_t *)(f+40);
    if(!jtsize || (uint64_t)jtsize*4u >= usize || (uint64_t)jtoff*4u + (uint64_t)jtsize*4u > usize) return 0;
    if(j->kind==FWJ_MEC_MAIN){ *src=f+uoff; *len=usize-jtsize*4u; return 1; }
    *src=f+uoff+jtoff*4u; *len=jtsize*4u; return 1;       /* FWJ_JT */
}

int amd_psp_load_fw_batch(void)
{
    volatile uint8_t *b=(volatile uint8_t *)(unsigned long long)selected_mmio;
    volatile uint32_t *cmd;
    uint8_t *raw, *buf;
    uint32_t maxlen=0, fence=2u;
    int i, abort_batch=0;

    fw_batch_accepted=0;
    for(i=0;i<FW_JOB_COUNT;i++){ fw_jobs[i].state=0; fw_jobs[i].resp=0; fw_jobs[i].size=0; }
    if(!active || !km_ring_addr || !selected_mmio || !cmd_addr || !fence_addr){
        fw_batch_status="FW batch skipped: PSP KM ring / buffers not ready"; return 0;
    }
    /* one buffer big enough for the largest job */
    for(i=0;i<FW_JOB_COUNT;i++){
        const uint8_t *src; uint32_t len;
        if(fw_job_resolve(&fw_jobs[i],&src,&len) && len>maxlen) maxlen=len;
    }
    if(!maxlen){ fw_batch_status="FW batch skipped: no usable firmware header"; return 0; }
    raw=(uint8_t*)kmalloc(maxlen+0x2000u);
    if(!raw){ fw_batch_status="FW batch skipped: out of memory"; return 0; }
    if((unsigned long long)raw+maxlen+0x2000u>GPU_DMA_LIMIT){ kfree(raw); fw_batch_status="FW batch skipped: buffer above 4 GiB"; return 0; }
    buf=(uint8_t*)(unsigned long long)(((uint32_t)(unsigned long long)raw+0xFFFu)&~0xFFFu);
    cmd=(volatile uint32_t *)(unsigned long long)(uint32_t)cmd_addr;

    for(i=0;i<FW_JOB_COUNT;i++){
        FwJob *j=&fw_jobs[i];
        const uint8_t *src; uint32_t len, k;
        if(abort_batch){ j->state=5; continue; }
        if(!fw_job_resolve(j,&src,&len)){ j->state=4; continue; }
        for(k=0;k<len;k++) buf[k]=src[k];
        clflush_range((volatile uint8_t *)buf,(len+63u)&~63u);
        j->size=len;
        cmd_last_fw_addr=(uint32_t)(unsigned long long)buf; cmd_last_fw_size=len; cmd_last_fw_type=j->type;
        if(!psp_submit_cmd(b,PSP_CMD_ID_LOAD_IP_FW,(uint32_t)(unsigned long long)buf,len,j->type,fence)){
            j->state=3; abort_batch=1; continue;
        }
        j->resp=cmd[216];
        cmd_last_id=PSP_CMD_ID_LOAD_IP_FW; cmd_last_fence=fence; cmd_last_status=j->resp;
        if(j->resp==0u){ j->state=1; fw_batch_accepted++; } else j->state=2;
        fence++;
    }
    fw_batch_status = abort_batch ? "FW batch ABORTED: PSP timeout" :
                      (fw_batch_accepted==(uint32_t)FW_JOB_COUNT ? "FW batch: all images accepted"
                                                                 : "FW batch: some images not accepted");
    return (int)fw_batch_accepted;
}
const char *amd_psp_fw_batch_status(void){return fw_batch_status;}
uint32_t amd_psp_fw_batch_accepted(void){return fw_batch_accepted;}
int amd_psp_fw_batch_count(void){return FW_JOB_COUNT;}
const char *amd_psp_fw_job_name(int i){return (i>=0&&i<FW_JOB_COUNT)?fw_jobs[i].name:"";}
uint32_t amd_psp_fw_job_state(int i){return (i>=0&&i<FW_JOB_COUNT)?fw_jobs[i].state:0;}
uint32_t amd_psp_fw_job_resp(int i){return (i>=0&&i<FW_JOB_COUNT)?fw_jobs[i].resp:0;}
uint32_t amd_psp_fw_job_size(int i){return (i>=0&&i<FW_JOB_COUNT)?fw_jobs[i].size:0;}
uint32_t amd_psp_fw_job_type(int i){return (i>=0&&i<FW_JOB_COUNT)?fw_jobs[i].type:0;}
