/* RTL8821CE: choosing a network at run time (stage 11). */
#include "rtl8821ce_internal.h"

/* ---- stage 11: choosing a network at run time ----
 * The desktop's Wi-Fi window (apps/wifi) lists the networks of the last scan, scans again on request, and joins a network with a passphrase the user
 * typed. The card has to be the adapter for that (LINK.adapter): it stays one without a connection when the boot did not make one (no WIFI.CFG, the
 * network was not there, a wrong passphrase), see the end of probe(). The passphrase goes into CFG like the one from the file, in memory only, so
 * that a lost connection is made again by link_service() with it. When joining fails, the old network (if there was one) is the one to reconnect to. */
static int IDLE_ENABLE = 1; /* the host tests of the earlier stages switch it off (they expect a card that did not connect to be switched off) */

void rtl8821ce_wifi_status(NetWifiStatus *st)
{
    st->present = LINK.adapter;
    st->state = 0;
    st->ssid[0] = 0;
    st->channel = 0;
    st->width = 20;
    if (!LINK.adapter)
        return;
    if (LINK.up && !CONN.dropped)
        st->state = 1;
    else if (LINK.want)
        st->state = 2;
    if (st->state)
    {
        unsigned i = 0;
        for (; i < 32 && CFG.ssid[i]; i++)
            st->ssid[i] = CFG.ssid[i];
        st->ssid[i] = 0;
        st->channel = (unsigned char)CONN.channel;
        st->width = 20u << CONN.cc.bw;
    }
}

int rtl8821ce_ap_count(void) { return SC.state ? (int)SC.n : -1; }

int rtl8821ce_ap(unsigned index, NetWifiAp *out)
{
    if (!SC.state || index >= SC.n || !out)
        return -1;
    const ScanAp *a = &SC.ap[index];
    unsigned i = 0;
    for (; i < 32 && a->ssid[i]; i++)
        out->ssid[i] = a->ssid[i];
    out->ssid[i] = 0;
    for (int k = 0; k < 6; k++)
        out->bssid[k] = a->bssid[k];
    out->channel = a->channel;
    out->sec = a->sec;
    out->ht = a->ht;
    out->vht = a->vht;
    out->beacons = a->beacons;
    return 0;
}

/* A scan in steps, so that nothing stands still for the four seconds it takes: scan_start() tunes the first channel and returns; link_service()
 * (called from every poll of the network stack) calls scan_service(), which after SCAN_DWELL_MS on a channel notes what was heard and tunes the next,
 * and at the end puts the radio back on the channel of the connection. Meanwhile nothing is sent (LINK.scanning) and the connection is not
 * judged dead for its silence. */
static struct
{
    int active;
    int was_up;
    unsigned idx;    /* the channel being listened to */
    unsigned before; /* RX.frames when it started */
    u64 t0;
} SCN;

/* tune to the next channel that works from SCN.idx on, or finish */
static void scan_next(u64 now)
{
    while (SCN.idx < SCAN_NCH && !scan_chan_begin(SCN.idx))
        SCN.idx++;
    if (SCN.idx < SCAN_NCH)
    {
        SCN.before = RX.frames;
        SCN.t0 = now;
        return;
    }
    scan_end_state();
    pick_target();
    if (SCN.was_up && LINK.up)
    {
        tune_chan(&CONN.cc);
        LINK.last_rx = LINK.last_probe = now; /* (nothing could be heard from the access point meanwhile) */
    }
    LINK.scanning = 0;
    SCN.active = 0;
    if (BOOT.pending)
        boot_finish();
}

void scan_service(u64 now)
{
    if (!SCN.active || now - SCN.t0 < SCAN_DWELL_MS)
        return;
    SC.per_channel[SCN.idx] = RX.frames - SCN.before;
    SCN.idx++;
    scan_next(now);
}

int scan_active(void) { return SCN.active; }

int rtl8821ce_wifi_scan_start(void)
{
    if (!LINK.adapter || !D.ready || !clock_ok())
        return NET_WIFI_E_NO_ADAPTER;
    if (BOOT.busy || JOIN.busy)
        return NET_WIFI_E_BUSY;
    if (SCN.active)
        return NET_WIFI_OK; /* already going on */
    scan_start_state();
    LINK.scanning = 1;
    SCN.active = 1;
    SCN.was_up = LINK.up;
    SCN.idx = 0;
    scan_next(now_ms());
    return NET_WIFI_OK;
}

