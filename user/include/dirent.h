#ifndef HARIS_H_DIRENT_H
#define HARIS_H_DIRENT_H 1
/* <dirent.h>: opendir / readdir on top of sys_readdir (no "." and ".." entries). */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent
{
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};
typedef struct DIR_ DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif
