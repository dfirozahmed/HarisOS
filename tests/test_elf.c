/* Host test for elf.c: the ELF header and segment checks, with images made in memory. */
#include "elf.h"
#include "paging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define BASE PAGING_USER_BASE

static void w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *p, unsigned v) { w16(p, v & 0xFFFF); w16(p + 2, v >> 16); }
static void w64(unsigned char *p, u64 v) { w32(p, (unsigned)v); w32(p + 4, (unsigned)(v >> 32)); }

static unsigned char IMG[16384];
enum { PH0 = 64 };

static void phdr(int i, unsigned type, unsigned flags, u64 off, u64 vaddr, u64 filesz, u64 memsz)
{
    unsigned char *p = IMG + PH0 + i * 56;
    w32(p, type);
    w32(p + 4, flags);
    w64(p + 8, off);
    w64(p + 16, vaddr);
    w64(p + 24, vaddr); /* paddr */
    w64(p + 32, filesz);
    w64(p + 40, memsz);
    w64(p + 48, 4096);
}

/* A good file: [ELF header + 3 program headers] then code at file offset 0x1000, data at 0x2000.
 *   segment 0: RX, vaddr BASE + 0x1000, 0x20 bytes from offset 0x1000
 *   segment 1: RW, vaddr BASE + 0x2000, 0x10 bytes from offset 0x2000 and 0x3000 bytes of memory (bss)
 * entry = BASE + 0x1008 */
static void good(void)
{
    memset(IMG, 0, sizeof(IMG));
    IMG[0] = 0x7F; IMG[1] = 'E'; IMG[2] = 'L'; IMG[3] = 'F';
    IMG[4] = 2; IMG[5] = 1; IMG[6] = 1;
    w16(IMG + 16, 2);   /* ET_EXEC */
    w16(IMG + 18, 62);  /* x86-64 */
    w32(IMG + 20, 1);
    w64(IMG + 24, BASE + 0x1008);
    w64(IMG + 32, PH0);
    w16(IMG + 52, 64);
    w16(IMG + 54, 56);
    w16(IMG + 56, 3);
    phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, BASE + 0x1000, 0x20, 0x20);
    phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, BASE + 0x2000, 0x10, 0x3000);
    phdr(2, 0x6474E551, ELF_PF_R | ELF_PF_W, 0, 0, 0, 0); /* PT_GNU_STACK: ignored */
}

static int parse(ElfInfo *i) { return elf_parse(IMG, 0x2100, i); }

