#include "amd_bringup.h"
#include "amd_gpu.h"
#include "amd_native.h"
#include "amd_psp.h"
#include "amd_firmware.h"
#include "gpu_log.h"
#include "pci.h"
#include "vfs.h"
#include "exec.h"

static const char *bringup_status = "AMD GPU not probed";
static int bringup_ran, bringup_found, bringup_probed; /* for the Device Manager line */
static int g_target;                                  /* exactly the machine the write steps were developed on (amd_gpu_is_target) */
static int g_writes = -1;                             /* -1 not decided yet, 0 no, 1 the write steps may run */
static uint64_t g_mmio;
static uint32_t g_bar0, g_bar1;

const char *amd_gpu_bringup_status(void) { return bringup_status; }

static void log_hex(const char *label, uint32_t v)
{
    serial_print(label);
    serial_print("0x");
    serial_hex32(v);
    serial_print("\n");
}

int amd_gpu_bringup(void)
{
    int i;
    bringup_ran = 1;
    amd_firmware_init();

    for (i = 0; i < pci_count(); i++)
    {
        const PciDevice *d = pci_get(i);
        if (!d || d->class_code != 0x03 || d->vendor_id != 0x1002)
            continue;

        uint32_t bar0 = pci_cfg_read(d->bus, d->dev, d->fn, 0x10);
        uint32_t bar1 = pci_cfg_read(d->bus, d->dev, d->fn, 0x14);
        uint32_t bar2 = pci_cfg_read(d->bus, d->dev, d->fn, 0x18);
        uint32_t bar3 = pci_cfg_read(d->bus, d->dev, d->fn, 0x1C);
        uint32_t bar4 = pci_cfg_read(d->bus, d->dev, d->fn, 0x20);
        uint32_t bar5 = pci_cfg_read(d->bus, d->dev, d->fn, 0x24);
        uint32_t subsys = pci_cfg_read(d->bus, d->dev, d->fn, 0x2C);
        uint16_t pcicmd = (uint16_t)pci_cfg_read(d->bus, d->dev, d->fn, 0x04);
        uint16_t sub_ven = (uint16_t)(subsys & 0xFFFF), sub_dev = (uint16_t)(subsys >> 16);

        bringup_found = 1;
        g_target = amd_gpu_is_target(d->vendor_id, d->device_id, sub_ven, sub_dev, d->revision);
        serial_print("AMD GPU: ");
        serial_print(amd_gpu_name(d->device_id));
        serial_print("\n");
        log_hex("  device     ", d->device_id);
        log_hex("  subsystem  ", subsys);
        log_hex("  revision   ", d->revision);
        log_hex("  BAR0       ", bar0);
        log_hex("  BAR1       ", bar1);
        log_hex("  BAR5       ", bar5);
        log_hex("  PCI command", pcicmd);
        serial_print(g_target ? "  this is the target machine (Renoir 1002:1636, HP 103C:87CF, rev C3)\n"
                              : "  not the target machine: read-only checks only\n");

        if (!amd_gpu_supported(d->vendor_id, d->device_id))
        {
            bringup_status = "AMD GPU is not in the supported list";
            return 0;
        }

        /* On AMD the register aperture is the last memory BAR (BAR5), as in Linux amdgpu; BAR0 is the VRAM window. */
        uint64_t mmio = (uint64_t)(bar5 & 0xFFFFFFF0u);
        if (!(pcicmd & 2u) || mmio == 0)
        {
            bringup_status = "AMD GPU: memory decode off or no register BAR";
            return 0;
        }

        g_mmio = mmio; g_bar0 = bar0; g_bar1 = bar1;
        (void)amd_gpu_probe(d->device_id, mmio, 0, 0, 0);
        serial_print("  "); serial_print(amd_gpu_native_status()); serial_print("\n");
        bringup_status = amd_gpu_native_status();

        int psp_ok = amd_psp_preflight(mmio, bar0, bar1, bar2, bar3, bar4, bar5, pcicmd);
        if (psp_ok && amd_native_preflight(d->vendor_id, d->device_id, sub_ven, sub_dev, d->revision, mmio))
            bringup_status = amd_native_status();
        serial_print("  preflight: "); serial_print(amd_native_status()); serial_print("\n");

        if (d->device_id == 0x1636)
        {
            /* Read-only: does the graphics core answer, and what do the VM hubs / VRAM range look like. */
            bringup_probed = 1;
            (void)amd_native_gc_probe(mmio);
            serial_print("  GC: "); serial_print(amd_native_gc_status()); serial_print("\n");
            log_hex("  GRBM_STATUS ", amd_native_gc_grbm_status());
            (void)amd_native_vm_probe(mmio);
            serial_print("  VM: "); serial_print(amd_native_vm_probe_status()); serial_print("\n");
            serial_print("  VRAM MiB: "); serial_dec32(amd_native_vram_mib()); serial_print("\n");
            log_hex("  MMHUB ctx0  ", amd_native_mmhub_ctx0_cntl());
            amd_native_fault_probe(mmio);
            log_hex("  fault MM    ", amd_native_fault_mm_status());
            log_hex("  fault GC    ", amd_native_fault_gc_status());
        }
        return 1;
    }

    bringup_status = "no AMD GPU on PCI";
    return 0;
}

