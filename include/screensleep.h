/* Screen off after a time without input (DPMS-like): the panel's backlight goes off (gfx_screen_sleep) and comes back at the first key
 * or mouse event, which is only used to wake up (it is not given to the desktop). The time is "screenoff=seconds" in THEME.CFG
 * (look.h), 0 = never; it is set in the Display Settings app. Works only where gfx has a real backlight (gfx_backlight_driver). */
#ifndef HARIS_SCREENSLEEP_H
#define HARIS_SCREENSLEEP_H

int screensleep_available(void); /* 1 = a real backlight can be switched off here */
int screensleep_asleep(void);
/* Call for every key / mouse event. Returns 1 when the screen was off and this event woke it: the caller drops the event. */
int screensleep_activity(void);
/* Call about once per main loop pass: switches the screen off when the time has come. */
void screensleep_tick(void);

#endif
