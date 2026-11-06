/* HarisOS file picker: a modal dialog where the user browses the drives and folders and chooses one file.
 *
 *   filepicker_open(my_window, "Choose a file", "/HARISOS/photo.jpg", on_picked, my_data);
 *
 * It opens over `owner` (which cannot be used meanwhile, like any dialog of wm_create_dialog). `start_path` (a path in the virtual file system,
 * may be NULL or empty) names the folder to start in, or a file: then its folder, with the file selected. A double click on a file, or the Select
 * button, calls on_pick(path, user) with the file's full path and closes the dialog; Cancel, Esc and the close button close it without a call.
 * Only one picker is open at a time: asking again brings the open one to the front. Apps use only this header. */
#ifndef HARIS_FILEPICKER_H
#define HARIS_FILEPICKER_H

#include "wm.h"

void filepicker_open(Window *owner, const char *title, const char *start_path, void (*on_pick)(const char *path, void *user), void *user);

#endif