/* Appends text / a hex or decimal number to out[n]; stops quietly when it is full. */
static void cat_str(char *out, int n, const char *s)
{
    int len = 0;
    while (out[len])
        len++;
    while (*s && len < n - 1)
        out[len++] = *s++;
    out[len] = 0;
}

static void cat_num(char *out, int n, uint32_t v, int hex)
{
    char tmp[12];
    int k = 0;
    if (hex) {
        cat_str(out, n, "0x");
        for (int i = 28; i >= 0; i -= 4)
            tmp[k++] = "0123456789ABCDEF"[(v >> i) & 0xF];
    } else {
        char rev[11];
        int r = 0;
        do { rev[r++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (r)
            tmp[k++] = rev[--r];
    }
    tmp[k] = 0;
    cat_str(out, n, tmp);
}

void amd_gpu_bringup_summary(char *out, int n)
{
    out[0] = 0;
    if (!bringup_ran || !bringup_found || n < 2)
        return;
    if (!bringup_probed) {
        cat_str(out, n, bringup_status);
        return;
    }
    cat_str(out, n, "GRBM ");
    cat_num(out, n, amd_native_gc_grbm_status(), 1);
    cat_str(out, n, ", VRAM ");
    cat_num(out, n, amd_native_vram_mib(), 0);
    cat_str(out, n, " MB, MMHUB ctx0 ");
    cat_num(out, n, amd_native_mmhub_ctx0_cntl(), 1);
    cat_str(out, n, amd_firmware_staged() ? ", fw staged" : ", no fw");
}

/* The steps that WRITE to the GPU run only on the machine they were developed and tested on (amd_gpu_is_target), and only if
 * there is no file AMDOFF.TXT in the top folder of a drive. That file is the way out if a boot ever hangs in one of the steps:
 * put it on the boot USB stick from another computer and the next boot skips them. Decided once, after the drives are mounted. */
static int writes_allowed(void)
{
    if (g_writes < 0) {
        if (!g_target || !bringup_probed)
            g_writes = 0;
        else if (exec_find("AMDOFF.TXT") >= 0) {
            serial_print("AMDOFF.TXT found: the steps that write to the GPU are skipped\n");
            g_writes = 0;
        } else
            g_writes = 1;
    }
    return g_writes;
}

#define LOG_NAME "GPULOG.TXT"

/* Writing the log file is OFF unless the build asks for it (cmake -DHARIS_AMD_GPU_LOG=ON): it unlocks the EFI System Partition and
 * writes to the boot drive on every boot. Without it nothing is written to any drive; the text still goes to COM1. */
#ifndef HARIS_AMD_GPU_LOG
#define HARIS_AMD_GPU_LOG 0
#endif

/* Writes text to GPULOG.TXT in the top folder of the boot drive (the one that holds KERNEL.EFI: the EFI System Partition, which
 * is locked against writing; it is unlocked for this one file and locked again). Returns 1 if the file was written. */
static int save_text(const char *text, unsigned len)
{
    if (!bringup_found)
        return 0;
    int k = exec_find("KERNEL.EFI");
    if (k < 0)
        return 0;
    int vol = vfs_volume_of(k);
    int dir = vfs_volume_node(vol);
    VfsVolInfo vi;
    if (dir < 0 || vfs_volume_info(vol, &vi) != 0)
        return 0;
    int was_locked = vi.locked;
    if (was_locked && vfs_set_locked(vol, 0) != 0)
        return 0;
    int ok = vfs_save(dir, LOG_NAME, text, len) >= 0;
    if (was_locked)
        vfs_set_locked(vol, 1);
    return ok;
}

/* Saves everything the driver printed to GPULOG.TXT. Called before every risky step, so a hang still leaves the last line that
 * was reached. Only with HARIS_AMD_GPU_LOG=ON; otherwise nothing is written (a fatal exception still saves, see below). */
int amd_gpu_log_save(void)
{
#if !HARIS_AMD_GPU_LOG
    return 0;
#else
    unsigned len;
    const char *text = gpu_log_text(&len);
    return save_text(text, len);
#endif
}

/* Fatal kernel exception (see fault_set_crash_hook): GPULOG.TXT gets the exception report followed by what the AMD driver had
 * logged, whatever HARIS_AMD_GPU_LOG says, so a crash in one of the GPU steps can be read afterwards. Best effort: this runs
 * inside the crash handler, with the other processors stopped; it only does anything if an AMD GPU was found. */
void amd_gpu_crash_save(const char *report)
{
    static char buf[1536 + 16384 + 64];
    unsigned n = 0, len;
    const char *head = "KERNEL EXCEPTION (written by the AMD GPU bring-up)\n";
    const char *mid = "\n--- AMD GPU log up to the exception ---\n";
    const char *log = gpu_log_text(&len);
    if (!bringup_found)
        return;
    for (const char *p = head; *p && n < sizeof buf - 1; p++)
        buf[n++] = *p;
    for (const char *p = report; *p && n < 1536; p++)
        buf[n++] = *p;
    for (const char *p = mid; *p && n < sizeof buf - 1; p++)
        buf[n++] = *p;
    for (unsigned i = 0; i < len && n < sizeof buf - 1; i++)
        buf[n++] = log[i];
    buf[n] = 0;
    (void)save_text(buf, n);
}

/* The register-writing step (GART / VMID0 on both hubs, one PTE). Only when built with HARIS_AMD_GPU_GART. Needs the
 * drives mounted (amd_gpu_log_save), so it runs after volume_mount_all(), not inside amd_gpu_bringup(). */
void amd_gpu_bringup_gart(void)
{
#if defined(HARIS_AMD_GPU_GART) && HARIS_AMD_GPU_GART
    if (!writes_allowed() || g_bar1 != 0 || !(g_bar0 & 0xFFFFFFF0u))
        return;
    serial_print("GART: about to enable VMID0 (writes GPU registers)\n");
    amd_gpu_log_save(); /* if the machine hangs now, this line is the last one in the file */
    if (amd_native_gart_enable(g_mmio, (uint64_t)(g_bar0 & 0xFFFFFFF0u)))
        (void)amd_native_gart_map(0, amd_native_vram_mc_addr(0x00100000u, 0x1000u));
    serial_print("GART: "); serial_print(amd_native_gart_status()); serial_print("\n");
    serial_print("GART map: "); serial_print(amd_native_gart_map_status()); serial_print("\n");
    (void)amd_native_vm_probe(g_mmio);
    serial_print("VM after: "); serial_print(amd_native_vm_probe_status()); serial_print("\n");
    log_hex("  MMHUB ctx0  ", amd_native_mmhub_ctx0_cntl());
    log_hex("  GFXHUB ctx0 ", amd_native_gfxhub_ctx0());
    amd_gpu_log_save();
#endif
}

/* Loads the firmware images from the boot drive and logs what was found. Reads only. Needs the drives mounted. */
void amd_gpu_bringup_firmware(void)
{
    if (!bringup_probed || !g_target)
        return;
    serial_print("Firmware:\n");
    (void)amd_firmware_load();
    serial_print("  "); serial_print(amd_firmware_status()); serial_print("\n");
}

/* PSP (security processor) steps, in the order of Linux psp_v12_0 and of the FirozOS V89 bring-up. They write PSP mailbox
 * registers, so they are compiled in only with HARIS_AMD_GPU_PSP_LEVEL (0 = off):
 *   1  create the KM ring, then one SETUP_TMR (a 4 MiB trusted memory region in VRAM, 192 MiB in)
 *   2  also LOAD_IP_FW for CE, PFP, ME, MEC1/2, RLC_G, SDMA0 (needs the firmware loaded by amd_gpu_bringup_firmware())
 *   3  (with HARIS_AMD_GPU_GART=ON) the CP tests of amd_gpu_bringup_cp(), which run after the GART step
 * It must run BEFORE the GART step: the PSP is given physical addresses, which stop being valid once VMID0 translates.
 * Every step saves the log first, so a hang leaves the last line that was reached. */
#ifndef HARIS_AMD_GPU_PSP_LEVEL
#define HARIS_AMD_GPU_PSP_LEVEL 0
#endif
#define PSP_TMR_VRAM_OFF 0x0C000000u /* 192 MiB into VRAM, 4 MiB aligned */
#define PSP_TMR_BYTES    0x00400000u

void amd_gpu_bringup_psp(void)
{
#if HARIS_AMD_GPU_PSP_LEVEL >= 1
    if (!writes_allowed() || (amd_native_mmhub_ctx0_cntl() & 1u))
        return; /* not probed, or VMID0 translation is already on */
    serial_print("PSP: about to create the KM ring (writes PSP mailbox registers)\n");
    amd_gpu_log_save();
    int ring_ok = amd_psp_ring_init();
    serial_print("PSP: "); serial_print(amd_psp_status()); serial_print("\n");
    log_hex("  ring cmd      ", amd_psp_ring_cmd());
    log_hex("  ring resp     ", amd_psp_ring_result());
    log_hex("  ring stage    ", amd_psp_ring_stage());
    log_hex("  C2PMSG_64     ", amd_psp_ring_post64());
    log_hex("  C2PMSG_81     ", amd_psp_ring_post81());
    amd_gpu_log_save();
    if (!ring_ok)
        return;

    uint64_t tmr_mc = amd_native_vram_mc_addr(PSP_TMR_VRAM_OFF, PSP_TMR_BYTES);
    if (!tmr_mc) {
        serial_print("PSP: TMR skipped: no VRAM MC address\n");
        amd_gpu_log_save();
        return;
    }
    serial_print("PSP: about to send SETUP_TMR\n");
    amd_gpu_log_save();
    int tmr_ok = amd_psp_setup_tmr_vram(tmr_mc, PSP_TMR_BYTES);
    serial_print("PSP: "); serial_print(amd_psp_tmr_test_status()); serial_print("\n");
    amd_gpu_log_save();

#if HARIS_AMD_GPU_PSP_LEVEL >= 2
    if (!tmr_ok || !amd_firmware_staged())
        return;
    serial_print("PSP: about to load the firmware images (LOAD_IP_FW)\n");
    amd_gpu_log_save();
    (void)amd_psp_load_fw_batch();
    serial_print("PSP: "); serial_print(amd_psp_fw_batch_status()); serial_print("\n");
    for (int i = 0; i < amd_psp_fw_batch_count(); i++) {
        serial_print("  "); serial_print(amd_psp_fw_job_name(i));
        log_hex(" state ", amd_psp_fw_job_state(i));
        log_hex("     resp ", amd_psp_fw_job_resp(i));
    }
    amd_gpu_log_save();
#else
    (void)tmr_ok;
#endif
#endif
}

/* CP (command processor) tests, in the order of the FirozOS V133 bring-up that passed on this machine. They run AFTER the
 * PSP firmware load and AFTER the GART step: the CP tests of earlier HarisOS boots (ring in VRAM, GART off) fetched the
 * packet but the register write never landed, while the FirozOS log of the same tests with VMID0 enabled (GART on) passes
 * (CP -> memory through GPU virtual addresses, IBs with fences, a persistent ring). So build with HARIS_AMD_GPU_GART=ON and
 * HARIS_AMD_GPU_PSP_LEVEL=3. All of it WRITES GPU registers; every step saves the log first. */
void amd_gpu_bringup_cp(void)
{
#if HARIS_AMD_GPU_PSP_LEVEL >= 3
    if (!writes_allowed() || g_bar1 != 0 || !(g_bar0 & 0xFFFFFFF0u))
        return;
    uint64_t vram = (uint64_t)(g_bar0 & 0xFFFFFFF0u);
    int fw_ok = amd_psp_fw_job_state(0) == 1 && amd_psp_fw_job_state(1) == 1 && amd_psp_fw_job_state(2) == 1;
    if (!fw_ok) {
        serial_print("CP: skipped: CE/PFP/ME firmware was not accepted by the PSP this boot\n");
        amd_gpu_log_save();
        return;
    }
    if (!(amd_native_mmhub_ctx0_cntl() & 1u)) {
        serial_print("CP: skipped: VMID0 translation (GART) is off; build with HARIS_AMD_GPU_GART=ON\n");
        amd_gpu_log_save();
        return;
    }

    serial_print("CP: about to run the CP pulse test (un-halts CE/PFP/ME and MEC for ~20 ms, then restores)\n");
    amd_gpu_log_save();
    (void)amd_native_cp_pulse_test(g_mmio);
    serial_print("CP pulse: "); serial_print(amd_native_cp_pulse_status()); serial_print("\n");
    log_hex("  SCRATCH_REG0  ", amd_native_cp_pulse_scratch());
    amd_gpu_log_save();

    serial_print("CP: about to run the CP ring matrix\n");
    amd_gpu_log_save();
    (void)amd_native_cp_ring_matrix(g_mmio, vram, fw_ok, 0);
    serial_print("CPM: "); serial_print(amd_native_cp_matrix_status()); serial_print("\n");
    amd_gpu_log_save();

    serial_print("CP: about to run the GPU-virtual-address CP test (VMID0, then VMID1)\n");
    amd_gpu_log_save();
    (void)amd_native_cp_vm_gart_test(g_mmio, vram, fw_ok);
    serial_print("CP VM: "); serial_print(amd_native_cp_vm_status()); serial_print("\n");
    amd_gpu_log_save();

    serial_print("CP: about to run the IB + fence tests, the persistent ring and the draw / fill / blit proofs\n");
    amd_gpu_log_save();
    int ib_ok = amd_native_cp_ib_fence_test(g_mmio);
    serial_print("CP IB: "); serial_print(amd_native_cp_ib_status()); serial_print("\n");
    serial_print("CP IB VA: "); serial_print(amd_native_cp_ib_va_status()); serial_print("\n");
    serial_print("RING: "); serial_print(amd_native_ring_status()); serial_print("\n");
    serial_print("DRAW: "); serial_print(amd_native_draw_status()); serial_print("\n");
    serial_print("FILL: "); serial_print(amd_native_fill_status()); serial_print("\n");
    serial_print("BLIT: "); serial_print(amd_native_blit_status()); serial_print("\n");
    serial_print(ib_ok ? "CP: all IB / ring tests PASSED\n" : "CP: some IB / ring test did not pass\n");
    amd_gpu_log_save();
#endif
}
