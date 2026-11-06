/* Processes: demand paging. A big program (Player is 2 MiB, mostly font data) is not read and copied into RAM when it starts. Its
 * segments are only written down (a LazyImage: which file, where each segment is); a page gets its bytes the first time ring 3 touches
 * it (a page fault, process_lazy_fault) or the kernel is about to use it for a system call (process_lazy_ensure, called by
 * aspace_user_ok). So the program starts after reading a few pages, not all of them, and pages it never uses are never read.
 *
 * The file must stay where it is: the node is checked at every load (still there, same size and name) and the process is ended (an
 * ordinary page fault) when the drive has gone. A page that could not be loaded is never left half mapped: the bytes are read into a
 * buffer first and the page is mapped only when they all arrived. */
#include "process_internal.h"
#include "vfs.h"

static int image_ok(const LazyImage *im)
{
    const VfsNode *vn = vfs_node(im->node);
    if (!vn || vn->is_dir || vn->size != im->size)
        return 0;
    for (int i = 0; i < VFS_NAME_MAX; i++)
    {
        if (vn->name[i] != im->name[i])
            return 0;
        if (!vn->name[i])
            break;
    }
    return 1;
}

/* Make the page at `page` (page aligned) exist if it belongs to a segment of the program. 1 = it is there now (it was, or it was
 * loaded), 0 = no segment has it, -1 = it should be there but cannot be loaded (no memory, the file is gone or unreadable). */
static int fill_page(const LazyImage *im, AddressSpace *as, u64 page)
{
    u64 phys;
    if (aspace_translate(as, page, &phys) == 0)
        return 1;
    for (int i = 0; i < im->nseg; i++)
    {
        const ElfSegment *s = &im->seg[i];
        u64 file_off;
        unsigned dst, len;
        if (!elf_page_source(s, page, &file_off, &dst, &len))
            continue;
        unsigned char *buf = 0; /* (heap, not stack: a 4 KiB frame needs the stack probe __chkstk, which the kernel does not have) */
        if (len)
        {
            if (!image_ok(im))
                return -1;
            buf = kmalloc(4096);
            if (!buf)
                return -1;
            for (unsigned done = 0; done < len;)
            {
                int got = vfs_read(im->node, (unsigned)(file_off + done), buf + done, len - done);
                if (got <= 0)
                {
                    kfree(buf);
                    return -1;
                }
                done += (unsigned)got;
            }
        }
        unsigned flags = ((s->flags & ELF_PF_W) ? ASPACE_WRITE : 0) | ((s->flags & ELF_PF_X) ? ASPACE_EXEC : 0);
        int ok = aspace_map_new(as, page, flags) == 0 && aspace_translate(as, page, &phys) == 0;
        if (ok)
        {
            volatile unsigned char *to = (volatile unsigned char *)phys + dst; /* (the new page is zero: only the file's bytes are copied; no memcpy) */
            for (unsigned k = 0; k < len; k++)
                to[k] = buf[k];
        }
        kfree(buf); /* (NULL for a page that is only .bss) */
        return ok ? 1 : -1;
    }
    return 0;
}

int process_lazy_fault(u64 addr)
{
    Process *p = process_current();
    AddressSpace *as = task_address_space();
    if (!p || !as || !p->lazy)
        return 0;
    int r = fill_page(p->lazy, as, addr & ~4095ULL);
    if (r < 0)
        serial_puts("process: a page of the program could not be loaded\n");
    return r == 1;
}

void process_lazy_ensure(const AddressSpace *as, u64 va, u64 len)
{
    Process *p = process_current();
    if (!p || !p->lazy || as != task_address_space() || len == 0 || va + len < va)
        return;
    const LazyImage *im = p->lazy;
    for (int i = 0; i < im->nseg; i++) /* only the parts of the range that lie in a segment: a huge bogus length costs nothing */
    {
        u64 lo = im->seg[i].vaddr & ~4095ULL, hi = (im->seg[i].vaddr + im->seg[i].memsz + 4095) & ~4095ULL;
        if (va > lo)
            lo = va & ~4095ULL;
        if (va + len < hi)
            hi = va + len;
        for (u64 page = lo; page < hi; page += 4096)
            (void)fill_page(im, (AddressSpace *)as, page); /* a failure shows when the range is checked right after */
    }
}

int process_lazy_populate_all(Process *p, AddressSpace *as)
{
    if (!p->lazy)
        return 0;
    const LazyImage *im = p->lazy;
    for (int i = 0; i < im->nseg; i++)
    {
        u64 lo = im->seg[i].vaddr & ~4095ULL, hi = (im->seg[i].vaddr + im->seg[i].memsz + 4095) & ~4095ULL;
        for (u64 page = lo; page < hi; page += 4096)
            if (fill_page(im, as, page) < 0)
                return -1;
    }
    process_lazy_free(p);
    return 0;
}

void process_lazy_free(Process *p)
{
    kfree(p->lazy); /* (NULL is fine) */
    p->lazy = 0;
}
