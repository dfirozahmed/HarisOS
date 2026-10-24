/* Wi-Fi app: the networks around (from the card's last scan), a Rescan button, and joining one with a passphrase.
 *
 * Click a network (or Up / Down), type its passphrase, press Connect (or Enter). Only WPA2-Personal networks can be joined; the others are listed
 * greyed out. The passphrase stays in memory: nothing is written to a disk, and the field is wiped when the window closes or the join worked.
 * Scanning (about 4 seconds) and joining (several seconds, then DHCP) wait for the radio: the window shows "Scanning..." / "Connecting..." first
 * and does the work on the next pass of the main loop, so the rest of the desktop stands still for that time.
 * An app uses only public headers (ui.h, wm.h, desktop.h, net.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "net.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "wm.h"

#define ROW_H 46
#define FOOT_H 112
#define HEAD_H 76
#define APS_MAX 32
#define BTN_RESCAN 0
#define BTN_CONNECT 1

enum { OP_NONE, OP_SCAN_WAIT, OP_SCAN, OP_JOIN_WAIT, OP_JOIN };

static Window *WF_WIN;
static NetWifiAp APS[APS_MAX]; /* one row per network name: the nearest access point of it */
static int NAPS;
static int SEL = -1;
static int SCROLL;
static int OP;
static char MSG[100];       /* the last result, under the passphrase field */
static int MSG_KIND;        /* 0 none, 1 good, 2 bad */
static char JOIN_SSID[33];  /* the network being joined */
static NetWifiStatus ST;    /* the connection, looked at again every second */
static unsigned long long LAST_POLL;

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

static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void set_msg(int kind, const char *text)
{
    int n = 0;
    MSG[0] = 0;
    put_str(MSG, sizeof MSG, &n, text);
    MSG_KIND = kind;
}

static void wipe_passphrase(Window *w)
{
    volatile char *p = (volatile char *)&w->edits[0];
    for (unsigned i = 0; i < sizeof w->edits[0]; i++)
        p[i] = 0;
    w->edits[0].flags = UI_EDIT_PASSWORD;
    w->edits[0].max = 63;
}

/* ---- the list ---- */
static int joinable(const NetWifiAp *a) { return a->sec == 3; }

/* rebuild the rows from the card's scan list: one per name (the access point heard most often), the nearest first, hidden networks left out */
static void rebuild(void)
{
    char keep[33];
    int had = SEL >= 0 && SEL < NAPS;
    if (had)
    {
        int n = 0;
        keep[0] = 0;
        put_str(keep, sizeof keep, &n, APS[SEL].ssid);
    }
    NAPS = 0;
    int total = net_wifi_ap_count();
    for (int i = 0; i < total; i++)
    {
        NetWifiAp a;
        if (net_wifi_ap((unsigned)i, &a) != 0 || a.ssid[0] == 0)
            continue;
        int at = -1;
        for (int k = 0; k < NAPS; k++)
            if (same_text(APS[k].ssid, a.ssid))
                at = k;
        if (at >= 0)
        {
            if (a.beacons > APS[at].beacons)
                APS[at] = a;
        }
        else if (NAPS < APS_MAX)
            APS[NAPS++] = a;
    }
    for (int i = 1; i < NAPS; i++) /* insertion sort: usable networks first, then the most beacons */
    {
        NetWifiAp a = APS[i];
        int k = i - 1;
        while (k >= 0 && ((joinable(&a) && !joinable(&APS[k])) || (joinable(&a) == joinable(&APS[k]) && a.beacons > APS[k].beacons)))
        {
            APS[k + 1] = APS[k];
            k--;
        }
        APS[k + 1] = a;
    }
    SEL = -1;
    if (had)
        for (int i = 0; i < NAPS; i++)
            if (same_text(APS[i].ssid, keep))
                SEL = i;
    if (SCROLL >= NAPS)
        SCROLL = 0;
}

