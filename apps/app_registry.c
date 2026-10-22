/* The list of installed apps. Order here = order in the start menu. */
#include "app.h"

extern const AppInfo APP_EXPLORER;
extern const AppInfo APP_DEVMGR;
extern const AppInfo APP_DISPLAY;
extern const AppInfo APP_TASKMGR;
extern const AppInfo APP_NOTEPAD;

static const AppInfo *const APPS[] = {
    &APP_EXPLORER,
    &APP_DEVMGR,
    &APP_DISPLAY,
    &APP_TASKMGR,
    &APP_NOTEPAD,
};

#define APP_N ((int)(sizeof APPS / sizeof APPS[0]))

int app_count(void)
{
    return APP_N < APP_MAX ? APP_N : APP_MAX;
}

const AppInfo *app_get(int index)
{
    return (index >= 0 && index < app_count()) ? APPS[index] : (const AppInfo *)0;
}
