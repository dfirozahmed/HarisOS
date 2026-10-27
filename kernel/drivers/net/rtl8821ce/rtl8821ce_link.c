/* RTL8821CE: keeping the connection and starting in the background. */
#include "rtl8821ce_internal.h"

/* ---- keeping the connection ----
 * A Wi-Fi link is not a cable: it ends when the access point reboots, when we are out of range for a while, when it changes the channel, or when it
 * simply decides that we are gone. And it needs care while it lasts: the access point renews the group key (the one for broadcast traffic) about
 * once an hour and throws out a station that does not take part. link_service(), called from every poll of the network stack, does
 *   - noticing the end: a deauthentication / disassociation (conn_note), or silence. The access point sends a beacon about every 100 ms, so after
 *     LINK_QUIET_MS without any frame from it we send it a probe request (it answers those) every LINK_PROBE_MS, and after LINK_DEAD_MS we give up.
 *   - connecting again: authenticate, associate, 4-way handshake on the old channel; every third failure a new scan first (the access point may be
 *     on another channel), with the pause between attempts doubling from 0.5 s to 30 s. The network stack notices nothing but a few lost packets.
 *   - the group key renewal and a repeated message 3 (see keys_service). */

/* a frame the receive path heard (not a control frame, without FCS): is it from our access point? Then it is alive. */
void alive_note(const volatile u8 *f, unsigned len)
{
    if (!LINK.up || len < 24 || !mac_is(f + 10, CONN.bssid))
        return;
    LINK.last_rx = now_ms();
}

/* a protected EAPOL-Key frame from the access point while the link is up (wifi_rx_data put it in EAPQ) */
static void keys_service(void)
{
    static const u8 BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    u8 reply[EAPOL_MAX];
    unsigned rl = 0;
    unsigned ki = EAPQ.len >= 7 ? (unsigned)EAPQ.b[5] << 8 | EAPQ.b[6] : 0;
    int r = WPA4_IGNORED;
    if (!(ki & 0x08)) /* no pairwise bit: the group key renewal */
    {
        r = wpa4_group_rx(&W4, EAPQ.b, EAPQ.len, reply, &rl);
        if (r == WPA4_SEND_GROUP2)
        {
            /* the new key goes in before the answer leaves: the access point may use it at once. The group keys sit in the entries 0..3 by key
             * id, so the old key stays valid next to it until the access point stops using it. */
            cam_write(W4.gtk_id, W4.gtk, W4.gtk_id, 1, BCAST);
            LINK.rx_pn_group[W4.gtk_id & 3] = 0;
            LINK.rekeys++;
        }
    }
    else if (ki & 0x100)
    {
        /* message 3 of the 4-way handshake again: our message 4 was lost and the access point asks once more. It is answered, but the keys are NOT
         * installed a second time: that would start the packet numbers again under the same key, and a packet number must never be used twice.
         * (A new message 1, which would renew the pairwise key, is not handled; an access point that insists ends the link and we connect again.) */
        static const u8 NO_NONCE[32];
        r = wpa4_rx(&W4, EAPQ.b, EAPQ.len, NO_NONCE, reply, &rl);
    }
    EAPQ.pending = 0;
    if (r == WPA4_SEND_GROUP2 || r == WPA4_SEND_MSG4)
    {
        u8 e[14 + EAPOL_MAX];
        for (int i = 0; i < 6; i++)
        {
            e[i] = CONN.bssid[i];
            e[6 + i] = EF.mac[i];
        }
        e[12] = 0x88;
        e[13] = 0x8E;
        for (unsigned i = 0; i < rl; i++)
            e[14 + i] = reply[i];
        wifi_send(e, 14 + rl); /* encrypted with the pairwise key, like everything now */
    }
    else if (r < 0)
        LINK.eapol_bad++;
}

/* the end of the link: no data until it is made again; the keys are wiped from the chip, the first new attempt is at once */
static void link_lost(u64 now)
{
    LINK.up = 0;
    ra_stop();
    ba_reset();
    LINK.fails = 0;
    LINK.backoff = LINK_RETRY_MIN_MS;
    LINK.next_try = now;
    cam_clear();
}

