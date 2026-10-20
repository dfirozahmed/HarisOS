/* HarisOS apps: programs that live in apps/<name>/ and are listed in the start menu.
 *
 * An app is one AppInfo (name, description, icon, open function). The desktop knows nothing
 * about individual apps: it only walks this registry. To add an app:
 *   1. write apps/<name>/<name>_app.c with `const AppInfo APP_<NAME> = {...};`
 *   2. declare it `extern` and list it in apps/app_registry.c
 *   3. add the .c file to APP_SOURCES in CMakeLists.txt
 * Apps may use only public headers (ui.h, wm.h, desktop.h, ...), not desktop_internal.h. */
#ifndef HARIS_APP_H
#define HARIS_APP_H

#include "ui.h"

#define APP_MAX 8

typedef struct
{
    const char *name; /* start menu title */
    const char *desc; /* start menu subtitle */
    UiIcon icon;
    void (*open)(void); /* open the app's window (or bring an open one to the front) */
} AppInfo;

int app_count(void);
const AppInfo *app_get(int index); /* NULL if out of range */

#endif
