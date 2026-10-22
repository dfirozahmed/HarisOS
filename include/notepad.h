/* Notepad: open a file from another app (the File Explorer).
 *
 * notepad_open_file() opens the Notepad window (or brings the open one to the front) and loads the file at `path`
 * (a full path in the virtual file system, see vfs_lookup). If the text in Notepad has unsaved changes it first
 * asks whether to discard them; the file then opens when the user agrees.
 * Returns NULL when it worked or the question was asked, else a short sentence saying why not (a literal or a
 * string that stays valid until the next call). */
#ifndef HARIS_NOTEPAD_H
#define HARIS_NOTEPAD_H

const char *notepad_open_file(const char *path);

#endif
