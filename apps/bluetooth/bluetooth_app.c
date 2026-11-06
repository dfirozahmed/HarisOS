/* Bluetooth app: look for devices around, connect a headset and play sound to it (A2DP).
 *
 * Scan lists the devices that are "discoverable" (a headset in pairing mode). Pick one and press Connect: the app does by itself what the
 * `btaudio` program does by hand - it makes the radio link (page the device), pairs (Secure Simple Pairing, or the old PIN 0000), and opens the
 * music stream, which is started only when sound comes. From then on the Player and `play` send their sound to the headset. Disconnect closes the
 * stream and the link, and sound goes to the speakers again.
 *
 * Nothing here blocks. The Bluetooth stack (kernel/bluetooth) is stepped by bt_poll() in the main loop; this window only starts a step, and
 * on_tick looks at the stack's text lines to see whether it has finished, failed, or taken too long. Which step is running is OP.
 * An app uses only public headers (ui.h, wm.h, desktop.h, bluetooth.h) - never desktop_internal.h. */
#include "app.h"
#include "bluetooth.h"
#include "bluetoothapp.h"
#include "desktop.h"
#include "filepicker.h"
#include "mouse.h"
#include "taskbar.h"
#include "toast.h"
#include "timer.h"
#include "ui.h"
#include "vfs.h"
#include "wm.h"

#define ROW_H 46
#define HEAD_H 76
#define FOOT_H 184
#define BTN_SCAN 0
#define BTN_DISCONNECT 1
#define BTN_CONNECT 2
#define BTN_SEND 3
#define BTN_BROWSE 4

/* how long each step may take before the app gives up (the stack has time-outs of its own, these are the safety net) */
#define T_LINK_MS 30000
#define T_PAIR_MS 40000
#define T_AV_MS 45000
#define T_SDP_MS 15000
#define T_DOWN_MS 10000

enum { OP_NONE, OP_SCAN, OP_LINK, OP_PAIR, OP_SDP, OP_AV, OP_SEND, OP_STOP_AV, OP_STOP_LINK };

static Window *BT_WIN;
static int OP;
static unsigned long long OP_T0; /* when the current step began */
static int SEL = -1;             /* row in the stack's device list */
static int SCROLL;
static UiScroll SB;
static UiMenu MENU; /* the right-click menu of a device: Connect, Disconnect, Copy name, Copy address, Scan */
enum { BTM_CONNECT, BTM_DISCONNECT, BTM_COPY_NAME, BTM_COPY_ADDR, BTM_SCAN }; /* its item ids */
static int FILE_CH;              /* the RFCOMM channel of the device's Object Push service (it takes files), 0 = it has none */
static int SEND_NODE = -1;       /* the file being sent (a vfs node) */
static char SEND_NAME[64];
static unsigned long long RX_T0;     /* when the file being received began */
static int RECV_ON;              /* visible to other devices, ready to take files */
static char RECV_PATH[96];       /* the folder they are saved in */
static int NO_SCAN;                /* the window was opened to send a file: do not start a scan by itself */
static int DISC_AFTER;           /* Disconnect was pressed during a transfer: cancel it, then disconnect */
static int VOL_DRAG;             /* the left button went down on the volume slider and is still held */
static char MSG[100];
static int MSG_KIND;             /* 0 none, 1 good, 2 bad */
static unsigned long long SIG;   /* what the window showed last (see signature()) */

/* ---- small text helpers (no libc) ---- */
static void put_str(char *s, int cap, int *n, const char *t)
{
    while (*t && *n < cap - 1)
        s[(*n)++] = *t++;
    s[*n] = 0;
}

static void put_dec(char *s, int cap, int *n, unsigned v)
{
    char tmp[10];
    int k = 0;
    do
    {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k && *n < cap - 1)
        s[(*n)++] = tmp[--k];
    s[*n] = 0;
}

/* ", 12.3 s, 160.4 KB/s": the time a transfer has taken so far and its speed (the speed only once a tenth of a second has passed) */
static void put_stats(char *s, int cap, int *n, unsigned bytes, unsigned long long ms)
{
    unsigned t = (unsigned)(ms / 100); /* tenths of a second */
    put_str(s, cap, n, ", ");
    put_dec(s, cap, n, t / 10);
    put_str(s, cap, n, ".");
    put_dec(s, cap, n, t % 10);
    put_str(s, cap, n, " s");
    if (ms >= 100)
    {
        unsigned long long r = (unsigned long long)bytes * 10000 / ms / 1024; /* tenths of KB per second */
        put_str(s, cap, n, ", ");
        put_dec(s, cap, n, (unsigned)(r / 10));
        put_str(s, cap, n, ".");
        put_dec(s, cap, n, (unsigned)(r % 10));
        put_str(s, cap, n, " KB/s");
    }
}

static int starts(const char *s, const char *w)
{
    while (*w && *s == *w)
        s++, w++;
    return *w == 0;
}

static void set_msg(int kind, const char *text)
{
    int n = 0;
    MSG[0] = 0;
    put_str(MSG, sizeof MSG, &n, text);
    MSG_KIND = kind;
}

/* "AA:BB:CC:DD:EE:FF" from an address as the chip reports it (least significant byte first) */
static void put_addr(char *s, int cap, int *n, const unsigned char a[6])
{
    static const char HEX[] = "0123456789ABCDEF";
    for (int i = 5; i >= 0; i--)
    {
        if (*n + 2 < cap)
        {
            s[(*n)++] = HEX[a[i] >> 4];
            s[(*n)++] = HEX[a[i] & 15];
        }
        if (i && *n < cap - 1)
            s[(*n)++] = ':';
    }
    s[*n] = 0;
}

