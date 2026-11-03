/* ============================================================
 * HarisOS WAV files: reading the header and turning the samples into what the sound hardware plays
 *
 *   wav_parse(file, len, &info);                     RIFF/WAVE with PCM samples (8 or 16 bit, mono or stereo)
 *   n = wav_frames_48k(&info);                       frames the converted sound has
 *   wav_to_48k_stereo16(file, &info, out, n);        fills out[2 * n]: 16-bit stereo at 48 kHz
 *
 * A WAV file is a RIFF container: "RIFF" size "WAVE", then chunks (4-byte name, 4-byte little-endian size, data, one pad
 * byte if the size is odd). The "fmt " chunk says how the samples are stored, the "data" chunk holds them. Pure code,
 * host-tested (tests/test_wav.c); nothing here touches hardware.
 * ============================================================ */
#ifndef HARIS_WAV_H
#define HARIS_WAV_H

#define WAV_RATE_MIN 8000
#define WAV_RATE_MAX 96000

typedef struct {
    unsigned rate;      /* frames per second */
    unsigned channels;  /* 1 or 2 */
    unsigned bits;      /* 8 (unsigned samples) or 16 (signed) */
    unsigned data_off;  /* where the samples start in the file */
    unsigned data_len;  /* bytes of samples (cut to the file if the header claims more), a whole number of frames */
} WavInfo;

#define WAV_ERR_FORMAT -1  /* not a RIFF/WAVE file, or the chunks are damaged */
#define WAV_ERR_CODEC -2   /* not plain PCM */
#define WAV_ERR_PARAMS -3  /* PCM, but a channel count, sample size or rate this code does not play */
#define WAV_ERR_EMPTY -4   /* no samples */

/* 0 = filled in, else WAV_ERR_*. */
int wav_parse(const unsigned char *file, unsigned long len, WavInfo *info);

/* Frames after conversion to 48 kHz (rounded down); 0 for an empty sound. */
unsigned long wav_frames_48k(const WavInfo *info);

/* Writes `frames` stereo frames (left, right) of 16-bit samples. The rate is changed by taking the nearest earlier source
 * frame (no filtering: fine for a first player, speech and tones, not for music). `frames` must not exceed
 * wav_frames_48k(); the rest is not written. */
void wav_to_48k_stereo16(const unsigned char *file, const WavInfo *info, short *out, unsigned long frames);

#endif
