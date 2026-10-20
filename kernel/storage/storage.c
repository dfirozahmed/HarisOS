/* Storage start-up: drivers, then partition tables. See include/storage.h. */
#include "blk.h"
#include "storage.h"
#include "storage_internal.h"

static int STARTED;

int storage_init(void)
{
    STARTED = 1;
    int disks = ahci_init() + nvme_init();
    int n = blk_count(); /* the partitions that part_scan() adds come after the disks */
    for (int i = 0; i < n; i++)
        part_scan(i);
    return disks;
}

int storage_controllers_started(void) { return STARTED; }
