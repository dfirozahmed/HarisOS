/* HarisOS Bangla phonetic typing: see include/bangla_ime.h. Freestanding, no allocation. */
#include "bangla_ime.h"

#define HAS 0x09CDu /* hasanta: joins two consonants */

enum { K_CONS, K_VOWEL, K_SIGN, K_SEP };

typedef struct
{
    const char *key;
    int kind;
    unsigned cp[3]; /* K_CONS / K_SIGN: the characters (0 = end); K_VOWEL: cp[0] = alone, cp[1] = sign after a consonant */
} Tok;

/* A lookup takes the longest key that matches, so the order here does not matter. */
static const Tok TOKS[] = {
    /* consonants */
    {"k", K_CONS, {0x0995}},          {"kh", K_CONS, {0x0996}},         {"g", K_CONS, {0x0997}},
    {"gh", K_CONS, {0x0998}},         {"Ng", K_CONS, {0x0999}},         {"c", K_CONS, {0x099A}},
    {"ch", K_CONS, {0x099A}},         {"chh", K_CONS, {0x099B}},        {"j", K_CONS, {0x099C}},
    {"jh", K_CONS, {0x099D}},         {"NG", K_CONS, {0x099E}},         {"T", K_CONS, {0x099F}},
    {"Th", K_CONS, {0x09A0}},         {"D", K_CONS, {0x09A1}},          {"Dh", K_CONS, {0x09A2}},
    {"N", K_CONS, {0x09A3}},          {"t", K_CONS, {0x09A4}},          {"th", K_CONS, {0x09A5}},
    {"d", K_CONS, {0x09A6}},          {"dh", K_CONS, {0x09A7}},         {"n", K_CONS, {0x09A8}},
    {"p", K_CONS, {0x09AA}},          {"ph", K_CONS, {0x09AB}},         {"f", K_CONS, {0x09AB}},
    {"b", K_CONS, {0x09AC}},          {"bh", K_CONS, {0x09AD}},         {"v", K_CONS, {0x09AD}},
    {"m", K_CONS, {0x09AE}},          {"z", K_CONS, {0x09AF}},          {"Z", K_CONS, {0x09AF}},
    {"r", K_CONS, {0x09B0}},          {"l", K_CONS, {0x09B2}},          {"sh", K_CONS, {0x09B6}},
    {"S", K_CONS, {0x09B6}},          {"Sh", K_CONS, {0x09B7}},         {"s", K_CONS, {0x09B8}},
    {"h", K_CONS, {0x09B9}},          {"R", K_CONS, {0x09DC}},          {"Rh", K_CONS, {0x09DD}},
    {"y", K_CONS, {0x09DF}},          {"q", K_CONS, {0x0995}},
    {"x", K_CONS, {0x0995, HAS, 0x09B8}},   /* ক্স */
    {"kkh", K_CONS, {0x0995, HAS, 0x09B7}}, /* ক্ষ */
    {"kSh", K_CONS, {0x0995, HAS, 0x09B7}},
    {"gg", K_CONS, {0x099C, HAS, 0x099E}},  /* জ্ঞ */
    /* vowels: alone, sign after a consonant */
    {"o", K_VOWEL, {0x0985, 0}},      {"a", K_VOWEL, {0x0986, 0x09BE}}, {"i", K_VOWEL, {0x0987, 0x09BF}},
    {"I", K_VOWEL, {0x0988, 0x09C0}}, {"u", K_VOWEL, {0x0989, 0x09C1}}, {"U", K_VOWEL, {0x098A, 0x09C2}},
    {"e", K_VOWEL, {0x098F, 0x09C7}}, {"oi", K_VOWEL, {0x0990, 0x09C8}}, {"O", K_VOWEL, {0x0993, 0x09CB}},
    {"ou", K_VOWEL, {0x0994, 0x09CC}}, {"rri", K_VOWEL, {0x098B, 0x09C3}},
    {"OI", K_VOWEL, {0x0990, 0x09C8}}, {"OU", K_VOWEL, {0x0994, 0x09CC}},
    /* signs */
    {"ng", K_SIGN, {0x0982}}, {":", K_SIGN, {0x0983}}, {"^", K_SIGN, {0x0981}}, {"t``", K_SIGN, {0x09CE}},
    /* ` keeps what is on both sides apart */
    {"`", K_SEP, {0}},
};
#define NTOKS ((int)(sizeof TOKS / sizeof TOKS[0]))