/* how far the scan is, in percent; -1 when none is going on */
int rtl8821ce_wifi_scan_progress(void) { return SCN.active ? (int)(SCN.idx * 100 / SCAN_NCH) : -1; }

/* the same, waiting for the end */
int rtl8821ce_wifi_rescan(void)
{
    int r = rtl8821ce_wifi_scan_start();
    if (r < 0)
        return r;
    while (SCN.active)
    {
        udelay(1000);
        rx_poll();
        scan_service(now_ms());
    }
    return (int)SC.n;
}

static int same_name(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

void wipe(volatile char *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        p[i] = 0;
}

int rtl8821ce_wifi_connect(const char *ssid, const char *psk)
{
    if (BOOT.pending || BOOT.busy)
        return NET_WIFI_E_BUSY; /* the first scan is still to come */
    if (!LINK.adapter || !D.ready || TXP.variant < 0 || !TXP.sent || !SC.state)
        return NET_WIFI_E_NO_ADAPTER;
    if (SCN.active)
        return NET_WIFI_E_BUSY; /* a scan is going on: wait for its end */
    unsigned sl = 0, pl = 0;
    while (ssid && ssid[sl])
        sl++;
    while (psk && psk[pl])
        pl++;
    if (sl < 1 || sl > 32 || pl < 8 || pl > 63)
        return NET_WIFI_E_ARGS;
    /* the access point: the nearest (most beacons) of that name; WPA2-Personal with CCMP only */
    int best = -1, named = 0;
    for (unsigned i = 0; i < SC.n; i++)
    {
        const ScanAp *a = &SC.ap[i];
        if (!same_name(a->ssid, ssid))
            continue;
        named = 1;
        if (a->sec != 3 || a->rsn_len == 0 || rsn_check(a->rsn, a->rsn_len) != RSN_OK)
            continue;
        if (best < 0 || a->beacons > SC.ap[best].beacons)
            best = (int)i;
    }
    if (best < 0)
        return named ? NET_WIFI_E_UNSUPPORTED : NET_WIFI_E_NOT_FOUND;

    /* what to go back to */
    char old_ssid[sizeof CFG.ssid], old_psk[sizeof CFG.psk];
    for (unsigned i = 0; i < sizeof old_ssid; i++)
        old_ssid[i] = CFG.ssid[i];
    for (unsigned i = 0; i < sizeof old_psk; i++)
        old_psk[i] = CFG.psk[i];
    unsigned old_ssid_len = CFG.ssid_len, old_psk_len = CFG.psk_len;
    int old_cfg_state = CFG.state, was_want = LINK.want, was_up = LINK.up;
    static Chan old_cc;
    static u8 old_bssid[6];
    unsigned old_channel = CONN.channel;
    old_cc = CONN.cc;
    for (int i = 0; i < 6; i++)
        old_bssid[i] = CONN.bssid[i];
    ScanAp old_ap; /* the capabilities of the old access point: CONN gets them back through conn_set_caps */
    old_ap.rates = CONN.rates;
    old_ap.ht = CONN.ht;
    old_ap.ht_mcs0 = CONN.ht_mcs0;
    old_ap.ht_cap = CONN.ht_cap;
    old_ap.vht = CONN.vht;
    old_ap.vht_map = CONN.vht_map;
    old_ap.vht_cap = CONN.vht_cap;
    old_ap.ht_sec = CONN.ht_sec;
    old_ap.ht_w40 = CONN.ht_w40;
    old_ap.vht_w = CONN.vht_w;
    old_ap.vht_seg0 = CONN.vht_seg0;
    old_ap.ht_ampdu = CONN.ht_ampdu;

    if (LINK.up) /* leave the old network politely */
    {
        u8 f[32];
        unsigned n = build_deauth(f, CONN.bssid);
        tx_mgmt(f, n, CONN.channel);
        LINK.up = 0;
        ra_stop();
        ba_reset();
        cam_clear();
    }
    LINK.want = 0; /* (nobody reconnects meanwhile) */
    for (unsigned i = 0; i < sl; i++)
        CFG.ssid[i] = ssid[i];
    CFG.ssid[sl] = 0;
    CFG.ssid_len = sl;
    for (unsigned i = 0; i < pl; i++)
        CFG.psk[i] = psk[i];
    CFG.psk[pl] = 0;
    CFG.psk_len = pl;
    CFG.state = CFG_OK;
    const ScanAp *ap = &SC.ap[best];
    for (int i = 0; i < 6; i++)
        CONN.bssid[i] = ap->bssid[i];
    CONN.channel = ap->channel;
    conn_set_caps(ap);
    tune_chan(&CONN.cc);
    rx_poll();
    connect_exchange();

    int result;
    if (LINK.up)
    {
        LINK.fails = 0;
        result = NET_WIFI_OK;
        if (was_up || was_want)
            LINK.reconnects++;
    }
    else
    {
        result = (CONN.auth_seen && CONN.auth_status == 0 && CONN.assoc_seen && CONN.assoc_status == 0) ? NET_WIFI_E_HANDSHAKE : NET_WIFI_E_NO_ANSWER;
        /* back to the old network, if there was one: link_service() connects to it again at once */
        for (unsigned i = 0; i < sizeof CFG.ssid; i++)
            CFG.ssid[i] = old_ssid[i];
        for (unsigned i = 0; i < sizeof CFG.psk; i++)
            CFG.psk[i] = old_psk[i];
        CFG.ssid_len = old_ssid_len;
        CFG.psk_len = old_psk_len;
        CFG.state = old_cfg_state;
        CONN.channel = old_channel;
        for (int i = 0; i < 6; i++)
            CONN.bssid[i] = old_bssid[i];
        conn_set_caps(&old_ap);
        CONN.cc = old_cc;
        LINK.want = was_want || was_up;
        LINK.fails = 0;
        LINK.backoff = LINK_RETRY_MIN_MS;
        LINK.next_try = now_ms();
        if (LINK.want)
            tune_chan(&CONN.cc);
    }
    wipe(old_psk, sizeof old_psk);
    return result;
}

/* A join in a thread of its own, like the first connection ("starting in the background"): connect_exchange() waits with udelay() for the access point
 * and the 4-way handshake for seconds, and in the main loop the whole desktop (mouse included) would stand still for that. begin() copies the
 * arguments and claims the chip (nobody else sends, receives or scans while busy), run() is the thread, poll() hands over the result once. */
int rtl8821ce_join_begin(const char *ssid, const char *psk)
{
    if (JOIN.busy)
        return NET_WIFI_E_BUSY;
    unsigned sl = 0, pl = 0;
    while (ssid && ssid[sl])
        sl++;
    while (psk && psk[pl])
        pl++;
    if (sl < 1 || sl > 32 || pl < 8 || pl > 63) /* (the same check as rtl8821ce_wifi_connect, before a thread is made for nothing) */
        return NET_WIFI_E_ARGS;
    for (unsigned i = 0; i <= sl; i++)
        JOIN.ssid[i] = ssid[i];
    for (unsigned i = 0; i <= pl; i++)
        JOIN.psk[i] = psk[i];
    JOIN.done = 0;
    JOIN.busy = 1;
    return NET_WIFI_OK;
}

void rtl8821ce_join_run(void)
{
    if (!JOIN.busy)
        return;
    JOIN.result = rtl8821ce_wifi_connect(JOIN.ssid, JOIN.psk);
    wipe(JOIN.psk, sizeof JOIN.psk);
    JOIN.done = 1;
    JOIN.busy = 0;
}

void rtl8821ce_join_abort(void)
{
    wipe(JOIN.psk, sizeof JOIN.psk);
    JOIN.busy = 0;
}

/* 1 while the join goes on; 0 when it has ended (*result is its NET_WIFI_ code, given once); -1 when there is none to report */
int rtl8821ce_join_poll(int *result)
{
    if (JOIN.busy)
        return 1;
    if (!JOIN.done)
        return -1;
    JOIN.done = 0;
    *result = JOIN.result;
    return 0;
}

static int rtl8821ce_recv_up(void *buf, unsigned max)
{
    if (!LINK.adapter || BOOT.busy || JOIN.busy)
        return 0;
    rx_poll();
    link_service();
    if (!LINK.up || LINK.head == LINK.tail)
        return 0;
    EthFrame *q = &LINK.q[LINK.tail];
    unsigned n = q->len <= max ? q->len : max;
    u8 *o = (u8 *)buf;
    for (unsigned i = 0; i < n; i++)
        o[i] = q->b[i];
    LINK.tail = (LINK.tail + 1) % RXE_SLOTS;
    return (int)n;
}

static int rtl8821ce_claims(const PciDevice *pd)
{
    return pd->vendor_id == 0x10EC && (pd->device_id == 0xB821 || pd->device_id == 0xC821);
}

static int rtl8821ce_probe(const PciDevice *pd)
{
    WF.regs = 0;
    WF.ok = 0;
    WF.fail = 0;
    WF.cfg1 = WF.cfg2 = 0;
    WF.func_en = WF.pw_ctrl = WF.mcufw = 0;
    WF.cr = 0;
    WF.pwr_state = PWR_NOT_TRIED;
    WF.pwr_step = 0;
    WF.pwr_phase = 0;
    WF.was_on = 0;
    WF.func_en2 = WF.pw_ctrl2 = WF.mcufw2 = 0;
    WF.cr2 = 0;
    FW.state = FW_NOT_CHECKED;
    u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
    pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x2u); /* memory space on (the firmware usually did it already) */
    u32 lo = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * PCI_BAR_MMIO);
    if (lo & 1u)
    {
        WF.fail = "BAR 2 is not memory";
        return 0;
    }
    u64 base = lo & ~0xFULL;
    if (((lo >> 1) & 3u) == 2u)
        base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x14 + 4 * PCI_BAR_MMIO) << 32;
    if (!base)
    {
        WF.fail = "BAR 2 is empty";
        return 0;
    }
    WF.regs = (volatile u8 *)base;
    WF.cfg1 = rd32(R_SYS_CFG1);
    WF.cfg2 = rd32(R_SYS_CFG2);
    read_state(&WF.func_en, &WF.pw_ctrl, &WF.mcufw, &WF.cr);
    if (WF.cfg1 == 0xFFFFFFFFu && WF.cfg2 == 0xFFFFFFFFu)
    {
        WF.fail = "registers read as all ones (card off?)";
        return 0;
    }
    WF.ok = 1;
    fw_check(); /* (only looks at a file; the chip is not involved) */
    cfg_load();

    /* stage 3 needs the chip to reach RAM: bus mastering on, and the DMA rings set up (Linux does this before the power-on, too) */
    int dma_ok = 0;
    DL.state = DL_NOT_TRIED;
    EF.state = EF_NOT_TRIED;
    MI.state = MI_NOT_TRIED;
    MI.why = 0;
    PH.state = PH_NOT_TRIED;
    PH.why = 0;
    H2.state = H2_NOT_TRIED;
    H2.why = 0;
    CHN.state = CH_NOT_TRIED;
    RX.state = RXS_NOT_TRIED;
    LINK.up = 0;
    LINK.adapter = 0;
    BOOT.pending = 0;
    LINK.want = LINK.scanning = 0;
    SCN.active = 0;
    RA.on = 0;
    CONN.state = 0;
    TARGET.found = 0;
    HS.state = 0;
    SC.state = 0;
    SC.n = 0;
    TXP.state = 0;
    TXP.wp = 0;
    TXP.replies = 0;
    EF.why = 0;
    DL.why = 0;
    DL.part = 0;
    DL.piece = 0;
    DL.off = 0;
    DL.mcufw = 0;
    DL.off_after = 0;
    int want_load = RTL8821CE_POWER_ON && RTL8821CE_FW_LOAD && FW.state == FW_OK;
    if (want_load)
    {
        pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (pci_cfg_read(pd->bus, pd->dev, pd->fn, 4) & 0xFFFFu) | 0x6u); /* memory space + bus master */
        dma_ok = dma_setup();
        if (dma_ok)
            hci_setup();
        else
        {
            DL.state = DL_SKIPPED;
            DL.why = "no DMA memory below 4 GiB";
        }
    }

    /* stage 2: power on, the way Linux does. A MAC that is already on is powered off and on again, unless a firmware of an earlier
     * user is still in the chip (0xC078): Linux first tells that firmware to stop through the RPWM register, which is not done here, so
     * such a card is left alone. */
    if (!RTL8821CE_POWER_ON)
        WF.pwr_state = PWR_DISABLED;
    else if (WF.cr != 0xEA && WF.mcufw == 0xC078)
        WF.pwr_state = PWR_LEFT_ALONE;
    else
    {
        WF.was_on = WF.cr != 0xEA;
        WF.pwr_step = power_cycle(WF.was_on);
        WF.pwr_state = WF.pwr_step ? PWR_FAILED : PWR_OK;
        if (!WF.pwr_step)
            init_system_cfg(); /* the rest of rtw_mac_power_on */
        read_state(&WF.func_en2, &WF.pw_ctrl2, &WF.mcufw2, &WF.cr2);

        /* stage 3b: the firmware. A failure leaves the chip powered off again, as Linux does (err_off). */
        if (!WF.pwr_step && dma_ok)
        {
            if (fw_download() == 0)
            {
                DL.state = DL_OK;
                DL.mcufw = rd16(R_MCUFW_CTRL);
                efuse_read_mac(); /* the chip is powered on and the firmware runs: read the board's data while we can */
                mac_init();       /* stage 4b-1: queues and MAC timing; the receive DMA is switched off again at the end */
                if (MI.state == MI_OK)
                    phy_init((WF.cfg1 >> 12) & 0xF); /* stage 4b-2: the baseband / AGC / RF tables */
                if (MI.state == MI_OK)
                    h2c_probe(); /* stage 4b-3: does the firmware read a command? */
                if (PH.state == PH_OK && !phy_rf_suspect() && H2.state == H2_OK)
                    channel_probe(6); /* stage 4c-1: antenna to Wi-Fi, channel 6 */
                if (CHN.state == CH_OK && DEFER_ENABLE)
                {
                    /* the scan and the connection come later, from link_service(); until then the card is an adapter without a connection */
                    BOOT.pending = 1;
                    LINK.adapter = 1;
                    LINK.up = 0;
                    LINK.want = 0;
                    return 1;
                }
                if (CHN.state == CH_OK)
                    scan_run(); /* stage 4c-2/3: listen on every channel, note the networks */
                if (TXP_ENABLE && SC.state && SC.channels_ok)
                    tx_probe_test(149); /* stage 5a: the first frames on the air: probe requests on channel 149 */
                pick_target();
                connect_try();
            }
            else
            {
                DL.state = DL_FAILED;
                DL.mcufw = rd16(R_MCUFW_CTRL);
            }
            if (LINK.up) /* connected: the card stays on, the DMA engines too, and the network stack takes it */
                return 1;
            if (IDLE_ENABLE && DL.state == DL_OK && MI.state == MI_OK && PH.state == PH_OK && H2.state == H2_OK && CHN.state == CH_OK && SC.state &&
                TXP.variant >= 0 && TXP.sent)
            {
                /* Everything works but no network was joined (no WIFI.CFG, the network was not in range, a wrong passphrase): the card stays on as an
                 * adapter without a connection, so that the Wi-Fi window of the desktop can pick a network (stage 11). */
                LINK.adapter = 1;
                LINK.up = 0;
                LINK.want = 0;
                return 1;
            }
            /* The DMA engines stay on from mac_init() to here, as in Linux: switching the command register off and on again between the
             * stages made the transmit queues lose their pages (see tx_probe_test()). Only now, before the power-off, they go off. */
            wr8(R_CR, 0);
            if (RTL8821CE_OFF_AFTER || DL.state == DL_FAILED)
            {
                WF.pwr_phase = PHASE_OFF;
                DL.off_after = run_steps(POWER_OFF, NPOWER_OFF) == 0 ? 1 : -1;
            }
        }
    }
    return 0; /* never a usable network adapter yet */
}

