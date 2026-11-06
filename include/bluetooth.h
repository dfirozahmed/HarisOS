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
    /* Send one HCI ACL data packet (handle + flags, length, data; no USB framing byte). 0 = sent, <0 = failed. */
    int (*send_acl)(void *ctx, const unsigned char *pkt, unsigned len);
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
    unsigned short acl_mtu;   /* largest ACL data packet the chip takes (Read Buffer Size) */
    unsigned short acl_pkts;  /* ACL packets the chip can hold at a time (flow control credits) */
    unsigned acl_rx, acl_tx, acl_dropped; /* ACL packets received / sent / dropped (queue full) */
} BtInfo;

/* bt_cmd() results below zero */
#define BT_E_TRANSPORT (-1)
#define BT_E_TIMEOUT (-2)
#define BT_E_SHORT (-3) /* the answer is too short to hold what was asked for */
#define BT_E_BUSY (-4)  /* no chip attached */
#define BT_E_PARTIAL (-5) /* bt_acl_send: the chip stopped taking packets after some pieces of the PDU went out */

/* ---- transport side ---- */
void bt_attach(const BtTransport *t, unsigned short vid, unsigned short pid); /* runs the start-up commands (blocks for a while) */
void bt_detach(void);                                                       /* chip unplugged */
void bt_rx_event(const unsigned char *evt, unsigned len);                   /* one complete HCI event packet */
void bt_rx_event_chunk(const unsigned char *data, unsigned len);            /* a piece of the event stream (one USB packet): put together by the length byte */
void bt_rx_acl(const unsigned char *pkt, unsigned len);                     /* one complete HCI ACL data packet */
void bt_rx_acl_chunk(const unsigned char *data, unsigned len);              /* a piece of the ACL stream (one USB packet): put together by the length field */
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

/* ---- ACL links and L2CAP (kernel/bluetooth/bt_link.c) ----
 * One link to one device at a time. bt_link_connect() pages the device (Create Connection); when it answers, the link is up and a short
 * probe runs on the L2CAP signalling channel: an Echo Request (is data flowing both ways, and how fast) and two Information Requests (what
 * the device supports). Requests of the device are answered politely. The link is stepped by bt_poll(); nothing blocks.
 * The device's requests for a link key or a PIN are refused: pairing comes later. */
typedef enum
{
    BT_LINK_NONE = 0, /* no link (never was one, or it ended: see the last line of text) */
    BT_LINK_CONNECTING,
    BT_LINK_UP,
    BT_LINK_CLOSING,
    BT_LINK_FAILED    /* the connection could not be made (status in the text) */
} BtLinkState;

#define BT_E_LINK_BUSY (-102) /* bt_link_connect: a link exists or is being made */

int bt_link_connect(const unsigned char addr[6]); /* addr most significant byte first, as people write it. 0 = started */
int bt_link_disconnect(void);                     /* 0 = started (or nothing to do) */
BtLinkState bt_link_state(void);
/* The link as text lines: line 0 the state (starts with "idle", "connecting", "connected", "closing", "failed" or "closed"), then
 * "echo:", "info:" and "acl:" lines. Returns the length, or -1 (no such line, does not fit). */
int bt_link_line(unsigned index, char *buf, unsigned cap);

/* Send an L2CAP PDU (length, channel id, payload: already framed) to a connection handle. Cut into ACL packets of the chip's size,
 * waits (a little) for room in the chip's buffers. Not to be called from inside an event or ACL handler. 0 or a BT_E_* value. */
int bt_acl_send(unsigned handle, const unsigned char *pdu, unsigned len);
void bt_acl_reset_credits(void); /* the link ended: whatever was in the chip's buffers is gone */
unsigned bt_acl_credits(void);   /* ACL packets we may send right now */

/* ---- service discovery (SDP): kernel/bluetooth/bt_sdp.c reads the answers, bt_link.c asks ----
 * A device lists what it can do in "service records". bt_sdp_browse() opens an L2CAP channel to the device's service directory (PSM 1) over
 * the link, asks for all records and closes the channel; the result is in the text lines 4 and up of bt_link_line(): line 4 the state
 * ("sdp: opening", "asking", "N services", "failed: ..."), then one line per service. Needs a link that is up. */
#define BT_SDP_MAX 16

typedef struct
{
    unsigned class_id[2];     /* the 16-bit service class numbers (first two; 0 = none): 0x110B = A2DP sink, 0x1108 = headset ... */
    unsigned profile;         /* the profile it follows (0 = none given) */
    unsigned profile_ver;     /* ... and its version, major in the high byte */
    int has_psm;
    unsigned psm;             /* the L2CAP channel number to connect to (0x19 = AVDTP, the music channel) */
    int has_rfcomm;
    unsigned rfcomm;          /* RFCOMM channel (serial-like services: hands-free, headset) */
    unsigned avdtp_ver;
    char name[24];
} BtSdpService;

unsigned bt_sdp_request(unsigned char *out, unsigned cap, unsigned tid, unsigned max_bytes, const unsigned char *cont, unsigned contlen);
int bt_sdp_parse(const unsigned char *lists, unsigned len, BtSdpService *out, unsigned max); /* the AttributeLists: services found, or -1 */
const char *bt_sdp_uuid_name(unsigned uuid); /* "A2DP sink" ... or "" */
int bt_sdp_browse(void);                     /* 0 = started; BT_E_NOT_UP without a link, BT_E_LINK_BUSY while one runs */

/* ---- pairing and link security (kernel/bluetooth/bt_pair.c) ----
 * bt_pair_start() makes the link authenticated and encrypted: Secure Simple Pairing "Just Works" (no keys or screens needed on either side), or
 * the old PIN ("0000") for devices that only know that. Needs a link that is up; the device must be willing (a headset usually: in pairing
 * mode). The link key is kept in memory for the next link to the same device (not on any disk). Result: bt_pair_line() 0 and 1, which
 * bt_link_line() also gives as lines 0x80 and 0x81. */
int bt_pair_start(void); /* 0 = started; BT_E_NOT_UP without a link; BT_E_LINK_BUSY while one runs */
int bt_pair_line(unsigned index, char *buf, unsigned cap);
void bt_pair_forget(void); /* drop the stored link key: the next link has to pair again */

/* The events we ask the chip to report: the chip's default set (bits 0 .. 44), Extended Inquiry Result (46) and the Secure Simple Pairing
 * events (IO Capability Request / Response, User Confirmation / Passkey Request, Remote OOB, Simple Pairing Complete, Passkey / Keypress
 * Notification, Remote Host Supported Features: bits 48 .. 52, 53, 58 .. 60), as in Linux hci_setup_event_mask(). */
#define BT_EVENT_MASK {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x5F, 0x3F, 0x1C}

unsigned bt_link_handle(void);
const unsigned char *bt_link_addr(void); /* the device of the link, least significant byte first */
void bt_pair_event(const unsigned char *evt, unsigned len);
void bt_pair_step(unsigned long long now);
void bt_pair_reset(void);

/* ---- between bt_hci.c and bt_scan.c / bt_link.c ---- */
void bt_link_event(const unsigned char *evt, unsigned len);
void bt_link_acl(const unsigned char *pkt, unsigned len);
void bt_link_step(void);
void bt_link_reset(void);
unsigned long long bt_now(void);
void bt_scan_event(const unsigned char *evt, unsigned len); /* an event nobody waits for in bt_cmd */
void bt_scan_step(void);
void bt_scan_reset(void); /* the chip was reset or unplugged: forget the setup and the list */

#endif
