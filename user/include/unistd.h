#ifndef HARIS_H_UNISTD_H
#define HARIS_H_UNISTD_H 1
/* <unistd.h>: the part of POSIX that the ported programs ask for (posix2.c). */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
int getpid(void);
ssize_t pread(int fd, void *buf, usize n, off_t off);
ssize_t pwrite(int fd, const void *buf, usize n, off_t off);
ssize_t read(int fd, void *buf, usize n);
ssize_t write(int fd, const void *buf, usize n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, usize size);
int access(const char *path, int mode);
int ftruncate(int fd, off_t length); /* not supported: -1, ENOSYS */
int isatty(int fd); /* fds 0..2 only */
char *realpath(const char *path, char *resolved); /* makes the path absolute with the working folder; does not resolve . or .. */
int getopt(int argc, char *const argv[], const char *optstring);
extern char *optarg;
extern int optind, opterr, optopt;
#endif