/* ", fw 24.11 ok" / ", no RTW8821C.BIN" / ", bad RTW8821C.BIN (why)" */
static void status_fw(NicText *t)
{
    switch (FW.state)
    {
    case FW_OK:
        nic_puts(t, ", fw ");
        nic_putd(t, FW.version);
        nic_putc(t, '.');
        nic_putd(t, FW.subversion);
        nic_puts(t, " ok");
        break;
    case FW_MISSING:
        nic_puts(t, ", no " FW_NAME);
        break;
    case FW_BAD:
        nic_puts(t, ", bad fw: ");
        nic_puts(t, FW.why ? FW.why : "?");
        break;
    default:
        break;
    }
}

/* the outcome of stage 3b, appended to the line: ", fw 24.11 loaded and ready (80=c078), off again" / ", load FAILED: why in dmem+4096 (80=0001)" */
static void status_download(NicText *t)
{
    static const char *const PART[] = {"dmem", "imem", "emem"};
    if (DL.state == DL_OK)
    {
        if (PH.state == PH_OK && phy_rf_suspect())
        {
            /* the RF read-back differs: the line has room for the register numbers instead of the usual details */
            nic_puts(t, ", phy rf");
            nic_putd(t, PH.rf_diff);
            nic_putc(t, ':');
            for (unsigned i = 0; i < PH.rf_diff && i < 7; i++) /* register number, then the differing bits */
            {
                nic_puth(t, PH.diff_addr[i], 2);
                nic_puth(t, PH.diff_xor[i], 5);
            }
            nic_puts(t, DL.off_after > 0 ? ", off" : ", power-off FAILED");
            return;
        }
        int all_ok = MI.state == MI_OK && PH.state == PH_OK && H2.state == H2_OK && SC.state; /* then the line has room for the scan result instead of the firmware version */
        if (!all_ok)
        {
            nic_puts(t, ", fw ");
            nic_putd(t, FW.version);
            nic_putc(t, '.');
            nic_putd(t, FW.subversion);
        }
        if (EF.state == EF_OK && !CONN.state) /* (the address leaves the line when a connection was tried: no room) */
        {
            nic_puts(t, ", MAC ");
            for (int i = 0; i < 6; i++)
            {
                nic_puth(t, EF.mac[i], 2);
                if (i < 5)
                    nic_putc(t, ':');
            }
        }
        else if (EF.state == EF_FAILED)
        {
            nic_puts(t, ", efuse: ");
            nic_puts(t, EF.why ? EF.why : "?");
        }
        if (MI.state == MI_OK && PH.state == PH_OK && H2.state == H2_OK) /* the three proven steps in one word: the line has to stay short */
        {
            if (!CONN.state) /* (a connection that got as far as being tried says more than this) */
                nic_puts(t, ", init ok");
        }
        else
        {
        if (MI.state == MI_OK)
            nic_puts(t, ", mac ok");
        else if (MI.state == MI_FAILED)
        {
            nic_puts(t, ", mac FAILED: ");
            nic_puts(t, MI.why ? MI.why : "?");
        }
        if (PH.state == PH_OK)
        {
            nic_puts(t, ", phy ");
            nic_puts(t, "ok");
        }
        else if (PH.state == PH_SKIPPED)
        {
            nic_puts(t, ", no phy: ");
            nic_puts(t, PH.why ? PH.why : "?");
        }
        if (H2.state == H2_OK)
            nic_puts(t, ", h2c ok");
        else if (H2.state == H2_FAILED)
        {
            nic_puts(t, ", h2c ");
            nic_puts(t, H2.why ? H2.why : "?");
        }
        }
        if (SC.state && TXP.state && !TXP.sent && TXP.failed)
        {
            /* the chip took no frame: show what it looked like, instead of the scan summary (the line has room for one of the two) */
            nic_puts(t, ", tx0/");
            nic_putd(t, TXP.failed);
            nic_putc(t, ' ');
            nic_puth(t, TXP.exq_before, 4);
            nic_puth(t, TXP.exq_after, 4);
            nic_putc(t, ' ');
            nic_puth(t, TXP.pub_before, 4);
            nic_puth(t, TXP.pub_after, 4);
        }
        else if (SC.state)
        {
            nic_puts(t, ", ");
            nic_putd(t, SC.n);
            nic_puts(t, "ap");
            if (TXP.state)
            {
                nic_puts(t, ", tx");
                nic_putd(t, TXP.sent);
                if (TXP.variant > 0) /* the plain start did not work, this one did */
                {
                    nic_puts(t, " v");
                    nic_putd(t, (unsigned)TXP.variant);
                }
                if (TXP.failed)
                {
                    nic_putc(t, '/');
                    nic_putd(t, TXP.sent + TXP.failed);
                }
                nic_puts(t, " pr");
                nic_putd(t, TXP.replies);
            }
            if (CFG.state == CFG_OK)
            {
                nic_puts(t, ", net ");
                if (TARGET.found)
                {
                    int why = SC.ap[TARGET.index].rsn_len ? rsn_check(SC.ap[TARGET.index].rsn, SC.ap[TARGET.index].rsn_len) : RSN_BAD_VERSION;
                    if (why == RSN_OK)
                    {
                        nic_puts(t, "ch");
                        nic_putd(t, SC.ap[TARGET.index].channel);
                    }
                    else /* the AP's security is not one this driver can join (RSN_WHY) */
                        nic_puts(t, RSN_WHY[why]);
                }
                else
                    nic_puts(t, "absent");
                if (CONN.state)
                {
                    if (!CONN.auth_seen)
                        nic_puts(t, ", auth none");
                    else if (CONN.auth_status)
                    {
                        nic_puts(t, ", auth s");
                        nic_putd(t, CONN.auth_status);
                    }
                    else if (!CONN.assoc_seen)
                        nic_puts(t, ", auth ok, assoc none");
                    else if (CONN.assoc_status)
                    {
                        nic_puts(t, ", auth ok, assoc s");
                        nic_putd(t, CONN.assoc_status);
                    }
                    else
                    {
                        nic_puts(t, ", assoc ok"); /* (an association needs the authentication to have worked) */
                        if (HS.state)
                        {
                            nic_puts(t, ", 4way ");
                            if (HS.result == HS_OK)
                            {
                                nic_puts(t, "ok");
                                if (HS.keys)
                                {
                                    nic_puts(t, ", dec ");
                                    nic_putd(t, HS.dec < 99 ? HS.dec : 99);
                                    if (HS.icv)
                                    {
                                        nic_puts(t, ", icv ");
                                        nic_putd(t, HS.icv < 99 ? HS.icv : 99);
                                    }
                                }
                            }
                            else if (HS.result == 0)
                                nic_puts(t, "none");
                            else if (HS.result == WPA4_E_MIC)
                                nic_puts(t, "mic"); /* the signature of message 3 does not fit: the passphrase is wrong */
                            else
                            {
                                nic_putc(t, 'e');
                                nic_putd(t, (unsigned)(-HS.result));
                            }
                        }
                    }
                }
            }
            else if (CFG.state == CFG_BAD)
            {
                nic_puts(t, ", cfg ");
                nic_puts(t, CFG.why ? CFG.why : "?");
            }
            else if (CFG.state == CFG_NONE)
                nic_puts(t, ", no cfg");
        }
        else if (CHN.state == CH_OK)
        {
            nic_puts(t, ", ch");
            nic_putd(t, CHN.channel);
            nic_puts(t, " ok");
        }
        else if (CHN.state == CH_FAILED)
        {
            nic_puts(t, ", ch");
            nic_putd(t, CHN.channel);
            nic_puts(t, " rf18=");
            nic_puth(t, CHN.rf18, 5);
        }
        nic_puts(t, DL.off_after > 0 ? ", off" : DL.off_after < 0 ? ", power-off FAILED" : "");
    }
    else if (DL.state == DL_FAILED)
    {
        nic_puts(t, ", load FAILED: ");
        nic_puts(t, DL.why ? DL.why : "?");
        if (DL.piece)
        {
            nic_puts(t, " in ");
            nic_puts(t, PART[DL.part < 3 ? DL.part : 0]);
            nic_putc(t, '+');
            nic_putd(t, DL.off);
        }
        nic_puts(t, " (80=");
        nic_puth(t, DL.mcufw, 4);
        nic_putc(t, ')');
    }
    else if (DL.state == DL_SKIPPED)
    {
        status_fw(t);
        nic_puts(t, ", not loaded: ");
        nic_puts(t, DL.why ? DL.why : "?");
    }
}