/* ---- layout ---- */
typedef struct
{
    UiRect head, list, foot, pass;
} Layout;

static Layout layout(UiRect body)
{
    Layout l;
    UiRect r = ui_inset(body, 8);
    l.head = ui_cut_top(&r, HEAD_H);
    l.foot = ui_cut_bottom(&r, FOOT_H);
    l.list = r;
    UiRect f = l.foot;
    ui_cut_top(&f, 28); /* the label above the field */
    l.pass = ui_cut_top(&f, 32);
    l.pass.w -= 124; /* room for the Connect button */
    return l;
}

static int visible_rows(const Layout *l)
{
    int n = l->list.h / ROW_H;
    return n < 1 ? 1 : n;
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
    int busy = OP != OP_NONE;
    w->btn[BTN_RESCAN].enabled = !busy && ST.present;
    w->btn[BTN_CONNECT].enabled = !busy && ST.present && SEL >= 0 && SEL < NAPS && joinable(&APS[SEL]) && w->edits[0].len >= 8;
}

static void select_row(Window *w, int row)
{
    if (row < 0 || row >= NAPS || row == SEL)
        return;
    SEL = row;
    wipe_passphrase(w);
    MSG[0] = 0;
    MSG_KIND = 0;
    w->focus = 0;
    Layout l = layout(wm_content(w));
    keep_visible(&l);
    update_buttons(w);
}

/* ---- drawing ---- */
static void put_band(char *s, int cap, int *n, const NetWifiAp *a)
{
    put_str(s, cap, n, a->channel <= 14 ? "2.4 GHz" : "5 GHz");
    put_str(s, cap, n, "  -  channel ");
    put_dec(s, cap, n, a->channel);
    put_str(s, cap, n, "  -  ");
    static const char *const SEC[] = {"open", "WEP", "WPA", "WPA2"};
    put_str(s, cap, n, SEC[a->sec & 3]);
    if (a->vht)
        put_str(s, cap, n, "  -  802.11ac");
    else if (a->ht)
        put_str(s, cap, n, "  -  802.11n");
}

