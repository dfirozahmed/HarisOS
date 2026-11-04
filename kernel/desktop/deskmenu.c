/* HarisOS desktop menu (right click on the empty desktop). The panel, its mouse and key handling and its drawing are the
 * HarisUI UiMenu (ui.h); this file decides when it opens, what is in it, and what the items do. */
#include "deskmenu.h"
#include "deskicons.h"
#include "deskfiles.h"
#include "appmenu.h"
#include "startmenu.h"
#include "volmenu.h"
#include "taskbar.h"
#include "mouse.h"
#include "wm.h"
#include "desktop_internal.h"

enum { DM_OPEN, DM_DELETE, DM_PERSONALIZE, DM_DISPLAY, DM_ARRANGE, DM_REFRESH }; /* the item ids */

static UiMenu M;
static int ICON = -1; /* the desktop icon the menu was opened on, -1 = the bare desktop */
static unsigned LAST_BUTTONS;

/* the area to draw again: the panel and its shadow (also what a close has to clear) */
static void dmg(void)
{
    wm_add_screen_damage(ui_menu_bounds(&M));
}

int deskmenu_is_open(void) { return M.open; }

UiRect deskmenu_bounds(void) { return ui_menu_bounds(&M); }

void deskmenu_close(void)
{
    if (!M.open)
        return;
    dmg();
    ui_menu_close(&M);
}

void deskmenu_open(int x, int y)
{
    deskmenu_open_icon(x, y, -1);
}

void deskmenu_open_icon(int x, int y, int icon)
{
    startmenu_close(); /* only one menu at a time */
    appmenu_close();
    volmenu_close();
    if (M.open)
        deskmenu_close();
    ui_menu_clear(&M);
    ICON = icon;
    if (icon >= 0)
    {
        int file = deskicons_is_file(icon);
        ui_menu_add(&M, DM_OPEN, "Open", "Enter", 0);
        ui_menu_add(&M, DM_DELETE, file ? "Delete" : "Remove from desktop", "Del", file ? UI_MENU_DANGER : 0);
        ui_menu_separator(&M);
    }
    ui_menu_add(&M, DM_PERSONALIZE, "Personalize", 0, 0);
    ui_menu_add(&M, DM_DISPLAY, "Display settings", 0, 0);
    ui_menu_add(&M, DM_ARRANGE, "Arrange icons", 0, 0);
    ui_menu_separator(&M);
    ui_menu_add(&M, DM_REFRESH, "Refresh", 0, 0);
    ui_menu_open(&M, x, y, ui_rect(0, 0, (int)gfx_width(), (int)gfx_height() - TASKBAR_HEIGHT));
    dmg();
}

static void run(int id)
{
    switch (id)
    {
    case DM_OPEN:
        deskicons_open(ICON);
        break;
    case DM_DELETE:
        deskicons_delete(ICON);
        break;
    case DM_ARRANGE:
        deskicons_arrange();
        break;
    case DM_PERSONALIZE:
        desktop_open_settings();
        break;
    case DM_DISPLAY:
        desktop_open_app("Display Settings");
        break;
    case DM_REFRESH:
        deskfiles_scan(1); /* the Desktop folder is read from the drive again */
        wm_invalidate();   /* every window and the wallpaper are drawn again */
        break;
    default:
        break;
    }
}

int deskmenu_pointer(int x, int y, unsigned buttons)
{
    unsigned now = buttons & 3u;
    unsigned pressed = now & ~LAST_BUTTONS; /* buttons that went down just now */
    LAST_BUTTONS = now;
    if (!M.open)
        return 0;

    int valid = x >= 0;
    int over = valid && ui_contains(ui_menu_rect(&M), x, y);
    if (pressed && valid && !over) /* any click away from the menu closes it and still goes through */
    {
        deskmenu_close();
        return 0;
    }
    if (ui_menu_hover(&M, valid ? x : -1, y))
        dmg();
    if ((pressed & MOUSE_LEFT) && over)
    {
        ui_menu_press(&M, x, y);
        dmg();
    }
    if (M.press >= 0) /* the button is held on an item: the pressed look follows the pointer, the item runs on release */
    {
        int id = -1;
        int r = ui_menu_release(&M, x, y, buttons, &id);
        if (r)
            dmg();
        if (r == 2) /* released on the item it was pressed on (the menu is closed now) */
            run(id);
    }
    return over;
}

int deskmenu_key(const KeyEvent *k)
{
    if (!M.open)
        return 0;
    int id = -1;
    int r = ui_menu_key(&M, k, &id);
    if (r)
        dmg();
    if (r == 2)
        run(id);
    return 1;
}

void deskmenu_draw(void)
{
    if (!M.open || !gfx_ready())
        return;
    ui_menu_draw(&M);
}
