/* Host test for the Bangla phonetic typing (kernel/input/bangla_ime.c): the letter -> Bangla conversion, and the filter that
 * turns typed keys into characters and Backspaces, checked against a tiny text buffer. */
#include "bangla_ime.h"
#include "utf8.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the conversion of an ASCII word as a UTF-8 string */
static const char *tr(const char *s)
{
    static char buf[256];
    unsigned cps[64];
    int n = bn_translit(s, (int)strlen(s), cps, 64), o = 0;
    for (int i = 0; i < n; i++)
        o += utf8_encode(cps[i], buf + o);
    buf[o] = 0;
    return buf;
}

#define EXPECT(ascii, utf8) do { if (strcmp(tr(ascii), utf8) != 0) { printf("FAIL line %d: \"%s\" -> \"%s\"\n", __LINE__, ascii, tr(ascii)); fails++; } } while (0)

/* ---- a text field: what the desktop would show after the events ---- */
static unsigned TEXT[256];
static int TLEN;
static void apply(const KeyEvent *e)
{
    if (e->key == KEY_BACKSPACE)
    {
        if (TLEN > 0)
            TLEN--;
    }
    else if (e->ch >= 32 && TLEN < 256)
        TEXT[TLEN++] = (unsigned)e->ch;
}
static const char *text(void)
{
    static char buf[512];
    int o = 0;
    for (int i = 0; i < TLEN; i++)
        o += utf8_encode(TEXT[i], buf + o);
    buf[o] = 0;
    return buf;
}
static int key(int ch, unsigned mods)
{
    KeyEvent in = {ch >= 32 && ch < 127 ? ch : 0, ch, 1, mods}, out[IME_MAX_EVENTS];
    if (ch >= 'A' && ch <= 'Z')
        in.key = ch + 32; /* a printable key is its lowercase character */
    if (mods & (KMOD_CTRL | KMOD_ALT))
        in.ch = 0; /* the keyboard driver types nothing for a shortcut */
    int n = ime_filter(&in, out, IME_MAX_EVENTS);
    for (int i = 0; i < n; i++)
        apply(&out[i]);
    return n;
}
/* a key with exact modifiers, as the keyboard driver delivers it: key = the key's own character, ch = what it types (0 with Ctrl / Alt) */
static void keym(int k, unsigned mods)
{
    KeyEvent in = {k, k, 1, mods}, out[IME_MAX_EVENTS];
    if ((mods & KMOD_SHIFT) && k == '1')
        in.ch = '!';
    if (mods & (KMOD_CTRL | KMOD_ALT))
        in.ch = 0;
    int n = ime_filter(&in, out, IME_MAX_EVENTS);
    for (int i = 0; i < n; i++)
        apply(&out[i]);
}
static void typed(const char *s)
{
    for (; *s; s++)
        key((unsigned char)*s, (*s >= 'A' && *s <= 'Z') ? KMOD_SHIFT : 0);
}
static void special(int k, unsigned mods)
{
    KeyEvent in = {k, 0, 1, mods}, out[IME_MAX_EVENTS];
    int n = ime_filter(&in, out, IME_MAX_EVENTS);
    for (int i = 0; i < n; i++)
        apply(&out[i]);
}
static void fresh(void)
{
    TLEN = 0;
    ime_reset();
}