static void wf_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    Layout l = layout(body);

    /* the connection */
    char line[120];
    int n = 0;
    line[0] = 0;
    UiRgb col = t->text_dim;
    if (!ST.present)
        put_str(line, sizeof line, &n, "No Wi-Fi adapter is in use");
    else if (ST.state == 1)
    {
        put_str(line, sizeof line, &n, "Connected to ");
        put_str(line, sizeof line, &n, ST.ssid);
        col = t->success;
    }
    else if (ST.state == 2)
    {
        put_str(line, sizeof line, &n, "Connection to ");
        put_str(line, sizeof line, &n, ST.ssid);
        put_str(line, sizeof line, &n, " lost - connecting again");
        col = t->warning;
    }
    else
        put_str(line, sizeof line, &n, "Not connected");
    ui_icon(UI_ICON_WIFI, l.head.x + 22, l.head.y + 22, 28, ST.state == 1 ? t->success : t->text_dim);
    ui_text_in_color(ui_rect(l.head.x + 48, l.head.y + 2, l.head.w - 48 - 112, 26), line, UI_TEXT_TITLE, UI_ALIGN_LEFT, col);
    line[0] = 0;
    n = 0;
    if (OP == OP_SCAN_WAIT || OP == OP_SCAN)
        put_str(line, sizeof line, &n, "Scanning the channels...");
    else if (OP == OP_JOIN_WAIT || OP == OP_JOIN)
    {
        put_str(line, sizeof line, &n, "Connecting to ");
        put_str(line, sizeof line, &n, JOIN_SSID);
        put_str(line, sizeof line, &n, "...");
    }
    else if (ST.state == 1 || ST.state == 2)
    {
        put_str(line, sizeof line, &n, "Channel ");
        put_dec(line, sizeof line, &n, ST.channel);
        put_str(line, sizeof line, &n, "  -  ");
        put_dec(line, sizeof line, &n, ST.width);
        put_str(line, sizeof line, &n, " MHz");
        if (net_ip_source() != 0)
        {
            const unsigned char *ip = net_ip();
            put_str(line, sizeof line, &n, "  -  ");
            for (int i = 0; i < 4; i++)
            {
                put_dec(line, sizeof line, &n, ip[i]);
                if (i < 3)
                    put_str(line, sizeof line, &n, ".");
            }
        }
        else
            put_str(line, sizeof line, &n, "  -  no IP address");
    }
    else if (ST.present)
        put_str(line, sizeof line, &n, "Pick a network below.");
    ui_text_in_color(ui_rect(l.head.x + 48, l.head.y + 32, l.head.w - 48 - 112, 22), line, UI_TEXT_DIM, UI_ALIGN_LEFT, t->text_dim);
    ui_separator(l.head.x, l.head.y + l.head.h - 6, l.head.w);

    /* the list */
    if (NAPS == 0)
    {
        ui_text_in(l.list, OP == OP_NONE ? (ST.present ? "No networks found - try Rescan." : "") : "", UI_TEXT_DIM, UI_ALIGN_CENTER);
    }
    int vis = visible_rows(&l);
    for (int i = 0; i < vis && SCROLL + i < NAPS; i++)
    {
        int ri = SCROLL + i;
        const NetWifiAp *a = &APS[ri];
        UiRect r = ui_rect(l.list.x, l.list.y + i * ROW_H, l.list.w, ROW_H);
        int ok = joinable(a);
        int cur = ST.state != 0 && same_text(ST.ssid, a->ssid);
        if (ri == SEL)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 40);
        ui_icon(UI_ICON_WIFI, r.x + 22, r.y + r.h / 2 - 1, 22, ok ? t->text : t->text_dim);
        int badge_w = 130;
        ui_text_in_color(ui_rect(r.x + 44, r.y + 3, r.w - 44 - badge_w - 8, 22), a->ssid, UI_TEXT_BODY, UI_ALIGN_LEFT, ok ? t->text : t->text_dim);
        char info[80];
        int k = 0;
        info[0] = 0;
        put_band(info, sizeof info, &k, a);
        ui_text_in_color(ui_rect(r.x + 44, r.y + 24, r.w - 44 - badge_w - 8, 18), info, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
        const char *badge = cur ? (ST.state == 1 ? "Connected" : "Reconnecting") : ok ? "" : "Not supported";
        if (*badge)
            ui_text_in_color(ui_rect(r.x + r.w - badge_w - 6, r.y, badge_w, r.h - 2), badge, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                             cur ? (ST.state == 1 ? t->success : t->warning) : t->warning);
    }

    /* the passphrase */
    ui_separator(l.foot.x, l.foot.y + 2, l.foot.w);
    if (SEL >= 0 && SEL < NAPS && joinable(&APS[SEL]))
    {
        char lab[60];
        int k = 0;
        lab[0] = 0;
        put_str(lab, sizeof lab, &k, "Passphrase for ");
        put_str(lab, sizeof lab, &k, APS[SEL].ssid);
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 6, l.foot.w - 4, 22), lab, UI_TEXT_LABEL, UI_ALIGN_LEFT, t->text);
        ui_edit_draw(l.pass, &w->edits[0], "8 to 63 characters", OP == OP_NONE);
    }
    else if (SEL >= 0 && SEL < NAPS)
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 6, l.foot.w - 4, 22), "Only WPA2-Personal networks can be joined here.", UI_TEXT_BODY, UI_ALIGN_LEFT, t->warning);
    else
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 6, l.foot.w - 4, 22), "Select a network.", UI_TEXT_DIM, UI_ALIGN_LEFT, t->text_dim);
    if (MSG[0])
        ui_text_in_color(ui_rect(l.foot.x + 2, l.foot.y + 72, l.foot.w - 4, 34), MSG, UI_TEXT_SMALL, UI_ALIGN_LEFT, MSG_KIND == 1 ? t->success : t->danger);
}

