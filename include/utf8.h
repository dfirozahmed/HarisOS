/* ============================================================
 * HarisOS UTF-8 helpers (header only: no library, no allocation)
 *
 * Text inside the system is UTF-8 (a C string whose bytes >= 0x80 are parts of multi-byte characters).
 * File systems store UTF-16 (FAT long names, exFAT, NTFS): utf8_from_utf16 / utf16_from_utf8 convert.
 * Bad input never reads past the terminating 0 and never produces a bad character: a stray or overlong byte
 * becomes U+FFFD and consumes one byte.
 * ============================================================ */
#ifndef HARIS_UTF8_H
#define HARIS_UTF8_H

#define UTF8_BAD 0xFFFDu

/* Decode one character at *ps and advance *ps past it (at least one byte, never past the terminating 0).
 * Returns the code point, or UTF8_BAD for invalid input. *ps must not point at the terminating 0. */
static inline unsigned utf8_decode(const char **ps)
{
    const unsigned char *p = (const unsigned char *)*ps;
    unsigned c = p[0], cp;
    int n;
    if (c < 0x80)
    {
        *ps += 1;
        return c;
    }
    if (c >= 0xC2 && c <= 0xDF)
        n = 1, cp = c & 0x1Fu;
    else if (c >= 0xE0 && c <= 0xEF)
        n = 2, cp = c & 0x0Fu;
    else if (c >= 0xF0 && c <= 0xF4)
        n = 3, cp = c & 0x07u;
    else
    {
        *ps += 1;
        return UTF8_BAD;
    }
    for (int i = 1; i <= n; i++)
    {
        if ((p[i] & 0xC0u) != 0x80u)
        {
            *ps += i; /* the lead byte and the continuation bytes seen so far */
            return UTF8_BAD;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *ps += n + 1;
    if ((n == 2 && cp < 0x800u) || (n == 3 && (cp < 0x10000u || cp > 0x10FFFFu)) || (cp >= 0xD800u && cp <= 0xDFFFu))
        return UTF8_BAD; /* overlong, out of range or a surrogate */
    return cp;
}

/* Write the UTF-8 form of cp (1..4 bytes, no terminating 0) and return the byte count. A surrogate or a value
 * above 0x10FFFF is written as U+FFFD. */
static inline int utf8_encode(unsigned cp, char *out)
{
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        cp = UTF8_BAD;
    if (cp < 0x80u)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u)
    {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u)
    {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Number of bytes of the character that starts with this lead byte: 1 for ASCII and for a stray byte. */
static inline int utf8_len_of_lead(unsigned char c)
{
    return c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 && c < 0xF0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 1;
}

/* A continuation byte (10xxxxxx): the middle or end of a character, never its start. */
static inline int utf8_is_cont(unsigned char c) { return (c & 0xC0u) == 0x80u; }

/* Like utf8_decode, but says whether the input was well-formed: 1 and *cp = the character, or 0 (and *cp = U+FFFD) for a
 * stray, overlong or surrogate sequence. A genuine U+FFFD (EF BF BD) counts as well-formed; utf8_decode alone cannot
 * tell the two apart, as it gives U+FFFD for both. */
static inline int utf8_decode_ok(const char **ps, unsigned *cp)
{
    const unsigned char *start = (const unsigned char *)*ps;
    *cp = utf8_decode(ps);
    if (*cp != UTF8_BAD)
        return 1;
    return (const unsigned char *)*ps - start == 3 && start[0] == 0xEF && start[1] == 0xBF && start[2] == 0xBD;
}

/* 1 if the whole string is well-formed UTF-8 (no stray, overlong or surrogate sequences, nothing above U+10FFFF).
 * A real U+FFFD (EF BF BD) is fine; the "bad byte" marker utf8_decode gives for broken input is not. */
static inline int utf8_valid(const char *s)
{
    while (*s)
    {
        unsigned cp;
        if (!utf8_decode_ok(&s, &cp))
            return 0;
    }
    return 1;
}

/* Bytes needed for n UTF-16 units as UTF-8, not counting a terminating 0. */
static inline int utf8_size_from_utf16(const unsigned short *s, int n)
{
    int bytes = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned cp = s[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < n && s[i + 1] >= 0xDC00u && s[i + 1] <= 0xDFFFu)
        {
            cp = 0x10000u;
            i++;
        }
        else if (cp >= 0xD800u && cp <= 0xDFFFu)
            cp = UTF8_BAD;
        bytes += cp < 0x80u ? 1 : cp < 0x800u ? 2 : cp < 0x10000u ? 3 : 4;
    }
    return bytes;
}

/* Convert n UTF-16 units (a surrogate pair gives one character; a lone surrogate becomes U+FFFD) to UTF-8 in
 * out[0..cap). Always terminates with 0 when cap > 0; stops cleanly (never mid-character) if out is full.
 * Returns the number of bytes written, not counting the 0. */
static inline int utf8_from_utf16(const unsigned short *s, int n, char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned cp = s[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < n && s[i + 1] >= 0xDC00u && s[i + 1] <= 0xDFFFu)
        {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (s[i + 1] - 0xDC00u);
            i++;
        }
        else if (cp >= 0xD800u && cp <= 0xDFFFu)
            cp = UTF8_BAD;
        char tmp[4];
        int k = utf8_encode(cp, tmp);
        if (o + k + 1 > cap)
            break;
        for (int j = 0; j < k; j++)
            out[o++] = tmp[j];
    }
    if (cap > 0)
        out[o] = 0;
    return o;
}

/* Convert a UTF-8 string to UTF-16 in out[0..cap) (no terminating 0 is written). Returns the number of units,
 * or -1 if the string does not fit. Invalid bytes become U+FFFD. */
static inline int utf16_from_utf8(const char *s, unsigned short *out, int cap)
{
    int o = 0;
    while (*s)
    {
        unsigned cp = utf8_decode(&s);
        if (cp >= 0x10000u)
        {
            if (o + 2 > cap)
                return -1;
            cp -= 0x10000u;
            out[o++] = (unsigned short)(0xD800u + (cp >> 10));
            out[o++] = (unsigned short)(0xDC00u + (cp & 0x3FFu));
        }
        else
        {
            if (o + 1 > cap)
                return -1;
            out[o++] = (unsigned short)cp;
        }
    }
    return o;
}

#endif
