/* <assert.h> for HarisOS user programs: a failed assert prints where, and ends the program (abort). */
#define HARIS_STD_HEADERS 1
#include "../libc.h"
#undef assert
#ifdef NDEBUG
#define assert(c) ((void)0)
#else
#define assert(c) ((c) ? (void)0 : __assert_fail(#c, __FILE__, __LINE__))
#endif
