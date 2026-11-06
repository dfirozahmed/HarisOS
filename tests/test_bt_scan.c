/* Host test of Bluetooth device discovery (kernel/bluetooth/bt_scan.c) against a fake chip that plays back scripted events. */
#include <stdio.h>
#include <string.h>

#include "bluetooth.h"

static int failures, checks;
#define CHECK(c)                                                  \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(c))                                                 \
        {                                                         \
            failures++;                                           \
            printf("FAIL line %d: %s\n", __LINE__, #c);           \
        }                                                         \
    } while (0)

/* ---- the fake chip ---- */
static struct
{
    unsigned sent[64];
    int nsent;
    unsigned char mask[8];
    int have_mask;
    int inq_mode;           /* last Write Inquiry Mode parameter, -1 = never */
    unsigned char features[8];
    int inquiry_status;     /* status in the Command Status of Inquiry */
    unsigned char inq_par[5];
    unsigned char name_req[10];
    int name_req_new;       /* a Remote Name Request arrived and has not been answered by the test yet */
    int cancels;            /* Remote Name Request Cancel / Inquiry Cancel seen */
    unsigned char pend[270];
    int have;
    unsigned long long clock;
} F;

static void cc(unsigned op, const unsigned char *ret, int rl)
{
    F.pend[0] = 0x0E;
    F.pend[1] = (unsigned char)(3 + rl);
    F.pend[2] = 1;
    F.pend[3] = (unsigned char)op;
    F.pend[4] = (unsigned char)(op >> 8);
    memcpy(F.pend + 5, ret, (size_t)rl);
    F.have = 5 + rl;
}

static void cs(unsigned op, int status)
{
    F.pend[0] = 0x0F;
    F.pend[1] = 4;
    F.pend[2] = (unsigned char)status;
    F.pend[3] = 1;
    F.pend[4] = (unsigned char)op;
    F.pend[5] = (unsigned char)(op >> 8);
    F.have = 6;
}

static int fake_send(void *ctx, const unsigned char *pkt, unsigned len)
{
    (void)ctx;
    unsigned op = pkt[0] | ((unsigned)pkt[1] << 8);
    if (F.nsent < 64)
        F.sent[F.nsent++] = op;
    (void)len;
    unsigned char ok = 0;
    switch (op)
    {
    case 0x1003:
    {
        unsigned char r[9] = {0};
        memcpy(r + 1, F.features, 8);
        cc(op, r, 9);
        break;
    }
    case 0x0C01:
        memcpy(F.mask, pkt + 3, 8);
        F.have_mask = 1;
        cc(op, &ok, 1);
        break;
    case 0x0C45:
        F.inq_mode = pkt[3];
        cc(op, &ok, 1);
        break;
    case 0x0401:
        memcpy(F.inq_par, pkt + 3, 5);
        cs(op, F.inquiry_status);
        break;
    case 0x0402:
        F.cancels++;
        cc(op, &ok, 1);
        break;
    case 0x0419:
        memcpy(F.name_req, pkt + 3, 10);
        F.name_req_new = 1;
        cs(op, 0);
        break;
    case 0x041A:
        F.cancels++;
        cc(op, &ok, 1);
        break;
    case 0x0C03:
        cc(op, &ok, 1);
        break;
    case 0x1001:
    {
        const unsigned char r[9] = {0, 8, 0x0C, 0, 8, 0x5D, 0, 0x21, 0x88};
        cc(op, r, 9);
        break;
    }
    case 0x1009:
    {
        const unsigned char r[7] = {0, 1, 2, 3, 4, 5, 6};
        cc(op, r, 7);
        break;
    }
    default:
    {
        unsigned char bad = 1;
        cc(op, &bad, 1);
    }
    }
    return 0;
}

static void fake_wait(void *ctx)
{
    (void)ctx;
    if (F.have)
    {
        bt_rx_event(F.pend, (unsigned)F.have);
        F.have = 0;
    }
}

static int fake_alive(void *ctx)
{
    (void)ctx;
    return 1;
}

static unsigned long long fake_now(void *ctx)
{
    (void)ctx;
    return F.clock;
}

