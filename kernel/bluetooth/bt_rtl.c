/* Realtek Bluetooth: firmware file parsing and download (stage 2). See include/bt_rtl.h. */
#include "bt_rtl.h"

#define OP_RTL_DOWNLOAD 0xFC20
#define HDR_LEN 14 /* signature (8) + fw_version (4) + num_patches (2) */
#define EXT_SIG_LEN 4

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p) { return le16(p) | (le16(p + 2) << 16); }

int bt_rtl_needs_firmware(const BtInfo *i) { return i->lmp_subver == 0x8821 && i->hci_rev == 0x000C && i->hci_ver == 8; }

unsigned bt_rtl_build(const unsigned char *fw, unsigned fw_len, const unsigned char *cfg, unsigned cfg_len, unsigned lmp_subver,
                      unsigned rom_version, unsigned char *out, unsigned out_cap, unsigned *fw_version, const char **why)
{
    static const unsigned char sig[8] = {'R', 'e', 'a', 'l', 't', 'e', 'c', 'h'};
    static const unsigned char ext_sig[EXT_SIG_LEN] = {0x51, 0x04, 0xfd, 0x77};
    *why = "";
    if (fw_len < 8)
    {
        *why = "short";
        return 0;
    }
    for (int i = 0; i < 8; i++)
        if (fw[i] != sig[i])
        {
            *why = "signature"; /* "RTBTCore" (version 2 files, newer chips) is not supported */
            return 0;
        }
    if (fw_len < HDR_LEN + EXT_SIG_LEN + 3)
    {
        *why = "short";
        return 0;
    }
    const unsigned char *end = fw + fw_len - EXT_SIG_LEN;
    for (int i = 0; i < EXT_SIG_LEN; i++)
        if (end[i] != ext_sig[i])
        {
            *why = "ext sig";
            return 0;
        }

    /* the extension entries, read backwards from the signature; we want the project id (opcode 0, length 1) */
    int project = -1;
    const unsigned char *p = end;
    while (p >= fw + HDR_LEN + 3)
    {
        unsigned op = *--p;
        unsigned len = *--p;
        unsigned data = *--p;
        if (op == 0xFF)
            break;
        if (len == 0)
        {
            *why = "ext len";
            return 0;
        }
        if (op == 0 && len == 1)
        {
            project = (int)data;
            break;
        }
        if ((unsigned)(p - fw) < len)
            break;
        p -= len;
    }
    if (project < 0)
    {
        *why = "no project";
        return 0;
    }
    /* project ids that belong to lmp_subver 0x8821 in btrtl.c: 2 (RTL8821A) and 10 (RTL8821C) */
    if (!(lmp_subver == 0x8821 && (project == 2 || project == 10)))
    {
        *why = "wrong chip";
        return 0;
    }

    unsigned ver = le32(fw + 8), num = le16(fw + 12);
    if (num == 0 || fw_len < HDR_LEN + EXT_SIG_LEN + 3 + 8 * num)
    {
        *why = "patch table";
        return 0;
    }
    const unsigned char *ids = fw + HDR_LEN, *lens = ids + 2 * num, *offs = lens + 2 * num;
    unsigned plen = 0, poff = 0;
    for (unsigned i = 0; i < num; i++)
        if (le16(ids + 2 * i) == rom_version + 1)
        {
            plen = le16(lens + 2 * i);
            poff = le32(offs + 4 * i);
            break;
        }
    if (poff == 0)
    {
        *why = "no patch";
        return 0;
    }
    if (plen < 4 || poff > fw_len || plen > fw_len - poff)
    {
        *why = "patch range";
        return 0;
    }
    if (cfg_len > BT_RTL_CFG_MAX)
    {
        *why = "config size";
        return 0;
    }
    if (plen + cfg_len > out_cap)
    {
        *why = "too big";
        return 0;
    }
    for (unsigned i = 0; i < plen - 4; i++)
        out[i] = fw[poff + i];
    for (int i = 0; i < 4; i++)
        out[plen - 4 + i] = fw[8 + i]; /* the patch ends with the file's version number */
    for (unsigned i = 0; i < cfg_len; i++)
        out[plen + i] = cfg[i];
    *fw_version = ver;
    return plen + cfg_len;
}

int bt_rtl_download(const unsigned char *data, unsigned len, unsigned *done, unsigned *total)
{
    /* Same arithmetic as btrtl.c rtl_download_firmware(): len / 252 + 1 pieces, the last one carries the rest (possibly 0 bytes).
     * The index byte counts 0, 1, ... 0x7f, then starts again at 1; the last piece has bit 7 set ("data end"). */
    unsigned frags = len / BT_RTL_FRAG_LEN + 1;
    *done = 0;
    *total = frags;
    unsigned j = 0;
    unsigned char par[1 + BT_RTL_FRAG_LEN];
    unsigned char ret[4];
    for (unsigned i = 0; i < frags; i++)
    {
        unsigned index = j++;
        if (index == 0x7F)
            j = 1;
        unsigned flen = BT_RTL_FRAG_LEN;
        if (i == frags - 1)
        {
            index |= 0x80;
            flen = len % BT_RTL_FRAG_LEN;
        }
        par[0] = (unsigned char)index;
        for (unsigned k = 0; k < flen; k++)
            par[1 + k] = data[(unsigned long)i * BT_RTL_FRAG_LEN + k];
        unsigned n = 0;
        int st = bt_cmd(OP_RTL_DOWNLOAD, par, 1 + flen, ret, sizeof ret, &n, 1000);
        if (st != 0)
            return st;
        if (n != 2) /* answer: status, index */
            return BT_E_SHORT;
        (*done)++;
    }
    return 0;
}
