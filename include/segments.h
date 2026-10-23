/* ============================================================
 * HarisOS segment selectors (the GDT layout, see gdt.h)
 *
 *   index  selector  what
 *     0     0x00     null
 *     1     0x08     kernel code, 64-bit, ring 0
 *     2     0x10     kernel data, ring 0
 *     3     0x18     user data, ring 3          (0x1B with RPL 3)
 *     4     0x20     user code, 64-bit, ring 3  (0x23 with RPL 3)
 *    5-6    0x28     TSS (a 16-byte descriptor)
 *
 * The order of the four middle entries is not free: SYSCALL / SYSRET (a later step) compute their selectors from one
 * base value in the STAR register: SYSCALL loads CS = base, SS = base + 8 (kernel code, data), SYSRET to 64-bit
 * code loads SS = base' + 8, CS = base' + 16 (user data, user code), with base' = 0x10 here.
 * ============================================================ */
#ifndef HARIS_SEGMENTS_H
#define HARIS_SEGMENTS_H

#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_DATA 0x1B /* index 3, requested privilege level 3 */
#define SEL_USER_CODE 0x23 /* index 4, requested privilege level 3 */
#define SEL_TSS 0x28

/* The value for STAR[63:48]. SYSRET loads SS = base + 8 and CS = base + 16 exactly as computed (the RPL bits are NOT forced
 * to 3 by a real CPU), so the base has to carry RPL 3 itself: 0x13 gives 0x1B and 0x23. */
#define SEL_SYSRET_BASE (SEL_USER_DATA - 8)

#define GDT_ENTRIES 7 /* null, 4 segments, and the two words of the TSS descriptor */

#endif
