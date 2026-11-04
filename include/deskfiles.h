/* HarisOS desktop files: the files and folders of the Desktop folder, shown as icons on the desktop (deskicons.h).
 *
 * The Desktop folder is a folder named "Desktop" in the top folder of the boot disk, next to THEME.CFG and ICONS.CFG. Nothing makes it at
 * boot (the drives start locked); "Send to desktop" makes it the first time, and the File Explorer can (New folder). Reading it needs
 * nothing; deleting from it needs the drive to be unlocked (like every write). What is in the folder is looked at again every second, so a file made or deleted by the Explorer, the
 * Notepad or a program shows up (or goes) at once; "Refresh" in the desktop menu reads the folder from the drive again.
 * "Send to desktop" (the File Explorer's menu) puts a copy of a file or folder in it, and makes the Desktop folder first if there is none
 * and the drive can be written.
 *
 *   Double click opens: a folder in the File Explorer, text in the Notepad, a program (.elf) in a console window.
 *   Delete asks first, then deletes for good (there is no Recycle Bin). */
#ifndef HARIS_DESKFILES_H
#define HARIS_DESKFILES_H

/* Find the Desktop folder, read it (from_disk = 1: from the drive again) and tell deskicons what is in it. At boot, after deskicons_load(). */
void deskfiles_scan(int from_disk);

/* On every main-loop pass: looks at the folder about once a second. 1 = what is in it changed (the icons are updated; draw again). */
int deskfiles_tick(void);

int deskfiles_folder(void); /* the Desktop folder's vfs node, -1 = there is none */

void deskfiles_open(int node);       /* what a double click does; says why in a toast when it cannot */
void deskfiles_ask_delete(int node); /* a window asks "Delete NAME?"; Delete there calls deskfiles_delete */
void deskfiles_delete(int node);     /* delete now, with no question; a toast says how it went, and the icons follow */

/* Send a file or folder to the desktop: a copy of it goes into the Desktop folder (a name that is taken there becomes "name (2).ext").
 * With no Desktop folder yet it is made first, in the top folder of the boot disk (when that can be written: the drives start locked).
 * A toast says how it went. Returns the node of the copy, or -1 (nothing was copied: the toast says why). */
int deskfiles_send(int node);

/* The same for several entries (n of them): one Desktop folder made if needed, each copied by itself (one that cannot be copied does not stop
 * the others), and ONE toast for all: "Sent 3 items to the desktop.", or how many could not be and why. Returns how many were copied. */
int deskfiles_send_many(const int *nodes, int n);

#endif
