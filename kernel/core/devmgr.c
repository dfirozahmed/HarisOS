/* Device Manager list: turns the PCI table and the input driver results into DevEntry rows. */
#include "blk.h"
#include "devmgr.h"
#include "mouse.h"
#include "pci.h"

static DevEntry g_list[DEVMGR_MAX];
static int g_count;

/* ---- tiny bounded string builder (no libc) ---- */
typedef struct {
    char *buf;
    int cap, len;
} Sb;

static void sb_char(Sb *s, char c)
{
    if (s->len < s->cap - 1)
        s->buf[s->len++] = c;
    s->buf[s->len] = 0;
}

static void sb_str(Sb *s, const char *t)
{
    while (*t)
        sb_char(s, *t++);
}

static void sb_dec(Sb *s, unsigned v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        sb_char(s, tmp[--n]);
}

/* "476.9 GB", "512 MB" ... from a byte count (1 GB = 10^9 bytes, like drive makers and Windows disk size) */
static void sb_size(Sb *s, unsigned long long bytes)
{
    if (bytes >= 1000000000ULL * 1000) {
        unsigned long long tenth = bytes / (100000000ULL * 1000);
        sb_dec(s, (unsigned)(tenth / 10));
        sb_char(s, '.');
        sb_dec(s, (unsigned)(tenth % 10));
        sb_str(s, " TB");
    } else if (bytes >= 1000000000ULL) {
        unsigned long long tenth = bytes / 100000000ULL;
        sb_dec(s, (unsigned)(tenth / 10));
        sb_char(s, '.');
        sb_dec(s, (unsigned)(tenth % 10));
        sb_str(s, " GB");
    } else {
        sb_dec(s, (unsigned)(bytes / 1000000ULL));
        sb_str(s, " MB");
    }
}

static void sb_hex(Sb *s, unsigned v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        sb_char(s, "0123456789ABCDEF"[(v >> (i * 4)) & 0xF]);
}

static void copy(char *dst, int cap, const char *src)
{
    Sb s = {dst, cap, 0};
    dst[0] = 0;
    sb_str(&s, src);
}

static DevEntry *add(DevCategory cat, const char *name, const char *detail, int driver)
{
    if (g_count >= DEVMGR_MAX)
        return (DevEntry *)0;
    DevEntry *e = &g_list[g_count++];
    e->category = cat;
    copy(e->name, sizeof e->name, name);
    copy(e->detail, sizeof e->detail, detail);
    e->has_driver = driver;
    e->note[0] = 0;
    return e;
}

static DevCategory pci_category(const PciDevice *d)
{
    switch (d->class_code) {
    case 0x01: return DEV_CAT_STORAGE;
    case 0x02: case 0x0D: return DEV_CAT_NETWORK;
    case 0x03: return DEV_CAT_DISPLAY;
    case 0x09: return DEV_CAT_INPUT;
    case 0x0C: return (d->subclass == 0x03) ? DEV_CAT_USB : DEV_CAT_SYSTEM;
    default: return DEV_CAT_SYSTEM;
    }
}

