/* WPA2-Personal building blocks: SHA-1, HMAC-SHA-1, PBKDF2, the 802.11i PRF, AES-128 and the RFC 3394 key wrap. See include/wpa.h.
 * Freestanding C: no libc, no memcpy / memset calls (loops written out, with the compiler told not to turn them back into calls). */
#include "wpa.h"

static u32 rol(u32 v, unsigned n) { return (v << n) | (v >> (32 - n)); }

static void zero(void *p, unsigned n)
{
    volatile u8 *q = (volatile u8 *)p;
    for (unsigned i = 0; i < n; i++)
        q[i] = 0;
}

static void copy(void *d, const void *s, unsigned n)
{
    volatile u8 *q = (volatile u8 *)d;
    const volatile u8 *p = (const volatile u8 *)s;
    for (unsigned i = 0; i < n; i++)
        q[i] = p[i];
}

/* ---------------------------------------------------------------- SHA-1 (FIPS 180-4) */

static void sha1_block(u32 h[5], const u8 *p)
{
    u32 w[80];
    for (unsigned i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | (u32)p[4 * i + 3];
    for (unsigned i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80; i++)
    {
        u32 f, k;
        if (i < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->bytes = 0;
    s->n = 0;
}

void sha1_update(Sha1 *s, const void *data, unsigned len)
{
    const u8 *p = (const u8 *)data;
    s->bytes += len;
    while (len)
    {
        unsigned take = 64 - s->n;
        if (take > len)
            take = len;
        copy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        len -= take;
        if (s->n == 64)
        {
            sha1_block(s->h, s->buf);
            s->n = 0;
        }
    }
}

void sha1_final(Sha1 *s, u8 out[20])
{
    u64 bits = s->bytes * 8;
    u8 pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56)
        sha1_update(s, &pad, 1);
    u8 len[8];
    for (unsigned i = 0; i < 8; i++)
        len[i] = (u8)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (unsigned i = 0; i < 5; i++)
    {
        out[4 * i] = (u8)(s->h[i] >> 24);
        out[4 * i + 1] = (u8)(s->h[i] >> 16);
        out[4 * i + 2] = (u8)(s->h[i] >> 8);
        out[4 * i + 3] = (u8)s->h[i];
    }
}

void sha1(const void *data, unsigned len, u8 out[20])
{
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

/* ---------------------------------------------------------------- HMAC-SHA-1 (RFC 2104) */

typedef struct
{
    Sha1 inner, outer; /* the two hashes, each already fed with its padded key block */
} HmacKey;

static void hmac_key(HmacKey *k, const u8 *key, unsigned key_len)
{
    u8 block[64];
    zero(block, 64);
    if (key_len > 64)
        sha1(key, key_len, block); /* a long key is hashed first */
    else
        copy(block, key, key_len);
    u8 ipad[64], opad[64];
    for (unsigned i = 0; i < 64; i++)
    {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5C;
    }
    sha1_init(&k->inner);
    sha1_update(&k->inner, ipad, 64);
    sha1_init(&k->outer);
    sha1_update(&k->outer, opad, 64);
    zero(block, 64);
    zero(ipad, 64);
    zero(opad, 64);
}

/* the MAC of msg1 || msg2 under a prepared key (the states are copied, the key stays usable) */
static void hmac_run(const HmacKey *k, const void *m1, unsigned l1, const void *m2, unsigned l2, u8 out[20])
{
    Sha1 in = k->inner, o = k->outer;
    u8 digest[20];
    if (l1)
        sha1_update(&in, m1, l1);
    if (l2)
        sha1_update(&in, m2, l2);
    sha1_final(&in, digest);
    sha1_update(&o, digest, 20);
    sha1_final(&o, out);
}

void hmac_sha1(const u8 *key, unsigned key_len, const void *msg1, unsigned len1, const void *msg2, unsigned len2, u8 out[20])
{
    HmacKey k;
    hmac_key(&k, key, key_len);
    hmac_run(&k, msg1, len1, msg2, len2, out);
    zero(&k, sizeof k);
}

/* ---------------------------------------------------------------- PBKDF2 (RFC 8018) and the WPA2 PMK */

void pbkdf2_sha1(const u8 *pass, unsigned pass_len, const u8 *salt, unsigned salt_len, unsigned iterations, u8 *out, unsigned out_len)
{
    HmacKey k;
    hmac_key(&k, pass, pass_len); /* the key part is the same in every round: prepared once, which halves the work */
    for (unsigned block = 1; out_len; block++)
    {
        u8 be[4] = {(u8)(block >> 24), (u8)(block >> 16), (u8)(block >> 8), (u8)block};
        u8 u[20], t[20];
        hmac_run(&k, salt, salt_len, be, 4, u);
        copy(t, u, 20);
        for (unsigned i = 1; i < iterations; i++)
        {
            hmac_run(&k, u, 20, 0, 0, u);
            for (unsigned j = 0; j < 20; j++)
                t[j] ^= u[j];
        }
        unsigned take = out_len < 20 ? out_len : 20;
        copy(out, t, take);
        out += take;
        out_len -= take;
        zero(u, 20);
        zero(t, 20);
    }
    zero(&k, sizeof k);
}

void wpa_pmk(const char *passphrase, unsigned pass_len, const char *ssid, unsigned ssid_len, u8 pmk[32])
{
    pbkdf2_sha1((const u8 *)passphrase, pass_len, (const u8 *)ssid, ssid_len, 4096, pmk, 32);
}

/* ---------------------------------------------------------------- the PRF and the PTK (IEEE 802.11i) */

void wpa_prf(const u8 *key, unsigned key_len, const char *label, const u8 *data, unsigned data_len, u8 *out, unsigned out_len)
{
    HmacKey k;
    hmac_key(&k, key, key_len);
    unsigned label_len = 0;
    while (label[label_len])
        label_len++;
    for (u8 counter = 0; out_len; counter++)
    {
        /* HMAC(key, label || 0x00 || data || counter) */
        Sha1 in = k.inner, o = k.outer;
        u8 zero_byte = 0, digest[20], full[20];
        sha1_update(&in, label, label_len);
        sha1_update(&in, &zero_byte, 1);
        sha1_update(&in, data, data_len);
        sha1_update(&in, &counter, 1);
        sha1_final(&in, digest);
        sha1_update(&o, digest, 20);
        sha1_final(&o, full);
        unsigned take = out_len < 20 ? out_len : 20;
        copy(out, full, take);
        out += take;
        out_len -= take;
    }
    zero(&k, sizeof k);
}

static int cmp(const u8 *a, const u8 *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

void wpa_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6], const u8 anonce[32], const u8 snonce[32], u8 ptk[48])
{
    /* min(AA, SPA) || max(AA, SPA) || min(ANonce, SNonce) || max(ANonce, SNonce) */
    u8 data[76];
    const u8 *lo = cmp(aa, spa, 6) < 0 ? aa : spa;
    const u8 *hi = lo == aa ? spa : aa;
    copy(data, lo, 6);
    copy(data + 6, hi, 6);
    const u8 *nlo = cmp(anonce, snonce, 32) < 0 ? anonce : snonce;
    const u8 *nhi = nlo == anonce ? snonce : anonce;
    copy(data + 12, nlo, 32);
    copy(data + 44, nhi, 32);
    wpa_prf(pmk, 32, "Pairwise key expansion", data, 76, ptk, 48);
}

/* ---------------------------------------------------------------- AES-128 (FIPS 197) */

static const u8 SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static u8 INV_SBOX[256]; /* filled from SBOX the first time it is needed */
static int INV_READY;

static void inv_sbox_init(void)
{
    if (INV_READY)
        return;
    for (unsigned i = 0; i < 256; i++)
        INV_SBOX[SBOX[i]] = (u8)i;
    INV_READY = 1;
}

static u8 xtime(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

/* multiplication in GF(2^8) */
static u8 gmul(u8 a, u8 b)
{
    u8 r = 0;
    while (b)
    {
        if (b & 1)
            r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

void aes128_set_key(Aes128 *a, const u8 key[16])
{
    copy(a->rk, key, 16);
    u8 rcon = 1;
    for (unsigned i = 16; i < 176; i += 4)
    {
        u8 t[4];
        copy(t, a->rk + i - 4, 4);
        if (i % 16 == 0)
        {
            u8 x = t[0];
            t[0] = (u8)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[x];
            rcon = xtime(rcon);
        }
        for (unsigned j = 0; j < 4; j++)
            a->rk[i + j] = (u8)(a->rk[i + j - 16] ^ t[j]);
    }
}

static void add_round_key(u8 s[16], const u8 *rk)
{
    for (unsigned i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

/* the state is column-major as in the standard: s[4 * column + row], which is the order of the bytes of the block */
static void shift_rows(u8 s[16])
{
    u8 t[16];
    for (unsigned c = 0; c < 4; c++)
        for (unsigned r = 0; r < 4; r++)
            t[4 * c + r] = s[4 * ((c + r) % 4) + r];
    copy(s, t, 16);
}

static void inv_shift_rows(u8 s[16])
{
    u8 t[16];
    for (unsigned c = 0; c < 4; c++)
        for (unsigned r = 0; r < 4; r++)
            t[4 * ((c + r) % 4) + r] = s[4 * c + r];
    copy(s, t, 16);
}

static void mix_columns(u8 s[16])
{
    for (unsigned c = 0; c < 4; c++)
    {
        u8 *p = s + 4 * c;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (u8)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        p[1] = (u8)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        p[2] = (u8)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        p[3] = (u8)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    }
}

static void inv_mix_columns(u8 s[16])
{
    for (unsigned c = 0; c < 4; c++)
    {
        u8 *p = s + 4 * c;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (u8)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        p[1] = (u8)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        p[2] = (u8)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        p[3] = (u8)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void aes128_encrypt(const Aes128 *a, const u8 in[16], u8 out[16])
{
    u8 s[16];
    copy(s, in, 16);
    add_round_key(s, a->rk);
    for (unsigned round = 1; round <= 10; round++)
    {
        for (unsigned i = 0; i < 16; i++)
            s[i] = SBOX[s[i]];
        shift_rows(s);
        if (round != 10)
            mix_columns(s);
        add_round_key(s, a->rk + 16 * round);
    }
    copy(out, s, 16);
}

void aes128_decrypt(const Aes128 *a, const u8 in[16], u8 out[16])
{
    inv_sbox_init();
    u8 s[16];
    copy(s, in, 16);
    add_round_key(s, a->rk + 160);
    for (int round = 9; round >= 0; round--)
    {
        inv_shift_rows(s);
        for (unsigned i = 0; i < 16; i++)
            s[i] = INV_SBOX[s[i]];
        add_round_key(s, a->rk + 16 * (unsigned)round);
        if (round != 0)
            inv_mix_columns(s);
    }
    copy(out, s, 16);
}

/* ---------------------------------------------------------------- AES key wrap (RFC 3394) */

void aes_wrap(const u8 kek[16], const u8 *plain, unsigned n, u8 *wrapped)
{
    Aes128 a;
    aes128_set_key(&a, kek);
    u8 A[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
    copy(wrapped + 8, plain, 8 * n);
    for (unsigned j = 0; j < 6; j++)
        for (unsigned i = 1; i <= n; i++)
        {
            u8 b[16], o[16];
            copy(b, A, 8);
            copy(b + 8, wrapped + 8 * i, 8);
            aes128_encrypt(&a, b, o);
            u32 t = n * j + i;
            copy(A, o, 8);
            A[7] ^= (u8)t;
            A[6] ^= (u8)(t >> 8);
            A[5] ^= (u8)(t >> 16);
            A[4] ^= (u8)(t >> 24);
            copy(wrapped + 8 * i, o + 8, 8);
        }
    copy(wrapped, A, 8);
    zero(&a, sizeof a);
}

int aes_unwrap(const u8 kek[16], const u8 *wrapped, unsigned n, u8 *plain)
{
    Aes128 a;
    aes128_set_key(&a, kek);
    u8 A[8];
    copy(A, wrapped, 8);
    copy(plain, wrapped + 8, 8 * n);
    for (int j = 5; j >= 0; j--)
        for (unsigned i = n; i >= 1; i--)
        {
            u32 t = n * (unsigned)j + i;
            u8 b[16], o[16];
            copy(b, A, 8);
            b[7] ^= (u8)t;
            b[6] ^= (u8)(t >> 8);
            b[5] ^= (u8)(t >> 16);
            b[4] ^= (u8)(t >> 24);
            copy(b + 8, plain + 8 * (i - 1), 8);
            aes128_decrypt(&a, b, o);
            copy(A, o, 8);
            copy(plain + 8 * (i - 1), o + 8, 8);
        }
    zero(&a, sizeof a);
    for (unsigned i = 0; i < 8; i++)
        if (A[i] != 0xA6)
            return -1;
    return 0;
}

/* ---------------------------------------------------------------- the 4-way handshake, station side */

static unsigned be16(const u8 *p) { return (unsigned)p[0] << 8 | p[1]; }
static u64 be64(const u8 *p)
{
    u64 v = 0;
    for (unsigned i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

void wpa_eapol_mic(const u8 kck[16], const u8 *frame, unsigned len, u8 mic[16])
{
    u8 tmp[EAPOL_MAX + 64], h[20];
    if (len > sizeof tmp)
        len = sizeof tmp;
    copy(tmp, frame, len);
    zero(tmp + EAPOL_KEY_MIC_AT, 16); /* the MIC is computed with its own field zero */
    hmac_sha1(kck, 16, tmp, len, 0, 0, h);
    copy(mic, h, 16);
}

void wpa4_init(Wpa4 *w, const u8 pmk[32], const u8 aa[6], const u8 spa[6], const u8 *rsn, unsigned rsn_len)
{
    zero(w, sizeof *w);
    copy(w->pmk, pmk, 32);
    copy(w->aa, aa, 6);
    copy(w->spa, spa, 6);
    if (rsn_len > sizeof w->rsn)
        rsn_len = sizeof w->rsn;
    copy(w->rsn, rsn, rsn_len);
    w->rsn_len = rsn_len;
}

/* message 2 or 4: the frame without key data or with `data`, signed with the KCK of the PTK */
static unsigned wpa4_build(const Wpa4 *w, u8 version, unsigned key_info, const u8 *replay8, const u8 *nonce, const u8 *data, unsigned data_len, u8 *out)
{
    zero(out, EAPOL_KEY_HDR + data_len);
    out[0] = version;
    out[1] = 3; /* EAPOL-Key */
    unsigned body = EAPOL_KEY_HDR - 4 + data_len;
    out[2] = (u8)(body >> 8);
    out[3] = (u8)body;
    out[4] = 2; /* descriptor type: RSN */
    out[5] = (u8)(key_info >> 8);
    out[6] = (u8)key_info;
    /* key length 0, as a supplicant sends it for RSN */
    copy(out + 9, replay8, 8);
    if (nonce)
        copy(out + 17, nonce, 32);
    out[97] = (u8)(data_len >> 8);
    out[98] = (u8)data_len;
    if (data_len)
        copy(out + EAPOL_KEY_HDR, data, data_len);
    wpa_eapol_mic(w->ptk, out, EAPOL_KEY_HDR + data_len, out + EAPOL_KEY_MIC_AT);
    return EAPOL_KEY_HDR + data_len;
}

/* the group key in the decrypted key data of message 3: elements, among them the vendor element "GTK KDE" = DD len 00-0F-AC 01, key id and
 * transmit flag, one reserved byte, then the key. The padding at the end is a DD with length 0 (and zeros). Returns 0 if a GTK was found. */
static int wpa4_find_gtk(Wpa4 *w, const u8 *p, unsigned len)
{
    for (unsigned i = 0; i + 2 <= len;)
    {
        unsigned tag = p[i], l = p[i + 1];
        if (tag == 0xDD && l == 0)
            break; /* padding */
        if (i + 2 + l > len)
            return -1;
        if (tag == 0xDD && l >= 6 + 16 && p[i + 2] == 0x00 && p[i + 3] == 0x0F && p[i + 4] == 0xAC && p[i + 5] == 1)
        {
            unsigned klen = l - 6;
            if (klen != 16) /* CCMP group key; other ciphers are not offered in our association request */
                return -1;
            w->gtk_id = p[i + 6] & 3;
            w->gtk_len = klen;
            copy(w->gtk, p + i + 8, klen);
            return 0;
        }
        i += 2 + l;
    }
    return -2;
}

int wpa4_rx(Wpa4 *w, const u8 *f, unsigned len, const u8 fresh_snonce[32], u8 *reply, unsigned *reply_len)
{
    if (len < 4 || f[1] != 3)
        return WPA4_IGNORED; /* not an EAPOL-Key (EAP packets, start, logoff...) */
    unsigned body = be16(f + 2);
    if (body < EAPOL_KEY_HDR - 4 || 4 + body > len || 4 + body > EAPOL_MAX)
        return WPA4_E_SHORT;
    len = 4 + body; /* trailing padding of the frame is not part of it */
    if (f[4] != 2)
        return WPA4_E_FORMAT;
    unsigned ki = be16(f + 5);
    if ((ki & 7) != 2 || !(ki & 0x08) || !(ki & 0x80)) /* HMAC-SHA1 / AES, pairwise, from the access point (ACK) */
        return WPA4_E_FORMAT;
    unsigned kd_len = be16(f + 97);
    if (EAPOL_KEY_HDR + kd_len > len)
        return WPA4_E_SHORT;
    const u8 *replay = f + 9;

    if (!(ki & 0x100)) /* no MIC: message 1 */
    {
        copy(w->anonce, f + 17, 32);
        copy(w->snonce, fresh_snonce, 32);
        wpa_ptk(w->pmk, w->aa, w->spa, w->anonce, w->snonce, w->ptk);
        w->have_ptk = 1;
        w->replay = be64(replay);
        w->have_replay = 1;
        w->state = 1;
        *reply_len = wpa4_build(w, f[0], 0x010A, replay, w->snonce, w->rsn, w->rsn_len, reply);
        return WPA4_SEND_MSG2;
    }

    /* message 3 */
    if (!w->have_ptk)
        return WPA4_E_ORDER;
    if (be64(replay) <= w->replay)
        return WPA4_E_REPLAY;
    u8 mic[16];
    wpa_eapol_mic(w->ptk, f, len, mic);
    unsigned diff = 0;
    for (unsigned i = 0; i < 16; i++)
        diff |= (unsigned)(mic[i] ^ f[EAPOL_KEY_MIC_AT + i]);
    if (diff)
        return WPA4_E_MIC;
    w->replay = be64(replay); /* only a signed frame moves the counter */
    if (cmp(f + 17, w->anonce, 32) != 0)
        return WPA4_E_NONCE;
    if (!(ki & 0x40) || !(ki & 0x1000) || be16(f + 7) != 16) /* install, encrypted key data, a 16 byte pairwise key */
        return WPA4_E_FORMAT;
    if (kd_len < 24 || kd_len % 8 || kd_len > 8 * 21)
        return WPA4_E_KEYDATA;
    u8 plain[8 * 20];
    if (aes_unwrap(w->ptk + 16, f + EAPOL_KEY_HDR, kd_len / 8 - 1, plain) != 0)
        return WPA4_E_KEYDATA;
    int g = wpa4_find_gtk(w, plain, kd_len - 8);
    zero(plain, sizeof plain);
    if (g == -2)
        return WPA4_E_NO_GTK;
    if (g != 0)
        return WPA4_E_KEYDATA;
    w->state = 2;
    *reply_len = wpa4_build(w, f[0], (ki & 0x0200) | 0x010A, replay, 0, 0, 0, reply);
    return WPA4_SEND_MSG4;
}
