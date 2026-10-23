/* ============================================================
 * HarisOS shared memory: named blocks of physical pages that several processes map into their own address spaces
 *
 *   int s = shm_open("name", size, SHM_CREATE);   find the segment of that name (made if SHM_CREATE and it is not there: `size`
 *                                                  bytes rounded up to pages, zeroed); -1 if it cannot be
 *   shm_pages(s), shm_page(s, i)                  how many pages, and the physical address of page i
 *   shm_ref(s) / shm_unref(s)                     one more / one less mapping of it (a process that maps it holds a reference)
 *   shm_unlink("name")                            the name goes away; the pages stay while anything still maps them and are
 *                                                  freed when the last mapping ends
 *
 * The kernel layer that maps the pages into a process is in syscall.c (SYS_SHM_MAP): this file only knows the segments. The pages are
 * not owned by any address space (no PAGING_OWNED), so fork shares them and destroying a space never frees them.
 * ============================================================ */
#ifndef HARIS_SHM_H
#define HARIS_SHM_H

#define SHM_MAX 64
#define SHM_NAME_MAX 32          /* including the final 0 */
#define SHM_MAX_PAGES 4096       /* 16 MiB per segment */

#define SHM_CREATE 1             /* shm_open: make it if it does not exist */
#define SHM_EXCL 2               /* with SHM_CREATE: fail if it exists */

int shm_open(const char *name, unsigned long long size, unsigned flags);
unsigned shm_pages(int s);                          /* 0 for a segment that does not exist */
unsigned long long shm_page(int s, unsigned i);     /* physical address, 0 if out of range */
void shm_ref(int s);
void shm_unref(int s);
int shm_unlink(const char *name);                   /* 0, or -1 if there is no such name */
int shm_in_use(void);                                /* segments that exist (named or still mapped), for the leak check */
int shm_valid_name(const char *name);                /* 1 .. SHM_NAME_MAX-1 characters, none of them '/' or a control character */

#endif
