/* Device Manager data: one flat list of everything the kernel knows about
 * (PCI functions + input devices), each with a category and a driver status.
 * No drawing here; the window in the desktop reads this list. */
#ifndef HARIS_DEVMGR_H
#define HARIS_DEVMGR_H

#define DEVMGR_MAX 96

typedef enum {
    DEV_CAT_INPUT,
    DEV_CAT_STORAGE,
    DEV_CAT_NETWORK,
    DEV_CAT_DISPLAY,
    DEV_CAT_USB,
    DEV_CAT_SYSTEM, /* bridges, audio, everything else */
    DEV_CAT_COUNT
} DevCategory;

typedef struct {
    DevCategory category;
    char name[40];   /* e.g. "USB controller" */
    char detail[48]; /* e.g. "Intel 8086:1237 at 00:1F.2" */
    int has_driver;  /* 1 = HarisOS drives this device */
    char note[64];   /* extra line for the details: why there is no driver, or a remark about the driver ("" = none) */
} DevEntry;

/* An I2C-HID device the firmware describes in ACPI (copied from i2c_hid_info()). */
typedef struct {
    unsigned addr;  /* 7-bit I2C address */
    unsigned speed; /* Hz */
    char ctrl[5];   /* ACPI name of the I2C controller */
    int started;    /* the driver is using it */
    int has_wheel;  /* started device: its mouse report has a scroll wheel field */
    int ptp_fingers; /* started device: contacts per report if a precision touchpad, else 0 */
    int ptp_mode;    /* ... with a "Device Mode" feature (switch to precision mode) */
    const char *reason; /* why it did not start (static text; NULL or "" = unknown) */
} DevmgrI2c;

#define DEVMGR_MAX_I2C 8
#define DEVMGR_MAX_USB_MICE 4

/* What the drivers reported at boot (filled by kernel.c). */
typedef struct {
    int keyboard_ps2;
    int mouse_sources; /* MOUSE_SRC_PS2 bit (USB comes from the counts below, I2C from i2c[]) */
    int usb_running;   /* usb_init() succeeded */
    int usb_mice;      /* USB mice attached right now */
    int usb_mouse_rp[DEVMGR_MAX_USB_MICE];    /* per mouse: 1 = report protocol, 0 = boot protocol */
    int usb_mouse_wheel[DEVMGR_MAX_USB_MICE]; /* per mouse: its report has a scroll wheel */
    int usb_keyboards; /* USB keyboards attached right now */
    int i2c_count;
    DevmgrI2c i2c[DEVMGR_MAX_I2C];
} DevmgrInputs;

/* Rebuilds the list from pci_count()/pci_get() and the inputs. Returns the entry count. */
int devmgr_build(const DevmgrInputs *in);

/* System glue (kernel/core/devmgr_sys.c): asks the drivers and PCI, then devmgr_build().
 * devmgr_init() once after the input drivers started (keyboard_ok = keyboard_init() == 0);
 * devmgr_refresh() any time later (the Device Manager's Refresh button). Returns the entry count. */
int devmgr_init(int keyboard_ok);
int devmgr_refresh(void);

int devmgr_count(void);
const DevEntry *devmgr_get(int index);
const char *devmgr_category_name(DevCategory c);

#endif