static int same_addr(const unsigned char *a, const unsigned char *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* ---- what the stack says ---- */
/* The chip gives no percentage for a scan. The inquiry lasts about 10 seconds (bt_scan.c), so the bar follows the clock up to 90 %; asking
 * the devices for their names has no known length and holds at 95 %. */
#define SCAN_INQUIRY_MS 10240
static int scan_percent(void)
{
    if (bt_scan_state() == BT_SCAN_NAMES)
        return 95;
    unsigned long long el = timer_ms() - OP_T0;
    return el >= SCAN_INQUIRY_MS ? 90 : (int)(el * 90 / SCAN_INQUIRY_MS);
}

static int link_active(void)
{
    BtLinkState s = bt_link_state();
    return s == BT_LINK_CONNECTING || s == BT_LINK_UP || s == BT_LINK_CLOSING;
}

static int line_of(unsigned idx, char *buf, unsigned cap)
{
    buf[0] = 0;
    return bt_link_line(idx, buf, cap) >= 0;
}

static const char *class_word(const BtScanDev *d)
{
    switch (d->cls[1] & 0x1F) /* major device class: bits 8 .. 12 of the class of device */
    {
    case 1:
        return "Computer";
    case 2:
        return "Phone";
    case 3:
        return "Network";
    case 4:
        return "Audio";
    case 5:
        return "Input device";
    case 6:
        return "Imaging";
    case 7:
        return "Wearable";
    case 8:
        return "Toy";
    case 9:
        return "Health";
    default:
        return "Device";
    }
}

/* what this app can do with a device: sound (audio class), receive files (phones, computers); a device that did not tell its class is tried */
static int usable(const BtScanDev *d)
{
    int major = d->cls[1] & 0x1F;
    return major == 1 || major == 2 || major == 4 || (d->cls[0] == 0 && d->cls[1] == 0 && d->cls[2] == 0);
}

static const char *dev_name(const BtScanDev *d) { return d->name_state == 1 && d->name[0] ? d->name : "Unknown device"; }

static int linked_to(const BtScanDev *d) { return link_active() && same_addr(bt_link_addr(), d->addr); }

/* A number that changes whenever something the window shows changes; on_tick compares it with the last one and asks for a redraw. */
static unsigned long long signature(void)
{
    unsigned long long s = (unsigned)OP;
    s = s * 7 + (unsigned)bt_hci_up();
    s = s * 7 + (unsigned)bt_link_state();
    s = s * 7 + (unsigned)bt_scan_state();
    s = s * 64 + (unsigned)bt_scan_count();
    s = s * 7 + (unsigned)bt_a2dp_audio_ready();
    s = s * 128 + (unsigned)bt_a2dp_volume();
    unsigned done = 0, total = 0;
    bt_obex_progress(&done, &total);
    s = s * 101 + (total ? (unsigned)((unsigned long long)done * 100 / total) : 0);
    s = s * 7 + (unsigned)bt_obex_state();
    BtServeInfo si;
    bt_serve_info(&si);
    s = s * 7 + (unsigned)si.receiving + 2 * (unsigned)si.on;
    s = s * 101 + (si.total ? (unsigned)((unsigned long long)si.got * 100 / si.total) : si.got / 4096);
    s = s * 1000 + si.files;
    if (si.receiving)
        s = s * 4096 + (unsigned)((timer_ms() - RX_T0) / 1000); /* the clock in the header */
    if (OP == OP_SCAN)
        s = s * 1024 + (unsigned)scan_percent(); /* the progress bar */
    else if (OP != OP_NONE)
        s = s * 1024 + (unsigned)((timer_ms() - OP_T0) / 1000); /* the seconds counter in the header */
    return s;
}

/* ---- layout ---- */
typedef struct
{
    UiRect head, list, foot, volume, file, recv;
} Layout;

static Layout layout(UiRect body)
{
    Layout l;
    UiRect r = ui_inset(body, 8);
    l.head = ui_cut_top(&r, HEAD_H);
    l.foot = ui_cut_bottom(&r, FOOT_H);
    ui_cut_right(&r, UI_SCROLL_W);
    l.list = r;
    l.volume = ui_rect(l.foot.x + 76, l.foot.y + 12, l.foot.w - 76 - 56, 20);
    l.recv = ui_rect(l.foot.x + 2, l.foot.y + 88, 40, 22); /* the "Receive files" switch */
    l.file = ui_rect(l.foot.x + 76, l.foot.y + 44, l.foot.w - 76 - 216, 30); /* the path of the file to send; the Browse and Send buttons are to its right */
    return l;
}

static int visible_rows(const Layout *l)
{
    int n = l->list.h / ROW_H;
    return n < 1 ? 1 : n;
}

static void sb_sync(const Layout *l)
{
    SB.total = bt_scan_count();
    SB.page = visible_rows(l);
    SB.pos = SCROLL;
    SB.track = ui_rect(l->list.x + l->list.w, l->list.y, UI_SCROLL_W, l->list.h);
}

static void keep_visible(const Layout *l)
{
    int vis = visible_rows(l);
    if (SEL < SCROLL)
        SCROLL = SEL;
    if (SEL >= SCROLL + vis)
        SCROLL = SEL - vis + 1;
    if (SCROLL < 0)
        SCROLL = 0;
}

static void update_buttons(Window *w)
{
    int idle = OP == OP_NONE;
    int up = bt_hci_up();
    BtLinkState ls = bt_link_state();
    w->btn[BTN_SCAN].enabled = idle && up && !link_active();
    w->btn[BTN_DISCONNECT].enabled = (idle || OP == OP_SEND) && up && (ls == BT_LINK_UP || ls == BT_LINK_CONNECTING);
    w->btn[BTN_SEND].enabled = idle && up && ls == BT_LINK_UP && FILE_CH && w->edits[0].len > 0;
    w->btn[BTN_BROWSE].enabled = 1; /* choosing a file does not disturb a scan or a connection, so it is always possible */
    const BtScanDev *d = (SEL >= 0 && SEL < bt_scan_count()) ? bt_scan_dev(SEL) : 0;
    w->btn[BTN_CONNECT].enabled = idle && up && !link_active() && bt_scan_state() != BT_SCAN_INQUIRY && bt_scan_state() != BT_SCAN_NAMES && d && usable(d);
}

static void select_row(Window *w, int row)
{
    if (row < 0 || row >= bt_scan_count() || row == SEL)
        return;
    SEL = row;
    MSG[0] = 0;
    MSG_KIND = 0;
    Layout l = layout(wm_content(w));
    keep_visible(&l);
    update_buttons(w);
}

/* ---- drawing ---- */
static void bt_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    Layout l = layout(body);
    char line[120];
    int n = 0;
    line[0] = 0;
    UiRgb col = t->text_dim;
    int audio = bt_a2dp_audio_ready();
    BtLinkState ls = bt_link_state();
    BtServeInfo si;
    bt_serve_info(&si);

    /* the title: what the radio is doing */
    if (!bt_hci_up())
        put_str(line, sizeof line, &n, "No Bluetooth adapter is working");
    else if (OP >= OP_STOP_AV)
    {
        put_str(line, sizeof line, &n, "Disconnecting...");
        col = t->warning;
    }
    else if (si.receiving)
    {
        put_str(line, sizeof line, &n, "Receiving ");
        put_str(line, sizeof line, &n, si.name);
        col = t->warning;
    }
    else if (OP == OP_SEND)
    {
        put_str(line, sizeof line, &n, "Sending ");
        put_str(line, sizeof line, &n, SEND_NAME);
        col = t->warning;
    }
    else if (OP == OP_LINK || OP == OP_PAIR || OP == OP_SDP || OP == OP_AV)
    {
        put_str(line, sizeof line, &n, OP == OP_LINK ? "Connecting..." : OP == OP_PAIR ? "Pairing..." : OP == OP_SDP ? "Asking what the device can do..." : "Preparing the music stream...");
        col = t->warning;
    }
    else if (ls == BT_LINK_UP)
    {
        const BtScanDev *c = 0;
        for (int i = 0; i < bt_scan_count(); i++)
            if (linked_to(bt_scan_dev(i)))
                c = bt_scan_dev(i);
        put_str(line, sizeof line, &n, audio ? "Audio on: " : "Connected to ");
        if (c)
            put_str(line, sizeof line, &n, dev_name(c));
        else
            put_addr(line, sizeof line, &n, bt_link_addr());
        col = t->success;
    }
    else
        put_str(line, sizeof line, &n, "Not connected");
    ui_icon(UI_ICON_BLUETOOTH, l.head.x + 22, l.head.y + 22, 28, ls == BT_LINK_UP ? t->success : t->text_dim);
    ui_text_in_color(ui_rect(l.head.x + 48, l.head.y + 2, l.head.w - 48 - 240, 26), line, UI_TEXT_TITLE, UI_ALIGN_LEFT, col);

    /* the second line: the chip, the scan, or how long a step has taken */
    line[0] = 0;
    n = 0;
    if (!bt_hci_up())
        bt_status(line, sizeof line);
    else if (si.receiving)
    {
        put_dec(line, sizeof line, &n, si.total ? (unsigned)((unsigned long long)si.got * 100 / si.total) : 0);
        put_str(line, sizeof line, &n, "% - ");
        put_dec(line, sizeof line, &n, si.got);
        if (si.total)
        {
            put_str(line, sizeof line, &n, " of ");
            put_dec(line, sizeof line, &n, si.total);
        }
        put_str(line, sizeof line, &n, " bytes");
        put_stats(line, sizeof line, &n, si.got, timer_ms() - RX_T0);
    }
    else if (OP == OP_SEND)
    {
        unsigned done, total;
        bt_obex_progress(&done, &total);
        put_dec(line, sizeof line, &n, total ? (unsigned)((unsigned long long)done * 100 / total) : 0);
        put_str(line, sizeof line, &n, "% - ");
        put_dec(line, sizeof line, &n, done);
        put_str(line, sizeof line, &n, " of ");
        put_dec(line, sizeof line, &n, total);
        put_str(line, sizeof line, &n, " bytes");
        put_stats(line, sizeof line, &n, done, timer_ms() - OP_T0);
    }
    else if (OP != OP_NONE && OP != OP_SCAN)
    {
        put_dec(line, sizeof line, &n, (unsigned)((timer_ms() - OP_T0) / 1000));
        put_str(line, sizeof line, &n, " s - please wait");
    }
    else if (bt_scan_state() == BT_SCAN_INQUIRY || bt_scan_state() == BT_SCAN_NAMES)
    {
        put_str(line, sizeof line, &n, bt_scan_state() == BT_SCAN_INQUIRY ? "Looking for devices... " : "Asking for names... ");
        put_dec(line, sizeof line, &n, (unsigned)bt_scan_count());
        put_str(line, sizeof line, &n, " found");
    }
    else if (ls == BT_LINK_UP)
        put_str(line, sizeof line, &n, audio ? "Sound from the Player goes to this device." : FILE_CH ? "This device accepts files: type a path below." : "Linked, but the music stream is not open.");
    else
        put_str(line, sizeof line, &n, RECV_ON ? "Visible as HarisOS - send a file to it from the phone." : bt_scan_count() ? "Pick a device below." : "Put the headset in pairing mode, then press Scan.");
    ui_text_in_color(ui_rect(l.head.x + 48, l.head.y + 32, l.head.w - 48 - 240, 22), line, UI_TEXT_DIM, UI_ALIGN_LEFT, t->text_dim);
    if (OP == OP_SCAN)
        ui_progress(ui_rect(l.head.x + 48, l.head.y + 58, l.head.w - 48 - 240, 8), scan_percent());
    else if (si.receiving)
        ui_progress(ui_rect(l.head.x + 48, l.head.y + 58, l.head.w - 48 - 240, 8), si.total ? (int)((unsigned long long)si.got * 100 / si.total) : 0);
    else if (OP == OP_SEND)
    {
        unsigned done, total;
        bt_obex_progress(&done, &total);
        ui_progress(ui_rect(l.head.x + 48, l.head.y + 58, l.head.w - 48 - 240, 8), total ? (int)((unsigned long long)done * 100 / total) : 0);
    }
    ui_separator(l.head.x, l.head.y + l.head.h - 6, l.head.w);

    /* the list */
    int total = bt_scan_count();
    if (total == 0)
    {
        int scanning = bt_scan_state() == BT_SCAN_INQUIRY || bt_scan_state() == BT_SCAN_NAMES;
        ui_text_in(l.list, scanning || !bt_hci_up() ? "" : bt_scan_state() == BT_SCAN_DONE ? "No devices found - try Scan again." : "", UI_TEXT_DIM, UI_ALIGN_CENTER);
    }
    int vis = visible_rows(&l);
    sb_sync(&l);
    ui_scroll_draw(SB.track, &SB);
    for (int i = 0; i < vis && SCROLL + i < total; i++)
    {
        int ri = SCROLL + i;
        const BtScanDev *d = bt_scan_dev(ri);
        if (!d)
            break;
        UiRect r = ui_rect(l.list.x, l.list.y + i * ROW_H, l.list.w, ROW_H);
        int ok = usable(d);
        int cur = linked_to(d);
        if (ri == SEL)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 40);
        ui_icon(UI_ICON_BLUETOOTH, r.x + 22, r.y + r.h / 2 - 1, 22, ok ? t->text : t->text_dim);
        int badge_w = 130;
        ui_text_in_color(ui_rect(r.x + 44, r.y + 3, r.w - 44 - badge_w - 8, 22), dev_name(d), UI_TEXT_BODY, UI_ALIGN_LEFT, ok ? t->text : t->text_dim);
        char info[80];
        int k = 0;
        info[0] = 0;
        put_str(info, sizeof info, &k, class_word(d));
        put_str(info, sizeof info, &k, "  -  ");
        put_addr(info, sizeof info, &k, d->addr);
        if (d->has_rssi)
        {
            put_str(info, sizeof info, &k, "  -  ");
            if (d->rssi < 0)
                put_str(info, sizeof info, &k, "-");
            put_dec(info, sizeof info, &k, (unsigned)(d->rssi < 0 ? -d->rssi : d->rssi));
            put_str(info, sizeof info, &k, " dBm");
        }
        ui_text_in_color(ui_rect(r.x + 44, r.y + 24, r.w - 44 - badge_w - 8, 18), info, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
        const char *badge = cur ? (ls == BT_LINK_UP ? (audio ? "Audio on" : FILE_CH ? "File transfer" : "Connected") : "Connecting...") : ok ? "" : "No audio";
        if (*badge)
            ui_text_in_color(ui_rect(r.x + r.w - badge_w - 6, r.y, badge_w, r.h - 2), badge, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                             cur ? (ls == BT_LINK_UP ? t->success : t->warning) : t->warning);
    }

    /* the volume (software volume of the stream: the headset has its own too) and the last message */
    ui_separator(l.foot.x, l.foot.y + 2, l.foot.w);
    ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 10, 70, 24), "Volume", UI_TEXT_BODY, UI_ALIGN_LEFT, audio ? t->text : t->text_dim);
    ui_slider(l.volume, bt_a2dp_volume());
    char vol[8];
    n = 0;
    vol[0] = 0;
    put_dec(vol, sizeof vol, &n, (unsigned)bt_a2dp_volume());
    put_str(vol, sizeof vol, &n, "%");
    ui_text_in_color(ui_rect(l.volume.x + l.volume.w + 6, l.foot.y + 10, 48, 24), vol, UI_TEXT_BODY, UI_ALIGN_RIGHT, audio ? t->text : t->text_dim);
    ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 46, 70, 26), "File", UI_TEXT_BODY, UI_ALIGN_LEFT, t->text);
    ui_edit_draw(l.file, &w->edits[0], "Path - or press Browse", OP == OP_NONE);
    ui_toggle(l.recv, RECV_ON);
    ui_text_in_color(ui_rect(l.recv.x + 50, l.recv.y - 1, 130, 24), "Receive files", UI_TEXT_BODY, UI_ALIGN_LEFT, t->text);
    ui_text_in_color(ui_rect(l.recv.x + 186, l.recv.y - 1, l.foot.w - 190, 24), RECV_ON ? RECV_PATH : "Phones can then send files to this computer.", UI_TEXT_SMALL, UI_ALIGN_LEFT,
                     t->text_dim);
    if (MSG[0])
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 120, l.foot.w - 140, 60), MSG, UI_TEXT_SMALL, UI_ALIGN_LEFT, MSG_KIND == 1 ? t->success : t->danger);
    else if (SEL >= 0 && SEL < total && !usable(bt_scan_dev(SEL)))
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 120, l.foot.w - 140, 60), "This kind of device has no sound or file service this app can use.", UI_TEXT_SMALL,
                         UI_ALIGN_LEFT, t->warning);
    else if (w->edits[0].len > 0 && !(ls == BT_LINK_UP && FILE_CH))
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 120, l.foot.w - 140, 60), "Connect a phone or computer that accepts files (select it, press Connect), then press Send.",
                         UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    if (MENU.open && w != wm_top())
        ui_menu_close(&MENU); /* focus moved elsewhere (another window or a dialog) */
    ui_menu_draw(&MENU);
}

