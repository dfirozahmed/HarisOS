/* Host test of the Bluetooth HCI core (kernel/bluetooth/bt_hci.c) against a fake chip. */
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
    unsigned sent[16];         /* opcodes the host sent, in order */
    int nsent;
    int reset_status;          /* status the chip answers to Reset */
    int silent;                /* never answers */
    int dead;                  /* transport reports "unplugged" */
    int rom_unknown;           /* vendor command answered with status 1 */
    int noise_first;           /* send a Hardware Error event before every answer */
    int wrong_opcode_first;    /* send a Command Complete for another opcode before the answer */
    int use_cmd_status;        /* answer Reset with Command Status instead of Command Complete */
    unsigned char pending[8][260];
    int npending;
    int steps;
} F;

static void push_ev(const unsigned char *e, int len)
{
    memcpy(F.pending[F.npending++], e, (size_t)len);
}

static void cc(unsigned op, const unsigned char *ret, int rl)
{
    unsigned char e[260];
    e[0] = 0x0E;
    e[1] = (unsigned char)(3 + rl);
    e[2] = 1; /* command packets the chip can take */
    e[3] = (unsigned char)op;
    e[4] = (unsigned char)(op >> 8);
    memcpy(e + 5, ret, (size_t)rl);
    push_ev(e, 5 + rl);
}

static int fake_send(void *ctx, const unsigned char *pkt, unsigned len)
{
    (void)ctx;
    unsigned op = pkt[0] | ((unsigned)pkt[1] << 8);
    F.sent[F.nsent++] = op;
    if (len != 3u + pkt[2])
        return -1;
    if (F.silent)
        return 0;
    if (F.noise_first)
    {
        unsigned char hw[3] = {0x10, 1, 0x42};
        push_ev(hw, 3);
    }
    if (F.wrong_opcode_first)
    {
        unsigned char z = 0;
        cc(0x1234, &z, 1);
    }
    if (op == 0x0C03)
    {
        if (F.use_cmd_status)
        {
            unsigned char e[6] = {0x0F, 4, (unsigned char)F.reset_status, 1, 0x03, 0x0C};
            push_ev(e, 6);
        }
        else
        {
            unsigned char s = (unsigned char)F.reset_status;
            cc(op, &s, 1);
        }
    }
    else if (op == 0x1001)
    {
        /* status, hci ver 8, rev 0x000C, lmp ver 8, manufacturer 93 (0x005D), lmp sub 0x8821 */
        const unsigned char r[9] = {0, 8, 0x0C, 0x00, 8, 0x5D, 0x00, 0x21, 0x88};
        cc(op, r, 9);
    }
    else if (op == 0xFC6D)
    {
        if (F.rom_unknown)
        {
            unsigned char s = 1;
            cc(op, &s, 1);
        }
        else
        {
            const unsigned char r[2] = {0, 3};
            cc(op, r, 2);
        }
    }
    else if (op == 0x1005)
    {
        /* status, ACL length 1021, SCO length 64, 8 ACL packets, 4 SCO packets */
        const unsigned char r[8] = {0, 0xFD, 0x03, 0x40, 0x08, 0x00, 0x04, 0x00};
        cc(op, r, 8);
    }
    else if (op == 0x1009)
    {
        const unsigned char r[7] = {0, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}; /* least significant byte first */
        cc(op, r, 7);
    }
    else
    {
        unsigned char s = 1; /* unknown command */
        cc(op, &s, 1);
    }
    return 0;
}

static void fake_wait(void *ctx)
{
    (void)ctx;
    F.steps++;
    for (int i = 0; i < F.npending; i++)
    {
        unsigned len = (unsigned)F.pending[i][1] + 2;
        bt_rx_event(F.pending[i], len);
    }
    F.npending = 0;
}

static int fake_alive(void *ctx)
{
    (void)ctx;
    return !F.dead;
}

static const BtTransport T = {fake_send, fake_wait, fake_alive, 0, 0, 0};

static void reset_fake(void) { memset(&F, 0, sizeof F); }

