/* The pure message functions of bt_avdtp.c are tested alone: the session part of that file calls into the link layer, these stand in for it. */
#include "bluetooth.h"

unsigned long long bt_now(void) { return 0; }
BtLinkState bt_link_state(void) { return BT_LINK_NONE; }
int bt_chan_open(unsigned psm, int user)
{
    (void)psm;
    (void)user;
    return -1;
}
void bt_chan_close(int slot) { (void)slot; }
void bt_a2dp_stop_audio(void) {}
int bt_chan_state(int slot)
{
    (void)slot;
    return BT_CHAN_FREE;
}
int bt_chan_result(int slot)
{
    (void)slot;
    return 0;
}
int bt_chan_queue(int slot, const unsigned char *data, unsigned n)
{
    (void)slot;
    (void)data;
    (void)n;
    return -1;
}