/* ---- the steps ---- */
static void begin(int op)
{
    OP = op;
    OP_T0 = timer_ms();
}

static void finish(Window *w, int kind, const char *text)
{
    OP = OP_NONE;
    set_msg(kind, text);
    update_buttons(w);
    wm_invalidate();
}

/* a step failed or ran out of time: say why (the stack's own line if there is one), drop whatever was built up */
static void fail(Window *w, const char *what, unsigned idx)
{
    char line[100], m[100];
    int n = 0;
    m[0] = 0;
    put_str(m, sizeof m, &n, what);
    if (line_of(idx, line, sizeof line) && line[0])
    {
        put_str(m, sizeof m, &n, " (");
        put_str(m, sizeof m, &n, line);
        put_str(m, sizeof m, &n, ")");
    }
    bt_avdtp_stop();
    bt_link_disconnect();
    finish(w, 2, m);
}

/* what the device offers, from the services SDP found */
static int has_class(const BtSdpService *s, unsigned id) { return s->class_id[0] == id || s->class_id[1] == id; }

static int sdp_has_audio(void)
{
    for (int i = 0; i < bt_sdp_count(); i++)
        if (has_class(bt_sdp_service(i), 0x110B)) /* A2DP sink */
            return 1;
    return 0;
}

static int sdp_file_channel(void)
{
    for (int i = 0; i < bt_sdp_count(); i++)
    {
        const BtSdpService *s = bt_sdp_service(i);
        if (has_class(s, 0x1105) && s->has_rfcomm && s->rfcomm >= 1 && s->rfcomm <= 30) /* Object Push */
            return (int)s->rfcomm;
    }
    return 0;
}