int main(void)
{
    char line[200];

    /* 1. the normal start of a Realtek chip */
    reset_fake();
    CHECK(bt_info()->state == BT_ABSENT);
    bt_attach(&T, 0x0BDA, 0xB00A);
    const BtInfo *i = bt_info();
    CHECK(i->state == BT_HCI_UP);
    CHECK(bt_hci_up());
    CHECK(F.nsent == 5 && F.sent[0] == 0x0C03 && F.sent[1] == 0x1001 && F.sent[2] == 0xFC6D && F.sent[3] == 0x1009 && F.sent[4] == 0x1005);
    CHECK(i->acl_mtu == 1021 && i->acl_pkts == 8);
    CHECK(i->hci_ver == 8 && i->hci_rev == 0x000C && i->lmp_ver == 8 && i->manufacturer == 93 && i->lmp_subver == 0x8821);
    CHECK(i->have_rom && i->rom_version == 3);
    CHECK(i->have_addr && i->bdaddr[0] == 0x11 && i->bdaddr[5] == 0x66); /* shown most significant byte first */
    CHECK(i->vid == 0x0BDA && i->pid == 0xB00A);
    bt_status(line, sizeof line);
    CHECK(strcmp(line, "BT 0bda:b00a hci 8 rev 000c sub 8821 mfr 93 rom 3, addr 11:22:33:44:55:66") == 0);
    CHECK(strlen(line) < 96);
    if (strcmp(line, "BT 0bda:b00a hci 8 rev 000c sub 8821 mfr 93 rom 3, addr 11:22:33:44:55:66"))
        printf("  got: %s\n", line);

    /* 1b. the diagnostic line: the status, whether the USB link is alive, what could not be delivered */
    {
        char dl[300];
        CHECK(bt_diag_line(dl, sizeof dl) > 0);
        CHECK(strcmp(dl, "chip: BT 0bda:b00a hci 8 rev 000c sub 8821 mfr 93 rom 3, addr 11:22:33:44:55:66; usb up, command errors 0 (last code 0), events dropped 0, hardware errors 0") == 0);
        bt_note_cmd_error(6);
        bt_note_cmd_error(-1);
        CHECK(bt_info()->cmd_errors == 2 && bt_info()->last_cmd_code == -1);
        bt_diag_line(dl, sizeof dl);
        CHECK(strstr(dl, "usb up, command errors 2 (last code -1)") != 0);
        F.dead = 1;
        bt_diag_line(dl, sizeof dl);
        CHECK(strstr(dl, "usb DOWN") != 0);
        F.dead = 0;
        for (unsigned cap = 2; cap < 40; cap++) /* always terminated, never longer than the buffer */
        {
            memset(dl, 'x', sizeof dl);
            int n = bt_diag_line(dl, cap);
            CHECK(n >= 0 && (unsigned)n < cap && strlen(dl) == (size_t)n);
        }
        CHECK(bt_diag_line(dl, 1) == -1);
    }

    /* 2. other events and answers for other commands arrive in between: they must not be taken for the answer */
    reset_fake();
    F.noise_first = 1;
    F.wrong_opcode_first = 1;
    bt_attach(&T, 0x0BDA, 0xB00A);
    CHECK(bt_info()->state == BT_HCI_UP);
    CHECK(bt_info()->hw_errors == 5); /* one per command */
    CHECK(bt_info()->events >= 12);

    /* 3. Command Status instead of Command Complete is an answer too (status first) */
    reset_fake();
    F.use_cmd_status = 1;
    bt_attach(&T, 0x0BDA, 0xB00A);
    CHECK(bt_info()->state == BT_HCI_UP);

    /* 4. the chip refuses Reset */
    reset_fake();
    F.reset_status = 0x0C;
    bt_attach(&T, 0x0BDA, 0xB00A);
    CHECK(bt_info()->state == BT_FAILED && !bt_hci_up());
    CHECK(strcmp(bt_info()->why, "reset") == 0 && bt_info()->why_code == 0x0C);
    CHECK(F.nsent == 1); /* it did not go on */
    bt_status(line, sizeof line);
    CHECK(strcmp(line, "BT 0bda:b00a: reset failed (0c)") == 0);

    /* 5. a chip that never answers: timeout, not a hang */
    reset_fake();
    F.silent = 1;
    bt_attach(&T, 0x0BDA, 0xB00A);
    CHECK(bt_info()->state == BT_FAILED && bt_info()->why_code == BT_E_TIMEOUT);
    CHECK(F.steps == 20000); /* 2000 ms in 100 us steps */
    bt_status(line, sizeof line);
    CHECK(strcmp(line, "BT 0bda:b00a: reset failed (-2)") == 0);

    /* 6. unplugged while waiting */
    reset_fake();
    F.silent = 1;
    F.dead = 1;
    bt_attach(&T, 0x0BDA, 0xB00A);
    CHECK(bt_info()->why_code == BT_E_TRANSPORT && F.steps == 0);

    /* 7. a chip from another maker: no vendor command; the Realtek command refused is not fatal */
    reset_fake();
    bt_attach(&T, 0x8087, 0x0A2B);
    CHECK(bt_info()->state == BT_HCI_UP && !bt_info()->have_rom && F.nsent == 4);
    reset_fake();
    F.rom_unknown = 1;
    bt_attach(&T, 0x0BDA, 0xB00A);
    CHECK(bt_info()->state == BT_HCI_UP && !bt_info()->have_rom && F.nsent == 5);
    bt_status(line, sizeof line);
    CHECK(strstr(line, " rom ") == 0);

    /* 8. bad event packets are refused: wrong length byte, too short, too long */
    reset_fake();
    bt_attach(&T, 0x0BDA, 0xB00A);
    unsigned before = bt_info()->events;
    unsigned char bad1[4] = {0x05, 5, 0, 0};        /* says 5 parameter bytes, has 2 */
    unsigned char bad2[1] = {0x05};
    bt_rx_event(bad1, sizeof bad1);
    bt_rx_event(bad2, sizeof bad2);
    bt_rx_event(bad1, 0);
    CHECK(bt_info()->events == before);

    /* 9. a full queue drops events and counts them (nobody drains it between commands) */
    unsigned char ok[3] = {0x05, 1, 0};
    for (int k = 0; k < 20; k++)
        bt_rx_event(ok, sizeof ok);
    CHECK(bt_info()->dropped == 4);
    /* ... the next command still works: it first drains the junk, then the fake chip's answer arrives in a free queue */
    unsigned char ret[8];
    unsigned n = 0;
    CHECK(bt_cmd(0x0C03, 0, 0, ret, sizeof ret, &n, 100) == 0);

    /* 10. return parameters are cut to the caller's buffer */
    reset_fake();
    bt_attach(&T, 0x0BDA, 0xB00A);
    unsigned char small[3];
    CHECK(bt_cmd(0x1001, 0, 0, small, sizeof small, &n, 100) == 0 && n == 3 && small[1] == 8);

    /* 11. too long a parameter block is refused before anything is sent */
    reset_fake();
    unsigned char big[300] = {0};
    int sent_before = F.nsent;
    CHECK(bt_cmd(0x0C03, big, 256, 0, 0, 0, 10) == BT_E_SHORT && F.nsent == sent_before);

    /* 12. status line fits any buffer and is always terminated */
    bt_attach(&T, 0x0BDA, 0xB00A);
    for (unsigned cap = 1; cap < 40; cap++)
    {
        memset(line, 'x', sizeof line);
        bt_status(line, cap);
        CHECK(strlen(line) < cap);
    }

    /* 13. detach */
    bt_detach();
    CHECK(bt_info()->state == BT_ABSENT && !bt_hci_up());
    CHECK(bt_cmd(0x0C03, 0, 0, 0, 0, 0, 10) == BT_E_BUSY);
    bt_status(line, sizeof line);
    CHECK(strcmp(line, "no Bluetooth chip") == 0);
    {
        char dl[300];
        CHECK(bt_diag_line(dl, sizeof dl) > 0 && strcmp(dl, "chip: no Bluetooth chip") == 0);
    }
    unsigned ev_after = bt_info()->events;
    bt_rx_event(ok, sizeof ok); /* late event from an unplugged chip */
    CHECK(bt_info()->events == ev_after);

    printf("bt_hci: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