int main(void)
{
    /* ---- the conversion ---- */
    EXPECT("ami", "\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xbf");                                     /* আমি */
    EXPECT("bangla", "\xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe");           /* বাংলা */
    EXPECT("gan", "\xe0\xa6\x97\xe0\xa6\xbe\xe0\xa6\xa8");                                     /* গান */
    EXPECT("gai", "\xe0\xa6\x97\xe0\xa6\xbe\xe0\xa6\x87");                                     /* গাই: a vowel after a vowel is whole */
    EXPECT("banglay", "\xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe\xe0\xa7\x9f");   /* বাংলায় (y after a vowel is য়) */
    EXPECT("kormo", "\xe0\xa6\x95\xe0\xa6\xb0\xe0\xa7\x8d\xe0\xa6\xae");                         /* কর্ম */
    EXPECT("kolom", "\xe0\xa6\x95\xe0\xa6\xb2\xe0\xa6\xae");                                   /* কলম: "o" gives no sign */
    EXPECT("shikkha", "\xe0\xa6\xb6\xe0\xa6\xbf\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xb7\xe0\xa6\xbe"); /* শিক্ষা */
    EXPECT("bondhu", "\xe0\xa6\xac\xe0\xa6\xa8\xe0\xa7\x8d\xe0\xa6\xa7\xe0\xa7\x81");           /* বন্ধু */
    EXPECT("prem", "\xe0\xa6\xaa\xe0\xa7\x8d\xe0\xa6\xb0\xe0\xa7\x87\xe0\xa6\xae");             /* প্রেম */
    EXPECT("khub", "\xe0\xa6\x96\xe0\xa7\x81\xe0\xa6\xac");                                    /* খুব */
    EXPECT("tOmake", "\xe0\xa6\xa4\xe0\xa7\x8b\xe0\xa6\xae\xe0\xa6\xbe\xe0\xa6\x95\xe0\xa7\x87"); /* তোমাকে: O is the sign ো */
    EXPECT("boi", "\xe0\xa6\xac\xe0\xa6\x87");                                                  /* বই: o, then the vowel i */
    EXPECT("oi", "\xe0\xa6\x90");                                                               /* ঐ at the start */
    EXPECT("bOI", "\xe0\xa6\xac\xe0\xa7\x88");                                                  /* বৈ: OI is the sign ৈ */
    EXPECT("krri", "\xe0\xa6\x95\xe0\xa7\x83");                                                 /* কৃ */
    EXPECT("rri", "\xe0\xa6\x8b");                                                              /* ঋ */
    EXPECT("du:kho", "\xe0\xa6\xa6\xe0\xa7\x81\xe0\xa6\x83\xe0\xa6\x96");                       /* দুঃখ */
    EXPECT("ca^d", "\xe0\xa6\x9a\xe0\xa6\xbe\xe0\xa6\x81\xe0\xa6\xa6");                         /* চাঁদ */
    EXPECT("kt``", "\xe0\xa6\x95\xe0\xa7\x8e");                                                 /* কৎ: t`` is ৎ */
    EXPECT("Bangladesh", "\xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe\xe0\xa6\xa6\xe0\xa7\x87\xe0\xa6\xb6"); /* B is b */
    EXPECT("k`h", "\xe0\xa6\x95\xe0\xa6\xb9");                                                  /* ` keeps k and h apart: কহ */
    EXPECT("kh", "\xe0\xa6\x96");
    EXPECT("kkh", "\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xb7");                                      /* ক্ষ */
    EXPECT("gg", "\xe0\xa6\x9c\xe0\xa7\x8d\xe0\xa6\x9e");                                       /* জ্ঞ */
    EXPECT("ky", "\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xaf");                                       /* ক্য: য-ফলা, not য় */
    EXPECT("ay", "\xe0\xa6\x86\xe0\xa7\x9f");                                                   /* আয় */
    EXPECT("w1", "w1");                                                                         /* what has no meaning stays as typed */
    EXPECT("", "");
    {   /* the output limit is respected and the full length is not exceeded */
        unsigned small[2];
        CHECK(bn_translit("bangla", 6, small, 2) == 2);
    }

    /* ---- the filter ---- */
    ime_set_active(0);
    fresh();
    typed("ami");
    CHECK(strcmp(text(), "ami") == 0);                       /* off: letters are letters */

    key(' ', KMOD_CTRL);                                      /* Ctrl+Space turns Bangla on, and types nothing */
    CHECK(ime_active() && strcmp(text(), "ami") == 0);
    fresh();
    typed("ami");
    CHECK(strcmp(text(), "\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xbf") == 0);
    typed(" ");                                               /* a space ends the word and is typed */
    typed("bangla");
    CHECK(strcmp(text(), "\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xbf \xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe") == 0);

    /* a later letter can change what was typed: k -> ক, h -> খ, then a -> খা */
    fresh();
    typed("k");
    CHECK(strcmp(text(), "\xe0\xa6\x95") == 0);
    typed("h");
    CHECK(strcmp(text(), "\xe0\xa6\x96") == 0);
    typed("a");
    CHECK(strcmp(text(), "\xe0\xa6\x96\xe0\xa6\xbe") == 0);
    typed("kk");                                              /* খা + k + k -> খাক্ক */
    CHECK(strcmp(text(), "\xe0\xa6\x96\xe0\xa6\xbe\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\x95") == 0);

    /* Backspace takes back the last key: "ami" -> "am" -> "a" -> "" and then it is an ordinary Backspace */
    fresh();
    typed("ami");
    special(KEY_BACKSPACE, 0);
    CHECK(strcmp(text(), "\xe0\xa6\x86\xe0\xa6\xae") == 0);
    special(KEY_BACKSPACE, 0);
    special(KEY_BACKSPACE, 0);
    CHECK(strcmp(text(), "") == 0);
    typed(" ");
    special(KEY_BACKSPACE, 0);                                /* the word is over: Backspace takes the space as usual */
    CHECK(strcmp(text(), "") == 0);

    /* typing "kh" then Backspace gives "ক" (the last key, h, is taken back) */
    fresh();
    typed("kh");
    special(KEY_BACKSPACE, 0);
    CHECK(strcmp(text(), "\xe0\xa6\x95") == 0);

    /* what ends a word: an arrow, Enter, a shortcut. The next letter starts fresh, and nothing already typed changes */
    fresh();
    typed("k");
    special(KEY_LEFT, 0);
    typed("h");
    CHECK(strcmp(text(), "\xe0\xa6\x95\xe0\xa6\xb9") == 0);   /* কহ, not খ */
    fresh();
    typed("k");
    key('c', KMOD_CTRL); /* a shortcut passes through (the field ignores it) and ends the word */
    typed("h");
    CHECK(strcmp(text(), "\xe0\xa6\x95\xe0\xa6\xb9") == 0);
    fresh();
    typed("k");
    ime_reset(); /* a mouse click */
    typed("h");
    CHECK(strcmp(text(), "\xe0\xa6\x95\xe0\xa6\xb9") == 0);

    /* digits, danda, taka */
    fresh();
    typed("a12");
    CHECK(strcmp(text(), "\xe0\xa6\x86\xe0\xa7\xa7\xe0\xa7\xa8") == 0); /* আ১২ */
    fresh();
    typed(".");
    CHECK(strcmp(text(), "\xe0\xa5\xa4") == 0);                          /* । */
    typed(".");
    CHECK(strcmp(text(), ".") == 0);                                     /* । and . -> a full stop */
    typed(".");
    CHECK(strcmp(text(), ".\xe0\xa5\xa4") == 0);
    fresh();
    typed("$");
    CHECK(strcmp(text(), "\xe0\xa7\xb3") == 0);                          /* ৳ */

    /* a key release and keys with Ctrl / Alt are not typing */
    {
        KeyEvent up = {'a', 'a', 0, 0}, out[IME_MAX_EVENTS];
        CHECK(ime_filter(&up, out, IME_MAX_EVENTS) == 1 && out[0].pressed == 0 && out[0].ch == 'a');
        KeyEvent alt = {'a', 0, 1, KMOD_ALT};
        CHECK(ime_filter(&alt, out, IME_MAX_EVENTS) == 1 && out[0].ch == 0);
    }

    /* a long word does not overflow: past the limit a new word starts */
    fresh();
    for (int i = 0; i < 60; i++)
        key('k', 0);
    CHECK(TLEN > 0 && TLEN < 256);

    /* ---- the national layout: Ctrl+Shift+Space switches to it ---- */
    CHECK(ime_mode() == IME_PHONETIC);
    fresh();
    keym(' ', KMOD_CTRL | KMOD_SHIFT);
    CHECK(ime_mode() == IME_NATIONAL && ime_active() && TLEN == 0);
    keym('f', 0);
    keym('h', 0);
    keym('q', KMOD_SHIFT);
    keym('v', KMOD_SHIFT);
    keym('h', 0);
    CHECK(strcmp(text(), "\xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe") == 0); /* ব া ং ল া = বাংলা */
    fresh();
    keym('j', 0);
    keym('d', 0);
    CHECK(strcmp(text(), "\xe0\xa6\x95\xe0\xa6\xbf") == 0);                                     /* ক ি: typed in spoken order */
    fresh();
    keym('h', KMOD_ALT | KMOD_ALTGR);                    /* AltGr: the independent vowels */
    keym('m', 0);
    keym('d', 0);
    CHECK(strcmp(text(), "\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xbf") == 0);                         /* আমি */
    keym('d', KMOD_ALT | KMOD_ALTGR);
    keym('d', KMOD_ALT | KMOD_ALTGR | KMOD_SHIFT);
    keym('h', KMOD_SHIFT);
    CHECK(strcmp(text(), "\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xbf\xe0\xa6\x87\xe0\xa6\x88\xe0\xa6\x85") == 0); /* ... ই ঈ অ */
    fresh();
    keym('j', 0);
    keym('g', 0);
    keym('j', 0);
    keym('g', KMOD_SHIFT);
    CHECK(strcmp(text(), "\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\x95\xe0\xa5\xa4") == 0);             /* ক ্ ক । : hasanta and danda */
    fresh();
    keym('1', 0);
    keym('0', 0);
    keym('4', KMOD_ALT | KMOD_ALTGR);
    CHECK(strcmp(text(), "\xe0\xa7\xa7\xe0\xa7\xa6\xe0\xa7\xb3") == 0);                         /* ১ ০ ৳ */
    fresh();
    keym('-', KMOD_ALT | KMOD_ALTGR);
    keym('=', KMOD_ALT | KMOD_ALTGR);
    CHECK(TLEN == 2 && TEXT[0] == 0x200C && TEXT[1] == 0x200D);                                /* ZWNJ ZWJ */
    /* what the layout leaves alone is typed as usual, and shortcuts are not typing */
    fresh();
    keym(',', 0);
    keym('1', KMOD_SHIFT);
    keym(' ', 0);
    keym('=', 0);
    CHECK(strcmp(text(), ",! =") == 0);
    {
        KeyEvent out[IME_MAX_EVENTS];
        KeyEvent ctrl_j = {'j', 0, 1, KMOD_CTRL}, left_alt = {'h', 0, 1, KMOD_ALT}, bs = {KEY_BACKSPACE, 0, 1, 0};
        CHECK(ime_filter(&ctrl_j, out, IME_MAX_EVENTS) == 1 && out[0].key == 'j' && out[0].ch == 0);
        CHECK(ime_filter(&left_alt, out, IME_MAX_EVENTS) == 1 && out[0].key == 'h' && out[0].ch == 0);
        CHECK(ime_filter(&bs, out, IME_MAX_EVENTS) == 1 && out[0].key == KEY_BACKSPACE);
        KeyEvent up = {'j', 'j', 0, 0};
        CHECK(ime_filter(&up, out, IME_MAX_EVENTS) == 1 && out[0].pressed == 0 && out[0].ch == 'j'); /* a release is not typing */
    }
    /* every table entry types an assigned Bangla character, and none is typed in two places by accident of a level */
    {
        int n = 0;
        for (int k = 32; k < 127; k++)
            for (unsigned m = 0; m < 4; m++)
            {
                KeyEvent in = {k, k, 1, ((m & 1) ? KMOD_SHIFT : 0) | ((m & 2) ? KMOD_ALT | KMOD_ALTGR : 0)}, out[IME_MAX_EVENTS];
                if (m & 2)
                    in.ch = 0;
                if (ime_filter(&in, out, IME_MAX_EVENTS) == 1 && out[0].key == 0 && out[0].ch)
                {
                    unsigned c = (unsigned)out[0].ch;
                    CHECK((c >= 0x0980 && c <= 0x09FF) || c == 0x0964 || c == 0x0965 || c == 0x200C || c == 0x200D);
                    CHECK(c != 0x09B3 && c != 0x09BB && c != 0x0902); /* the code points xkeyboard-config marks as unassigned */
                    n++;
                }
            }
        CHECK(n == 95); /* the 98 characters of xkeyboard-config's layout, less the 3 it marks as unassigned (counted by a script from the file) */
    }
    /* back to the phonetic layout; its state is clean */
    keym(' ', KMOD_CTRL | KMOD_SHIFT);
    CHECK(ime_mode() == IME_PHONETIC);
    fresh();
    typed("ami");
    CHECK(strcmp(text(), "\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xbf") == 0);

    /* off again */
    key(' ', KMOD_CTRL);
    CHECK(!ime_active() && ime_mode() == IME_OFF);
    fresh();
    typed("k");
    CHECK(strcmp(text(), "k") == 0);

    if (fails == 0)
        printf("bangla ime tests passed\n");
    return fails ? 1 : 0;
}