static const BtTransport T = {fake_send, fake_wait, fake_alive, 0, fake_now, 0};

static void fresh(void)
{
    bt_detach();
    memset(&F, 0, sizeof F);
    F.inq_mode = -1;
    F.features[3] = 0x40;
    F.features[6] = 0x01;
    F.clock = 1000;
    bt_attach(&T, 0x0BDA, 0xB00A);
}

/* ---- event builders ---- */
static void push(const unsigned char *e)
{
    bt_rx_event(e, (unsigned)e[1] + 2u);
}

static void ev_inq(const unsigned char *addr, const unsigned char *cls, unsigned clock)
{
    unsigned char e[2 + 1 + 14] = {0x02, 15, 1};
    memcpy(e + 3, addr, 6);
    e[9] = 1; /* page scan repetition mode */
    e[10] = 0;
    e[11] = 0;
    memcpy(e + 12, cls, 3);
    e[15] = (unsigned char)clock;
    e[16] = (unsigned char)(clock >> 8);
    push(e);
}

static void ev_rssi(const unsigned char *addr, const unsigned char *cls, int rssi)
{
    unsigned char e[2 + 1 + 14] = {0x22, 15, 1};
    memcpy(e + 3, addr, 6);
    e[9] = 1;
    e[10] = 0;
    memcpy(e + 11, cls, 3);
    e[14] = 0x34;
    e[15] = 0x12;
    e[16] = (unsigned char)(signed char)rssi;
    push(e);
}

static void ev_ext(const unsigned char *addr, const unsigned char *cls, int rssi, const unsigned char *eir, unsigned eirlen)
{
    unsigned char e[2 + 255] = {0x2F, 255, 1};
    memcpy(e + 3, addr, 6);
    e[9] = 1;
    e[10] = 0;
    memcpy(e + 11, cls, 3);
    e[14] = 0x34;
    e[15] = 0x12;
    e[16] = (unsigned char)(signed char)rssi;
    memcpy(e + 17, eir, eirlen);
    push(e);
}

static void ev_complete(int status)
{
    unsigned char e[3] = {0x01, 1, (unsigned char)status};
    push(e);
}

static void ev_name(const unsigned char *addr, int status, const char *name)
{
    unsigned char e[2 + 255] = {0x07, 255, (unsigned char)status};
    memcpy(e + 3, addr, 6);
    size_t n = strlen(name);
    if (n > 248)
        n = 248;
    memcpy(e + 9, name, n);
    push(e);
}

static const unsigned char A[6] = {0x60, 0xaa, 0x0b, 0x80, 0x6c, 0x64}; /* shown as 64:6c:80:0b:aa:60 */
static const unsigned char Bd[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0xb0};
static const unsigned char C[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xc0};
static const unsigned char D[6] = {0x03, 0x00, 0x00, 0x00, 0x00, 0xd0};
static const unsigned char CLS_PHONE[3] = {0x0C, 0x02, 0x5A};      /* major 2 */
static const unsigned char CLS_HEADPHONES[3] = {0x18, 0x04, 0x24}; /* major 4, minor 6 */
static const unsigned char CLS_KEYBOARD[3] = {0x40, 0x05, 0x00};   /* major 5, minor bits 6..7 = 01 */
static const unsigned char CLS_MOUSE[3] = {0x80, 0x05, 0x00};
static const unsigned char CLS_OTHER[3] = {0, 0x1F, 0};

static int line_is(unsigned i, const char *want)
{
    char buf[256];
    int n = bt_scan_line(i, buf, sizeof buf);
    if (n < 0 || strcmp(buf, want) != 0)
    {
        printf("  line %u: got \"%s\" (%d), want \"%s\"\n", i, n < 0 ? "-" : buf, n, want);
        return 0;
    }
    return n == (int)strlen(want);
}

/* run the step loop until the scan is over, answering name requests from a small table */
static void run_names(const unsigned char *a1, const char *n1, int st1, const unsigned char *a2, const char *n2, int st2)
{
    for (int guard = 0; guard < 200 && bt_scan_state() != BT_SCAN_DONE; guard++)
    {
        bt_poll();
        if (F.name_req_new)
        {
            F.name_req_new = 0;
            if (a1 && memcmp(F.name_req, a1, 6) == 0)
                ev_name(a1, st1, n1);
            else if (a2 && memcmp(F.name_req, a2, 6) == 0)
                ev_name(a2, st2, n2);
        }
        F.clock += 50;
    }
}

