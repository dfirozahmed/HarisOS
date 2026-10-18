/* PS/2 controller byte router. See include/ps2.h. */
#include "ps2.h"
#include "io.h"

#if defined(__x86_64__) || defined(__i386__)

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define ST_OUT_FULL 0x01
#define ST_AUX 0x20 /* the waiting byte comes from the mouse */

#define QSIZE 256 /* power of two */
typedef struct
{
    unsigned char buf[QSIZE];
    unsigned head, tail; /* read at head, write at tail */
} Queue;

static Queue KBD, MOUSE;

static void push(Queue *q, unsigned char b)
{
    if (q->tail - q->head >= QSIZE)
        return; /* full: drop the new byte */
    q->buf[q->tail++ % QSIZE] = b;
}

static int pop(Queue *q, unsigned char *b)
{
    if (q->head == q->tail)
        return 0;
    *b = q->buf[q->head++ % QSIZE];
    return 1;
}

void ps2_poll(void)
{
    for (int n = 0; n < 64; n++) /* never spend long here */
    {
        unsigned char st = io_in8(PS2_STATUS);
        if (st == 0xFF) /* floating bus: there is no controller */
            return;
        if (!(st & ST_OUT_FULL))
            return;
        unsigned char b = io_in8(PS2_DATA);
        push((st & ST_AUX) ? &MOUSE : &KBD, b);
    }
}

int ps2_kbd_pop(unsigned char *b) { return pop(&KBD, b); }
int ps2_mouse_pop(unsigned char *b) { return pop(&MOUSE, b); }

#else

void ps2_poll(void) {}
int ps2_kbd_pop(unsigned char *b) { (void)b; return 0; }
int ps2_mouse_pop(unsigned char *b) { (void)b; return 0; }

#endif
