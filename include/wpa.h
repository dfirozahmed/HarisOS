/* WPA2-Personal (the 4-way handshake of IEEE 802.11i) without any hardware: hashes, key derivation, AES, and the pieces of the handshake.
 *
 *   passphrase + network name --PBKDF2-HMAC-SHA1, 4096 rounds--> PMK (32 bytes, the "pairwise master key")
 *   PMK + both addresses + both nonces --PRF-384--> PTK (48 bytes) = KCK (16, signs the messages) | KEK (16, wraps the group key) | TK (16, the
 *   key of the data frames, AES-CCMP)
 * The access point (authenticator) sends message 1 with its random number ANonce; we answer with message 2 (our SNonce, signed with the KCK);
 * message 3 (signed, with the group key GTK wrapped by the KEK) is checked and acknowledged by message 4. Then both sides install the keys.
 * Everything is plain C without libc (the kernel has none); the host tests in tests/test_wpa.c check every piece against test vectors that
 * were computed with OpenSSL, plus the vectors of the standards. */
#ifndef HARIS_WPA_H
#define HARIS_WPA_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* ---- SHA-1 and HMAC-SHA-1 ---- */
typedef struct
{
    u32 h[5];
    u64 bytes;
    u8 buf[64];
    unsigned n; /* bytes in buf */
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, unsigned len);
void sha1_final(Sha1 *s, u8 out[20]);
void sha1(const void *data, unsigned len, u8 out[20]);

/* HMAC-SHA-1 over msg1 followed by msg2 (msg2 may be 0 with len 0) */
void hmac_sha1(const u8 *key, unsigned key_len, const void *msg1, unsigned len1, const void *msg2, unsigned len2, u8 out[20]);

/* ---- key derivation ---- */
/* PBKDF2-HMAC-SHA-1 (RFC 8018): out[out_len] from a password and a salt, `iterations` rounds. */
void pbkdf2_sha1(const u8 *pass, unsigned pass_len, const u8 *salt, unsigned salt_len, unsigned iterations, u8 *out, unsigned out_len);

/* The PMK of WPA2-Personal: PBKDF2(passphrase 8..63 characters, SSID, 4096, 32 bytes). */
void wpa_pmk(const char *passphrase, unsigned pass_len, const char *ssid, unsigned ssid_len, u8 pmk[32]);

/* The PRF of IEEE 802.11i: out = HMAC(key, label || 0 || data || counter) for counter 0, 1, 2 ... joined; out_len bytes. */
void wpa_prf(const u8 *key, unsigned key_len, const char *label, const u8 *data, unsigned data_len, u8 *out, unsigned out_len);

/* The pairwise transient key: aa = the access point's address (BSSID), spa = ours. ptk[0..15] KCK, [16..31] KEK, [32..47] TK. */
void wpa_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6], const u8 anonce[32], const u8 snonce[32], u8 ptk[48]);

/* ---- AES-128 and the key wrap of RFC 3394 ---- */
typedef struct
{
    u8 rk[176]; /* the 11 round keys */
} Aes128;

void aes128_set_key(Aes128 *a, const u8 key[16]);
void aes128_encrypt(const Aes128 *a, const u8 in[16], u8 out[16]);
void aes128_decrypt(const Aes128 *a, const u8 in[16], u8 out[16]);

/* Wrap n 8-byte blocks of plain into n + 1 blocks (8 * (n + 1) bytes) / unwrap them again. unwrap returns 0, or -1 if the integrity check fails. */
void aes_wrap(const u8 kek[16], const u8 *plain, unsigned n_blocks, u8 *wrapped);
int aes_unwrap(const u8 kek[16], const u8 *wrapped, unsigned n_blocks, u8 *plain);

