/* Host test for the USB audio descriptor search (aud_find in usb_xhci_audio.c): it must pick the alternate setting that plays
 * 48 kHz 16-bit stereo through an isochronous OUT endpoint, and nothing else. The configuration descriptors are built the way a
 * real UAC 1.0 device (and QEMU's usb-audio) lays them out: AudioControl, then AudioStreaming alt 0 (no endpoint), alt 1... The
 * transfer part (rings, doorbells, pacing) was proven in QEMU with a WAV capture: 879.5 Hz for an 880 Hz tone, 1.0 s, no dropouts. */
#include <stdio.h>
#include <string.h>
#include "../kernel/drivers/input/usb_xhci/usb_xhci_audio.c"

int io_dummy;
Dev DEV[MAX_DEV];
int ring_init(Xhci *x, Ring *r) { (void)x; (void)r; return -1; }
void ring_put(Ring *r, u64 p, u32 s, u32 c) { (void)r; (void)p; (void)s; (void)c; }
int command(Xhci *x, u64 p, u32 c, u32 *s) { (void)x; (void)p; (void)c; (void)s; return -1; }
int control(Xhci *x, int s, Ring *r, u8 t, u8 q, u16 v, u16 i, u16 l, int in) { (void)x; (void)s; (void)r; (void)t; (void)q; (void)v; (void)i; (void)l; (void)in; return -1; }
volatile u32 *ctx_at(Xhci *x, int n) { (void)x; (void)n; return 0; }
void inctx_clear(Xhci *x) { (void)x; }
void fill_slot(volatile u32 *c, const Loc *L, int e, int h, int t) { (void)c; (void)L; (void)e; (void)h; (void)t; }
void drop_slot(Xhci *x, u32 s) { (void)x; (void)s; }
void pump(void) {}
u32 ep_interval(int speed, u32 b) { (void)speed; return b; }
int ctrl_ok(int c) { return c == 1; }
unsigned long long timer_ms(void) { return 0; }
int timer_ready(void) { return 0; }
unsigned long long pmm_alloc_contiguous_tag(unsigned long long c, PmmTag t) { (void)c; (void)t; return 0; }
void pmm_free_contiguous(unsigned long long p, unsigned long long c) { (void)p; (void)c; }
void pmm_free_page(unsigned long long p) { (void)p; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static u8 CFG[512];
static unsigned N;
static void put(const u8 *d, unsigned n) { memcpy(CFG + N, d, n); N += n; }
static void iface(u8 num, u8 alt, u8 eps, u8 cls, u8 sub) { u8 d[9] = {9, 4, num, alt, eps, cls, sub, 0, 0}; put(d, 9); }
static void fmt(u8 ch, u8 subframe, u8 bits, unsigned nfreq, const unsigned *freq)
{
    u8 d[8 + 3 * 4] = {(u8)(8 + 3 * nfreq), 0x24, 2, 1, ch, subframe, bits, (u8)nfreq};
    for (unsigned i = 0; i < nfreq; i++) { d[8 + 3 * i] = (u8)freq[i]; d[9 + 3 * i] = (u8)(freq[i] >> 8); d[10 + 3 * i] = (u8)(freq[i] >> 16); }
    put(d, 8 + 3 * nfreq);
}
static void ep(u8 addr, u8 attr, unsigned mps, u8 interval) { u8 d[9] = {9, 5, addr, attr, (u8)mps, (u8)(mps >> 8), interval, 0, 0}; put(d, 9); }

static int find(AudDesc *ad)
{
    Xhci x;
    memset(&x, 0, sizeof x);
    static u8 dma[512];
    memcpy(dma, CFG, N);
    x.dma = dma;
    return aud_find(&x, N, ad);
}

int main(void)
{
    static const unsigned f48[] = {48000}, f44_48[] = {44100, 48000}, f44[] = {44100};
    AudDesc ad;

    /* QEMU-like: control interface, streaming alt 0 (idle), alt 1 = stereo 16-bit 48 kHz isochronous OUT 0x01 */
    N = 0;
    iface(0, 0, 0, 1, 1);
    iface(1, 0, 0, 1, 2);
    iface(1, 1, 1, 1, 2);
    fmt(2, 2, 16, 1, f48);
    ep(0x01, 0x05, 196, 1);
    CHECK(find(&ad) == 1 && ad.iface == 1 && ad.alt == 1 && ad.addr == 0x01 && ad.mps == 196 && ad.interval == 1);

    /* a headset with a 24-bit alt 1 and the 16-bit one on alt 2; the 44.1/48 kHz list is found */
    N = 0;
    iface(1, 0, 0, 1, 2);
    iface(1, 1, 1, 1, 2); fmt(2, 3, 24, 1, f48); ep(0x02, 0x09, 294, 1);
    iface(1, 2, 1, 1, 2); fmt(2, 2, 16, 2, f44_48); ep(0x02, 0x09, 196, 1);
    CHECK(find(&ad) == 1 && ad.alt == 2 && ad.mps == 196 && ad.addr == 0x02);

    /* only 44.1 kHz, only mono, or an IN (microphone) endpoint: not usable */
    N = 0; iface(1, 1, 1, 1, 2); fmt(2, 2, 16, 1, f44); ep(0x01, 0x05, 196, 1);
    CHECK(find(&ad) == 0);
    N = 0; iface(1, 1, 1, 1, 2); fmt(1, 2, 16, 1, f48); ep(0x01, 0x05, 98, 1);
    CHECK(find(&ad) == 0);
    N = 0; iface(1, 1, 1, 1, 2); fmt(2, 2, 16, 1, f48); ep(0x81, 0x05, 196, 1);
    CHECK(find(&ad) == 0);
    /* a bulk endpoint is not isochronous; a packet size under 192 bytes cannot carry 1 ms of audio */
    N = 0; iface(1, 1, 1, 1, 2); fmt(2, 2, 16, 1, f48); ep(0x01, 0x02, 196, 1);
    CHECK(find(&ad) == 0);
    N = 0; iface(1, 1, 1, 1, 2); fmt(2, 2, 16, 1, f48); ep(0x01, 0x05, 100, 1);
    CHECK(find(&ad) == 0);
    /* not an audio streaming interface at all (a HID interface with an endpoint) */
    N = 0; iface(0, 0, 1, 3, 1); ep(0x81, 0x03, 8, 10);
    CHECK(find(&ad) == 0);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("usbaudio tests passed\n");
    return 0;
}
