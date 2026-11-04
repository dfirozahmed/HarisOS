/* HarisOS desktop look: the theme (dark / light), the accent colour and the wallpaper (a ready-made one or a picture
 * (.bmp, .png or .jpg)), as the user picked them. Every change is shown at once (all windows, the taskbar and the wallpaper are
 * drawn again) and written to THEME.CFG in the top folder of the boot disk, which look_load() reads at boot:
 *
 *   theme=dark                          or light
 *   accent=3678F6                       RRGGBB, or "theme" for the theme's own
 *   wallpaper=2                         a ready-made one (ui_wallpaper_preset_name), or the path of a picture:
 *   wallpaper=/HARISOS (nvme0p1)/Pictures/hills.bmp
 *
 * The Personalize app (apps/personalize) is the place to change them; the start menu's Settings opens it. */
#ifndef HARIS_LOOK_H
#define HARIS_LOOK_H

#include "ui.h"

void look_load(void); /* at boot, after the drives are mounted and before the desktop's first frame */

int look_dark(void);
void look_set_dark(int dark);

UiRgb look_accent(void);                   /* the accent in use */
int look_accent_own(void);                 /* 1 = the user picked it, 0 = the theme's own */
void look_set_accent(const UiRgb *accent); /* NULL: back to the theme's own */

int look_wallpaper(void);         /* the ready-made one in use, -1 = a picture */
void look_set_wallpaper(int preset);
int look_set_picture(int node);   /* a picture file (.bmp .png .jpg; vfs node) as the wallpaper: 0, or -1 (look_message() says why) */
int look_picture(void);           /* the picture's node, -1 = none */

/* picture files (picture.h) in the top folder of every drive and in a "Pictures" or "Wallpapers" folder there; returns how many
 * (at most max) */
int look_find_pictures(int *ids, int max);

/* Screen off after this many seconds without input, 0 = never (default 300); "screenoff=" in THEME.CFG. See screensleep.h. */
int look_screenoff(void);
void look_set_screenoff(int seconds);

/* The folder the settings files (THEME.CFG, ICONS.CFG) are written to: where THEME.CFG is, else the boot disk, else the first
 * drive that can be written; -1 = none. A vfs node for vfs_save(). */
int look_config_folder(void);
/* The same folder for reading: a drive that is locked counts too (the Desktop folder is read from here; nothing needs to be unlocked). */
int look_data_folder(void);

/* What the last change did: "" when fine, else why it failed (no picture, not saved ...). */
const char *look_message(void);

#endif
