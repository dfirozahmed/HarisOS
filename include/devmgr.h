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
} DevEntry;

/* What the drivers reported at boot (filled by kernel.c). */
typedef struct {
    int keyboard_ps2;
    int mouse_sources; /* MOUSE_SRC_* bits */
    int usb_running;   /* usb_init() succeeded */
} DevmgrInputs;

/* Rebuilds the list from pci_count()/pci_get() and the inputs. Returns the entry count. */
int devmgr_build(const DevmgrInputs *in);

int devmgr_count(void);
const DevEntry *devmgr_get(int index);
const char *devmgr_category_name(DevCategory c);

#endif
