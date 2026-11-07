/* <limits.h>: the compiler's header plus the POSIX limits. */
#include_next <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
