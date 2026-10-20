/* HarisOS storage: finds the disks (NVMe SSDs, SATA SSDs / hard disks) and their partitions.
 * See blk.h for reading and writing sectors. */
#ifndef HARIS_STORAGE_H
#define HARIS_STORAGE_H

/* Once, after pci_scan() and pmm_init(): starts every AHCI and NVMe controller, registers the disks as
 * block devices and reads their partition tables. Returns the number of disks found (0 is fine). */
int storage_init(void);

/* 1 when a storage controller driver ran (even if it found no disk), for the Device Manager. */
int storage_controllers_started(void);

#endif
