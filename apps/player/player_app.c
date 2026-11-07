/* Player (start menu entry and file association): starts PLAYER.ELF, which opens a window of its own and plays the songs. The program is
 * user code (user/player.c) because the MP3 decoder uses floating point, which the kernel does not; this file only launches it. */
#include "app.h"
#include "dlog.h"
#include "exec.h"
#include "timer.h"
#include "player.h"
#include "process.h"
#include "toast.h"

#define PLAYER_ELF "PLAYER.ELF"

/* arguments as the program receives them: NUL-terminated strings one after the other */
static unsigned add_arg(char *blob, unsigned len, unsigned cap, const char *s)
{
    for (; *s && len + 1 < cap; s++)
        blob[len++] = *s;
    blob[len++] = 0;
    return len;
}

const char *player_open_file(const char *path)
{
    unsigned long long t0 = timer_ms();
    int node = exec_find(PLAYER_ELF);
    dlog("player_open", "exec_find", (long long)(timer_ms() - t0), 0, 0); /* (dlog.h) */
    if (node < 0)
        return "The Player program (PLAYER.ELF) is not on any drive.";
    static char blob[320];
    unsigned len = add_arg(blob, 0, sizeof blob, PLAYER_ELF);
    int argc = 1;
    if (path && *path)
    {
        len = add_arg(blob, len, sizeof blob, path);
        argc = 2;
    }
    ProcessSpawn sp = {.console = -1, .argc = argc, .args = blob, .args_len = len, .parent = -1}; /* it has a window of its own: no console */
    int err = 0;
    int id = exec_spawn_node(node, PLAYER_ELF, &sp, &err);
    dlog("player_open", "done", (long long)(timer_ms() - t0), id, 0); /* (dlog.h) */
    return id >= 0 ? (const char *)0 : exec_error_text(id, err);
}

static void player_app_open(void)
{
    const char *why = player_open_file((const char *)0);
    if (why)
        toast_show(TOAST_WARNING, why);
}

const AppInfo APP_PLAYER = {
    .name = "Player",
    .desc = "Play MP3 and WAV music",
    .icon = UI_ICON_VOLUME,
    .open = player_app_open,
};
