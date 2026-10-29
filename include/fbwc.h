/* ============================================================
 * HarisOS frame buffer caching: make video memory "write-combining".
 *
 *   fbwc_enable(phys, bytes);   once, after pmm_init(): mark the frame buffer write-combining
 *   fbwc_status();              what happened (FBWC_*)
 *   fbwc_mtrr_type();           memory type the MTRRs give the frame buffer, -1 unknown
 *
 * Why: by default the CPU treats video memory as uncached, so every store goes to the graphics card on its
 * own, which is very slow. Write-combining collects stores in a CPU buffer and sends them in bursts.
 * It is set per page in the page tables with the PAT (page attribute table) feature, so the firmware's big
 * pages around the frame buffer are first split into 4 KiB pages. If the MTRRs already say write-combining,
 * nothing is changed. If a range of them says uncached, nothing can be done (uncached always wins). If no
 * range covers the frame buffer and the default is uncached, a write-combining MTRR is added instead.
 * ============================================================ */
#ifndef HARIS_FBWC_H
#define HARIS_FBWC_H

#define FBWC_APPLIED 0       /* page tables changed: the frame buffer is write-combining now */
#define FBWC_ALREADY 1       /* the MTRRs already make it write-combining */
#define FBWC_ADDED 2         /* no MTRR covered it (uncached by default): a write-combining MTRR was added */
#define FBWC_MTRR_FAIL -5    /* that MTRR could not be added (overlap with a cached range, or no free MTRR) */
#define FBWC_NO_PAT -1      /* CPU has no PAT */
#define FBWC_NO_PAGING -2    /* 5-level paging or unusable page tables */
#define FBWC_MTRR_UC -3      /* MTRRs make it uncached: PAT cannot change that */
#define FBWC_NO_MEMORY -4    /* could not allocate or walk the page tables */
#define FBWC_NOT_RUN -9

int fbwc_enable(unsigned long long phys, unsigned long long bytes);
/* The same for another range later in the boot (say all of a graphics card's memory window), before the other
 * processors start; returns FBWC_*, fbwc_status() / fbwc_mtrr_type() keep the frame buffer's answer. */
int fbwc_enable_more(unsigned long long phys, unsigned long long bytes);
int fbwc_status(void);
int fbwc_mtrr_type(void);

#endif