/* ---- the work ---- */
static void poll_status(void)
{
    net_wifi_status(&ST);
    LAST_POLL = timer_ms();
}

static void start_scan(Window *w)
{
    if (OP != OP_NONE || !ST.present)
        return;
    OP = OP_SCAN_WAIT;
    MSG[0] = 0;
    MSG_KIND = 0;
    update_buttons(w);
    wm_invalidate();
}

static void start_join(Window *w)
{
    if (OP != OP_NONE || SEL < 0 || SEL >= NAPS || !joinable(&APS[SEL]) || w->edits[0].len < 8)
        return;
    int n = 0;
    JOIN_SSID[0] = 0;
    put_str(JOIN_SSID, sizeof JOIN_SSID, &n, APS[SEL].ssid);
    OP = OP_JOIN_WAIT;
    MSG[0] = 0;
    MSG_KIND = 0;
    update_buttons(w);
    wm_invalidate();
}

static const char *join_error(int code)
{
    switch (code)
    {
    case NET_WIFI_E_ARGS:
        return "The passphrase must have 8 to 63 characters.";
    case NET_WIFI_E_NOT_FOUND:
        return "That network is not in the list any more - Rescan.";
    case NET_WIFI_E_UNSUPPORTED:
        return "The security of this network is not supported (WPA2-Personal with CCMP only).";
    case NET_WIFI_E_NO_ANSWER:
        return "The access point did not answer. Come closer or Rescan.";
    case NET_WIFI_E_HANDSHAKE:
        return "Could not join: the key exchange failed - is the passphrase right?";
    default:
        return "There is no Wi-Fi adapter in use.";
    }
}

static int wf_tick(Window *w)
{
    if (OP == OP_SCAN_WAIT || OP == OP_JOIN_WAIT)
    {
        OP++; /* the "...ing" screen was drawn: now do it */
        return 1;
    }
    if (OP == OP_SCAN)
    {
        int r = net_wifi_rescan();
        OP = OP_NONE;
        poll_status();
        rebuild();
        if (r < 0)
            set_msg(2, join_error(r));
        else
            set_msg(1, r == 0 ? "No networks heard." : "Scan finished.");
        update_buttons(w);
        return 1;
    }
    if (OP == OP_JOIN)
    {
        char pass[UI_EDIT_MAX + 1];
        for (int i = 0; i <= w->edits[0].len && i < (int)sizeof pass; i++)
            pass[i] = w->edits[0].text[i];
        int r = net_wifi_connect(JOIN_SSID, pass);
        for (unsigned i = 0; i < sizeof pass; i++)
            ((volatile char *)pass)[i] = 0;
        OP = OP_NONE;
        poll_status();
        if (r == NET_WIFI_OK)
        {
            char m[100];
            int n = 0;
            m[0] = 0;
            put_str(m, sizeof m, &n, "Connected to ");
            put_str(m, sizeof m, &n, JOIN_SSID);
            put_str(m, sizeof m, &n, net_ip_source() == 0 ? " (no IP address from DHCP)." : ".");
            set_msg(1, m);
            wipe_passphrase(w);
        }
        else
            set_msg(2, join_error(r));
        update_buttons(w);
        return 1;
    }
    unsigned long long now = timer_ms();
    if (now - LAST_POLL >= 1000)
    {
        NetWifiStatus old = ST;
        poll_status();
        if (old.state != ST.state || old.present != ST.present || !same_text(old.ssid, ST.ssid))
        {
            update_buttons(w);
            return 1;
        }
    }
    return 0;
}

/* ---- input ---- */
static void on_rescan(UiButton *b, void *user)
{
    (void)b;
    start_scan((Window *)user);
}

