/* Host tests for kernel/net/wpa.c: SHA-1, HMAC-SHA-1, PBKDF2, the 802.11i PRF / PTK, AES-128 and the RFC 3394 key wrap.
 * The vectors come from the standards (FIPS 180, FIPS 197, RFC 2202, RFC 6070, RFC 3394, IEEE 802.11i) and, for everything with made-up inputs,
 * from OpenSSL (the commands are in the comments): an implementation that did not come from the same head. */
#include <stdio.h>
#include <string.h>
#include "wpa.h"

static int fails;
#define CHECK(c)                                          \
    do                                                    \
    {                                                     \
        if (!(c))                                         \
        {                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #c);   \
            fails++;                                      \
        }                                                 \
    } while (0)

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10; }

/* hex text -> bytes; returns the length */
static unsigned unhex(const char *h, unsigned char *out)
{
    unsigned n = 0;
    for (; h[0] && h[1]; h += 2)
        out[n++] = (unsigned char)(hexval(h[0]) << 4 | hexval(h[1]));
    return n;
}

static int same(const unsigned char *got, const char *want_hex)
{
    unsigned char want[256];
    unsigned n = unhex(want_hex, want);
    return memcmp(got, want, n) == 0;
}

static void pattern(unsigned char *p, unsigned n, unsigned mul, unsigned add)
{
    for (unsigned i = 0; i < n; i++)
        p[i] = (unsigned char)((i * mul + add) % 256);
}

