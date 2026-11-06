/* Bluetooth: loads the Realtek firmware files from a drive into the chip (stage 2). Kernel only: it reads files through the VFS.
 * The files are not part of the repository (Realtek's licence): firmware/rtl8821c_fw.bin and firmware/rtl8821c_config.bin from
 * linux-firmware (rtl_bt/) go onto the boot disk as BTFW.BIN and BTCFG.BIN (a FAT root folder here only has 8.3 names, see
 * CMakeLists.txt). */
#include "bluetooth.h"
#include "bt_rtl.h"
#include "exec.h"
#include "vfs.h"

#define FW_NAME "BTFW.BIN"
#define CFG_NAME "BTCFG.BIN"
#define FILE_MAX (96u * 1024) /* the file is 54 KB today */

static unsigned char FILEBUF[FILE_MAX];
static unsigned char CFGBUF[BT_RTL_CFG_MAX];
static unsigned char PATCH[FILE_MAX]; /* the patch for this chip (35 KB) + the config */

/* Reads the whole file `name` into buf. Returns its size, 0 if there is no such file, -1 if it cannot be used. */
static int read_file(const char *name, unsigned char *buf, unsigned cap)
{
    int node = exec_find(name);
    if (node < 0)
        return 0;
    const VfsNode *n = vfs_node(node);
    if (!n || n->is_dir)
        return 0;
    if (n->size == 0 || n->size > cap)
        return -1;
    if (vfs_read(node, 0, buf, n->size) != (int)n->size)
        return -1;
    return (int)n->size;
}

void bt_firmware_boot(void)
{
    const BtInfo *i = bt_info();
    if (!bt_hci_up() || !i->have_rom)
        return;
    if (!bt_rtl_needs_firmware(i))
    {
        bt_fw_report(BT_FW_NOT_NEEDED, 0, 0, 0, 0, 0);
        return;
    }

    int fl = read_file(FW_NAME, FILEBUF, sizeof FILEBUF);
    if (fl == 0)
    {
        bt_fw_report(BT_FW_MISSING, 0, 0, 0, 0, 0);
        return;
    }
    int cl = read_file(CFG_NAME, CFGBUF, sizeof CFGBUF); /* the config is optional: without it the chip keeps its defaults */
    if (fl < 0 || cl < 0)
    {
        bt_fw_report(BT_FW_BAD, "file read", 0, 0, 0, 0);
        return;
    }

    const char *why = "";
    unsigned ver = 0;
    unsigned len = bt_rtl_build(FILEBUF, (unsigned)fl, CFGBUF, (unsigned)cl, i->lmp_subver, i->rom_version, PATCH, sizeof PATCH, &ver, &why);
    if (!len)
    {
        bt_fw_report(BT_FW_BAD, why, 0, 0, 0, 0);
        return;
    }

    unsigned done = 0, total = 0;
    int st = bt_rtl_download(PATCH, len, &done, &total);
    if (st != 0)
    {
        bt_fw_report(BT_FW_FAILED, 0, st, ver, done, total);
        return;
    }

    /* The chip now runs the patch. Linux resets it and asks for the version again; so do we (hci_rev / lmp_subver change). */
    bt_restart();
    if (bt_hci_up())
        bt_fw_report(BT_FW_LOADED, 0, 0, ver, done, total);
    else
        bt_fw_report(BT_FW_FAILED, 0, bt_info()->why_code, ver, done, total); /* it does not answer after the patch */
}