static void on_connect(UiButton *b, void *user)
{
    (void)b;
    start_join((Window *)user);
}

static int wf_press(Window *w, int x, int y)
{
    if (OP != OP_NONE)
        return 0;
    Layout l = layout(wm_content(w));
    if (ui_contains(l.list, x, y))
    {
        int row = SCROLL + (y - l.list.y) / ROW_H;
        if ((y - l.list.y) / ROW_H >= visible_rows(&l) || row >= NAPS)
            return 0;
        select_row(w, row);
        return 1;
    }
    UiRect f = w->edits[0].field;
    if (f.w > 0 && SEL >= 0 && SEL < NAPS && joinable(&APS[SEL]) && ui_contains(f, x, y))
    {
        ui_edit_mouse(&w->edits[0], x, y, 1);
        ui_caret_restart();
        return 1;
    }
    return 0;
}

static int wf_drag(Window *w, int x, int y, unsigned buttons)
{
    if (!w->edits[0].dragging || !ui_edit_mouse(&w->edits[0], x, y, buttons))
        return 0;
    ui_caret_restart();
    wm_add_damage(ui_inset(w->edits[0].field, -3));
    return 2;
}

static int wf_wheel(Window *w, int steps)
{
    Layout l = layout(wm_content(w));
    int max = NAPS - visible_rows(&l);
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

static void wf_key(Window *w, const KeyEvent *k)
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
    if (k->key == KEY_F5)
    {
        start_scan(w);
        return;
    }
    if (k->key == KEY_UP && NAPS > 0)
    {
        select_row(w, SEL < 0 ? NAPS - 1 : SEL - 1);
        wm_invalidate();
        return;
    }
    if (k->key == KEY_DOWN && NAPS > 0)
    {
        select_row(w, SEL + 1);
        wm_invalidate();
        return;
    }
    if (k->key == KEY_ENTER)
    {
        start_join(w);
        return;
    }
    if (SEL >= 0 && SEL < NAPS && joinable(&APS[SEL]) && ui_edit_key(&w->edits[0], k))
    {
        ui_caret_restart();
        update_buttons(w);
        wm_invalidate();
    }
}

static void wf_close(Window *w)
{
    wipe_passphrase(w);
    WF_WIN = 0;
    OP = OP_NONE;
}

static void wifi_app_open(void)
{
    if (WF_WIN)
    {
        wm_activate(WF_WIN);
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
    Window *win = wm_create("Wi-Fi", x, y, w, h, wf_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = wf_key;
    win->on_press = wf_press;
    win->on_drag = wf_drag;
    win->on_wheel = wf_wheel;
    win->on_tick = wf_tick;
    win->on_close = wf_close;
    win->icon = TASKBAR_ICON_WIFI;
    win->edit_count = 1;
    win->focus = 0;
    win->caret = 1;
    wipe_passphrase(win);
    WF_WIN = win;
    OP = OP_NONE;
    SEL = -1;
    SCROLL = 0;
    MSG[0] = 0;
    MSG_KIND = 0;
    desktop_set_status(0);

    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    wm_add_button_ex(win, "Rescan", UI_BTN_SECONDARY, ui_rect(cw - 8 - 100, 8 + 14, 100, 34), WM_ANCHOR_RIGHT, on_rescan, win);
    wm_add_button_ex(win, "Connect", UI_BTN_PRIMARY, ui_rect(cw - 8 - 116, ch - 8 - FOOT_H + 28, 116, 32), WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_connect,
                     win);
    poll_status();
    rebuild();
    update_buttons(win);
    if (ST.present && NAPS == 0) /* nothing to show yet: look around (done on the next pass, after the window is drawn) */
        start_scan(win);
}

const AppInfo APP_WIFI = {
    .name = "Wi-Fi",
    .desc = "Choose a wireless network",
    .icon = UI_ICON_WIFI,
    .open = wifi_app_open,
};