static int send_read(void *ctx, unsigned off, unsigned char *buf, unsigned n)
{
    (void)ctx;
    return vfs_read(SEND_NODE, off, buf, n);
}

static void start_send(Window *w)
{
    if (OP != OP_NONE || !w->btn[BTN_SEND].enabled)
        return;
    int id = vfs_lookup(w->edits[0].text);
    const VfsNode *f = id >= 0 ? vfs_node(id) : 0;
    if (!f || f->is_dir)
    {
        finish(w, 2, "No such file. Type its full path, for example /HARISOS/photo.jpg.");
        return;
    }
    unsigned char probe;
    if (f->size && vfs_read(id, 0, &probe, 1) < 1)
    {
        finish(w, 2, "This file cannot be read (files on the RAM disk have no content).");
        return;
    }
    SEND_NODE = id;
    SEND_NAME[0] = 0;
    int k = 0;
    put_str(SEND_NAME, sizeof SEND_NAME, &k, f->name);
    int r = bt_obex_send(f->name, f->size, send_read, 0, (unsigned)FILE_CH);
    if (r != 0)
    {
        finish(w, 2, r == BT_E_LINK_BUSY ? "The last transfer is still closing - try again in a few seconds." : "Could not start the transfer.");
        return;
    }
    MSG[0] = 0;
    MSG_KIND = 0;
    DISC_AFTER = 0;
    begin(OP_SEND);
    update_buttons(w);
    wm_invalidate();
}

/* ---- receiving: where the files go, and the sink that writes them ---- */
static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* The folder "Bluetooth" on the first drive that can be written (the RAM disk keeps no content, so it is no place for files). -1 if there is none. */
static int find_dest(void)
{
    for (int v = 0; v < vfs_volume_count(); v++)
    {
        VfsVolInfo vi;
        if (vfs_volume_info(v, &vi) != 0 || !vi.writable || vi.locked || same_ci(vi.fs_name, "RAM"))
            continue;
        int ids[128];
        int n = vfs_list(vi.node, ids, 128);
        if (n > 128)
            n = 128;
        int dir = -1;
        for (int i = 0; i < n; i++)
        {
            const VfsNode *c = vfs_node(ids[i]);
            if (c && c->is_dir && same_ci(c->name, "Bluetooth"))
                dir = ids[i];
        }
        if (dir < 0)
            dir = vfs_add(vi.node, "Bluetooth", 1, 0);
        return dir >= 0 ? dir : vi.node; /* (if the folder cannot be made, the files go to the drive's top folder) */
    }
    return -1;
}

