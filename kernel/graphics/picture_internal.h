/* The PNG and JPEG decoders behind picture.c: each turns a whole file in memory into a kmalloc'ed block of
 * w x h pixels (0x00RRGGBB, w per row), which the caller frees with kfree. NULL: not a file it can read, or no memory. */
#ifndef HARIS_PICTURE_INTERNAL_H
#define HARIS_PICTURE_INTERNAL_H

unsigned int *png_decode(const unsigned char *file, unsigned size, int *w, int *h);
unsigned int *jpeg_decode(const unsigned char *file, unsigned size, int *w, int *h);

#endif
