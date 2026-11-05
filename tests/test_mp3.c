/* Host test for the MP3 decoder (user/mp3dec.c): decodes a whole .mp3 file and compares every sample with the PCM that a reference decoder
 * (mpg123) made from the same file. run_mp3_tests.sh makes the test files with lame and the reference with mpg123; without those tools
 * it skips. Usage:
 *   test_mp3 gen RATE CHANNELS KIND out.wav   a test sound (tones; KIND 1 adds clicks so the encoder uses short blocks)
 *   test_mp3 cmp in.mp3 ref.raw               compare (ref.raw = 16 bit little endian PCM, channels interleaved)
 * Exit status 0 = passed. The decoder is a float implementation and the reference rounds differently, so a small difference is normal:
 * the check is the largest error of any sample and the mean square error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../user/mp3dec.c"

static Mp3Dec DEC;

static int gen(int rate, int ch, int kind, const char *path)
{
    int n = rate * 2; /* 2 seconds */
    short *pcm = malloc((size_t)n * (size_t)ch * 2);
    unsigned seed = 12345;
    for (int i = 0; i < n; i++)
    {
        double t = (double)i / rate;
        double l = 9000 * sin(2 * M_PI * 440 * t) + 5000 * sin(2 * M_PI * 1250 * t) + 2500 * sin(2 * M_PI * 3300 * t);
        double r = 9000 * sin(2 * M_PI * 523.25 * t) + 4000 * sin(2 * M_PI * 2100 * t);
        seed = seed * 1103515245u + 12345u;
        double noise = ((int)((seed >> 16) & 0x7FFF) - 16384) / 16384.0 * 800;
        if (kind == 1 && (i % (rate / 5)) < 200) /* a burst of noise 5 times a second: a transient, so short blocks */
            noise *= 12;
        l += noise;
        r += noise * 0.7;
        if (ch == 1)
            pcm[i] = (short)l;
        else
        {
            pcm[2 * i] = (short)l;
            pcm[2 * i + 1] = (short)r;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        return 1;
    unsigned data = (unsigned)n * (unsigned)ch * 2, v;
    unsigned short s;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f);
    s = (unsigned short)ch; fwrite(&s, 2, 1, f);
    v = (unsigned)rate; fwrite(&v, 4, 1, f);
    v = (unsigned)rate * (unsigned)ch * 2; fwrite(&v, 4, 1, f);
    s = (unsigned short)(ch * 2); fwrite(&s, 2, 1, f);
    s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 1, data, f);
    fclose(f);
    free(pcm);
    return 0;
}

static unsigned char *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)*size + 8);
    if (fread(b, 1, (size_t)*size, f) != (size_t)*size)
        return 0;
    fclose(f);
    return b;
}

static int cmp(const char *mp3path, const char *refpath)
{
    long mp3_size, ref_size;
    unsigned char *mp3 = slurp(mp3path, &mp3_size);
    short *ref = (short *)slurp(refpath, &ref_size);
    if (!mp3 || !ref)
    {
        printf("cannot read the files\n");
        return 2;
    }
    long ref_samples = ref_size / 2;
    short *out = malloc((size_t)ref_samples * 2 + 1152 * 2 * 2 * 4);
    long got = 0;
    int pos = 0, channels = 0, hz = 0, frames = 0, skipped = 0;
    mp3_init(&DEC);
    while (pos < mp3_size)
    {
        Mp3Info info;
        int n = mp3_decode_frame(&DEC, mp3 + pos, (int)(mp3_size - pos), out + got, &info);
        if (n > 0)
        {
            channels = info.channels;
            hz = info.hz;
            got += (long)n * info.channels;
            frames++;
        }
        else if (info.layer != 0 && info.layer != 3)
            skipped++;
        if (info.frame_bytes <= 0)
            break;
        pos += info.frame_bytes;
        if (got > ref_samples + 1152 * 2 * 2)
            break;
    }
    printf("%s: %d frames, %d channel(s), %d Hz, %ld samples (reference %ld)\n", mp3path, frames, channels, hz, got, ref_samples);
    if (frames == 0 || skipped)
    {
        printf("FAIL: nothing decoded\n");
        return 1;
    }
    long n = got < ref_samples ? got : ref_samples;
    if (labs(got - ref_samples) > 3L * 1152 * channels)
    {
        printf("FAIL: length differs by more than two frames\n");
        return 1;
    }
    /* mpg123 skips lame's \"Info\" frame at the start and this decoder plays it (it is silence); find the offset (in samples) that fits best */
    long best_off = 0;
    double best_err = 1e30;
    for (long off = -3L * 1152 * channels; off <= 3L * 1152 * channels; off += channels) /* up to three frames either way (the reference may skip a leading Info frame) */
    {
        double e = 0;
        long cnt = 0;
        for (long i = 20000; i < 20000 + 4000 && i + off < n && i + off >= 0; i++, cnt++)
        {
            double d = out[i + off] - ref[i];
            e += d * d;
        }
        if (cnt && e / cnt < best_err)
        {
            best_err = e / cnt;
            best_off = off;
        }
    }
    double sum = 0;
    long maxd = 0, cnt = 0, big = 0;
    for (long i = 0; i < n; i++)
    {
        long j = i + best_off;
        if (j < 0 || j >= got)
            continue;
        long d = labs((long)out[j] - ref[i]);
        if (d > maxd)
            maxd = d;
        if (d > 8)
            big++;
        sum += (double)d * d;
        cnt++;
    }
    double mse = cnt ? sum / cnt : 1e30;
    printf("  offset %ld samples, max error %ld, mean square error %.4f, samples off by more than 8: %ld of %ld\n", best_off, maxd, mse, big, cnt);
    if (maxd > 32 || mse > 4.0 || big > cnt / 1000)
    {
        printf("FAIL: too far from the reference decoder\n");
        return 1;
    }
    printf("  ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "gen"))
        return gen(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), argv[5]);
    if (argc == 4 && !strcmp(argv[1], "cmp"))
        return cmp(argv[2], argv[3]);
    printf("usage: test_mp3 gen RATE CHANNELS KIND out.wav | test_mp3 cmp in.mp3 ref.raw\n");
    return 2;
}