/* ---- the 4-way handshake, the station (supplicant) side ----
 * An EAPOL-Key frame (IEEE 802.1X): 802.1X header (version, type 3, body length 2 bytes), then the key descriptor: descriptor type (2 = RSN),
 * key information (2), key length (2), replay counter (8), key nonce (32), key IV (16), key RSC (8), reserved (8), key MIC (16), key data length (2),
 * key data. The key information bits: 0..2 descriptor version (2 = HMAC-SHA1 MIC and AES key wrap), 3 pairwise, 6 install, 7 ACK (from the
 * access point), 8 MIC present, 9 secure, 12 key data encrypted. */
#define EAPOL_KEY_HDR 99     /* the bytes before the key data */
#define EAPOL_KEY_MIC_AT 81
#define EAPOL_MAX 256        /* the largest EAPOL-Key frame handled (messages 1 and 3 of a plain WPA2 network are 100 and about 170 bytes) */

enum
{
    WPA4_IGNORED = 0,    /* not an EAPOL-Key frame of the handshake: nothing to do */
    WPA4_SEND_MSG2 = 1,  /* message 1 accepted: reply[0 .. *reply_len) is message 2 */
    WPA4_SEND_MSG4 = 2,  /* message 3 accepted: reply is message 4, and the keys (ptk, gtk) are ready to install */
    WPA4_SEND_GROUP2 = 3, /* group key message 1 accepted (wpa4_group_rx): reply is message 2, and the new gtk / gtk_id are ready to install */
    WPA4_E_SHORT = -1,   /* the frame is shorter than its own header says, or too long */
    WPA4_E_FORMAT = -2,  /* not the key information / descriptor this driver handles (only version 2, pairwise, RSN) */
    WPA4_E_REPLAY = -3,  /* a replay counter that is not larger than the last one accepted */
    WPA4_E_MIC = -4,     /* the signature is wrong: the passphrase is wrong, or the frame was altered */
    WPA4_E_KEYDATA = -5, /* the encrypted key data does not unwrap, or does not hold what message 3 must hold */
    WPA4_E_NONCE = -6,   /* message 3 carries another ANonce than message 1 */
    WPA4_E_NO_GTK = -7,  /* message 3 has no group key */
    WPA4_E_ORDER = -8    /* message 3 before message 1 */
};

typedef struct
{
    u8 pmk[32];
    u8 aa[6], spa[6];     /* the access point's address and ours */
    u8 rsn[40];           /* the RSN element (with tag and length) we sent in the association request: repeated in message 2 */
    unsigned rsn_len;
    u8 anonce[32], snonce[32];
    u8 ptk[48];           /* KCK | KEK | TK */
    int have_ptk;
    int have_replay;
    u64 replay;           /* the largest replay counter accepted from the access point */
    int state;            /* 0 waiting for message 1, 1 message 2 sent, 2 done */
    u8 gtk[32];
    unsigned gtk_len, gtk_id;
} Wpa4;

void wpa4_init(Wpa4 *w, const u8 pmk[32], const u8 aa[6], const u8 spa[6], const u8 *rsn, unsigned rsn_len);

/* One EAPOL-Key frame from the access point, starting at the 802.1X header. fresh_snonce: 32 random bytes the caller supplies for the case that
 * this is message 1 (it is used only then). reply[EAPOL_MAX] receives message 2 or 4. See the WPA4_ codes. */
int wpa4_rx(Wpa4 *w, const u8 *eapol, unsigned len, const u8 fresh_snonce[32], u8 *reply, unsigned *reply_len);

/* The group key handshake after the 4-way handshake has finished (the access point renews the group key now and then): one EAPOL-Key frame from
 * the access point that is not part of the pairwise handshake. On WPA4_SEND_GROUP2 reply[0 .. *reply_len) is the answer (it must be sent, or the
 * access point drops the station) and the new key is in w->gtk (key id w->gtk_id). Other codes: nothing to send, nothing to install. */
int wpa4_group_rx(Wpa4 *w, const u8 *eapol, unsigned len, u8 *reply, unsigned *reply_len);

/* MIC of an EAPOL-Key frame: HMAC-SHA1 over the frame with its MIC field zeroed, the first 16 bytes. */
void wpa_eapol_mic(const u8 kck[16], const u8 *frame, unsigned len, u8 mic[16]);

#endif
