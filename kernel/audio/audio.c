/* Sound for programs: turns a WAV file image into 48 kHz stereo samples and hands them to the HD Audio driver. */
#include "audio.h"
#include "hda.h"
#include "pmm.h"
#include "wav.h"

int audio_volume(void) { return hda_ready() ? hda_volume() : -1; }

int audio_set_volume(int percent) { return hda_ready() ? hda_set_volume(percent) : -1; }

int audio_volume_step(int delta)
{
    if (!hda_ready())
        return -1;
    return hda_set_volume(hda_volume() + delta);
}

int audio_play_wav(const unsigned char *file, unsigned long len)
{
    WavInfo wi;
    int e = wav_parse(file, len, &wi);
    if (e)
        return e;
    if (!hda_ready())
        return AUDIO_ERR_NONE;
    unsigned long frames = wav_frames_48k(&wi);
    if (frames == 0 || frames > HDA_MAX_FRAMES)
        return AUDIO_ERR_MEMORY;
    unsigned long bytes = frames * 4ul;
    unsigned long pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    unsigned long long buf = pmm_alloc_contiguous_tag(pages, PMM_TAG_OTHER);
    if (!buf)
        return AUDIO_ERR_MEMORY;
    wav_to_48k_stereo16(file, &wi, (short *)buf, frames);
    int r = hda_play_pcm((const short *)buf, frames);
    pmm_free_contiguous(buf, pages);
    return r == 0 ? 0 : AUDIO_ERR_BUSY; /* the only way to fail here is a stream that is in use or does not move */
}