/* The longest token whose key starts s[0..n) and is at most maxlen long; 0 if none. */
static const Tok *match(const char *s, int n, int maxlen, int *len)
{
    const Tok *best = 0;
    int bl = 0;
    for (int i = 0; i < NTOKS; i++)
    {
        int l = 0;
        while (TOKS[i].key[l] && l < n && TOKS[i].key[l] == s[l])
            l++;
        if (!TOKS[i].key[l] && l > bl && l <= maxlen)
        {
            best = &TOKS[i];
            bl = l;
        }
    }
    *len = bl;
    return best;
}

enum { P_START, P_CONS, P_VOWEL }; /* what the last token was: nothing, a consonant (one may join it), a vowel / sign */

static int put(unsigned *out, int n, int max, unsigned cp)
{
    if (n < max)
        out[n] = cp;
    return n + 1;
}

int bn_translit(const char *s, int n, unsigned *out, int max)
{
    int o = 0, prev = P_START, i = 0;
    while (i < n)
    {
        int len;
        const Tok *t = match(s + i, n - i, 3, &len);
        char lower;
        if (t && t->kind == K_VOWEL && prev == P_CONS && len == 2 && t->key[0] == 'o')
            t = match(s + i, n - i, 1, &len); /* "oi" / "ou" after a consonant: "o" (no sign), then the vowel */
        if (!t && s[i] >= 'A' && s[i] <= 'Z') /* a capital with no meaning of its own is the small letter */
        {
            lower = (char)(s[i] + 32);
            t = match(&lower, 1, 1, &len);
        }
        if (!t) /* anything else is typed as it is */
        {
            o = put(out, o, max, (unsigned char)s[i]);
            prev = P_START;
            i++;
            continue;
        }
        if (len == 0)
            len = 1;
        switch (t->kind)
        {
        case K_CONS:
        {
            if (prev == P_CONS)
                o = put(out, o, max, HAS); /* consonant after consonant: joined */
            for (int k = 0; k < 3 && t->cp[k]; k++)
            {
                unsigned cp = t->cp[k];
                if (cp == 0x09DF && prev == P_CONS) /* য় on its own, but য-ফলা after a consonant */
                    cp = 0x09AF;
                o = put(out, o, max, cp);
            }
            prev = P_CONS;
            break;
        }
        case K_VOWEL:
            if (t->cp[1] == 0 && prev == P_CONS) /* "o" after a consonant: no sign, and the consonant stays alone */
                ;
            else
                o = put(out, o, max, prev == P_CONS ? t->cp[1] : t->cp[0]);
            prev = P_VOWEL;
            break;
        case K_SIGN:
            o = put(out, o, max, t->cp[0]);
            prev = P_VOWEL;
            break;
        default: /* K_SEP */
            prev = P_VOWEL;
            break;
        }
        i += len;
    }
    return o < max ? o : max;
}

/* ---- the national layout (BDS 1738): every key is a Bangla character ---- */
/* The table of the Bangladesh Computer Council's National Bangla keyboard, as in xkeyboard-config (symbols/bd, "basic").
 * Four levels per key: plain, Shift, AltGr, Shift+AltGr; 0 = the key has nothing there (it types what it normally does).
 * Left out, as xkeyboard-config itself marks them: U+09B3 and U+09BB (not allocated in Unicode) and U+0902 (a Devanagari sign). */
