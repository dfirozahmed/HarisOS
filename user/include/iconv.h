#ifndef HARIS_H_ICONV_H
#define HARIS_H_ICONV_H 1
/* <iconv.h>: a small iconv (posix2.c): UTF-8, ASCII, ISO-8859-1 and Windows-1252, with //TRANSLIT and //IGNORE accepted. */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
typedef void *iconv_t;
iconv_t iconv_open(const char *to, const char *from);
size_t iconv(iconv_t cd, char **in, size_t *inleft, char **out, size_t *outleft);
int iconv_close(iconv_t cd);
#endif
