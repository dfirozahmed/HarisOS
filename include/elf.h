/* ============================================================
 * HarisOS ELF loader (the parsing half)
 *
 *   ElfInfo info;
 *   int err = elf_parse(file_bytes, file_size, &info);   0, or a negative ELF_ERR_* code
 *
 * Reads and checks the headers of an ELF64 executable that the kernel is about to run in ring 3, and tells the caller
 * what to map where: an entry point and a list of loadable segments (address, where the bytes are in the file, how many
 * bytes the file has, how many the memory needs, and read / write / execute). It does not touch memory itself; process.c
 * builds the address space from this list.
 *
 * What is accepted: a static executable (type ET_EXEC), x86-64, little endian, whose PT_LOAD segments lie inside the
 * user part of the address space (paging.h), do not share a page with each other, and are never writable and
 * executable at once. No interpreter and no dynamic linking: the program is one file that needs nothing else.
 *
 * Everything in the file is untrusted. Each field is checked before it is used, sizes and offsets are compared
 * so that they cannot wrap around, and a file that is merely odd is refused, not "fixed".
 *
 * How to build a program for it (CMakeLists.txt does this for user/hello.c): clang --target=x86_64-unknown-none-elf
 * -ffreestanding -fPIE -mno-sse, then ld.lld -static --image-base=0x400000000000 -z separate-code -z max-page-size=4096.
 * (-fPIE: position independent code needs no relocations, so it works at whatever address the linker chose.)
 * ============================================================ */
#ifndef HARIS_ELF_H
#define HARIS_ELF_H

#define ELF_MAX_SEGMENTS 8          /* loadable segments per program */
#define ELF_MAX_PROGRAM_HEADERS 32  /* program headers per file */
#define ELF_MAX_PAGES 8192          /* 32 MiB of pages (file + zero fill) per program */

/* The stack lives at the top of the user part (process.h). A program may not reach into it. */
#define ELF_STACK_RESERVE (8ULL * 1024 * 1024) /* the user stack can grow down to this much below the top of the user half (process.h) */

#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

typedef struct
{
    unsigned long long vaddr;  /* virtual address of the first byte */
    unsigned long long offset; /* where its bytes start in the file */
    unsigned long long filesz; /* bytes to copy from the file */
    unsigned long long memsz;  /* bytes in memory (the rest after filesz is zero: .bss) */
    unsigned flags;            /* ELF_PF_* */
} ElfSegment;

typedef struct
{
    unsigned long long entry;
    int nseg;
    ElfSegment seg[ELF_MAX_SEGMENTS];
} ElfInfo;

#define ELF_OK 0
#define ELF_ERR_FORMAT -1      /* too small, or not an ELF file at all */
#define ELF_ERR_UNSUPPORTED -2 /* ELF, but not 64-bit little endian version 1 */
#define ELF_ERR_TYPE -3        /* not a static x86-64 executable (a library, a PIE, another CPU) */
#define ELF_ERR_PHDRS -4       /* the program header table is outside the file or the wrong size */
#define ELF_ERR_SEGMENT -5     /* a segment lies outside the file or outside the user address range */
#define ELF_ERR_OVERLAP -6     /* two segments share a page */
#define ELF_ERR_WX -7          /* a segment is writable and executable */
#define ELF_ERR_ENTRY -8       /* the entry point is not inside an executable segment */
#define ELF_ERR_DYNAMIC -9     /* wants an interpreter or dynamic linking */
#define ELF_ERR_TOO_BIG -10    /* too many segments or too much memory */
#define ELF_ERR_EMPTY -11      /* nothing to load */

int elf_parse(const void *file, unsigned long long size, ElfInfo *out);
const char *elf_error_text(int err);

#endif
