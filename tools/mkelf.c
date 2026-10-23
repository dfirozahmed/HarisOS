/*
 * mkelf - turns a linked Windows (PE32+) executable into an ELF64 executable for HarisOS user programs.
 *
 *   mkelf <in.exe> <out.elf>
 *
 * Why this exists: the kernel loads ELF files (kernel/core/elf.c), but the toolchain on a Windows machine (MSYS2 clang +
 * lld) has no ELF linker: its lld and binutils only know PE. clang can still compile freestanding code, and lld-link can
 * link it at a fixed address with page-aligned sections (CMakeLists.txt does that for the programs in user/). The linked result already
 * is what an ELF executable is: sections at fixed addresses with known contents and permissions, and an entry point.
 * This tool only repackages it, one PT_LOAD segment per section:
 *
 *   section        ->  segment
 *   address        ->  p_vaddr           (ImageBase + VirtualAddress)
 *   bytes in file  ->  p_filesz          (zero filled up to p_memsz: .bss)
 *   VirtualSize    ->  p_memsz
 *   code / data    ->  flags: executable, or writable, or read only (never both: the tool refuses)
 *
 * On a machine with an ELF-capable ld.lld the same programs can be linked directly instead; the kernel does not care how
 * the file was made. Sections must start on a 4 KiB boundary (link with -align:4096). Debug and relocation sections
 * (discardable) are left out. The output has no section headers: the loader only reads the program headers.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096u
#define MAX_SEG 16

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "mkelf: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

#define SCN_DISCARDABLE 0x02000000u
#define SCN_EXECUTE 0x20000000u
#define SCN_WRITE 0x80000000u

typedef struct
{
    uint64_t vaddr, filesz, memsz;
    const uint8_t *data; /* filesz bytes */
    uint32_t flags;      /* ELF PF_*: X = 1, W = 2, R = 4 */
} Seg;

int main(int argc, char **argv)
{
    if (argc != 3)
        die("usage: mkelf <in.exe> <out.elf>", 0);

    FILE *f = fopen(argv[1], "rb");
    if (!f)
        die("cannot open", argv[1]);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *pe = malloc(n > 0 ? (size_t)n : 1);
    if (!pe || n < 0x100 || fread(pe, 1, (size_t)n, f) != (size_t)n)
        die("cannot read", argv[1]);
    fclose(f);
    size_t size = (size_t)n;

    /* DOS header -> PE signature -> COFF header -> optional header (PE32+) -> section table */
    if (pe[0] != 'M' || pe[1] != 'Z')
        die("not a PE file (no MZ)", argv[1]);
    uint32_t pehdr = rd32(pe + 0x3C);
    if (pehdr > size - 24 || memcmp(pe + pehdr, "PE\0\0", 4) != 0)
        die("not a PE file (no PE signature)", argv[1]);
    const uint8_t *coff = pe + pehdr + 4;
    if (rd16(coff) != 0x8664)
        die("not an x86-64 PE file", argv[1]);
    unsigned nsections = rd16(coff + 2), opt_size = rd16(coff + 16);
    const uint8_t *opt = coff + 20;
    if ((size_t)(opt - pe) + opt_size > size || opt_size < 112 || rd16(opt) != 0x20B)
        die("not a PE32+ file", argv[1]);
    uint32_t entry_rva = rd32(opt + 16);
    uint64_t image_base = rd64(opt + 24);
    uint32_t section_align = rd32(opt + 32);
    if (section_align < PAGE || section_align % PAGE)
        die("sections are not page aligned: link with -align:4096", 0);
    const uint8_t *sec = opt + opt_size;
    if (nsections == 0 || (size_t)(sec - pe) + (size_t)nsections * 40 > size)
        die("bad section table", 0);

    Seg seg[MAX_SEG];
    int nseg = 0;
    for (unsigned i = 0; i < nsections; i++)
    {
        const uint8_t *s = sec + i * 40;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12), rawsize = rd32(s + 16), rawptr = rd32(s + 20), ch = rd32(s + 36);
        if (vsize == 0 || (ch & SCN_DISCARDABLE))
            continue;
        if (va % PAGE)
            die("a section does not start on a page boundary", 0);
        if ((ch & SCN_EXECUTE) && (ch & SCN_WRITE))
            die("a section is both writable and executable", 0);
        uint32_t filesz = rawsize < vsize ? rawsize : vsize;
        if (filesz && ((uint64_t)rawptr + filesz > size))
            die("a section reaches past the end of the file", 0);
        if (nseg == MAX_SEG)
            die("too many sections", 0);
        seg[nseg].vaddr = image_base + va;
        seg[nseg].filesz = filesz;
        seg[nseg].memsz = vsize;
        seg[nseg].data = pe + rawptr;
        seg[nseg].flags = 4u | ((ch & SCN_EXECUTE) ? 1u : 0u) | ((ch & SCN_WRITE) ? 2u : 0u);
        nseg++;
    }
    if (nseg == 0)
        die("nothing to load", 0);

    /* The ELF file: header and program headers in the first page, then every segment's bytes starting on a page
     * boundary (so file offset and address have the same position in their page, which the loader requires). */
    uint64_t offset[MAX_SEG];
    uint64_t at = PAGE;
    for (int i = 0; i < nseg; i++)
    {
        offset[i] = at;
        at += (seg[i].filesz + PAGE - 1) / PAGE * PAGE;
    }
    uint8_t *out = calloc(1, (size_t)(at ? at : PAGE));
    if (!out)
        die("out of memory", 0);

    memcpy(out, "\x7f" "ELF", 4);
    out[4] = 2; /* 64-bit */
    out[5] = 1; /* little endian */
    out[6] = 1; /* version */
    wr16(out + 16, 2);    /* ET_EXEC */
    wr16(out + 18, 62);   /* x86-64 */
    wr32(out + 20, 1);
    wr64(out + 24, image_base + entry_rva);
    wr64(out + 32, 64);   /* program headers right after this header */
    wr16(out + 52, 64);   /* header size */
    wr16(out + 54, 56);   /* program header size */
    wr16(out + 56, (uint16_t)nseg);
    wr16(out + 58, 64);   /* section header size (there are none) */

    for (int i = 0; i < nseg; i++)
    {
        uint8_t *ph = out + 64 + (size_t)i * 56;
        wr32(ph, 1); /* PT_LOAD */
        wr32(ph + 4, seg[i].flags);
        wr64(ph + 8, offset[i]);
        wr64(ph + 16, seg[i].vaddr);
        wr64(ph + 24, seg[i].vaddr);
        wr64(ph + 32, seg[i].filesz);
        wr64(ph + 40, seg[i].memsz);
        wr64(ph + 48, PAGE);
        if (seg[i].filesz)
            memcpy(out + offset[i], seg[i].data, (size_t)seg[i].filesz);
    }

    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(out, 1, (size_t)at, o) != (size_t)at || fclose(o) != 0)
        die("cannot write", argv[2]);
    printf("mkelf: %s: %d segments, entry 0x%llx, %llu bytes\n", argv[2], nseg,
           (unsigned long long)(image_base + entry_rva), (unsigned long long)at);
    return 0;
}