static void rtl8821ce_status(char *buf, unsigned cap)
{
    NicText t;
    nic_text_init(&t, buf, cap);
    if (LINK.adapter)
    {
        nic_puts(&t, "RTL8821CE Wi-Fi: ");
        if (!LINK.up && !LINK.want) /* a card without a network: nothing of a connection to show */
        {
            nic_puts(&t, "not connected");
            return;
        }
        nic_puts(&t, CFG.ssid);
        nic_puts(&t, " ch");
        nic_putd(&t, CONN.channel);
        nic_puts(&t, link_is_up() ? ", up" : LINK.want ? ", reconnecting" : ", not connected");
        if (RA.on) /* the firmware chooses the rates: HT or VHT, at 20 MHz */
            nic_puts(&t, RA.vht ? ", vht" : ", ht");
        if (RA.on && RA.bw) /* ... and the width, when it is more than 20 MHz */
            nic_putd(&t, RA.bw == 2 ? 80 : 40);
        if (RA.on && (BA.rx_on || BA.tx_state == 2)) /* aggregation is in use, in either direction */
            nic_puts(&t, ", agg");
        nic_puts(&t, ", tx ");
        nic_putd(&t, LINK.tx_frames);
        nic_puts(&t, " rx ");
        nic_putd(&t, LINK.rx_frames);
        if (LINK.reconnects) /* (only when it happened: rc = reconnections, gk = group key renewals) */
        {
            nic_puts(&t, ", rc ");
            nic_putd(&t, LINK.reconnects);
        }
        if (LINK.rekeys)
        {
            nic_puts(&t, ", gk ");
            nic_putd(&t, LINK.rekeys);
        }
        return;
    }
    nic_puts(&t, "RTL8821CE (no driver): ");
    if (!WF.ok)
    {
        nic_puts(&t, WF.fail ? WF.fail : "not probed");
        return;
    }
    nic_puts(&t, "cut ");
    unsigned cut = (WF.cfg1 >> 12) & 0xF;
    nic_putc(&t, cut < 7 ? (char)('A' + cut) : '?');
    switch (WF.pwr_state)
    {
    case PWR_OK:
    case PWR_FAILED:
        if (DL.state == DL_OK || DL.state == DL_FAILED || DL.state == DL_SKIPPED)
        {
            if (DL.state != DL_OK)
                nic_puts(&t, ", powered on");
            status_download(&t);
            break;
        }
        nic_puts(&t, WF.was_on ? ", was on, " : ", was off, ");
        if (WF.pwr_state == PWR_OK)
            nic_puts(&t, "powered on");
        else
        {
            nic_puts(&t, WF.pwr_phase == PHASE_OFF ? "power-off FAILED at step " : "power-on FAILED at step ");
            nic_putd(&t, (unsigned)WF.pwr_step);
        }
        nic_puts(&t, " (CR=");
        nic_puth(&t, WF.cr2, 2);
        nic_puts(&t, " 02=");
        nic_puth(&t, WF.func_en2, 4);
        nic_puts(&t, " 04=");
        nic_puth(&t, WF.pw_ctrl2, 4);
        nic_putc(&t, ')');
        if (DL.state == DL_SKIPPED)
            status_download(&t);
        else
            status_fw(&t);
        break;
    default: /* only read: show the registers as they were */
        nic_puts(&t, " CR=");
        nic_puth(&t, WF.cr, 2);
        nic_puts(&t, " 80=");
        nic_puth(&t, WF.mcufw, 4);
        nic_puts(&t, " 02=");
        nic_puth(&t, WF.func_en, 4);
        nic_puts(&t, " 04=");
        nic_puth(&t, WF.pw_ctrl, 4);
        nic_puts(&t, WF.pwr_state == PWR_LEFT_ALONE ? " (firmware resident: left alone)" : WF.pwr_state == PWR_DISABLED ? " (read only build)" : "");
    }
}


const NicDriver NIC_RTL8821CE = {
    "Realtek RTL8821CE (Wi-Fi)", rtl8821ce_claims, rtl8821ce_probe, rtl8821ce_mac_up, link_is_up, wifi_send,
    rtl8821ce_recv_up, rtl8821ce_status, 0, 0, 0, 0,
};
