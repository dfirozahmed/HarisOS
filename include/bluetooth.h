/* ============================================================
 * Bluetooth: the HCI layer (Host Controller Interface) that sits on top of a transport.
 *
 * A Bluetooth chip in a PC is a small computer of its own. The host (us) talks to it with HCI packets:
 *   command  (host -> chip)   opcode (2 bytes: OGF << 10 | OCF), parameter length (1 byte), parameters
 *   event    (chip -> host)   event code (1 byte), parameter length (1 byte), parameters
 *   ACL data (both ways)      connection handle, length, payload   (not used yet)
 * How the packets travel depends on the transport; on a laptop it is USB (kernel/drivers/input/usb_xhci/usb_xhci_bt.c: commands
 * go as control transfers, events arrive on an interrupt endpoint, ACL data on two bulk endpoints). This file knows nothing about
 * USB, so the same code can run against a fake chip in the host tests.
 *
 *   stage 1 (this file): reset the chip, read its version, ROM version (Realtek) and Bluetooth address, report it.
 *   later: Realtek firmware download, inquiry (device discovery), L2CAP, SDP, AVDTP / A2DP.
 *
 * Polled like the rest of the kernel: no interrupts. All of it runs in the caller's context.
 * ============================================================ */
#ifndef HARIS_BLUETOOTH_H
#define HARIS_BLUETOOTH_H

/* What the transport must provide. */
typedef struct
{
    /* Send one HCI command packet (opcode, length, parameters; no USB framing byte). 0 = sent, <0 = failed. */
    int (*send_cmd)(void *ctx, const unsigned char *pkt, unsigned len);
    /* Wait about 100 microseconds and collect whatever the chip reported meanwhile (the transport then calls bt_rx_event()). */
    void (*wait_step)(void *ctx);
    /* 0 once the chip is unplugged or broken. */
    int (*alive)(void *ctx);
    void *ctx;
    /* Milliseconds since boot (for time-outs of scans); may be 0 for a transport without a clock: then nothing times out. */
    unsigned long long (*now_ms)(void *ctx);
} BtTransport;

typedef enum
{
    BT_ABSENT = 0, /* no Bluetooth chip */
    BT_INIT,       /* attached, the start-up commands are running */
    BT_HCI_UP,     /* reset and version read worked: the chip answers HCI */
    BT_FAILED      /* the chip does not answer as expected (see why) */
} BtState;

/* Firmware (Realtek chips load a patch from a file after power-up; see bt_rtl.h) */
typedef enum
{
    BT_FW_NONE = 0,   /* not looked at yet */
    BT_FW_NOT_NEEDED, /* the chip is not one that needs a patch, or one is already running */
    BT_FW_MISSING,    /* the files are not on any drive */
    BT_FW_BAD,        /* a file is wrong for this chip (fw_why) */
    BT_FW_LOADED,     /* patch sent, the chip restarted and answers */
    BT_FW_FAILED      /* sending the patch failed (fw_code = HCI status or BT_E_*, fw_done pieces went through) */
} BtFwState;

typedef struct
{
    BtState state;
    unsigned short vid, pid;  /* USB ids of the chip (0 when the transport has none) */
    unsigned char hci_ver;    /* HCI version: 6 = 4.0, 8 = 5.0, ... */
    unsigned short hci_rev;   /* HCI revision: chip / firmware specific */
    unsigned char lmp_ver;    /* link manager version */
    unsigned short manufacturer; /* Bluetooth SIG company id: 93 = Realtek */
    unsigned short lmp_subver;   /* chip specific; Realtek: names the chip (0x8821 = RTL8821C) */
    int have_rom;             /* Realtek only: vendor command "read ROM version" worked */
    unsigned char rom_version;
    unsigned char bdaddr[6];  /* the chip's Bluetooth address, most significant byte first */
    int have_addr;
    const char *why;          /* BT_FAILED: which step failed (static text) */
    int why_code;             /* ... and its HCI status, or a BT_E_* value */
    unsigned events;          /* events received since attach */
    unsigned dropped;         /* ... of which dropped (queue full) */
    unsigned hw_errors;       /* Hardware Error events */
    BtFwState fw_state;
    const char *fw_why;       /* BT_FW_BAD: what is wrong (static text) */
    int fw_code;
    unsigned fw_version;      /* version number in the firmware file header */
    unsigned fw_done, fw_total; /* pieces sent / to send */
} BtInfo;