/* A name the phone sent, made safe for a file system: no folders (only the part after the last / or \\), no characters FAT forbids, no leading or
 * trailing dots and spaces, not longer than 120 bytes; "received.bin" if nothing is left. */
static void clean_name(const char *in, char *out, int cap)
{
    const char *base = in;
    for (const char *q = in; *q; q++)
        if (*q == '/' || *q == '\\')
            base = q + 1;
    int n = 0;
    for (; *base && n < cap - 1 && n < 120; base++)
    {
        unsigned char c = (unsigned char)*base;
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        out[n++] = (char)c;
    }
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' '))
        n--;
    int a = 0;
    while (a < n && (out[a] == '.' || out[a] == ' '))
        a++;
    for (int i = a; i < n; i++)
        out[i - a] = out[i];
    n -= a;
    out[n] = 0;
    if (n == 0)
    {
        const char *d = "received.bin";
        for (n = 0; d[n]; n++)
            out[n] = d[n];
        out[n] = 0;
    }
}

/* the file in `dir`: under the name given, or "name (2).ext", "name (3).ext" ... when it exists */
static int create_unique(int dir, const char *name)
{
    int id = vfs_add(dir, name, 0, 0);
    if (id >= 0)
        return id;
    int len = 0, dot = -1;
    while (name[len])
        len++;
    for (int i = 1; i < len; i++)
        if (name[i] == '.')
            dot = i;
    if (dot < 0)
        dot = len;
    for (unsigned k = 2; k < 30; k++)
    {
        char nm[160];
        int m = 0;
        for (int i = 0; i < dot && m < 130; i++)
            nm[m++] = name[i];
        nm[m] = 0;
        put_str(nm, sizeof nm, &m, " (");
        put_dec(nm, sizeof nm, &m, k);
        put_str(nm, sizeof nm, &m, ")");
        put_str(nm, sizeof nm, &m, name + dot);
        id = vfs_add(dir, nm, 0, 0);
        if (id >= 0)
            return id;
    }
    return -1;
}

static int RX_DIR = -1;       /* the folder (a vfs node) */
static int RX_NODE = -1;      /* the file being written */
static unsigned RX_OFF;       /* bytes of it already on the drive */
static unsigned char RX_BUF[65536]; /* written in big pieces: a drive is slow with small ones */
static unsigned RX_FILL;
static char RX_NAME[64];

static int rx_flush(void)
{
    if (RX_FILL == 0)
        return 0;
    int r = vfs_write(RX_NODE, RX_OFF, RX_BUF, RX_FILL);
    if (r != (int)RX_FILL)
        return -1;
    RX_OFF += RX_FILL;
    RX_FILL = 0;
    return 0;
}

static int rx_begin(void *ctx, const char *name, unsigned size)
{
    (void)ctx;
    (void)size;
    if (RX_DIR < 0 || RX_NODE >= 0)
        return -1;
    char clean[128];
    clean_name(name, clean, sizeof clean);
    int id = create_unique(RX_DIR, clean);
    if (id < 0)
        return -1;
    RX_NODE = id;
    RX_OFF = RX_FILL = 0;
    RX_T0 = timer_ms();
    const VfsNode *f = vfs_node(id);
    int k = 0;
    RX_NAME[0] = 0;
    put_str(RX_NAME, sizeof RX_NAME, &k, f ? f->name : clean);
    return 0;
}

static int rx_write(void *ctx, const unsigned char *d, unsigned n)
{
    (void)ctx;
    if (RX_NODE < 0)
        return -1;
    while (n)
    {
        unsigned part = sizeof RX_BUF - RX_FILL;
        if (part > n)
            part = n;
        for (unsigned i = 0; i < part; i++)
            RX_BUF[RX_FILL + i] = d[i];
        RX_FILL += part;
        d += part;
        n -= part;
        if (RX_FILL == sizeof RX_BUF && rx_flush() != 0)
            return -1;
    }
    return 0;
}

static void rx_end(void *ctx, int ok)
{
    (void)ctx;
    if (RX_NODE < 0)
        return;
    if (ok && rx_flush() != 0)
        ok = 0;
    char m[TOAST_TEXT_MAX];
    int k = 0;
    m[0] = 0;
    if (ok)
    {
        put_str(m, sizeof m, &k, "Received ");
        put_str(m, sizeof m, &k, RX_NAME);
        put_str(m, sizeof m, &k, " (");
        put_dec(m, sizeof m, &k, RX_OFF);
        put_str(m, sizeof m, &k, " bytes");
        put_stats(m, sizeof m, &k, RX_OFF, timer_ms() - RX_T0);
        put_str(m, sizeof m, &k, ") in ");
        put_str(m, sizeof m, &k, RECV_PATH);
        toast_show(TOAST_SUCCESS, m);
    }
    else
    {
        vfs_remove(RX_NODE); /* a broken file is no use */
        put_str(m, sizeof m, &k, "Receiving ");
        put_str(m, sizeof m, &k, RX_NAME);
        put_str(m, sizeof m, &k, " did not finish - the part received was removed.");
        toast_show(TOAST_WARNING, m);
    }
    RX_NODE = -1;
    RX_FILL = 0;
}

static void set_receive(Window *w, int on)
{
    if (OP != OP_NONE)
        return;
    if (on)
    {
        if (!bt_hci_up())
        {
            finish(w, 2, "No working Bluetooth adapter.");
            return;
        }
        RX_DIR = find_dest();
        if (RX_DIR < 0)
        {
            finish(w, 2, "There is no drive to save files on that can be written.");
            return;
        }
        RECV_PATH[0] = 0;
        vfs_path(RX_DIR, RECV_PATH, sizeof RECV_PATH);
        BtObexSink sink = {rx_begin, rx_write, rx_end, 0};
        int r = bt_serve_set(1, &sink);
        if (r != 0)
        {
            finish(w, 2, "The chip could not be made visible.");
            return;
        }
        RECV_ON = 1;
        finish(w, 1, "On. On the phone: send a file by Bluetooth to HarisOS (pair if it asks - there is no PIN).");
    }
    else
    {
        bt_serve_set(0, 0);
        RECV_ON = 0;
        finish(w, 1, "Receiving is off.");
    }
}

