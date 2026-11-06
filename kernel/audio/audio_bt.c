/* Sound to a Bluetooth headset (A2DP): the program's side. The samples go into the ring of bt_a2dp.c; the main loop (bt_poll ->
 * bt_a2dp_step) encodes them to SBC and sends them at the speed of the music. This function only has to keep the ring filled and to wait
 * until everything has been taken, so it blocks for as long as the sound lasts, like hda_play_pcm does. */
#include "audio.h"
#include "bluetooth.h"
#include "task.h"

#define NO_PROGRESS_MS 3000 /* the ring did not move for this long: the link is stuck or gone */
#define POLL_MS 5

int audio_bt_ready(void) { return bt_a2dp_audio_ready(); }

/* Plays `frames` sample frames (48 kHz, stereo, 16 bit). 0 = played, AUDIO_ERR_BUSY = in use or the stream did not move,
 * AUDIO_ERR_STOPPED = audio_stop() was called. */
int audio_bt_play(const short *pcm, unsigned long frames)
{
    if (bt_a2dp_producer_begin() != 0)
        return AUDIO_ERR_BUSY;
    int result = 0;
    unsigned long done = 0;
    static const short SILENCE[2 * 128]; /* the end is padded with silence to whole SBC frames (128 samples is a multiple of every frame size) */
    unsigned long total = (frames + 127) / 128 * 128;
    int ended = 0; /* the producer flag is released as soon as everything is written: the sender must not count the draining ring as a program that is late */
    unsigned long long moved_at = bt_now();
    unsigned last_pending = bt_a2dp_ring_pending();
    while (done < total || bt_a2dp_ring_pending() > 0)
    {
        if (audio_stop_requested())
        {
            bt_a2dp_flush();
            result = AUDIO_ERR_STOPPED;
            break;
        }
        if (!bt_a2dp_audio_ready())
        {
            result = AUDIO_ERR_BUSY; /* the link went away */
            break;
        }
        if (done < total)
        {
            unsigned n;
            if (done < frames)
            {
                unsigned long left = frames - done;
                n = bt_a2dp_ring_write(pcm + 2 * done, left > 4096 ? 4096 : (unsigned)left);
            }
            else
                n = bt_a2dp_ring_write(SILENCE, (unsigned)(total - done < 128 ? total - done : 128));
            done += n;
            if (n)
                moved_at = bt_now();
            if (done >= total)
            {
                bt_a2dp_producer_end();
                ended = 1;
            }
        }
        unsigned pending = bt_a2dp_ring_pending();
        if (pending != last_pending)
        {
            if (pending < last_pending)
                moved_at = bt_now();
            last_pending = pending;
        }
        if (bt_now() - moved_at > NO_PROGRESS_MS)
        {
            bt_a2dp_flush();
            result = AUDIO_ERR_BUSY;
            break;
        }
        task_sleep_ms(POLL_MS);
    }
    if (!ended)
        bt_a2dp_producer_end();
    return result;
}