int main(void)
{
    ElfInfo info;

    /* a good file */
    good();
    CHECK(parse(&info) == 0);
    CHECK(info.nseg == 2 && info.entry == BASE + 0x1008);
    CHECK(info.seg[0].vaddr == BASE + 0x1000 && info.seg[0].offset == 0x1000 && info.seg[0].filesz == 0x20 &&
          info.seg[0].memsz == 0x20 && info.seg[0].flags == (ELF_PF_R | ELF_PF_X));
    CHECK(info.seg[1].memsz == 0x3000 && info.seg[1].filesz == 0x10 && (info.seg[1].flags & ELF_PF_W));
    CHECK(strcmp(elf_error_text(0), "ok") == 0 && elf_error_text(-99)[0] != 0);

    /* the header */
    CHECK(elf_parse(IMG, 63, &info) == ELF_ERR_FORMAT);
    CHECK(elf_parse(0, 100, &info) == ELF_ERR_FORMAT && elf_parse(IMG, 0x2100, 0) == ELF_ERR_FORMAT);
    good(); IMG[0] = 'X';           CHECK(parse(&info) == ELF_ERR_FORMAT);
    good(); IMG[3] = 'G';           CHECK(parse(&info) == ELF_ERR_FORMAT);
    good(); IMG[4] = 1;             CHECK(parse(&info) == ELF_ERR_UNSUPPORTED); /* 32-bit */
    good(); IMG[5] = 2;             CHECK(parse(&info) == ELF_ERR_UNSUPPORTED); /* big endian */
    good(); IMG[6] = 2;             CHECK(parse(&info) == ELF_ERR_UNSUPPORTED); /* version */
    good(); w16(IMG + 16, 3);       CHECK(parse(&info) == ELF_ERR_TYPE);        /* ET_DYN (a PIE or a library) */
    good(); w16(IMG + 16, 1);       CHECK(parse(&info) == ELF_ERR_TYPE);        /* object file */
    good(); w16(IMG + 18, 3);       CHECK(parse(&info) == ELF_ERR_TYPE);        /* i386 */
    good(); w16(IMG + 18, 183);     CHECK(parse(&info) == ELF_ERR_TYPE);        /* aarch64 */

    /* the program header table */
    good(); w16(IMG + 54, 32);      CHECK(parse(&info) == ELF_ERR_PHDRS);
    good(); w16(IMG + 56, 0);       CHECK(parse(&info) == ELF_ERR_PHDRS);
    good(); w16(IMG + 56, ELF_MAX_PROGRAM_HEADERS + 1); CHECK(parse(&info) == ELF_ERR_PHDRS);
    good(); w64(IMG + 32, 0x2100 - 100);    CHECK(parse(&info) == ELF_ERR_PHDRS); /* runs past the end of the file */
    good(); w64(IMG + 32, ~0ULL);           CHECK(parse(&info) == ELF_ERR_PHDRS); /* offset that wraps around */
    good(); w64(IMG + 32, ~0ULL - 100);     CHECK(parse(&info) == ELF_ERR_PHDRS);
    good(); w64(IMG + 32, 0x2100);          CHECK(parse(&info) == ELF_ERR_PHDRS); /* starts exactly at the end */

    /* segments: bounds in the file */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, BASE + 0x1000, 0x40, 0x20);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* filesz > memsz */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x2000, BASE + 0x1000, 0x200, 0x200);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* bytes beyond the end of the file */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, BASE + 0x1000, ~0ULL, ~0ULL);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, ~0ULL - 8, BASE + 0xFF8, 0x20, 0x20);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* offset + filesz wraps around */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1010, BASE + 0x1000, 0x20, 0x20);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* page offset of file and memory differ */

    /* segments: address range */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, 0x1000, 0x20, 0x20);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* kernel address */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, BASE - 0x1000, 0x20, 0x20);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* just below the user part */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, 0xFFFFFFFFFFFFF000ULL, 0x20, 0x20);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* kernel half of the address space */
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, PAGING_USER_END - ELF_STACK_RESERVE - 0x1000, 0x10, 0x2000);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* reaches into the stack */
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, PAGING_USER_END - ELF_STACK_RESERVE + 0x1000, 0x10, 0x10);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* starts inside the stack area */
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, PAGING_USER_END - 0x1000, 0x10, 0x10);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, BASE + 0x2000, 0x10, ~0ULL);
    CHECK(parse(&info) == ELF_ERR_SEGMENT);                                       /* memsz that wraps the address */
    {
        /* the last byte just below the stack reserve is fine, one more is not */
        good();
        u64 last = PAGING_USER_END - ELF_STACK_RESERVE;
        phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, last - 0x1000, 0x10, 0x1000);
        CHECK(parse(&info) == 0);
        phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, last - 0x1000, 0x10, 0x1001);
        CHECK(parse(&info) == ELF_ERR_SEGMENT);
    }

    /* segments: permissions, overlap, entry */
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W | ELF_PF_X, 0x2000, BASE + 0x2000, 0x10, 0x10);
    CHECK(parse(&info) == ELF_ERR_WX);
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, BASE + 0x1000 + 0x10, 0x10, 0x10);
    CHECK(parse(&info) == ELF_ERR_SEGMENT); /* (page offset differs from the file's) */
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x1010, BASE + 0x1010, 0x10, 0x10);
    CHECK(parse(&info) == ELF_ERR_OVERLAP); /* same page as segment 0, other permissions */
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x1000, BASE + 0x1000, 0x10, 0x3000);
    CHECK(parse(&info) == ELF_ERR_OVERLAP);
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, BASE + 0x2000, 0x10, 0x4000);
    phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, BASE + 0x1000, 0x20, 0x2000); /* code reaches into the data page's range */
    CHECK(parse(&info) == ELF_ERR_OVERLAP);
    good(); w64(IMG + 24, BASE);                    CHECK(parse(&info) == ELF_ERR_ENTRY);  /* not in any segment */
    good(); w64(IMG + 24, BASE + 0x2000);           CHECK(parse(&info) == ELF_ERR_ENTRY);  /* in the data, not executable */
    good(); w64(IMG + 24, BASE + 0x1020);           CHECK(parse(&info) == ELF_ERR_ENTRY);  /* one past the file bytes of the code */
    good(); w64(IMG + 24, BASE + 0x101F);           CHECK(parse(&info) == 0);              /* the last byte */
    good(); w64(IMG + 24, 0);                       CHECK(parse(&info) == ELF_ERR_ENTRY);
    good(); w64(IMG + 24, ~0ULL);                   CHECK(parse(&info) == ELF_ERR_ENTRY);

    /* things that do not fit this loader */
    good(); phdr(2, 3, ELF_PF_R, 0, 0, 0, 0);       CHECK(parse(&info) == ELF_ERR_DYNAMIC); /* PT_INTERP */
    good(); phdr(2, 2, ELF_PF_R, 0, 0, 0, 0);       CHECK(parse(&info) == ELF_ERR_DYNAMIC); /* PT_DYNAMIC */
    good(); phdr(0, 1, ELF_PF_R | ELF_PF_X, 0, 0, 0, 0); phdr(1, 1, ELF_PF_R, 0, 0, 0, 0);
    CHECK(parse(&info) == ELF_ERR_EMPTY);           /* only empty LOAD segments */
    good(); phdr(0, 0x6474E551, 0, 0, 0, 0, 0); phdr(1, 4, 0, 0, 0, 0, 0);
    CHECK(parse(&info) == ELF_ERR_EMPTY);           /* no LOAD at all */

    /* limits: too many segments, too much memory */
    {
        good();
        w16(IMG + 56, ELF_MAX_SEGMENTS + 1);
        for (int i = 0; i < ELF_MAX_SEGMENTS + 1; i++)
            phdr(i, 1, ELF_PF_R, 0x1000, BASE + (u64)(i + 1) * 0x10000, 0x10, 0x10);
        CHECK(parse(&info) == ELF_ERR_TOO_BIG);
        w16(IMG + 56, ELF_MAX_SEGMENTS); /* exactly the limit: fine (each at the same file bytes: allowed) */
        w64(IMG + 24, BASE + 0x10000);
        phdr(0, 1, ELF_PF_R | ELF_PF_X, 0x1000, BASE + 0x10000, 0x10, 0x10);
        CHECK(parse(&info) == 0 && info.nseg == ELF_MAX_SEGMENTS);
    }
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, BASE + 0x2000, 0x10, (u64)ELF_MAX_PAGES * 4096);
    CHECK(parse(&info) == ELF_ERR_TOO_BIG);         /* with the code page: one page more than the limit */
    good(); phdr(1, 1, ELF_PF_R | ELF_PF_W, 0x2000, BASE + 0x2000, 0x10, (u64)(ELF_MAX_PAGES - 1) * 4096);
    CHECK(parse(&info) == 0);                       /* 1 + (MAX - 1) pages */

    /* the real file the build makes (user/hello.c -> PE -> mkelf): HARIS_TEST_ELF=<path>, set by run_elf_tests.sh when
     * the build exists. It must be acceptable to the loader, and every segment must be one the program really needs. */
    const char *path = getenv("HARIS_TEST_ELF");
    if (path)
    {
        FILE *f = fopen(path, "rb");
        static unsigned char real[1 << 20];
        size_t n = f ? fread(real, 1, sizeof(real), f) : 0;
        if (f)
            fclose(f);
        CHECK(n > 0);
        ElfInfo r;
        CHECK(elf_parse(real, n, &r) == 0);
        CHECK(r.nseg >= 2 && r.entry >= BASE && r.entry < BASE + 0x100000);
        int exec = 0, writable = 0, bss = 0;
        for (int i = 0; i < r.nseg; i++)
        {
            exec += (r.seg[i].flags & ELF_PF_X) != 0;
            writable += (r.seg[i].flags & ELF_PF_W) != 0;
            bss += r.seg[i].memsz > r.seg[i].filesz;
            CHECK(!((r.seg[i].flags & ELF_PF_W) && (r.seg[i].flags & ELF_PF_X)));
        }
        CHECK(exec == 1 && writable == 1 && bss == 1); /* hello.c: one code segment, one data segment with .bss */
        printf("checked %s: %d segments, entry 0x%llx\n", path, r.nseg, r.entry);
    }

    if (fails == 0)
        printf("elf tests passed\n");
    return fails ? 1 : 0;
}
