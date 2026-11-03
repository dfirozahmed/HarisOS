/* HarisOS BMP pictures: reading a .bmp file that is already in memory (no hardware, no allocation: tests/test_bmp.c).
 *
 *   bmp_info(file, size, &w, &h)                  0 when it is a picture this reader knows, -1 if not
 *   bmp_cover(file, size, out, ow, oh)            the picture made to fill ow x oh pixels (0x00RRGGBB, ow per row)
 *
 * Known: uncompressed 24-bit (BI_RGB) and 32-bit (BI_RGB, or BI_BITFIELDS with 8-bit masks), rows stored bottom-up
 * (height > 0) or top-down (height < 0), any of the usual header sizes (40, 52, 56, 108, 124 bytes).
 * "Cover" keeps the picture's shape: it is scaled until it fills the whole area and what reaches past it is cut off
 * equally on both sides (like a desktop background set to "fill"); the scaling mixes the four nearest source pixels
 * (bilinear). */
#ifndef HARIS_BMP_H
#define HARIS_BMP_H

#define BMP_MAX_SIDE 16384

int bmp_info(const unsigned char *file, unsigned size, int *w, int *h);
int bmp_cover(const unsigned char *file, unsigned size, unsigned int *out, int ow, int oh);

#endif
