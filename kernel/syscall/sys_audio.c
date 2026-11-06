/* System calls: sound. */
#include "audio.h"
#include "pmm.h"
#include "syscall_internal.h"

/* SYS_AUDIO_PLAY: rdi = pointer to a WAV file image in memory, rsi = its length (1 .. SYS_AUDIO_MAX). Plays it and returns when it has
 * finished (audio.h says what is understood). 0, -1 for a bad pointer or length, else -100 + the code of audio_play_wav(). The image is
 * copied to kernel memory first: the player sleeps while it plays and must not depend on the caller's pages meanwhile. */
long long sys_audio_play(const SyscallFrame *f)
{
    ull ptr = f->rdi, len = f->rsi;
    if (len == 0 || len > SYS_AUDIO_MAX)
        return -1;
    const AddressSpace *as = task_address_space();
    if (!as || !aspace_user_ok(as, ptr, len, 0))
        return -1;
    ull pages = (len + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    unsigned long long copy0 = timer_ms();
    unsigned char *buf = (unsigned char *)pmm_alloc_contiguous_tag(pages, PMM_TAG_OTHER);
    if (!buf)
        return -1;
    for (ull done = 0; done < len;)
    {
        unsigned n = (len - done) < CHUNK ? (unsigned)(len - done) : CHUNK;
        copy_from_user((char *)buf + done, ptr + done, n);
        done += n;
    }
    serial_puts("audio: copied ");
    serial_putd((long long)(len / 1024));
    serial_puts(" KiB in ");
    serial_putd((long long)(timer_ms() - copy0));
    serial_puts(" ms\n");
    int r = audio_play_wav(buf, len);
    pmm_free_contiguous((unsigned long long)buf, pages);
    return r == 0 ? 0 : -100 + r; /* -101 .. -104 file errors, -110 none, -111 busy, -112 memory */
}

/* SYS_AUDIO_STOP: rdi = 1 asks the sound that plays (or the next one that starts) to stop, rdi = 0 takes the request back. See audio.h. */
long long sys_audio_stop(const SyscallFrame *f)
{
    audio_stop(f->rdi != 0);
    return 0;
}

/* SYS_AUDIO_VOLUME: rdi = -1 (ask) or the new volume in percent (clamped to 0 .. 100). The volume now in force, or -1 without sound hardware. */
long long sys_audio_volume(const SyscallFrame *f)
{
    long long v = (long long)f->rdi;
    return v == -1 ? audio_volume() : audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : (int)v);
}