int main(void)
{
    unsigned char out[64], buf[512];

    /* ---- SHA-1: FIPS 180 examples, then lengths around the block boundaries (OpenSSL: openssl dgst -sha1) ---- */
    sha1("", 0, out);
    CHECK(same(out, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    sha1("abc", 3, out);
    CHECK(same(out, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    CHECK(same(out, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    {
        Sha1 s;
        sha1_init(&s);
        for (int i = 0; i < 1000000; i++)
            sha1_update(&s, "a", 1);
        sha1_final(&s, out);
        CHECK(same(out, "34aa973cd4c4daa4f61eeb2bdbad27316534016f")); /* a million 'a' */
    }
    static const struct { unsigned n; const char *hex; } SHA_LEN[] = {
        {55, "c81bf1d74014b0769701d371369e479788b9091b"}, {56, "cc77d948b8ac59d164f45209d0d5bae5ec834a8a"},
        {57, "e3476e2b9325cdbc0cd85d65d612c67270acb30b"}, {63, "6cc51226fb5c4ba3f05fdb0237e68f22658a0bec"},
        {64, "3a834c6733c13354477ffa76cfea58f915da3737"}, {65, "633304443ac09a0a57637573105c99a99738782d"},
        {119, "645c4bfbf3175d1bd2ff038e655d5e9d11ec3592"}, {120, "20081ecab2bc8f4ec7f75a185fcc53855d284a2b"},
        {128, "7c185107ea601e73da9617e6767cbd7bc919beb3"}, {200, "ccaf6f4eaf9b2106161c5e0077a8b202b456fd2a"},
    };
    for (unsigned i = 0; i < sizeof SHA_LEN / sizeof SHA_LEN[0]; i++)
    {
        pattern(buf, SHA_LEN[i].n, 31, 11);
        sha1(buf, SHA_LEN[i].n, out);
        if (!same(out, SHA_LEN[i].hex))
        {
            printf("FAIL sha1 of %u bytes\n", SHA_LEN[i].n);
            fails++;
        }
        /* the same in pieces of 7 bytes: the buffering must not matter */
        Sha1 s;
        sha1_init(&s);
        for (unsigned off = 0; off < SHA_LEN[i].n; off += 7)
            sha1_update(&s, buf + off, SHA_LEN[i].n - off < 7 ? SHA_LEN[i].n - off : 7);
        sha1_final(&s, out);
        CHECK(same(out, SHA_LEN[i].hex));
    }

    /* ---- HMAC-SHA-1: RFC 2202 cases 1, 2 and 6 (a key longer than the block), and one made-up case (OpenSSL: openssl dgst -sha1 -mac HMAC) ---- */
    {
        unsigned char key[100];
        memset(key, 0x0b, 20);
        hmac_sha1(key, 20, "Hi There", 8, 0, 0, out);
        CHECK(same(out, "b617318655057264e28bc0b6fb378c8ef146be00"));
        hmac_sha1((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, 0, 0, out);
        CHECK(same(out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
        memset(key, 0xaa, 80);
        hmac_sha1(key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, 0, 0, out);
        CHECK(same(out, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
        unsigned char k20[20], m[130];
        pattern(k20, 20, 1, 1);
        pattern(m, 130, 13, 7);
        hmac_sha1(k20, 20, m, 130, 0, 0, out);
        CHECK(same(out, "435bbd26c0c12ceab1b5348a0362c4bc6ef2a6ac"));
        hmac_sha1(k20, 20, m, 50, m + 50, 80, out); /* the message in two parts */
        CHECK(same(out, "435bbd26c0c12ceab1b5348a0362c4bc6ef2a6ac"));
    }

    /* ---- PBKDF2-HMAC-SHA-1: RFC 6070 ---- */
    pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 1, out, 20);
    CHECK(same(out, "0c60c80f961f0e71f3a9b524af6012062fe037a6"));
    pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 2, out, 20);
    CHECK(same(out, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"));
    pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 4096, out, 20);
    CHECK(same(out, "4b007901b765489abead49d926f721d065a429c1"));
    pbkdf2_sha1((const u8 *)"passwordPASSWORDpassword", 24, (const u8 *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, out, 25);
    CHECK(same(out, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"));

    /* ---- the PMK of WPA2-Personal: the IEEE 802.11i test case (OpenSSL: openssl kdf -keylen 32 -kdfopt digest:SHA1 ... PBKDF2), and the longest
     * passphrase with the longest name ---- */
    wpa_pmk("ThisIsAPassword", 15, "ThisIsASSID", 11, out);
    CHECK(same(out, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));
    {
        char pass[64], ssid[33];
        memset(pass, 'p', 63);
        memset(ssid, 's', 32);
        wpa_pmk(pass, 63, ssid, 32, out);
        CHECK(same(out, "b280c572a48c8025006d017f46a75b7759308f3fe3223b5a08bd3019d6d17970"));
    }

    /* ---- the PRF and the PTK (computed with HMAC from OpenSSL over label || 0 || data || counter) ---- */
    {
        unsigned char pmk[32], aa[6] = {0x02, 0x00, 0x00, 0x00, 0xa1, 0x00}, spa[6] = {0x00, 0xe0, 0x4c, 0x68, 0x12, 0x34}, an[32], sn[32], ptk[48];
        unhex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", pmk);
        for (unsigned i = 0; i < 32; i++)
        {
            an[i] = (unsigned char)(i * 3 + 1);
            sn[i] = (unsigned char)(255 - i * 5);
        }
        const char *want = "d6e21035f629913ccdc545c7bee00459b7adadb41b7c972bf7d4859f4eaae1257909ada3f119ec785c71a7f7426cfb07";
        wpa_ptk(pmk, aa, spa, an, sn, ptk);
        CHECK(same(ptk, want));
        /* it is symmetric: the access point computes the same from its side (the order of the addresses and of the nonces does not matter) */
        unsigned char ptk2[48];
        wpa_ptk(pmk, spa, aa, sn, an, ptk2);
        CHECK(memcmp(ptk, ptk2, 48) == 0);
        /* the PRF output is a prefix of a longer one */
        unsigned char data[76], p16[16], p60[60];
        for (unsigned i = 0; i < 76; i++)
            data[i] = (unsigned char)i;
        wpa_prf(pmk, 32, "Pairwise key expansion", data, 76, p16, 16);
        wpa_prf(pmk, 32, "Pairwise key expansion", data, 76, p60, 60);
        CHECK(memcmp(p16, p60, 16) == 0);
    }

    /* ---- AES-128: FIPS 197 appendix C.1, and the round trip with many blocks ---- */
    {
        Aes128 a;
        unsigned char key[16], pt[16], ct[16], back[16];
        unhex("000102030405060708090a0b0c0d0e0f", key);
        unhex("00112233445566778899aabbccddeeff", pt);
        aes128_set_key(&a, key);
        aes128_encrypt(&a, pt, ct);
        CHECK(same(ct, "69c4e0d86a7b0430d8cdb78070b4c55a"));
        aes128_decrypt(&a, ct, back);
        CHECK(memcmp(back, pt, 16) == 0);
        for (unsigned t = 0; t < 50; t++)
        {
            unsigned char k2[16], p2[16];
            pattern(k2, 16, 7 + t, t);
            pattern(p2, 16, 11 + t, 3 * t);
            aes128_set_key(&a, k2);
            aes128_encrypt(&a, p2, ct);
            aes128_decrypt(&a, ct, back);
            CHECK(memcmp(back, p2, 16) == 0 && memcmp(ct, p2, 16) != 0);
        }
    }

    /* ---- the key wrap: RFC 3394 section 4.1 (128 bits under a 128 bit KEK), and a 5 block case from OpenSSL
     * (openssl enc -id-aes128-wrap -K <kek> -iv A6A6A6A6A6A6A6A6) ---- */
    {
        unsigned char kek[16], pl[64], wrapped[80], back[64];
        unhex("000102030405060708090a0b0c0d0e0f", kek);
        unhex("00112233445566778899aabbccddeeff", pl);
        aes_wrap(kek, pl, 2, wrapped);
        CHECK(same(wrapped, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
        CHECK(aes_unwrap(kek, wrapped, 2, back) == 0 && memcmp(back, pl, 16) == 0);
        unhex("05162738495a6b7c8d9eafc0d1e2f310", kek);
        pattern(pl, 40, 7, 3);
        aes_wrap(kek, pl, 5, wrapped);
        CHECK(same(wrapped, "5ff401e94095e4da41ebefc8ec2bc164b52944ae0574ddfea1ae9a36fd76a394b09a3fd7bdba3e4f1fcd464d78dc8785"));
        CHECK(aes_unwrap(kek, wrapped, 5, back) == 0 && memcmp(back, pl, 40) == 0);
        /* a damaged wrapped key, or the wrong KEK, is refused */
        wrapped[20] ^= 1;
        CHECK(aes_unwrap(kek, wrapped, 5, back) == -1);
        wrapped[20] ^= 1;
        kek[0] ^= 1;
        CHECK(aes_unwrap(kek, wrapped, 5, back) == -1);
    }

    /* ---- the 4-way handshake against an authenticator built in the test (it uses the same primitives, which were checked above, but builds
     * the frames on its own, field by field, as the layout of hostap's struct wpa_eapol_key says) ---- */
    {
        const char *pass = "ThisIsAPassword", *ssid = "ThisIsASSID";
        unsigned char pmk[32], aa[6] = {0x02, 0x00, 0x00, 0x00, 0xa1, 0x00}, spa[6] = {0x00, 0xe0, 0x4c, 0x68, 0x12, 0x34}, an[32], sn[32];
        wpa_pmk(pass, 15, ssid, 11, pmk);
        for (unsigned i = 0; i < 32; i++)
        {
            an[i] = (unsigned char)(i * 3 + 1);
            sn[i] = (unsigned char)(255 - i * 5);
        }
        static const unsigned char RSN[22] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
        unsigned char ptk[48];
        wpa_ptk(pmk, aa, spa, an, sn, ptk);

        /* the authenticator's frame builder: descriptor version 2, pairwise */
#define BUILD(frame, info, replay_n, nonce_p, kdata, kdlen, kck)                 \
    do                                                                           \
    {                                                                            \
        memset(frame, 0, sizeof frame);                                          \
        frame[0] = 2;                                                            \
        frame[1] = 3;                                                            \
        frame[2] = (unsigned char)((95 + (kdlen)) >> 8);                         \
        frame[3] = (unsigned char)(95 + (kdlen));                                \
        frame[4] = 2;                                                            \
        frame[5] = (unsigned char)((info) >> 8);                                 \
        frame[6] = (unsigned char)(info);                                        \
        frame[8] = 16;                                                           \
        frame[16] = (unsigned char)(replay_n);                                   \
        memcpy(frame + 17, nonce_p, 32);                                         \
        frame[97] = (unsigned char)((kdlen) >> 8);                               \
        frame[98] = (unsigned char)(kdlen);                                      \
        memcpy(frame + 99, kdata, kdlen);                                        \
        if ((info) & 0x100)                                                      \
            wpa_eapol_mic(kck, frame, 99 + (kdlen), frame + 81);                 \
    } while (0)

        /* message 3's key data: the RSN element, the GTK KDE (key id 1, transmit), padding */
        unsigned char plain[8 * 6], wrapped[8 * 7];
        memset(plain, 0, sizeof plain);
        memcpy(plain, RSN, 22);
        plain[22] = 0xDD;
        plain[23] = 22; /* OUI 3 + type 1 + key id/tx 1 + reserved 1 + 16 key bytes = 22 */
        plain[24] = 0x00;
        plain[25] = 0x0F;
        plain[26] = 0xAC;
        plain[27] = 1;
        plain[28] = 0x05; /* key id 1, transmit */
        plain[29] = 0;
        for (unsigned i = 0; i < 16; i++)
            plain[30 + i] = (unsigned char)(0x40 + i);
        plain[46] = 0xDD; /* padding to 48 bytes */
        aes_wrap(ptk + 16, plain, 6, wrapped);

        unsigned char m1[200], m2[EAPOL_MAX], m3[300], m4[EAPOL_MAX];
        unsigned l2 = 0, l4 = 0;
        Wpa4 w;

        BUILD(m1, 0x008A, 1, an, "", 0, ptk);
        wpa4_init(&w, pmk, aa, spa, RSN, 22);
        CHECK(wpa4_rx(&w, m1, 0, sn, m2, &l2) == WPA4_IGNORED); /* nothing at all */
        CHECK(wpa4_rx(&w, m1, 99, sn, m2, &l2) == WPA4_SEND_MSG2 && l2 == 99 + 22);
        /* message 2: key info 0x010A, key length 0, the replay counter of message 1, our SNonce, the RSN element, signed with the KCK */
        CHECK(m2[0] == 2 && m2[1] == 3 && m2[2] == 0 && m2[3] == 95 + 22 && m2[4] == 2 && m2[5] == 0x01 && m2[6] == 0x0A && m2[7] == 0 && m2[8] == 0);
        CHECK(m2[16] == 1 && memcmp(m2 + 17, sn, 32) == 0 && m2[97] == 0 && m2[98] == 22 && memcmp(m2 + 99, RSN, 22) == 0);
        {
            unsigned char mic[16];
            wpa_eapol_mic(ptk, m2, l2, mic);
            CHECK(memcmp(mic, m2 + 81, 16) == 0);
            /* the MIC by hand: the frame hashed with its MIC field zero, the first 16 bytes of the HMAC */
            unsigned char copy2[EAPOL_MAX], h[20];
            memcpy(copy2, m2, l2);
            memset(copy2 + 81, 0, 16);
            hmac_sha1(ptk, 16, copy2, l2, 0, 0, h);
            CHECK(memcmp(h, m2 + 81, 16) == 0);
        }
        CHECK(w.state == 1 && w.have_ptk && memcmp(w.ptk, ptk, 48) == 0);

        /* message 3 */
        BUILD(m3, 0x13CA, 2, an, wrapped, 56, ptk);
        unsigned l3 = 99 + 56;
        CHECK(wpa4_rx(&w, m3, l3, sn, m4, &l4) == WPA4_SEND_MSG4 && l4 == 99);
        CHECK(m4[0] == 2 && m4[1] == 3 && m4[3] == 95 && m4[5] == 0x03 && m4[6] == 0x0A && m4[16] == 2 && m4[97] == 0 && m4[98] == 0);
        {
            unsigned char mic[16];
            wpa_eapol_mic(ptk, m4, l4, mic);
            CHECK(memcmp(mic, m4 + 81, 16) == 0);
        }
        CHECK(w.state == 2 && w.gtk_len == 16 && w.gtk_id == 1 && memcmp(w.gtk, plain + 30, 16) == 0);

        /* failures: each with a fresh handshake up to message 1, and a message 3 that is wrong in one way */
        static const char *const WHAT[] = {"bad MIC",         "replayed counter", "other ANonce", "no GTK",       "damaged key data",
                                           "wrong passphrase", "too short",        "key info v1",  "key length",   "no install bit"};
        static const int WANT[] = {WPA4_E_MIC,    WPA4_E_REPLAY, WPA4_E_NONCE, WPA4_E_NO_GTK, WPA4_E_KEYDATA,
                                   WPA4_E_MIC,    WPA4_E_SHORT,  WPA4_E_FORMAT, WPA4_E_FORMAT, WPA4_E_FORMAT};
        for (unsigned t = 0; t < 10; t++)
        {
            Wpa4 v;
            wpa4_init(&v, pmk, aa, spa, RSN, 22);
            wpa4_rx(&v, m1, 99, sn, m2, &l2);
            unsigned char f[300], an2[32], wr2[56], ptk_wrong[48];
            const unsigned char *kck = ptk;
            memcpy(wr2, wrapped, 56);
            memcpy(an2, an, 32);
            unsigned info = 0x13CA, rc = 2, kdl = 56;
            if (t == 1)
                rc = 1; /* not larger than message 1's */
            if (t == 2)
                an2[5] ^= 1;
            if (t == 3) /* only the RSN element, no group key */
            {
                unsigned char p2[24];
                memset(p2, 0, 24);
                memcpy(p2, RSN, 22);
                p2[22] = 0xDD;
                aes_wrap(ptk + 16, p2, 3, wr2);
                kdl = 32;
            }
            if (t == 4)
                wr2[10] ^= 1; /* damaged ciphertext, but signed correctly */
            if (t == 5) /* the access point has another passphrase: another PMK, another KCK */
            {
                unsigned char pmk2[32];
                wpa_pmk("OtherPassword", 13, ssid, 11, pmk2);
                wpa_ptk(pmk2, aa, spa, an, sn, ptk_wrong);
                kck = ptk_wrong;
            }
            if (t == 7)
                info = 0x13C9; /* descriptor version 1 */
            if (t == 9)
                info = 0x138A; /* no install bit */
            BUILD(f, info, rc, an2, wr2, kdl, kck);
            if (t == 8)
            {
                f[7] = 0;
                f[8] = 32; /* key length 32 */
                wpa_eapol_mic(kck, f, 99 + kdl, f + 81);
            }
            if (t == 0)
                f[90] ^= 1; /* the MIC itself */
            unsigned fl = 99 + kdl;
            if (t == 6)
                fl = 50;
            int r = wpa4_rx(&v, f, fl, sn, m4, &l4);
            if (r != WANT[t])
            {
                printf("FAIL handshake case %u (%s): returned %d, want %d\n", t, WHAT[t], r, WANT[t]);
                fails++;
            }
            /* a frame that failed the signature or the length leaves the state alone: the right message 3 still works */
            if (t == 0 || t == 5 || t == 6)
                CHECK(wpa4_rx(&v, m3, l3, sn, m4, &l4) == WPA4_SEND_MSG4);
        }
        /* message 3 before message 1 */
        {
            Wpa4 v;
            wpa4_init(&v, pmk, aa, spa, RSN, 22);
            CHECK(wpa4_rx(&v, m3, l3, sn, m4, &l4) == WPA4_E_ORDER);
        }
        /* the same message 3 twice (a retransmission after our message 4 was lost): the second is refused by its counter; a message 1 again
         * (new replay counter, new ANonce) starts over */
        CHECK(wpa4_rx(&w, m3, l3, sn, m4, &l4) == WPA4_E_REPLAY);
        {
            unsigned char an3[32], snew[32], f1[200], ptk3[48];
            memcpy(an3, an, 32);
            an3[0] ^= 0x55;
            memcpy(snew, sn, 32);
            snew[0] ^= 0xAA;
            BUILD(f1, 0x008A, 3, an3, "", 0, ptk);
            CHECK(wpa4_rx(&w, f1, 99, snew, m2, &l2) == WPA4_SEND_MSG2);
            wpa_ptk(pmk, aa, spa, an3, snew, ptk3);
            CHECK(memcmp(w.ptk, ptk3, 48) == 0 && w.state == 1 && memcmp(m2 + 17, snew, 32) == 0);
        }
#undef BUILD
    }

    if (fails)
    {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("wpa tests passed\n");
    return 0;
}