static int HOOK_MODE = -1;
static void hook_set_mode(int mode) { HOOK_MODE = mode; }

int main(void)
{
    /* 1. not up / no chip */
    bt_detach();
    CHECK(bt_scan_start() == BT_E_NOT_UP);
    CHECK(bt_scan_state() == BT_SCAN_IDLE);
    CHECK(line_is(0, "idle: no scan yet"));
    bt_poll(); /* harmless without a chip */

    /* 2. the full story: four devices, three kinds of answer, names asked for */
    fresh();
    CHECK(bt_scan_start() == 0);
    CHECK(F.inq_mode == 2 && F.have_mask && F.mask[5] == 0x5F && F.mask[0] == 0xFF); /* setup ran first */
    CHECK(memcmp(F.inq_par, "\x33\x8b\x9e\x08\x00", 5) == 0);                        /* GIAC, 8 x 1.28 s, no limit */
    CHECK(bt_scan_state() == BT_SCAN_INQUIRY);
    CHECK(line_is(0, "scanning: 0 found so far"));
    ev_inq(A, CLS_PHONE, 0x1111);
    ev_inq(Bd, CLS_KEYBOARD, 0x2222);
    ev_rssi(C, CLS_MOUSE, -60);
    unsigned char eir_d[] = {2, 0x01, 0x06, 11, 0x09, 'H', 'e', 'a', 'd', 'p', 'h', 'o', 'n', 'e', 's', 0};
    ev_ext(D, CLS_HEADPHONES, -45, eir_d, sizeof eir_d);
    unsigned char eir_a[] = {6, 0x08, 'P', 'h', 'o', 'n', 'e', 0}; /* shortened name */
    ev_ext(A, CLS_PHONE, -70, eir_a, sizeof eir_a);                 /* the same device again, now with RSSI and a name */
    bt_poll();
    CHECK(bt_scan_count() == 4);
    CHECK(line_is(0, "scanning: 4 found so far"));
    ev_complete(0);
    bt_poll(); /* the poll sends the first name request (for B) */
    CHECK(bt_scan_state() == BT_SCAN_NAMES);
    CHECK(F.name_req_new && memcmp(F.name_req, Bd, 6) == 0 && F.name_req[6] == 1 && F.name_req[8] == 0x22 && F.name_req[9] == (0x22 | 0x80));
    run_names(Bd, "Keyboard K", 0, C, "", 4);
    CHECK(bt_scan_state() == BT_SCAN_DONE);
    CHECK(line_is(0, "done: 4 devices"));
    CHECK(line_is(1, "64:6c:80:0b:aa:60  rssi -70  phone  Phone"));
    CHECK(line_is(2, "b0:00:00:00:00:01  rssi ?  keyboard  Keyboard K"));
    CHECK(line_is(3, "c0:00:00:00:00:02  rssi -60  pointing device  (no name)"));
    CHECK(line_is(4, "d0:00:00:00:00:03  rssi -45  headphones  Headphones"));
    char buf[256];
    CHECK(bt_scan_line(5, buf, sizeof buf) == -1);
    CHECK(bt_scan_dev(0) && bt_scan_dev(3) && !bt_scan_dev(4) && !bt_scan_dev(-1));
    /* Remote Name Requests went only to the devices that had no name (B and C), the Inquiry mode was the extended one */
    int nreq = 0;
    for (int i = 0; i < F.nsent; i++)
        nreq += F.sent[i] == 0x0419;
    CHECK(nreq == 2);

    /* 3. answers of an inquiry that is over are ignored; the list only changes by a new scan */
    ev_inq(D, CLS_PHONE, 0);
    ev_rssi(D, CLS_PHONE, -1);
    bt_poll();
    CHECK(line_is(4, "d0:00:00:00:00:03  rssi -45  headphones  Headphones"));

    /* 4. a second scan clears the list and does not set the chip up again */
    int sent_before = F.nsent;
    CHECK(bt_scan_start() == 0);
    CHECK(bt_scan_count() == 0 && F.sent[sent_before] == 0x0401); /* the first command is the Inquiry itself */
    CHECK(bt_scan_start() == BT_E_SCANNING);

    /* 5. no "Inquiry Complete" ever: after the limit we cancel and go on */
    F.clock += 20000;
    bt_poll();
    CHECK(F.cancels == 1 && bt_scan_state() == BT_SCAN_DONE);
    CHECK(line_is(0, "done: 0 devices, inquiry ended with -2; inquiry 20000 ms, 2 events, 0 results, 0 hw errors"));

    /* 6. a name request nobody answers: given up after the time limit, with a cancel */
    fresh();
    CHECK(bt_scan_start() == 0);
    ev_inq(A, CLS_OTHER, 0);
    ev_complete(0);
    bt_poll();
    CHECK(F.name_req_new && bt_scan_state() == BT_SCAN_NAMES);
    F.clock += 7000;
    bt_poll();
    CHECK(F.cancels == 1 && bt_scan_state() == BT_SCAN_NAMES);
    bt_poll(); /* nothing left to ask */
    CHECK(bt_scan_state() == BT_SCAN_DONE && line_is(1, "64:6c:80:0b:aa:60  rssi ?  device  (no name)"));
    ev_name(A, 0, "Late"); /* the answer comes after we gave up: no effect */
    bt_poll();
    CHECK(line_is(1, "64:6c:80:0b:aa:60  rssi ?  device  (no name)"));

    /* 7. the chip refuses the Inquiry */
    fresh();
    F.inquiry_status = 0x0C;
    CHECK(bt_scan_start() == 0x0C);
    CHECK(bt_scan_state() == BT_SCAN_DONE && line_is(0, "done: 0 devices, inquiry ended with status 0c"));
    F.inquiry_status = 0;
    CHECK(bt_scan_start() == 0); /* and it can be tried again */

    /* 8. a chip without the features: no inquiry mode command; list overflow; bad events */
    fresh();
    memset(F.features, 0, 8);
    F.features[3] = 0x40; /* RSSI only */
    CHECK(bt_scan_start() == 0 && F.inq_mode == 1);
    fresh();
    memset(F.features, 0, 8);
    CHECK(bt_scan_start() == 0 && F.inq_mode == -1);
    for (int i = 0; i < 20; i++)
    {
        unsigned char ad[6] = {(unsigned char)i, 0, 0, 0, 0, 0x77};
        ev_inq(ad, CLS_OTHER, 0);
        bt_poll(); /* the event queue of the core holds 16, so drain it as the main loop does */
    }
    unsigned char junk1[] = {0x02, 1, 5};             /* says 5 devices, has none */
    unsigned char junk2[] = {0x2F, 3, 1, 2, 3};       /* extended result cut short */
    unsigned char junk3[] = {0x07, 2, 0, 1};          /* name event cut short */
    push(junk1);
    push(junk2);
    push(junk3);
    bt_poll();
    CHECK(bt_scan_count() == 16);
    ev_complete(0);
    for (int guard = 0; guard < 400 && bt_scan_state() != BT_SCAN_DONE; guard++)
    {
        bt_poll();
        F.name_req_new = 0;
        F.clock += 7000; /* nobody answers; each request times out */
    }
    CHECK(bt_scan_state() == BT_SCAN_DONE);
    CHECK(line_is(0, "done: 16 devices, list full"));

    /* 9. names: a long name is cut at a character boundary, EIR parsing survives odd data */
    fresh();
    CHECK(bt_scan_start() == 0);
    {
        unsigned char eir[240] = {0};
        unsigned n = 0;
        eir[n++] = 60;
        eir[n++] = 0x09;
        eir[n++] = 'A'; /* then 17 x (3-byte character): the limit of 39 bytes falls inside the 13th character, which must go whole */
        for (int i = 0; i < 17; i++)
        {
            eir[n++] = 0xE0;
            eir[n++] = 0xA6;
            eir[n++] = 0x85;
        }
        ev_ext(A, CLS_OTHER, -50, eir, n + 8);
        unsigned char odd[240] = {250, 0x09, 'x'}; /* a length that runs over the end */
        ev_ext(Bd, CLS_OTHER, -50, odd, sizeof odd);
        unsigned char zero[240] = {0};            /* an empty response */
        ev_ext(C, CLS_OTHER, -50, zero, sizeof zero);
        bt_poll();
    }
    const BtScanDev *d = bt_scan_dev(0);
    CHECK(d && d->name_state == 1 && strlen(d->name) == 37 && d->name[0] == 'A' && (unsigned char)d->name[36] == 0x85);
    CHECK(bt_scan_dev(1) && bt_scan_dev(1)->name_state == 0 && bt_scan_dev(2) && bt_scan_dev(2)->name_state == 0);

    /* 10. text lines: a line that does not fit is refused, not cut */
    CHECK(bt_scan_line(1, buf, 10) == -1);
    CHECK(bt_scan_line(0, buf, 1) == -1);
    CHECK(bt_scan_line(0, buf, 2) == -1); /* too small for any real line */

    /* 11. class words */
    {
        unsigned char c[3] = {0, 0, 0};
        const unsigned char X[6] = {1, 1, 1, 1, 1, 1};
        struct { unsigned char c0, c1; const char *word; } K[] = {
            {0x00, 0x01, "computer"}, {0x00, 0x03, "network"}, {0x04, 0x04, "headset"}, {0x08, 0x04, "hands-free"}, {0x14, 0x04, "speaker"},
            {0x1C, 0x04, "portable audio"}, {0x00, 0x04, "audio/video"}, {0xC0, 0x05, "keyboard+mouse"}, {0x00, 0x05, "input device"},
            {0x00, 0x06, "imaging"}, {0x00, 0x07, "wearable"}, {0x00, 0x08, "toy"}, {0x00, 0x09, "health"}, {0x00, 0x00, "device"}};
        for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++)
        {
            fresh();
            bt_scan_start();
            c[0] = K[i].c0;
            c[1] = K[i].c1;
            ev_inq(X, c, 0);
            bt_poll();
            char want[100];
            snprintf(want, sizeof want, "01:01:01:01:01:01  rssi ?  %s  (name?)", K[i].word);
            CHECK(line_is(1, want));
        }
    }

    /* 12. an inquiry that ends by itself but heard nothing: the line says how long it took and how many events came (the Command Status of
     * the Inquiry and the Inquiry Complete), so "nobody there" can be told from "the radio is blocked" */
    fresh();
    CHECK(bt_scan_start() == 0);
    F.clock += 10240;
    ev_complete(0);
    bt_poll();
    bt_poll();
    CHECK(bt_scan_state() == BT_SCAN_DONE);
    CHECK(line_is(0, "done: 0 devices; inquiry 10240 ms, 2 events, 0 results, 0 hw errors"));
    /* a hardware error event is counted */
    fresh();
    CHECK(bt_scan_start() == 0);
    {
        unsigned char hw[3] = {0x10, 1, 0x42};
        push(hw);
    }
    F.clock += 500;
    ev_complete(0);
    bt_poll();
    bt_poll();
    CHECK(line_is(0, "done: 0 devices; inquiry 500 ms, 3 events, 0 results, 1 hw errors"));
    /* with devices the diagnostics are left out */
    fresh();
    CHECK(bt_scan_start() == 0);
    ev_inq(A, CLS_OTHER, 0);
    F.clock += 100;
    ev_complete(0);
    run_names(A, "X", 0, 0, "", 0);
    CHECK(line_is(0, "done: 1 device"));

    /* 13. the antenna hook: nothing happens without one; with one, the mode is passed on */
    bt_coex(1); /* no hook yet: harmless */
    bt_set_coex_hook(hook_set_mode);
    bt_coex(2);
    CHECK(HOOK_MODE == 2);
    bt_coex(0);
    CHECK(HOOK_MODE == 0);
    bt_set_coex_hook(0);
    bt_coex(1);
    CHECK(HOOK_MODE == 0); /* the old hook was not called */

    printf("bt_scan: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