static void start_scan(Window *w)
{
    if (OP != OP_NONE || !bt_hci_up() || link_active())
        return;
    bt_coex(1); /* Bluetooth may use the shared antenna (the default the btscan program also sets) */
    int r = bt_scan_start();
    if (r < 0)
    {
        finish(w, 2, r == BT_E_NOT_UP ? "The Bluetooth chip is not ready." : r == BT_E_SCANNING ? "A scan is already running." : "The chip refused to scan.");
        return;
    }
    OP = OP_SCAN;
    OP_T0 = timer_ms();
    SEL = -1;
    SCROLL = 0;
    MSG[0] = 0;
    MSG_KIND = 0;
    update_buttons(w);
    wm_invalidate();
}

static void start_connect(Window *w)
{
    if (OP != OP_NONE || SEL < 0 || SEL >= bt_scan_count() || !w->btn[BTN_CONNECT].enabled)
        return;
    const BtScanDev *d = bt_scan_dev(SEL);
    unsigned char addr[6]; /* the chip gives least significant byte first; bt_link_connect wants it the way people write it */
    for (int i = 0; i < 6; i++)
        addr[i] = d->addr[5 - i];
    int r = bt_link_connect(addr);
    if (r != 0)
    {
        finish(w, 2, r == BT_E_NOT_UP ? "The Bluetooth chip is not ready." : r == BT_E_LINK_BUSY ? "A link exists already - Disconnect first." : "The chip refused the connection.");
        return;
    }
    MSG[0] = 0;
    MSG_KIND = 0;
    FILE_CH = 0;
    begin(OP_LINK);
    update_buttons(w);
    wm_invalidate();
}

static void start_disconnect(Window *w)
{
    if (OP == OP_SEND) /* cancel the transfer first; the tick then disconnects */
    {
        bt_obex_cancel();
        DISC_AFTER = 1;
        return;
    }
    if (OP != OP_NONE || !bt_hci_up() || !link_active())
        return;
    bt_a2dp_stop_audio();
    bt_avdtp_stop();
    MSG[0] = 0;
    MSG_KIND = 0;
    FILE_CH = 0;
    begin(OP_STOP_AV);
    update_buttons(w);
    wm_invalidate();
}

static int bt_tick(Window *w)
{
    char line[160];
    unsigned long long now = timer_ms(), el = now - OP_T0;
    switch (OP)
    {
    case OP_SCAN:
        if (bt_scan_state() == BT_SCAN_DONE || bt_scan_state() == BT_SCAN_IDLE)
        {
            OP = OP_NONE;
            int n = bt_scan_count();
            if (n == 0)
            {
                bt_scan_line(0, line, sizeof line); /* why nothing: the summary line says what the chip did */
                set_msg(2, line);
            }
            else
                set_msg(1, n == 1 ? "Scan finished: 1 device." : "Scan finished.");
            update_buttons(w);
        }
        break;
    case OP_LINK:
    {
        BtLinkState s = bt_link_state();
        if (s == BT_LINK_UP && line_of(1, line, sizeof line) && !starts(line, "echo: waiting") && !starts(line, "echo: -"))
        {
            int r = bt_pair_start();
            if (r != 0)
                fail(w, "Could not start the pairing.", 0);
            else
                begin(OP_PAIR);
        }
        else if (s == BT_LINK_FAILED || s == BT_LINK_NONE)
            fail(w, "The device did not answer - is it in pairing mode?", 0);
        else if (el > T_LINK_MS)
            fail(w, "No answer from the device in time.", 0);
        break;
    }
    case OP_PAIR:
        if (line_of(0x80, line, sizeof line) && starts(line, "pair: done"))
        {
            if (bt_sdp_browse() == 0) /* what can the device do: sound, files? */
                begin(OP_SDP);
            else if (bt_avdtp_start_keep() == 0) /* no SDP possible: try the music stream as before */
                begin(OP_AV);
            else
                fail(w, "Could not start the music stream.", 0xA0);
        }
        else if (line_of(0x80, line, sizeof line) && starts(line, "pair: failed"))
            fail(w, "Pairing did not work.", 0x80);
        else if (bt_link_state() != BT_LINK_UP)
            fail(w, "The link was lost while pairing.", 0);
        else if (el > T_PAIR_MS)
            fail(w, "Pairing took too long.", 0x80);
        break;
    case OP_SDP:
    {
        int st = bt_sdp_status();
        if (st == 2 || st == 3)
        {
            FILE_CH = st == 2 ? sdp_file_channel() : 0;
            if ((st == 2 && sdp_has_audio()) || (st == 3 && !FILE_CH))
            {
                if (bt_avdtp_start_keep() == 0) /* a failed SDP is tried as a headset: that is what the old way did */
                    begin(OP_AV);
                else
                    fail(w, "Could not start the music stream.", 0xA0);
            }
            else if (FILE_CH)
                finish(w, 1, "Connected. This device accepts files: type the path of a file and press Send.");
            else
                fail(w, "The device offers no sound or file service this app can use.", 0);
        }
        else if (bt_link_state() != BT_LINK_UP)
            fail(w, "The link was lost.", 0);
        else if (el > T_SDP_MS)
            fail(w, "The device did not tell its services in time.", 0);
        break;
    }
    case OP_SEND:
    {
        BtObexState os = bt_obex_state();
        if (os == BT_OBEX_DONE)
        {
            char m[100];
            int k = 0;
            unsigned done, total;
            bt_obex_progress(&done, &total);
            m[0] = 0;
            put_str(m, sizeof m, &k, "Sent ");
            put_str(m, sizeof m, &k, SEND_NAME);
            put_str(m, sizeof m, &k, " (");
            put_dec(m, sizeof m, &k, total);
            put_str(m, sizeof m, &k, " bytes");
            put_stats(m, sizeof m, &k, total, timer_ms() - OP_T0);
            put_str(m, sizeof m, &k, ").");
            finish(w, 1, m);
        }
        else if (os == BT_OBEX_FAILED)
        {
            int code;
            const char *why = bt_obex_why(&code);
            char m[100];
            int k = 0;
            int cancelled = starts(why, "cancelled");
            m[0] = 0;
            if (cancelled)
                put_str(m, sizeof m, &k, "Transfer cancelled.");
            else
            {
                put_str(m, sizeof m, &k, "Not sent: ");
                put_str(m, sizeof m, &k, why);
                put_str(m, sizeof m, &k, ".");
            }
            finish(w, cancelled ? 1 : 2, m);
            if (DISC_AFTER)
            {
                DISC_AFTER = 0;
                start_disconnect(w);
            }
        }
        break;
    }
    case OP_AV:
        if (line_of(0xA0, line, sizeof line) && starts(line, "a2dp: ready"))
            finish(w, 1, FILE_CH ? "Connected. Sound goes to the device, and it accepts files: type a path and press Send."
                                 : "Connected. Sound from the Player goes to the device; the volume keys change the volume.");
        else if (line_of(0xA0, line, sizeof line) && (starts(line, "a2dp: failed") || starts(line, "a2dp: closed")))
            fail(w, "The device did not accept the music stream.", 0xA0);
        else if (bt_link_state() != BT_LINK_UP)
            fail(w, "The link was lost.", 0);
        else if (el > T_AV_MS)
            fail(w, "The music stream took too long.", 0xA0);
        break;
    case OP_STOP_AV:
        if (!line_of(0xA0, line, sizeof line) || starts(line, "a2dp: closed") || starts(line, "a2dp: failed") || starts(line, "a2dp: -") || el > T_DOWN_MS)
        {
            bt_link_disconnect();
            begin(OP_STOP_LINK);
        }
        break;
    case OP_STOP_LINK:
    {
        BtLinkState s = bt_link_state();
        if ((s != BT_LINK_UP && s != BT_LINK_CLOSING && s != BT_LINK_CONNECTING) || el > T_DOWN_MS)
            finish(w, 1, "Disconnected. Sound goes to the speakers again.");
        break;
    }
    default:
        break;
    }
    unsigned long long sig = signature();
    if (sig == SIG)
        return 0;
    SIG = sig;
    update_buttons(w);
    return 1;
}

