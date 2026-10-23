/* Terminal app: a console window running the shell (SH.ELF). See include/terminal.h. */
#include "app.h"
#include "console.h"
#include "exec.h"
#include "terminal.h"

static unsigned text_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

int terminal_open(const char *program)
{
    int con = console_window_open("Terminal");
    if (con < 0)
        return -1;
    int node = exec_find(program);
    int err = 0;
    int id = node >= 0 ? exec_run_node(node, program, con, &err) : EXEC_ERR_NOTFOUND;
    if (id >= 0)
        return 0;

    /* the window stays, so the reason is not lost */
    static const char head[] = "Cannot run ";
    const char *why = exec_error_text(id, err);
    console_write(con, head, sizeof(head) - 1);
    console_write(con, program, text_len(program));
    console_write(con, ": ", 2);
    console_write(con, why, text_len(why));
    console_write(con, ".\n", 2);
    console_finish(con, -1);
    return 0;
}

static void terminal_app_open(void) { terminal_open("SH.ELF"); }

const AppInfo APP_TERMINAL = {
    .name = "Terminal",
    .desc = "Command line shell",
    .icon = UI_ICON_TERMINAL,
    .open = terminal_app_open,
};
