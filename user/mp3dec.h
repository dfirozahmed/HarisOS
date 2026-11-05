/* ============================================================
 * HarisOS MP3 decoder: MPEG-1/2/2.5 Layer III (the ".mp3" files), 16-bit PCM out. Pure code: no libc, no system calls, so the host test
 * (tests/test_mp3.c) runs it against files decoded by mpg123.
 *
 *   mp3_init(&dec);                                        once per stream
 *   n = mp3_decode_frame(&dec, data, bytes, pcm, &info);   one frame
 *
 * `data` points at (or some bytes before) a frame; the decoder looks for the frame header itself, so junk in front (an ID3 tag, a lost
 * frame) is skipped. The result is the number of samples per channel written to `pcm` (1152, 576 for MPEG-2/2.5; channels interleaved),
 * or 0 when no frame was decoded: then info.frame_bytes says how many bytes to drop before trying again (0 = need more data), and
 * info.layer is 1 or 2 if the frame is one this decoder does not read. Layer I / II and free format streams are not supported.
 *
 * How an MP3 frame becomes sound, in short: the frame holds the Huffman coded, quantized spectrum of 576 lines per channel per
 * "granule" (2 granules per MPEG-1 frame). Decoding = read the side info and scale factors, Huffman decode the lines, requantize
 * (x^(4/3) times a gain), undo joint stereo (mid/side, intensity), reorder short blocks, reduce the aliasing between the 32 subbands,
 * inverse MDCT in each subband (with overlap from the previous granule), and a 32 band polyphase synthesis filter that turns the 32
 * subband samples back into 32 time samples. The structure follows ISO 11172-3; the constants are in mp3tab.h.
 * ============================================================ */
#ifndef HARIS_MP3DEC_H
#define HARIS_MP3DEC_H

#define MP3_MAX_SAMPLES (1152 * 2) /* room for one frame of PCM: 1152 samples per channel, 2 channels */
#define MP3_RESERVOIR 511          /* bytes of main data a later frame may reach back into */
#define MP3_MAX_FRAME 2304         /* largest frame payload we accept */

typedef struct
{
    int frame_bytes;  /* bytes the frame took (with anything skipped in front); on failure, bytes to skip */
    int frame_offset; /* where the frame header was found */
    int channels, hz, layer, bitrate_kbps;
} Mp3Info;

typedef struct
{
    const unsigned char *buf;
    int pos, limit; /* in bits */
} Mp3Bits;

typedef struct
{
    const unsigned char *sfbtab;
    unsigned short part_23_length, big_values, scalefac_compress;
    unsigned char global_gain, block_type, mixed_block_flag, n_long_sfb, n_short_sfb;
    unsigned char table_select[3], region_count[3], subblock_gain[3];
    unsigned char preflag, scalefac_scale, count1_table, scfsi;
} Mp3Gr;

typedef struct
{
    Mp3Bits bs;
    unsigned char maindata[MP3_RESERVOIR + MP3_MAX_FRAME];
    Mp3Gr gr[4];
    float grbuf[2][576], scf[40], syn[18 + 15][2 * 32];
    unsigned char ist_pos[2][39];
} Mp3Scratch;

typedef struct
{
    float mdct_overlap[2][9 * 32], qmf_state[15 * 2 * 32];
    int reserv;
    unsigned char header[4], reserv_buf[MP3_RESERVOIR];
    Mp3Scratch s;
} Mp3Dec;

void mp3_init(Mp3Dec *d);
int mp3_decode_frame(Mp3Dec *d, const unsigned char *data, int bytes, short *pcm, Mp3Info *info);

#endif
