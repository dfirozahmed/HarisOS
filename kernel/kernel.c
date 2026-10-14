/* HarisOS kernel core: UEFI-independent entry point. */
#include "bootinfo.h"
#include "desktop.h"
#include "pmm.h"

static char *put_str(char *out, const char *s)
{
    while (*s)
        *out++ = *s++;
    return out;
}

static char *put_num(char *out, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *out++ = tmp[--n];
    return out;
}

void kernel_main(const BootInfo *info)
{
    static char status[96];

    desktop_set_framebuffer(info->framebuffer, info->width, info->height,
                            info->pitch, info->red_mask_mode);

    char *p = status;
    if (pmm_init(info) == 0)
    {
        p = put_str(p, "MEMORY ");
        p = put_num(p, pmm_free_pages() * PMM_PAGE_SIZE / (1024 * 1024));
        p = put_str(p, " MB FREE OF ");
        p = put_num(p, pmm_total_pages() * PMM_PAGE_SIZE / (1024 * 1024));
        p = put_str(p, " MB");
    }
    else
    {
        p = put_str(p, "MEMORY MANAGER FAILED");
    }
    *p = 0;
    desktop_set_status(status);

    desktop_draw();

    for (;;)
    {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
#endif
    }
}