static void reconnect_try(void)
{
    if (LINK.fails % 3 == 2) /* looked on the old channel twice in vain: where is the access point now? */
    {
        scan_run();
        pick_target();
        if (TARGET.found)
        {
            const ScanAp *ap = &SC.ap[TARGET.index];
            if (rsn_check(ap->rsn, ap->rsn_len) == RSN_OK)
            {
                for (int i = 0; i < 6; i++)
                    CONN.bssid[i] = ap->bssid[i];
                CONN.channel = ap->channel;
                conn_set_caps(ap);
            }
        }
        tune_chan(&CONN.cc); /* (the scan left the radio on its last channel) */
    }
    cam_clear();
    connect_exchange();
    u64 now = now_ms();
    if (LINK.up)
    {
        LINK.fails = 0;
        LINK.reconnects++;
        return;
    }
    LINK.fails++;
    LINK.next_try = now + LINK.backoff;
    LINK.backoff = LINK.backoff * 2 > LINK_RETRY_MAX_MS ? LINK_RETRY_MAX_MS : LINK.backoff * 2;
}

void scan_service(u64 now);
int scan_active(void);
int rtl8821ce_wifi_scan_start(void);

/* ---- starting in the background ----
 * The first scan listens on every channel for about 0.3 s each (5 s in all) and the connection after it takes a while too. Done inside probe() they hold
 * up the whole boot, with a black screen. With DEFER_ENABLE probe() only brings the hardware up (power, firmware, MAC, baseband, radio) and returns the
 * card as an adapter without a connection; the first scan, the first frames on the air (tx_probe_test) and the connection to the network of WIFI.CFG
 * follow from link_service(), in the steps of the stage 11 scan, once the clock runs. Nothing is lost: it is the same sequence as before. */
int DEFER_ENABLE = 1; /* the host tests of the earlier stages switch it off (they look at the result of probe()) */

/* the end of the first scan: the rest of what probe() does when it is not deferred */
void boot_finish(void)
{
    BOOT.pending = 0;
    if (TXP_ENABLE && SC.state && SC.channels_ok)
        tx_probe_test(149); /* stage 5a: the first frames on the air: probe requests on channel 149 */
    pick_target();
    connect_try();
}

/* The same work in a thread of its own (the kernel starts one, kernel.c): the whole first scan and the connection take several seconds, and the
 * desktop must not stand still for them. The thread does it blocking, the way probe() does without DEFER_ENABLE. begin() is called early (it claims the
 * chip, so that nothing else touches it before the thread runs), run() is the thread, abort() gives the chip back if no thread could be made, and then
 * link_service() does the work in steps. */
int rtl8821ce_boot_begin(void)
{
    if (!BOOT.pending || BOOT.busy || !LINK.adapter || !D.ready || !clock_ok())
        return 0;
    BOOT.busy = 1;
    return 1;
}

void rtl8821ce_boot_run(void)
{
    if (!BOOT.busy)
        return;
    scan_run(); /* stage 4c-2/3: listen on every channel, note the networks */
    boot_finish();
    BOOT.busy = 0;
}

void rtl8821ce_boot_abort(void) { BOOT.busy = 0; }

void link_service(void)
{
    if (!clock_ok())
        return;
    u64 now = now_ms();
    if (BOOT.pending && !scan_active())
        rtl8821ce_wifi_scan_start(); /* (the first scan; boot_finish() runs at its end) */
    scan_service(now); /* (a scan in steps goes on from here) */
    if (LINK.up && !scan_active()) /* (while the radio is away on other channels nothing is heard from the access point) */
    {
        u64 quiet = now - LINK.last_rx;
        if (CONN.dropped || quiet >= LINK_DEAD_MS)
            link_lost(now);
        else if (quiet >= LINK_QUIET_MS && now - LINK.last_probe >= LINK_PROBE_MS)
        {
            u8 f[64];
            unsigned n = build_probe_request(f, EF.mac, CONN.channel);
            for (int i = 0; i < 6; i++) /* directed: addressed to the access point, which answers it with a probe response */
            {
                f[4 + i] = CONN.bssid[i];
                f[16 + i] = CONN.bssid[i];
            }
            tx_mgmt(f, n, CONN.channel);
            LINK.last_probe = now;
        }
    }
    if (LINK.up && !scan_active()) /* (while the radio is away on other channels nothing is heard from the access point) */
    {
        if (EAPQ.pending)
            keys_service();
        if (ACTQ.pending)
            act_service();
        ba_expire(now);
        ba_tx_service(now);
    }
    else if (LINK.want && !scan_active() && now >= LINK.next_try)
        reconnect_try();
}
