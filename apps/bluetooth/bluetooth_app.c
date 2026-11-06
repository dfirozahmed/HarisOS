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
#include "desktop.h"
#include "mouse.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "wm.h"

#define ROW_H 46
#define HEAD_H 76
#define FOOT_H 100
#define BTN_SCAN 0
#define BTN_DISCONNECT 1
#define BTN_CONNECT 2

/* how long each step may take before the app gives up (the stack has time-outs of its own, these are the safety net) */
#define T_LINK_MS 30000
#define T_PAIR_MS 40000
#define T_AV_MS 45000
#define T_DOWN_MS 10000

enum { OP_NONE, OP_SCAN, OP_LINK, OP_PAIR, OP_AV, OP_STOP_AV, OP_STOP_LINK };

static Window *BT_WIN;
static int OP;
static unsigned long long OP_T0; /* when the current step began */
static int SEL = -1;             /* row in the stack's device list */
static int SCROLL;
static UiScroll SB;
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

/* only audio devices (or ones that did not tell their class) can be used for sound */
static int usable(const BtScanDev *d)
{
    int major = d->cls[1] & 0x1F;
    return major == 4 || (d->cls[0] == 0 && d->cls[1] == 0 && d->cls[2] == 0);
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
    if (OP != OP_NONE)
        s = s * 1024 + (unsigned)((timer_ms() - OP_T0) / 1000); /* the seconds counter in the header */
    return s;
}

/* ---- layout ---- */
typedef struct
{
    UiRect head, list, foot, volume;
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
    w->btn[BTN_DISCONNECT].enabled = idle && up && (ls == BT_LINK_UP || ls == BT_LINK_CONNECTING);
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

    /* the title: what the radio is doing */
    if (!bt_hci_up())
        put_str(line, sizeof line, &n, "No Bluetooth adapter is working");
    else if (OP >= OP_STOP_AV)
    {
        put_str(line, sizeof line, &n, "Disconnecting...");
        col = t->warning;
    }
    else if (OP == OP_LINK || OP == OP_PAIR || OP == OP_AV)
    {
        put_str(line, sizeof line, &n, OP == OP_LINK ? "Connecting..." : OP == OP_PAIR ? "Pairing..." : "Preparing the music stream...");
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
        put_str(line, sizeof line, &n, audio ? "Sound from the Player goes to this device." : "Linked, but the music stream is not open.");
    else
        put_str(line, sizeof line, &n, bt_scan_count() ? "Pick a device below." : "Put the headset in pairing mode, then press Scan.");
    ui_text_in_color(ui_rect(l.head.x + 48, l.head.y + 32, l.head.w - 48 - 240, 22), line, UI_TEXT_DIM, UI_ALIGN_LEFT, t->text_dim);
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
        const char *badge = cur ? (ls == BT_LINK_UP ? (audio ? "Audio on" : "Connected") : "Connecting...") : ok ? "" : "No audio";
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
    if (MSG[0])
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 44, l.foot.w - 140, 50), MSG, UI_TEXT_SMALL, UI_ALIGN_LEFT, MSG_KIND == 1 ? t->success : t->danger);
    else if (SEL >= 0 && SEL < total && !usable(bt_scan_dev(SEL)))
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 44, l.foot.w - 140, 50), "Only audio devices (headsets, speakers) can be connected here.", UI_TEXT_SMALL,
                         UI_ALIGN_LEFT, t->warning);
    (void)w;
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
    begin(OP_LINK);
    update_buttons(w);
    wm_invalidate();
}

static void start_disconnect(Window *w)
{
    if (OP != OP_NONE || !bt_hci_up() || !link_active())
        return;
    bt_a2dp_stop_audio();
    bt_avdtp_stop();
    MSG[0] = 0;
    MSG_KIND = 0;
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
            int r = bt_avdtp_start_keep(); /* open the music stream, but start it only when sound comes */
            if (r != 0)
                fail(w, "Could not start the music stream.", 0xA0);
            else
                begin(OP_AV);
        }
        else if (line_of(0x80, line, sizeof line) && starts(line, "pair: failed"))
            fail(w, "Pairing did not work.", 0x80);
        else if (bt_link_state() != BT_LINK_UP)
            fail(w, "The link was lost while pairing.", 0);
        else if (el > T_PAIR_MS)
            fail(w, "Pairing took too long.", 0x80);
        break;
    case OP_AV:
        if (line_of(0xA0, line, sizeof line) && starts(line, "a2dp: ready"))
            finish(w, 1, "Connected. Sound from the Player goes to the device; the volume keys change the volume.");
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

static void on_disconnect(UiButton *b, void *user)
{
    (void)b;
    start_disconnect((Window *)user);
}

static int vol_at(const Layout *l, int x)
{
    int p = (x - l->volume.x) * 100 / (l->volume.w > 0 ? l->volume.w : 1);
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static int bt_press(Window *w, int x, int y)
{
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
    (void)y;
    return 0;
}

static int bt_hover(Window *w, int x, int y)
{
    Layout l = layout(wm_content(w));
    sb_sync(&l);
    return ui_scroll_hover(&SB, x, y);
}

static int bt_wheel(Window *w, int steps)
{
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
    return SCROLL != old;
}

static void bt_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    if (k->key == KEY_ESC)
    {
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
        start_connect(w);
}

static void bt_close(Window *w)
{
    (void)w;
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
    win->on_tick = bt_tick;
    win->on_close = bt_close;
    win->icon = TASKBAR_ICON_BLUETOOTH;
    BT_WIN = win;
    OP = OP_NONE;
    SEL = -1;
    SCROLL = 0;
    VOL_DRAG = 0;
    SB.dragging = SB.hover = 0;
    MSG[0] = 0;
    MSG_KIND = 0;
    SIG = 0;
    desktop_set_status(0);

    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    wm_add_button_ex(win, "Scan", UI_BTN_SECONDARY, ui_rect(cw - 8 - 100, 8 + 14, 100, 34), WM_ANCHOR_RIGHT, on_scan, win);
    wm_add_button_ex(win, "Disconnect", UI_BTN_SECONDARY, ui_rect(cw - 8 - 100 - 8 - 124, 8 + 14, 124, 34), WM_ANCHOR_RIGHT, on_disconnect, win);
    wm_add_button_ex(win, "Connect", UI_BTN_PRIMARY, ui_rect(cw - 8 - 116, ch - 8 - 42, 116, 34), WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_connect, win);
    update_buttons(win);
    if (bt_hci_up() && bt_scan_count() == 0 && bt_scan_state() == BT_SCAN_IDLE && !link_active()) /* nothing to show yet: look around */
        start_scan(win);
}

const AppInfo APP_BLUETOOTH = {
    .name = "Bluetooth",
    .desc = "Find and connect a headset",
    .icon = UI_ICON_BLUETOOTH,
    .open = bluetooth_app_open,
};
