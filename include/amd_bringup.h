/* AMD Radeon (Renoir) bring-up: finds the GPU on PCI and runs the driver's checks.
 * By default it only READS: PCI config, the GC / MMHUB / GFXHUB registers and the VM fault registers. The register
 * writing steps (GART / VMID0) are compiled in only with -DHARIS_AMD_GPU_GART=1 (cmake -DHARIS_AMD_GPU_GART=ON). */
#ifndef HARIS_AMD_BRINGUP_H
#define HARIS_AMD_BRINGUP_H

/* Needs pci_scan() and heap_init() first. Returns 1 if an AMD GPU with a register BAR was found and probed. */
int amd_gpu_bringup(void);

/* One status of the last stage that ran, valid after amd_gpu_bringup(). */
const char *amd_gpu_bringup_status(void);

/* Saves everything the driver printed to GPULOG.TXT on the boot drive (unlocks it for that one file). 1 = written.
 * Call after volume_mount_all(). */
int amd_gpu_log_save(void);

/* The GART / VMID0 step: does something only when built with HARIS_AMD_GPU_GART. Saves the log before it runs. */
void amd_gpu_bringup_gart(void);

/* One short line for the Device Manager (GC state, VRAM, VM hub, firmware); "" if there is no AMD GPU. */
void amd_gpu_bringup_summary(char *out, int n);

#endif
