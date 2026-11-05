/* The Player: a music player window that runs as the user program PLAYER.ELF (user/player.c).
 *
 * player_open_file() starts it for a song, or for no file (path NULL or ""), where it shows the songs of the first drive that has some.
 * Returns NULL when it was started, else a sentence that says why not (for a toast or the status bar). Every call starts another window. */
#ifndef HARIS_PLAYER_H
#define HARIS_PLAYER_H

const char *player_open_file(const char *path);

#endif
