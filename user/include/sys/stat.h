#ifndef HARIS_H_SYS_STAT_H
#define HARIS_H_SYS_STAT_H 1
/* <sys/stat.h>: stat on top of sys_stat. HarisOS only knows a size and a file/folder flag, so the other fields are 0 and modes are
 * rwx for everybody. */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
struct stat
{
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    off_t st_size;
    time_t st_atime, st_mtime, st_ctime;
};
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st); /* regular files only: the size, found by seeking */
int mkdir(const char *path, mode_t mode);
#endif
