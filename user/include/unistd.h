/* <unistd.h>: the small part of POSIX that the ported programs ask for. Names that need a system call HarisOS does not have yet
 * are declared in posix.c's section "unistd" as they are added. */
#define HARIS_STD_HEADERS 1
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
int getpid(void);
ssize_t pread(int fd, void *buf, usize n, off_t off);
ssize_t pwrite(int fd, const void *buf, usize n, off_t off);
