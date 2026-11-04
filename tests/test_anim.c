/* Host-side tests for HarisUI animation (kernel/ui/ui_anim.c): easing curves and the timed value glide. */
#include "ui_anim.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    /* easing: fixed ends, never goes backwards, never overshoots */
    CHECK(ui_ease_out_cubic(0) == 0 && ui_ease_out_cubic(UI_ANIM_ONE) == UI_ANIM_ONE);
    CHECK(ui_ease_in_out(0) == 0 && ui_ease_in_out(UI_ANIM_ONE) == UI_ANIM_ONE);
    CHECK(ui_ease_out_cubic(-50) == 0 && ui_ease_out_cubic(5000) == UI_ANIM_ONE);
    int po = 0, pi = 0;
    for (int t = 0; t <= UI_ANIM_ONE; t++)
    {
        int o = ui_ease_out_cubic(t), i = ui_ease_in_out(t);
        CHECK(o >= po && o <= UI_ANIM_ONE);
        CHECK(i >= pi && i <= UI_ANIM_ONE);
        po = o;
        pi = i;
    }
    CHECK(ui_ease_out_cubic(UI_ANIM_ONE / 2) > UI_ANIM_ONE / 2); /* ease out is ahead of linear at the middle */
    CHECK(ui_ease_in_out(UI_ANIM_ONE / 2) == UI_ANIM_ONE / 2);   /* smoothstep is symmetric */

    /* glide down: 12 -> 0 over 160 ms starting at t=1000 */
    UiAnim a = {0};
    ui_anim_start(&a, 12, 0, 160, 1000);
    CHECK(ui_anim_value(&a, 1000) == 12);
    CHECK(ui_anim_active(&a, 1000) && ui_anim_active(&a, 1159));
    CHECK(!ui_anim_active(&a, 1160));
    CHECK(ui_anim_value(&a, 1160) == 0 && ui_anim_value(&a, 99999) == 0);
    int prev = 12;
    for (unsigned long long t = 1000; t <= 1160; t++)
    {
        int v = ui_anim_value(&a, t);
        CHECK(v <= prev && v >= 0); /* only moves toward the target, no overshoot */
        prev = v;
    }
    CHECK(ui_anim_value(&a, 1040) < 6); /* ease out: more than half the way after a quarter of the time */
    CHECK(ui_anim_value(&a, 900) == 12); /* a clock that is early (should not happen) shows the start */

    /* glide up (negative direction) */
    ui_anim_start(&a, 0, 100, 200, 50);
    prev = 0;
    for (unsigned long long t = 50; t <= 250; t++)
    {
        int v = ui_anim_value(&a, t);
        CHECK(v >= prev && v <= 100);
        prev = v;
    }
    CHECK(prev == 100);

    /* no animation: zero duration, same value, set */
    ui_anim_start(&a, 3, 40, 0, 10);
    CHECK(!ui_anim_active(&a, 10) && ui_anim_value(&a, 10) == 40);
    ui_anim_start(&a, 7, 7, 300, 10);
    CHECK(!ui_anim_active(&a, 11) && ui_anim_value(&a, 11) == 7);
    ui_anim_start(&a, 0, 9, 300, 10);
    ui_anim_set(&a, 5);
    CHECK(!ui_anim_active(&a, 11) && ui_anim_value(&a, 11) == 5);

    printf("anim tests: ok\n");
    return 0;
}
