/* ============================================================
 * HarisUI animation: a value that glides from one number to another over a few milliseconds.
 *
 *   UiAnim a = {0};
 *   ui_anim_start(&a, 12, 0, 160, timer_ms());     // from 12 to 0 in 160 ms
 *   ...
 *   int dy = ui_anim_value(&a, timer_ms());        // every frame: where it is now (ends exactly on `to`)
 *   if (ui_anim_active(&a, timer_ms())) redraw;     // still moving
 *
 * Integer maths only (the kernel has no floating point in the UI path). The clock is passed in, so the
 * same code runs in the host tests with a fake clock. Easing is "ease out": fast start, soft landing,
 * which is what makes a menu feel like it settles instead of stopping.
 * ============================================================ */
#ifndef HARIS_UI_ANIM_H
#define HARIS_UI_ANIM_H

#define UI_ANIM_ONE 1024 /* the fixed-point "1.0" used by the easing functions */

typedef struct
{
    int from, to;
    unsigned long long start_ms;
    unsigned dur_ms; /* 0 = no animation running (value is `to`) */
} UiAnim;

/* Easing: t runs 0 .. UI_ANIM_ONE, the result too (0 -> 0, UI_ANIM_ONE -> UI_ANIM_ONE). */
int ui_ease_out_cubic(int t);
int ui_ease_in_out(int t);

/* Start gliding from `from` to `to`. dur_ms 0 jumps straight to `to`. */
void ui_anim_start(UiAnim *a, int from, int to, unsigned dur_ms, unsigned long long now_ms);
/* Jump to `v` and stop (no animation). */
void ui_anim_set(UiAnim *a, int v);
/* Where the value is at now_ms. */
int ui_anim_value(const UiAnim *a, unsigned long long now_ms);
/* 1 while the value is still on its way (the caller should draw again). */
int ui_anim_active(const UiAnim *a, unsigned long long now_ms);

/* ---- UiFade: a 0..255 level that follows an on/off target (hover highlights, pressed glow) ----
 *
 *   ui_fade_to(&f, hovered ? 255 : 0, UI_FADE_MS, now);   // every frame is fine: nothing happens while the target is the same
 *   int level = ui_fade_level(&f, now);                    // 0..255: scale the highlight's alpha / colour by it
 *
 * Turning around half way starts from where the level is now, so a quick in-and-out never jumps. A zeroed UiFade is level 0. */
#define UI_FADE_MS 130

typedef struct
{
    UiAnim a;
    int target;
} UiFade;

void ui_fade_reset(UiFade *f); /* level 0, nothing running */
void ui_fade_to(UiFade *f, int target, unsigned dur_ms, unsigned long long now_ms);
int ui_fade_level(const UiFade *f, unsigned long long now_ms);
int ui_fade_active(const UiFade *f, unsigned long long now_ms);

#endif
