/* SBC (the low-complexity subband codec every A2DP music sink must understand): an encoder for 48 kHz / 44.1 kHz stereo, 8 subbands,
 * 4 .. 16 blocks, joint stereo or stereo, loudness or SNR bit allocation. Integer arithmetic only (the kernel does not use floating point).
 *
 * How it works (A2DP specification, appendix B):
 *   1. The filter bank splits the signal into 8 frequency bands, 8 new samples at a time giving one sample per band (a "block").
 *   2. A frame has 4 .. 16 blocks. For every band a scale factor (4 bits) says how large its samples can get in this frame.
 *   3. Bits are shared out to the bands by need: loud bands (relative to what the ear hears) get more bits (the bit allocation).
 *   4. Each sample is rounded to that many bits. Joint stereo sends some bands as sum and difference of left and right.
 * The frame is a 4 byte header (syncword 0x9C, setting, bitpool, CRC), the joint stereo flags, the scale factors, then the samples. */
#ifndef HARIS_BT_SBC_H
#define HARIS_BT_SBC_H

#include "bluetooth.h"

#define BT_SBC_MAX_FRAME 256 /* the most bytes of a frame this encoder makes: bt_sbc_init() refuses a setting with longer frames */

typedef struct
{
    int freq_code;  /* in the frame header: 0 = 16 kHz, 1 = 32, 2 = 44.1, 3 = 48 */
    int blocks;     /* 4, 8, 12 or 16 */
    int mode_code;  /* 2 = stereo, 3 = joint stereo */
    int alloc_code; /* 0 = loudness, 1 = SNR */
    int bitpool;
    short hist[2][80]; /* the last 80 samples of each channel, the newest first */
} BtSbcEnc;

/* Takes the setting that was agreed with the sink (BtSbcConfig, the "on the air" codes). 0, or -1 for one this encoder does not make
 * (it makes what bt_avdtp_choose() chooses: 8 subbands, stereo or joint stereo). */
int bt_sbc_init(BtSbcEnc *e, const BtSbcConfig *cfg);

unsigned bt_sbc_samples(const BtSbcEnc *e);     /* sample frames (left + right) one SBC frame takes: blocks * 8 */
unsigned bt_sbc_frame_bytes(const BtSbcEnc *e); /* the length of every frame */

/* The bits per sample of every subband (left, right) for these scale factors: the bit allocation, which a decoder repeats. Public for the tests. */
void bt_sbc_allocate(const BtSbcEnc *e, const int sf[2][8], int bits[2][8]);

/* Encodes bt_sbc_samples() sample frames of interleaved left, right 16-bit samples into one frame. Returns its length, or -1 if `cap` is too small. */
int bt_sbc_encode(BtSbcEnc *e, const short *pcm, unsigned char *out, unsigned cap);

#endif