/* bt_cmd() results below zero */
#define BT_E_TRANSPORT (-1)
#define BT_E_TIMEOUT (-2)
#define BT_E_SHORT (-3) /* the answer is too short to hold what was asked for */
#define BT_E_BUSY (-4)  /* no chip attached */

/* ---- transport side ---- */
void bt_attach(const BtTransport *t, unsigned short vid, unsigned short pid); /* runs the start-up commands (blocks for a while) */
void bt_detach(void);                                                       /* chip unplugged */
void bt_rx_event(const unsigned char *evt, unsigned len);                   /* one complete HCI event packet */
void bt_fw_report(BtFwState state, const char *why, int code, unsigned fw_version, unsigned done, unsigned total); /* firmware outcome, for the status line */
void bt_restart(void); /* Reset + read the versions and the address again (after a firmware patch changed the chip) */

/* Kernel only (kernel/bluetooth/bt_fw.c): once the drives are mounted, loads the firmware files BTFW.BIN and BTCFG.BIN (copies of
 * linux-firmware rtl_bt/rtl8821c_fw.bin and rtl8821c_config.bin, see CMakeLists.txt) into a Realtek chip that needs them. */
void bt_firmware_boot(void);

/* ---- users ---- */
const BtInfo *bt_info(void);
int bt_hci_up(void);
/* One line (< 96 characters) for the Device Manager: chip, versions, address, or why it did not start. */
void bt_status(char *buf, unsigned cap);

/* Send one HCI command and wait for its Command Complete / Command Status event. `ret` (if not 0) receives the return parameters
 * (the first byte is the HCI status). Returns the HCI status (0 = success), or a BT_E_* value. */
int bt_cmd(unsigned opcode, const void *params, unsigned plen, unsigned char *ret, unsigned ret_max, unsigned *ret_len, unsigned timeout_ms);

/* ---- device discovery (kernel/bluetooth/bt_scan.c) ----
 * bt_scan_start() sends an Inquiry (about 10 s, the chip listens for devices that are in "discoverable" mode), the answers are collected
 * from the events, then the names of the devices that did not tell theirs are asked for one by one. All of it is stepped by bt_poll(),
 * which the main loop calls: nothing blocks. Needs bt_hci_up(). */
#define BT_SCAN_MAX 16
#define BT_NAME_MAX 40

typedef enum
{
    BT_SCAN_IDLE = 0, /* nothing scanned yet */
    BT_SCAN_INQUIRY,  /* listening for devices */
    BT_SCAN_NAMES,    /* asking the devices without a name for it */
    BT_SCAN_DONE
} BtScanState;

typedef struct
{
    unsigned char addr[6]; /* as the chip reports it: least significant byte first */
    unsigned char cls[3];  /* class of device, least significant byte first */
    int has_rssi;
    signed char rssi;      /* dBm; closer to 0 = nearer */
    unsigned char name_state; /* 0 unknown, 1 have it, 2 asked for, 3 the device did not answer */
    unsigned char psrm;    /* page scan repetition mode, needed for the name request */
    unsigned short clock;  /* clock offset */
    char name[BT_NAME_MAX];
} BtScanDev;

#define BT_E_NOT_UP (-100)  /* bt_scan_start: no working chip */
#define BT_E_SCANNING (-101) /* ... a scan is already running */

/* The chip shares its antenna with the Wi-Fi card. The Wi-Fi driver owns the switch, so the kernel hands us a function that asks it:
 * mode 0 = Bluetooth gets no radio, 1 = it may always use it (Wi-Fi on 5 GHz), 2 = hardware arbitration. Without a hook nothing happens. */
void bt_set_coex_hook(void (*hook)(int mode));
void bt_coex(int mode); /* pass a mode on to the hook */

int bt_scan_start(void);
void bt_poll(void); /* call often (every pass of the main loop): handles events, steps the scan */
BtScanState bt_scan_state(void);
int bt_scan_count(void);
const BtScanDev *bt_scan_dev(int i);
/* The scan as lines of text (like net_wifi_scan_line): line 0 is a summary that starts with "idle", "scanning", "names" or "done",
 * lines 1 .. n one device each. Returns the length of the line, or -1 if there is no such line or it does not fit in `cap`. */
int bt_scan_line(unsigned index, char *buf, unsigned cap);

/* ---- between bt_hci.c and bt_scan.c ---- */
unsigned long long bt_now(void);
void bt_scan_event(const unsigned char *evt, unsigned len); /* an event nobody waits for in bt_cmd */
void bt_scan_step(void);
void bt_scan_reset(void); /* the chip was reset or unplugged: forget the setup and the list */

#endif