static const struct
{
    char key; /* the key, named by the character it types on a US keyboard without Shift */
    unsigned short cp[4];
} NATIONAL[] = {
    {'1', {0x09E7, 0, 0x09F4, 0}},      {'2', {0x09E8, 0, 0x09F5, 0}},      {'3', {0x09E9, 0, 0x09F6, 0}},
    {'4', {0x09EA, 0, 0x09F3, 0x09F2}}, {'5', {0x09EB, 0, 0x09F7, 0}},      {'6', {0x09EC, 0, 0x09F8, 0}},
    {'7', {0x09ED, 0, 0, 0}},           {'8', {0x09EE, 0, 0, 0}},           {'9', {0x09EF, 0, 0, 0}},
    {'0', {0x09E6, 0, 0x09F9, 0}},      {'-', {0, 0, 0x200C, 0}},           {'=', {0, 0, 0x200D, 0}},
    {'q', {0x0999, 0x0982, 0x09E2, 0x09E3}}, {'w', {0x09AF, 0x09DF, 0, 0}}, {'e', {0x09A1, 0x09A2, 0x09C4, 0}},
    {'r', {0x09AA, 0x09AB, 0, 0}},      {'t', {0x099F, 0x09A0, 0, 0}},      {'y', {0x099A, 0x099B, 0, 0}},
    {'u', {0x099C, 0x099D, 0, 0}},      {'i', {0x09B9, 0x099E, 0x09BD, 0}}, {'o', {0x0997, 0x0998, 0, 0}},
    {'p', {0x09DC, 0x09DD, 0, 0}},
    {'a', {0x09C3, 0x09D7, 0x098B, 0x09E0}}, {'s', {0x09C1, 0x09C2, 0x0989, 0x098A}},
    {'d', {0x09BF, 0x09C0, 0x0987, 0x0988}}, {'f', {0x09AC, 0x09AD, 0x09F0, 0x09F1}},
    {'g', {0x09CD, 0x0964, 0x0965, 0}}, {'h', {0x09BE, 0x0985, 0x0986, 0}}, {'j', {0x0995, 0x0996, 0, 0}},
    {'k', {0x09A4, 0x09A5, 0x09CE, 0}}, {'l', {0x09A6, 0x09A7, 0x098C, 0x09E1}},
    {'z', {0x0981, 0x0983, 0x09FA, 0}}, {'x', {0x09CB, 0x09CC, 0x0993, 0x0994}},
    {'c', {0x09C7, 0x09C8, 0x098F, 0x0990}}, {'v', {0x09B0, 0x09B2, 0, 0}}, {'b', {0x09A8, 0x09A3, 0, 0}},
    {'n', {0x09B8, 0x09B7, 0, 0}},      {'m', {0x09AE, 0x09B6, 0, 0}},      {'.', {0, 0, 0x09BC, 0}},
};

/* The character a key types in the national layout at this level, 0 if the layout leaves it alone. */
static unsigned national_char(int key, unsigned mods)
{
    int level = ((mods & KMOD_SHIFT) ? 1 : 0) + ((mods & KMOD_ALTGR) ? 2 : 0);
    for (unsigned i = 0; i < sizeof NATIONAL / sizeof NATIONAL[0]; i++)
        if (NATIONAL[i].key == key)
            return NATIONAL[i].cp[level];
    return 0;
}

/* ---- the filter ---- */
#define WORD_MAX 24
#define OUT_MAX 64

static int ACTIVE;
static int LAYOUT = IME_PHONETIC; /* which Bangla layout is used while ACTIVE */
static char WORD[WORD_MAX];
static int WLEN;
static unsigned SENT[OUT_MAX]; /* what was handed on for the word so far */
static int SLEN;
static int LAST_DANDA; /* the last key typed "।": another "." makes it a full stop */

int ime_active(void) { return ACTIVE; }

int ime_mode(void) { return ACTIVE ? LAYOUT : IME_OFF; }

void ime_reset(void)
{
    WLEN = 0;
    SLEN = 0;
    LAST_DANDA = 0;
}

void ime_set_active(int on)
{
    ACTIVE = on != 0;
    ime_reset();
}

void ime_set_layout(int layout)
{
    LAYOUT = layout == IME_NATIONAL ? IME_NATIONAL : IME_PHONETIC;
    ime_reset();
}

static void ev_char(KeyEvent *e, unsigned cp)
{
    e->key = 0;
    e->ch = (int)cp;
    e->pressed = 1;
    e->mods = 0;
}

static int is_word_char(int ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '`' || ch == ':' || ch == '^';
}

