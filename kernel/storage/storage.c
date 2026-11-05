/* Storage start-up: drivers, then partition tables. See include/storage.h. */
#include "blk.h"
#include "serial.h"
#include "storage.h"
#include "storage_internal.h"

static int STARTED;

int storage_init(void)
{
    STARTED = 1;
    serial_puts("DBG storage_init\n"); /*TMPDBG*/
    int first = blk_count(); /* USB drives found by the xHCI driver came earlier, with their partitions (usb_msc.c) */
    int disks = ahci_init() + nvme_init() + virtio_blk_init();
    for (int i = first; i < blk_count(); i++) /* one boot log line per disk: which driver found it, how big */
    {
        const BlkDev *b = blk_get(i);
        if (b && b->parent < 0 && b->removed == 0)
        {
            serial_puts("HARISOS: storage: ");
            serial_puts(b->name);
            serial_puts(" ");
            serial_puts(b->model);
            serial_puts(", ");
            serial_putd((long long)(b->sectors * b->sector_size >> 20));
            serial_puts(" MiB\n");
        }
    }
    { char t[513]; for (int i = first; i < blk_count(); i++) { if (blk_read(i, 0, 1, t) == 0) { t[16] = 0; serial_puts("PROBE "); serial_puts(t); serial_puts("\n"); } } } /*TMPPROBE*/
    int n = blk_count(); /* the partitions that part_scan() adds come after the disks */
    for (int i = first; i < n; i++)
        part_scan(i);
    return disks;
}

int storage_controllers_started(void) { return STARTED; }
