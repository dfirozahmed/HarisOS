/* WAV file reading (see include/wav.h). Pure code: the host test tests/test_wav.c runs it as it is. */
#include "wav.h"

typedef unsigned char u8;
typedef unsigned int u32;

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static int tag(const u8 *p, const char *t) { return p[0] == (u8)t[0] && p[1] == (u8)t[1] && p[2] == (u8)t[2] && p[3] == (u8)t[3]; }

int wav_parse(const unsigned char *file, unsigned long len, WavInfo *info)
{
    if (len < 12 || !tag(file, "RIFF") || !tag(file + 8, "WAVE"))
        return WAV_ERR_FORMAT;
    int have_fmt = 0;
    u32 format = 0, channels = 0, rate = 0, bits = 0, block = 0;
    unsigned long pos = 12;
    while (pos + 8 <= len)
    {
        unsigned long size = le32(file + pos + 4), body = pos + 8;
        if (tag(file + pos, "fmt "))
        {
            if (size < 16 || body + 16 > len)
                return WAV_ERR_FORMAT;
            format = le16(file + body);
            channels = le16(file + body + 2);
            rate = le32(file + body + 4);
            block = le16(file + body + 12);
            bits = le16(file + body + 14);
            if (format == 0xFFFE) /* extensible: the real format is the first two bytes of the sub-format GUID */
            {
                if (size < 40 || body + 40 > len)
                    return WAV_ERR_FORMAT;
                format = le16(file + body + 24);
            }
            have_fmt = 1;
        }
        else if (tag(file + pos, "data"))
        {
            if (!have_fmt)
                return WAV_ERR_FORMAT; /* the samples must come after their description */
            if (format != 1)
                return WAV_ERR_CODEC;
            if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16) || rate < WAV_RATE_MIN || rate > WAV_RATE_MAX ||
                block != channels * bits / 8)
                return WAV_ERR_PARAMS;
            unsigned long avail = len - body; /* a header that claims more than the file holds (a cut file, or a stream) */
            if (size > avail)
                size = avail;
            size -= size % block;
            if (size == 0)
                return WAV_ERR_EMPTY;
            info->rate = rate;
            info->channels = channels;
            info->bits = bits;
            info->data_off = (unsigned)body;
            info->data_len = (unsigned)size;
            return 0;
        }
        pos = body + size + (size & 1u); /* chunks are padded to an even size */
    }
    return WAV_ERR_FORMAT;
}

unsigned long wav_frames_48k(const WavInfo *info)
{
    unsigned long frames = info->data_len / (info->channels * info->bits / 8);
    return (unsigned long)((unsigned long long)frames * 48000ull / info->rate);
}

void wav_to_48k_stereo16(const unsigned char *file, const WavInfo *info, short *out, unsigned long frames)
{
    unsigned bytes = info->bits / 8, step = info->channels * bytes;
    const u8 *src = file + info->data_off;
    for (unsigned long i = 0; i < frames; i++)
    {
        unsigned long long j = (unsigned long long)i * info->rate / 48000ull; /* the source frame at the same moment */
        const u8 *f = src + j * step;
        int l, r;
        if (info->bits == 16)
        {
            l = (short)le16(f);
            r = info->channels == 2 ? (short)le16(f + 2) : l;
        }
        else /* unsigned 8 bit: 128 is silence */
        {
            l = ((int)f[0] - 128) << 8;
            r = info->channels == 2 ? ((int)f[1] - 128) << 8 : l;
        }
        out[2 * i] = (short)l;
        out[2 * i + 1] = (short)r;
    }
}