/* Convert the word again and hand on the difference to what was sent: Backspaces for the characters that changed, then
 * the new ones. */
static int emit_word(KeyEvent *out, int max)
{
    unsigned now[OUT_MAX];
    int n = bn_translit(WORD, WLEN, now, OUT_MAX);
    int common = 0;
    while (common < n && common < SLEN && now[common] == SENT[common])
        common++;
    int bs = SLEN - common, add = n - common, k = 0;
    if (bs + add > max)
        return -1;
    for (int i = 0; i < bs; i++, k++)
    {
        out[k].key = KEY_BACKSPACE;
        out[k].ch = 0;
        out[k].pressed = 1;
        out[k].mods = 0;
    }
    for (int i = common; i < n; i++, k++)
        ev_char(&out[k], now[i]);
    for (int i = 0; i < n; i++)
        SENT[i] = now[i];
    SLEN = n;
    return k;
}

int ime_filter(const KeyEvent *in, KeyEvent *out, int max)
{
    if (max < 1)
        return 0;
    if (in->pressed && in->key == ' ' && (in->mods & KMOD_CTRL) && !(in->mods & KMOD_ALT))
    {
        if (in->mods & KMOD_SHIFT) /* Ctrl+Shift+Space: the other Bangla layout, and Bangla on */
        {
            ime_set_layout(LAYOUT == IME_PHONETIC ? IME_NATIONAL : IME_PHONETIC);
            ACTIVE = 1;
        }
        else /* Ctrl+Space: Bangla on / off */
            ime_set_active(!ACTIVE);
        return 0;
    }
    if (!ACTIVE || !in->pressed || max < IME_MAX_EVENTS)
    {
        out[0] = *in;
        return 1;
    }
    if (LAYOUT == IME_NATIONAL)
    {
        /* every key has its own character; Ctrl and the left Alt are shortcuts, the right Alt (AltGr) is the third level */
        int shortcut = (in->mods & KMOD_CTRL) || ((in->mods & KMOD_ALT) && !(in->mods & KMOD_ALTGR));
        unsigned cp = !shortcut && in->key >= 32 && in->key < 127 ? national_char(in->key, in->mods) : 0;
        if (cp)
        {
            ev_char(&out[0], cp);
            return 1;
        }
        out[0] = *in; /* a key the layout leaves alone: punctuation, Space, Enter, the arrows */
        return 1;
    }
    int was_danda = LAST_DANDA;
    LAST_DANDA = 0;
    int plain = !(in->mods & (KMOD_CTRL | KMOD_ALT));

    if (plain && in->key == KEY_BACKSPACE && WLEN > 0) /* takes back the last key typed */
    {
        WLEN--;
        int n = emit_word(out, max);
        if (n >= 0)
            return n;
        ime_reset();
        out[0] = *in;
        return 1;
    }
    if (plain && in->ch >= 32 && in->ch < 127)
    {
        if (is_word_char(in->ch))
        {
            if (WLEN == WORD_MAX) /* a very long word: a new one starts here */
                ime_reset();
            WORD[WLEN++] = (char)in->ch;
            int n = emit_word(out, max);
            if (n >= 0)
                return n;
            ime_reset();
            out[0] = *in;
            return 1;
        }
        ime_reset(); /* digits, space, punctuation: the word ends */
        if (in->ch >= '0' && in->ch <= '9')
        {
            ev_char(&out[0], 0x09E6u + (unsigned)(in->ch - '0'));
            return 1;
        }
        if (in->ch == '$')
        {
            ev_char(&out[0], 0x09F3u); /* ৳ */
            return 1;
        }
        if (in->ch == '.')
        {
            if (was_danda) /* "।" and "." again: a full stop */
            {
                out[0].key = KEY_BACKSPACE;
                out[0].ch = 0;
                out[0].pressed = 1;
                out[0].mods = 0;
                ev_char(&out[1], '.');
                return 2;
            }
            ev_char(&out[0], 0x0964u); /* । */
            LAST_DANDA = 1;
            return 1;
        }
    }
    else
        ime_reset(); /* an arrow, Enter, a shortcut ...: the word is over */
    out[0] = *in;
    return 1;
}
