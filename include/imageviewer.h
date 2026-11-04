/* Image viewer: open a picture from another app (the File Explorer, the desktop).
 *
 * imageviewer_open_file() opens the Image Viewer window (or brings the open one to the front) and shows the picture at
 * `path` (a full path in the virtual file system, see vfs_lookup). BMP, PNG and JPEG files are read (picture.h).
 * Returns NULL when it worked, else a short sentence saying why not (a literal or a string that stays valid until the
 * next call). The window stays open on a failure and shows the same sentence. */
#ifndef HARIS_IMAGEVIEWER_H
#define HARIS_IMAGEVIEWER_H

const char *imageviewer_open_file(const char *path);

#endif
