/* HarisOS ELF loader (parsing): see include/elf.h. No hardware access, tested on the host. */
#include "elf.h"
#include "paging.h"

typedef unsigned long long u64;

/* Fields are read byte by byte: the file is only a pile of bytes, with no alignment promise, and the CPU's byte order
 * is not the file's by definition (ELF here is little endian). */
static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }
static u64 rd64(const unsigned char *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }

#define EHDR_SIZE 64
#define PHDR_SIZE 56

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3

#define ET_EXEC 2
#define EM_X86_64 62

const char *elf_error_text(int err)
{
    switch (err)
    {
    case ELF_OK:
        return "ok";
    case ELF_ERR_FORMAT:
        return "not an ELF file";
    case ELF_ERR_UNSUPPORTED:
        return "not a 64-bit little endian ELF";
    case ELF_ERR_TYPE:
        return "not a static x86-64 executable";
    case ELF_ERR_PHDRS:
        return "bad program header table";
    case ELF_ERR_SEGMENT:
        return "a segment is outside the file or the user address range";
    case ELF_ERR_OVERLAP:
        return "two segments share a page";
    case ELF_ERR_WX:
        return "a segment is both writable and executable";
    case ELF_ERR_ENTRY:
        return "the entry point is not in an executable segment";
    case ELF_ERR_DYNAMIC:
        return "dynamic linking is not supported";
    case ELF_ERR_TOO_BIG:
        return "too many segments or too much memory";
    case ELF_ERR_EMPTY:
        return "nothing to load";
    default:
        return "unknown error";
    }
}

/* first and last page (as page numbers) a segment touches */
static u64 first_page(const ElfSegment *s) { return s->vaddr >> 12; }
static u64 last_page(const ElfSegment *s) { return (s->vaddr + s->memsz - 1) >> 12; }

int elf_parse(const void *file, u64 size, ElfInfo *out)
{
    const unsigned char *f = (const unsigned char *)file;
    if (!f || !out || size < EHDR_SIZE)
        return ELF_ERR_FORMAT;
    if (f[0] != 0x7F || f[1] != 'E' || f[2] != 'L' || f[3] != 'F')
        return ELF_ERR_FORMAT;
    if (f[4] != 2 || f[5] != 1 || f[6] != 1) /* 64-bit, little endian, version 1 */
        return ELF_ERR_UNSUPPORTED;
    if (rd16(f + 16) != ET_EXEC || rd16(f + 18) != EM_X86_64)
        return ELF_ERR_TYPE;

    u64 entry = rd64(f + 24), phoff = rd64(f + 32);
    unsigned phentsize = rd16(f + 54), phnum = rd16(f + 56);
    if (phentsize != PHDR_SIZE || phnum == 0 || phnum > ELF_MAX_PROGRAM_HEADERS)
        return ELF_ERR_PHDRS;
    /* the table must lie inside the file; compared as phoff <= size - table so that nothing can wrap around */
    u64 table = (u64)phnum * PHDR_SIZE;
    if (phoff > size || table > size - phoff)
        return ELF_ERR_PHDRS;

    out->entry = entry;
    out->nseg = 0;
    u64 pages = 0;

    for (unsigned i = 0; i < phnum; i++)
    {
        const unsigned char *ph = f + phoff + (u64)i * PHDR_SIZE;
        unsigned type = rd32(ph);
        if (type == PT_INTERP || type == PT_DYNAMIC)
            return ELF_ERR_DYNAMIC;
        if (type != PT_LOAD)
            continue; /* notes, stack hints, unwind tables ...: nothing to do */

        ElfSegment s;
        s.flags = rd32(ph + 4);
        s.offset = rd64(ph + 8);
        s.vaddr = rd64(ph + 16);
        s.filesz = rd64(ph + 32);
        s.memsz = rd64(ph + 40);
        if (s.memsz == 0)
            continue; /* an empty segment maps nothing */

        /* inside the file */
        if (s.filesz > s.memsz || s.offset > size || s.filesz > size - s.offset)
            return ELF_ERR_SEGMENT;
        /* inside the user part, below the stack, with no wrap-around */
        const u64 limit = PAGING_USER_END - ELF_STACK_RESERVE;
        if (s.vaddr < PAGING_USER_BASE || s.vaddr >= limit || s.memsz > limit - s.vaddr)
            return ELF_ERR_SEGMENT;
        /* a page is mapped as a whole, so the bytes of the file must sit at the same place in their page as in memory */
        if ((s.vaddr & 0xFFF) != (s.offset & 0xFFF))
            return ELF_ERR_SEGMENT;
        if ((s.flags & ELF_PF_W) && (s.flags & ELF_PF_X))
            return ELF_ERR_WX;

        if (out->nseg == ELF_MAX_SEGMENTS)
            return ELF_ERR_TOO_BIG;
        pages += last_page(&s) - first_page(&s) + 1;
        if (pages > ELF_MAX_PAGES)
            return ELF_ERR_TOO_BIG;
        for (int k = 0; k < out->nseg; k++) /* a page can have only one set of permissions */
            if (first_page(&s) <= last_page(&out->seg[k]) && first_page(&out->seg[k]) <= last_page(&s))
                return ELF_ERR_OVERLAP;
        out->seg[out->nseg++] = s;
    }

    if (out->nseg == 0)
        return ELF_ERR_EMPTY;

    /* the program must start in code it may execute */
    int ok = 0;
    for (int k = 0; k < out->nseg; k++)
    {
        const ElfSegment *s = &out->seg[k];
        if ((s->flags & ELF_PF_X) && entry >= s->vaddr && entry - s->vaddr < s->filesz)
            ok = 1;
    }
    if (!ok)
        return ELF_ERR_ENTRY;
    return ELF_OK;
}