int devmgr_build(const DevmgrInputs *in)
{
    g_count = 0;

    for (int i = 0; i < pci_count(); i++) {
        const PciDevice *d = pci_get(i);
        char detail[48];
        Sb s = {detail, sizeof detail, 0};
        detail[0] = 0;
        sb_str(&s, pci_vendor_name(d->vendor_id));
        sb_char(&s, ' ');
        sb_hex(&s, d->vendor_id, 4);
        sb_char(&s, ':');
        sb_hex(&s, d->device_id, 4);
        sb_str(&s, " at ");
        sb_hex(&s, d->bus, 2);
        sb_char(&s, ':');
        sb_hex(&s, d->dev, 2);
        sb_char(&s, '.');
        sb_hex(&s, d->fn, 1);
        /* xHCI (class 0C03, prog-if 0x30), AHCI (0106, prog-if 01) and NVMe (0108, prog-if 02) have a HarisOS driver. */
        int driver = in && in->usb_running && d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30;
        if (in && in->storage_running && d->class_code == 0x01 &&
            ((d->subclass == 0x06 && d->prog_if == 0x01) || (d->subclass == 0x08 && d->prog_if == 0x02)))
            driver = 1;
        add(pci_category(d), pci_class_name(d->class_code, d->subclass), detail, driver);
    }

    if (in) {
        if (in->keyboard_ps2)
            add(DEV_CAT_INPUT, "PS/2 keyboard", "i8042 port 0x60", 1);
        if (in->mouse_sources & MOUSE_SRC_PS2)
            add(DEV_CAT_INPUT, "PS/2 mouse", "i8042 aux port", 1);
        for (int i = 0; i < in->usb_mice; i++)
        {
            DevEntry *e = add(DEV_CAT_INPUT, "USB mouse", "HID mouse (xHCI)", 1);
            if (e && i < DEVMGR_MAX_USB_MICE)
                copy(e->note, sizeof e->note,
                     !in->usb_mouse_rp[i]    ? "Boot protocol: 3-byte reports, no scroll wheel"
                     : in->usb_mouse_wheel[i] ? "Report protocol; scroll wheel: yes"
                                              : "Report protocol; scroll wheel: no");
        }
        for (int i = 0; i < in->disk_count && i < DEVMGR_MAX_DISKS; i++) {
            const DevmgrDisk *k = &in->disks[i];
            char name[40], detail[48], note[64];
            Sb sn = {name, sizeof name, 0}, sd = {detail, sizeof detail, 0}, sf = {note, sizeof note, 0};
            name[0] = detail[0] = note[0] = 0;
            sb_str(&sn, blk_kind_name(k->kind));
            sb_char(&sn, ' ');
            sb_str(&sn, k->name);
            sb_str(&sd, k->model[0] ? k->model : "(no model)");
            sb_str(&sd, ", ");
            sb_size(&sd, k->bytes);
            sb_dec(&sf, (unsigned)k->parts);
            sb_str(&sf, k->parts == 1 ? " partition; " : " partitions; ");
            sb_str(&sf, k->locked ? "writing is locked" : "writing is unlocked");
            DevEntry *e = add(DEV_CAT_STORAGE, name, detail, 1);
            if (e)
                copy(e->note, sizeof e->note, note);
        }
        for (int i = 0; i < in->usb_keyboards; i++)
            add(DEV_CAT_INPUT, "USB keyboard", "HID boot protocol (xHCI)", 1);
        /* I2C-HID devices from the ACPI tables; a started one is the touchpad in use */
        int n = in->i2c_count < DEVMGR_MAX_I2C ? in->i2c_count : DEVMGR_MAX_I2C;
        for (int i = 0; i < n; i++) {
            const DevmgrI2c *c = &in->i2c[i];
            char detail[48];
            Sb s = {detail, sizeof detail, 0};
            detail[0] = 0;
            sb_str(&s, "I2C 0x");
            sb_hex(&s, c->addr, 2);
            sb_str(&s, " on ");
            sb_str(&s, c->ctrl[0] ? c->ctrl : "?");
            sb_str(&s, ", ");
            sb_dec(&s, c->speed / 1000);
            sb_str(&s, " kHz");
            DevEntry *e = add(DEV_CAT_INPUT, c->started ? "I2C-HID touchpad" : "I2C-HID device", detail, c->started);
            if (e && !c->started && c->reason)
                copy(e->note, sizeof e->note, c->reason);
            else if (e && c->started && !c->has_wheel && c->ptp_fingers > 0)
            {
                /* e.g. "No wheel; precision pad: 5 contacts, mode switch: yes" */
                char note[64];
                Sb n = {note, sizeof note, 0};
                note[0] = 0;
                sb_str(&n, "No wheel; precision pad: ");
                sb_dec(&n, (unsigned)c->ptp_fingers);
                sb_str(&n, " contacts, mode switch: ");
                sb_str(&n, c->ptp_mode ? "yes" : "no");
                copy(e->note, sizeof e->note, note);
            }
            else if (e && c->started)
                copy(e->note, sizeof e->note,
                     c->has_wheel ? "Mouse mode; the report has a scroll wheel"
                                  : "Mouse mode; no scroll wheel in its report");
        }
    }
    return g_count;
}

int devmgr_count(void)
{
    return g_count;
}

const DevEntry *devmgr_get(int index)
{
    return (index >= 0 && index < g_count) ? &g_list[index] : (const DevEntry *)0;
}

const char *devmgr_category_name(DevCategory c)
{
    switch (c) {
    case DEV_CAT_INPUT: return "Input devices";
    case DEV_CAT_STORAGE: return "Storage controllers";
    case DEV_CAT_NETWORK: return "Network adapters";
    case DEV_CAT_DISPLAY: return "Display adapters";
    case DEV_CAT_USB: return "USB controllers";
    case DEV_CAT_SYSTEM: return "System devices";
    default: return "Other";
    }
}
