/* HarisUI animation: easing curves and a time-based value glide (see include/ui_anim.h). Integer maths only. */
#include "ui_anim.h"

/* 1 - (1 - t)^3: starts fast, slows toward the end. */
int ui_ease_out_cubic(int t)
{
    if (t <= 0)
        return 0;
    if (t >= UI_ANIM_ONE)
        return UI_ANIM_ONE;
    long long u = UI_ANIM_ONE - t;
    long long cube = u * u * u / ((long long)UI_ANIM_ONE * UI_ANIM_ONE);
    return (int)(UI_ANIM_ONE - cube);
}

/* smoothstep 3t^2 - 2t^3: gentle at both ends (for things that move across and stop again). */
int ui_ease_in_out(int t)
{
    if (t <= 0)
        return 0;
    if (t >= UI_ANIM_ONE)
        return UI_ANIM_ONE;
    long long tt = t;
    return (int)((3 * tt * tt * UI_ANIM_ONE - 2 * tt * tt * tt) / ((long long)UI_ANIM_ONE * UI_ANIM_ONE));
}

void ui_anim_start(UiAnim *a, int from, int to, unsigned dur_ms, unsigned long long now_ms)
{
    a->from = from;
    a->to = to;
    a->start_ms = now_ms;
    a->dur_ms = from == to ? 0 : dur_ms;
}

void ui_anim_set(UiAnim *a, int v)
{
    a->from = a->to = v;
    a->start_ms = 0;
    a->dur_ms = 0;
}

int ui_anim_value(const UiAnim *a, unsigned long long now_ms)
{
    if (a->dur_ms == 0 || now_ms < a->start_ms)
        return now_ms < a->start_ms && a->dur_ms ? a->from : a->to;
    unsigned long long el = now_ms - a->start_ms;
    if (el >= a->dur_ms)
        return a->to;
    int t = (int)(el * UI_ANIM_ONE / a->dur_ms);
    long long e = ui_ease_out_cubic(t);
    long long d = (long long)a->to - a->from;
    long long v = a->from + (d * e + (d >= 0 ? UI_ANIM_ONE / 2 : -UI_ANIM_ONE / 2)) / UI_ANIM_ONE;
    return (int)v;
}

int ui_anim_active(const UiAnim *a, unsigned long long now_ms)
{
    return a->dur_ms != 0 && now_ms < a->start_ms + a->dur_ms;
}
