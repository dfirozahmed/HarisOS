/* Sound for programs: turns a WAV file image into 48 kHz stereo samples and hands them to the HD Audio driver. */
#include "audio.h"
#include "hda.h"
#include "pmm.h"
#include "usb.h"
#include "wav.h"

/* Volume belongs to the device that plays: HD Audio when there is one, else the USB sound device (if it has a volume control). */
int audio_volume(void) { return hda_ready() ? hda_volume() : usb_audio_volume(); }

int audio_set_volume(int percent) { return hda_ready() ? hda_set_volume(percent) : usb_audio_set_volume(percent); }

int audio_volume_step(int delta)
{
    int cur = audio_volume();
    return cur < 0 ? -1 : audio_set_volume(cur + delta);
}

static volatile int STOP_REQUEST;

void audio_stop(int request) { STOP_REQUEST = request != 0; }
int audio_stop_requested(void) { return STOP_REQUEST; }

/* A request that was seen is used up: the sound it stopped (or the one it kept from starting) reports AUDIO_ERR_STOPPED once. */
static int stopped(void)
{
    if (!STOP_REQUEST)
        return 0;
    STOP_REQUEST = 0;
    return 1;
}

int audio_play_wav(const unsigned char *file, unsigned long len)
{
    if (stopped())
        return AUDIO_ERR_STOPPED;
    WavInfo wi;
    int e = wav_parse(file, len, &wi);
    if (e)
        return e;
    int use_hda = hda_ready();
    if (!use_hda && !usb_audio_ready())
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
    int r = use_hda ? hda_play_pcm((const short *)buf, frames) : usb_audio_play_pcm((const short *)buf, frames); /* HD Audio first, else a USB sound device */
    pmm_free_contiguous(buf, pages);
    if (stopped())
        return AUDIO_ERR_STOPPED;
    return r == 0 ? 0 : AUDIO_ERR_BUSY; /* the only way to fail here is a stream that is in use or does not move */
}
