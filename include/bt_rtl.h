/* Realtek Bluetooth chips (RTL8821C and relatives): their ROM only does a little; the rest comes as a "patch" the host sends after
 * power-up. Linux does the same (drivers/bluetooth/btrtl.c); the file layout and the download protocol below are checked against
 * that source and against the real linux-firmware files rtl_bt/rtl8821c_fw.bin and rtl8821c_config.bin.
 *
 * Firmware file ("epatch", signature "Realtech"):
 *   0   signature "Realtech" (8)
 *   8   fw_version (u32 LE)
 *   12  num_patches (u16 LE)
 *   14  chip_id[num_patches] (u16 LE each), patch_length[num_patches] (u16 LE), patch_offset[num_patches] (u32 LE)
 *   ... the patches (one per ROM version: chip_id == ROM version + 1) ...
 *   end an "extension" section read backwards from the signature 51 04 fd 77: entries (data, length, opcode) one byte each
 *       before it; opcode 0 / length 1 = project id (10 = RTL8821C), opcode 0xff = end.
 * The patch for this chip gets its last 4 bytes replaced by fw_version; the config file (a few bytes of settings) is appended
 * unchanged; the whole is sent in pieces of 252 bytes with vendor command 0xFC20. */
#ifndef HARIS_BT_RTL_H
#define HARIS_BT_RTL_H

#include "bluetooth.h"

#define BT_RTL_FRAG_LEN 252
#define BT_RTL_CFG_MAX 1024

/* 1 when the chip looks like an RTL8821C with its ROM firmware only (lmp_subver 0x8821, hci_rev 0x000c, hci_ver 8, as in btrtl.c's
 * table; after a patch is running, hci_rev and lmp_subver have other values). */
int bt_rtl_needs_firmware(const BtInfo *i);

/* Picks the patch for `rom_version` out of the firmware file and appends the config file. Returns the number of bytes written to
 * `out` (> 0), or 0 with *why set to a short static text. *fw_version = the version from the file header. */
unsigned bt_rtl_build(const unsigned char *fw, unsigned fw_len, const unsigned char *cfg, unsigned cfg_len, unsigned lmp_subver,
                      unsigned rom_version, unsigned char *out, unsigned out_cap, unsigned *fw_version, const char **why);

/* Sends `data` with vendor command 0xFC20 in pieces. 0 = all accepted; otherwise the HCI status or BT_E_* of the piece that failed.
 * *done = pieces accepted, *total = pieces to send. */
int bt_rtl_download(const unsigned char *data, unsigned len, unsigned *done, unsigned *total);

#endif
