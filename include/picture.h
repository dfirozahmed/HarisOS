/* HarisOS pictures: one entry point for every picture type the wallpaper can use (tests/test_picture.c).
 *
 *   picture_is_name("a.PNG")                      1 when the file name ends in .bmp .png .jpg or .jpeg
 *   picture_cover(file, size, out, ow, oh)        the picture (found by its first bytes, not its name) made to fill
 *                                                 ow x oh pixels (0x00RRGGBB, ow per row); 0, or -1 if it is not a
 *                                                 picture this can read or there is no memory for it
 *
 * Types: BMP (bmp.h), PNG (all colour types and bit depths, also interlaced; the alpha channel is laid over black) and
 * JPEG (baseline / extended sequential Huffman, 1 or 3 components, any chroma subsampling, restart markers; not
 * progressive, not arithmetic coded, not CMYK). PNG and JPEG are first decoded into a temporary w x h block from the
 * heap (heap.h), which is freed again before picture_cover returns. */
#ifndef HARIS_PICTURE_H
#define HARIS_PICTURE_H

#define PICTURE_MAX_PIXELS (36u * 1024u * 1024u) /* a decoded picture bigger than this is refused (4 bytes per pixel) */

int picture_is_name(const char *name);
int picture_cover(const unsigned char *file, unsigned size, unsigned int *out, int ow, int oh);

/* The picture at its own size: a kmalloc'ed block of w x h pixels (0x00RRGGBB, w per row) that the caller frees with
 * kfree. NULL when it is not a picture this can read, is over PICTURE_MAX_PIXELS, or there is no memory. */
unsigned int *picture_decode(const unsigned char *file, unsigned size, int *w, int *h);

#endif
