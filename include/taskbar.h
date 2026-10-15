/* HarisOS taskbar */
#ifndef HARIS_TASKBAR_H
#define HARIS_TASKBAR_H

#define TASKBAR_HEIGHT 48

typedef enum
{
    TASKBAR_ICON_FILES,
    TASKBAR_ICON_TERMINAL,
    TASKBAR_ICON_SETTINGS,
    TASKBAR_ICON_BROWSER
} TaskbarIcon;

typedef struct
{
    TaskbarIcon icon;
    int running; /* shows a small dot under the icon */
    int active;  /* focused app: highlighted, wide indicator */
} TaskbarApp;

void taskbar_set_apps(const TaskbarApp *apps, int count);

/* Draw the taskbar along the bottom of the screen. */
void taskbar_draw(void);

#endif
