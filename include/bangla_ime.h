/* ============================================================
 * HarisOS Bangla typing: a phonetic layout (Avro style)
 *
 * With Bangla on (toggle: Ctrl+Space) the Latin letters typed on the keyboard turn into Bangla: "ami" -> আমি,
 * "bangla" -> বাংলা, "kormo" -> কর্ম. Text fields know nothing about this: the filter sits between the keyboard
 * and the desktop and hands on ordinary key events, a Unicode character in KeyEvent.ch (key = 0) and, when a
 * later letter changes what was typed before ("k" -> ক, then "h" -> খ), a Backspace and the new characters.
 *
 * How: the letters of the word being typed are kept; after every key the whole word is converted again (bn_translit),
 * and what differs from the text sent so far is sent. A space, Enter, an arrow, a mouse click ... ends the word.
 * Backspace takes back the last key typed, not the last character.
 *
 * Two layouts. Ctrl+Space turns Bangla on / off; Ctrl+Shift+Space switches between them (and turns Bangla on).
 *
 * National layout (জাতীয়, BDS 1738): every key is a Bangla character, as on the Bangladesh Computer Council's
 * keyboard: ক on J, ম on M, া on H, ি on D, ্ (hasanta) on G, ... Shift gives the second character of a key; the right Alt
 * (AltGr) the third (আ ই উ ঋ এ ও ... the independent vowels), AltGr+Shift the fourth. Letters are typed in the order
 * they are spoken: the ি sign is typed after its consonant and drawn before it by the text drawing code. The table is
 * from xkeyboard-config (symbols/bd). Keys the layout does not use (punctuation, Space) type what they normally do.
 *
 * Phonetic layout (Avro style): the rules (a table of letters, not a full copy of Avro):
 *   consonants  k ক  kh খ  g গ  gh ঘ  Ng ঙ  c চ  ch চ  chh ছ  j জ  jh ঝ  NG ঞ  T ট  Th ঠ  D ড  Dh ঢ  N ণ
 *               t ত  th থ  d দ  dh ধ  n ন  p প  ph ফ  f ফ  b ব  bh ভ  v ভ  m ম  z য  Z য  r র  l ল
 *               sh শ  S শ  Sh ষ  s স  h হ  R ড়  Rh ঢ়  y য়  q ক  x ক্স  kkh ক্ষ  kSh ক্ষ  gg জ্ঞ
 *   vowels      alone / after a vowel:  o অ  a আ  i ই  I ঈ  u উ  U ঊ  e এ  oi ঐ  O ও  ou ঔ  rri ঋ
 *               after a consonant, the sign: া ি ী ু ূ ে ৃ ; O ো  OI ৈ  OU ৌ ; "o" gives no sign
 *               (the consonant keeps its own sound: "kolom" -> কলম) and "oi" / "ou" there are "o" followed by
 *               a vowel ("boi" -> বই)
 *   signs       ng ং  : ঃ  ^ ঁ  t`` ৎ
 *   a consonant that follows a consonant joins it (hasanta between): "bondhu" -> বন্ধু, "prem" -> প্রেম, "kormo" -> কর্ম;
 *               a vowel (also "o") or the ` key between them keeps them apart
 *   digits 0-9 -> ০-৯,  . -> ।  (typed twice: .),  $ -> ৳
 *   a capital letter that has no meaning of its own is the small letter; anything else is typed as it is
 * ============================================================ */
#ifndef HARIS_BANGLA_IME_H
#define HARIS_BANGLA_IME_H

#include "keyboard.h"

#define IME_MAX_EVENTS 48 /* the most events one key can turn into (a word is at most 24 letters) */

/* The phonetic conversion of the letters s[0..n) to Unicode characters in out[0..max): returns how many. */
int bn_translit(const char *s, int n, unsigned *out, int max);

enum { IME_OFF = 0, IME_PHONETIC = 1, IME_NATIONAL = 2 };

int ime_active(void);
int ime_mode(void); /* IME_OFF, or the Bangla layout in use */
void ime_set_active(int on);
void ime_set_layout(int layout); /* IME_PHONETIC or IME_NATIONAL (does not turn Bangla on) */

/* Forget the word being typed (focus moved, the mouse was clicked ...): the next letter starts a new one. */
void ime_reset(void);

/* Run one keyboard event through the filter. Writes the events to hand to the desktop into out[0..max) and returns
 * how many (0 = the key was used up, 1 = unchanged or replaced). Bangla off: the event is returned as it is. */
int ime_filter(const KeyEvent *in, KeyEvent *out, int max);

#endif