/* ---- input ---- */
static void on_scan(UiButton *b, void *user)
{
    (void)b;
    start_scan((Window *)user);
}

static void on_connect(UiButton *b, void *user)
{
    (void)b;
    start_connect((Window *)user);
}

/* the picker's answer: the path goes into the File box */
static void file_picked(const char *path, void *user)
{
    (void)user;
    Window *w = BT_WIN;
    if (!w)
        return;
    ui_edit_set_text(&w->edits[0], path);
    MSG[0] = 0;
    MSG_KIND = 0;
    update_buttons(w);
    wm_invalidate();
}

static void on_browse(UiButton *b, void *user)
{
    (void)b;
    Window *w = (Window *)user;
    filepicker_open(w, "Choose a file to send", w->edits[0].text, file_picked, 0);
}

static void on_send(UiButton *b, void *user)
{
    (void)b;
    start_send((Window *)user);
}

static void on_disconnect(UiButton *b, void *user)
{
    (void)b;
    start_disconnect((Window *)user);
}

/* the menu's commands are the buttons' and keys' own functions; the buttons' enabled flags say which of them can run now */
static void menu_run(int id)
{
    Window *w = BT_WIN;
    if (!w)
        return;
    const BtScanDev *d = (SEL >= 0 && SEL < bt_scan_count()) ? bt_scan_dev(SEL) : 0;
    char text[40];
    int len = 0;
    text[0] = 0;
    switch (id)
    {
    case BTM_CONNECT:
        start_connect(w);
        break;
    case BTM_DISCONNECT:
        start_disconnect(w);
        break;
    case BTM_SCAN:
        start_scan(w);
        break;
    case BTM_COPY_NAME:
    case BTM_COPY_ADDR:
        if (!d)
            break;
        if (id == BTM_COPY_NAME)
            put_str(text, sizeof text, &len, dev_name(d));
        else
            put_addr(text, sizeof text, &len, d->addr);
        ui_clipboard_set(text, len);
        break;
    default:
        break;
    }
}

/* right click on a device: select it and open its menu; elsewhere in the list only Scan. Nothing while a step is running. */
static int bt_rpress(Window *w, int x, int y)
{
    ui_menu_close(&MENU);
    if (OP != OP_NONE)
        return 1;
    Layout l = layout(wm_content(w));
    int row = SCROLL + (y - l.list.y) / ROW_H;
    int on_dev = ui_contains(l.list, x, y) && (y - l.list.y) / ROW_H < visible_rows(&l) && row >= 0 && row < bt_scan_count();
    if (!on_dev && !ui_contains(l.list, x, y))
        return 0; /* the header and the footer have no menu */
    if (on_dev)
        select_row(w, row);
    ui_menu_clear(&MENU);
    if (on_dev)
    {
        ui_menu_add(&MENU, BTM_CONNECT, "Connect", "Enter", w->btn[BTN_CONNECT].enabled ? 0 : UI_MENU_DISABLED);
        ui_menu_add(&MENU, BTM_DISCONNECT, "Disconnect", 0, w->btn[BTN_DISCONNECT].enabled ? 0 : UI_MENU_DISABLED);
        ui_menu_add(&MENU, BTM_COPY_NAME, "Copy name", 0, 0);
        ui_menu_add(&MENU, BTM_COPY_ADDR, "Copy address", 0, 0);
        ui_menu_separator(&MENU);
    }
    ui_menu_add(&MENU, BTM_SCAN, "Scan", "F5", w->btn[BTN_SCAN].enabled ? 0 : UI_MENU_DISABLED);
    ui_menu_open(&MENU, x, y, wm_content(w));
    return 1;
}

static int vol_at(const Layout *l, int x)
{
    int p = (x - l->volume.x) * 100 / (l->volume.w > 0 ? l->volume.w : 1);
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static int bt_press(Window *w, int x, int y)
{
    if (MENU.open) /* a click on an item shows it pressed and it runs when the button is released (bt_drag); a click anywhere else only closes the menu */
    {
        ui_menu_press(&MENU, x, y);
        return 1;
    }
    Layout l = layout(wm_content(w));
    sb_sync(&l);
    if (ui_scroll_press(&SB, x, y))
    {
        SCROLL = SB.pos;
        return 1;
    }
    if (ui_contains(ui_inset(l.volume, -6), x, y))
    {
        VOL_DRAG = 1;
        bt_a2dp_set_volume(vol_at(&l, x));
        return 1;
    }
    if (ui_contains(ui_rect(l.recv.x, l.recv.y, 190, l.recv.h), x, y))
    {
        set_receive(w, !RECV_ON);
        return 1;
    }
    if (w->edits[0].field.w > 0 && ui_contains(w->edits[0].field, x, y))
    {
        ui_edit_mouse(&w->edits[0], x, y, 1);
        ui_caret_restart();
        return 1;
    }
    if (OP != OP_NONE)
        return 0;
    if (ui_contains(l.list, x, y))
    {
        int row = SCROLL + (y - l.list.y) / ROW_H;
        if ((y - l.list.y) / ROW_H >= visible_rows(&l) || row >= bt_scan_count())
            return 0;
        select_row(w, row);
        return 1;
    }
    return 0;
}

static int bt_drag(Window *w, int x, int y, unsigned buttons)
{
    if (MENU.open && MENU.press >= 0)
    {
        int id = -1;
        int r = ui_menu_release(&MENU, x, y, buttons, &id); /* 2 = released on the item it was pressed on */
        if (r == 2)
            menu_run(id);
        return r != 0;
    }
    Layout l = layout(wm_content(w));
    if (VOL_DRAG)
    {
        if (!(buttons & MOUSE_LEFT))
            VOL_DRAG = 0;
        else
            bt_a2dp_set_volume(vol_at(&l, x));
        return 1;
    }
    if (SB.dragging)
    {
        sb_sync(&l);
        int changed = ui_scroll_drag(&SB, x, y, buttons);
        SCROLL = SB.pos;
        return changed;
    }
    if (w->edits[0].dragging && ui_edit_mouse(&w->edits[0], x, y, buttons))
    {
        ui_caret_restart();
        wm_add_damage(ui_inset(w->edits[0].field, -3));
        return 2;
    }
    return 0;
}

static int bt_hover(Window *w, int x, int y)
{
    if (MENU.open)
        return ui_menu_hover(&MENU, x, y);
    Layout l = layout(wm_content(w));
    sb_sync(&l);
    return ui_scroll_hover(&SB, x, y);
}

static int bt_wheel(Window *w, int steps)
{
    int had_menu = MENU.open;
    ui_menu_close(&MENU); /* the rows move under it */
    Layout l = layout(wm_content(w));
    int max = bt_scan_count() - visible_rows(&l);
    if (max < 0)
        max = 0;
    int old = SCROLL;
    SCROLL += steps * 2;
    if (SCROLL > max)
        SCROLL = max;
    if (SCROLL < 0)
        SCROLL = 0;
    return SCROLL != old || had_menu;
}

static void bt_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    if (MENU.open) /* the menu has the keyboard: Up / Down / Enter / Esc, every other key is ignored */
    {
        int id = -1;
        if (ui_menu_key(&MENU, k, &id) == 2)
            menu_run(id);
        return;
    }
    if (k->key == KEY_ESC)
    {
        if (OP == OP_SEND)
            bt_obex_cancel(); /* Esc stops the transfer first; a second Esc closes the window */
        else
            wm_close(w);
        return;
    }
    if (OP != OP_NONE)
        return;
    int n = bt_scan_count();
    if (k->key == KEY_F5)
        start_scan(w);
    else if (k->key == KEY_UP && n > 0)
    {
        select_row(w, SEL < 0 ? n - 1 : SEL - 1);
        wm_invalidate();
    }
    else if (k->key == KEY_DOWN && n > 0)
    {
        select_row(w, SEL + 1);
        wm_invalidate();
    }
    else if (k->key == KEY_ENTER)
    {
        if (w->btn[BTN_SEND].enabled)
            start_send(w);
        else
            start_connect(w);
    }
    else if (ui_edit_key(&w->edits[0], k)) /* anything else types the path of the file */
    {
        ui_caret_restart();
        update_buttons(w);
        wm_invalidate();
    }
}

static void bt_close(Window *w)
{
    (void)w;
    ui_menu_close(&MENU);
    BT_WIN = 0; /* a running link stays: Bluetooth audio goes on without the window */
    OP = OP_NONE;
    VOL_DRAG = 0;
}

static void bluetooth_app_open(void)
{
    if (BT_WIN)
    {
        wm_activate(BT_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 520, h = 520;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create("Bluetooth", x, y, w, h, bt_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = bt_key;
    win->on_press = bt_press;
    win->on_drag = bt_drag;
    win->on_wheel = bt_wheel;
    win->on_hover = bt_hover;
    win->on_rpress = bt_rpress;
    ui_menu_clear(&MENU);
    win->on_tick = bt_tick;
    win->on_close = bt_close;
    win->icon = TASKBAR_ICON_BLUETOOTH;
    win->edit_count = 1; /* the path of the file to send */
    win->focus = 0;
    win->caret = 1;
    win->edits[0].max = 255;
    BT_WIN = win;
    OP = OP_NONE;
    SEL = -1;
    SCROLL = 0;
    VOL_DRAG = 0;
    SB.dragging = SB.hover = 0;
    MSG[0] = 0;
    MSG_KIND = 0;
    SIG = 0;
    DISC_AFTER = 0;
    if (!link_active())
        FILE_CH = 0; /* (with a link still up from before, the app remembers what the device offered) */
    desktop_set_status(0);

    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    wm_add_button_ex(win, "Scan", UI_BTN_SECONDARY, ui_rect(cw - 8 - 100, 8 + 14, 100, 34), WM_ANCHOR_RIGHT, on_scan, win);
    wm_add_button_ex(win, "Disconnect", UI_BTN_SECONDARY, ui_rect(cw - 8 - 100 - 8 - 124, 8 + 14, 124, 34), WM_ANCHOR_RIGHT, on_disconnect, win);
    wm_add_button_ex(win, "Connect", UI_BTN_PRIMARY, ui_rect(cw - 8 - 116, ch - 8 - 42, 116, 34), WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_connect, win);
    wm_add_button_ex(win, "Browse", UI_BTN_SECONDARY, ui_rect(cw - 8 - 108 - 8 - 92, ch - 8 - FOOT_H + 44, 92, 32), WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_browse, win);
    wm_add_button_ex(win, "Send", UI_BTN_PRIMARY, ui_rect(cw - 8 - 108, ch - 8 - FOOT_H + 44, 108, 32), WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_send, win);
    update_buttons(win);
    if (!NO_SCAN && bt_hci_up() && bt_scan_count() == 0 && bt_scan_state() == BT_SCAN_IDLE && !link_active()) /* nothing to show yet: look around */
        start_scan(win);
}

/* "Send via Bluetooth" in the File Explorer: the window opens with the file in its File box and, if a device that takes files is connected, sends it */
const char *bluetooth_app_send_file(const char *path)
{
    if (!path || !*path)
        return "No file given.";
    NO_SCAN = 1;
    bluetooth_app_open();
    NO_SCAN = 0;
    Window *w = BT_WIN;
    if (!w)
        return "The Bluetooth window could not be opened.";
    if (OP == OP_SEND)
        return "A file is being sent - wait until it is done.";
    ui_edit_set_text(&w->edits[0], path);
    MSG[0] = 0;
    MSG_KIND = 0;
    update_buttons(w);
    if (w->btn[BTN_SEND].enabled)
        start_send(w);
    wm_invalidate();
    return 0;
}

const AppInfo APP_BLUETOOTH = {
    .name = "Bluetooth",
    .desc = "Find and connect a headset",
    .icon = UI_ICON_BLUETOOTH,
    .open = bluetooth_app_open,
};
