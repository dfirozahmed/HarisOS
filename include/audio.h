/* ============================================================
 * HarisOS sound, the part programs use
 *
 *   audio_play_wav(file, len);    plays a WAV file image (see wav.h for what is understood); blocks until it has played
 *
 * Returns 0, a WAV_ERR_* (the file is not playable), AUDIO_ERR_NONE (no sound hardware with an output), AUDIO_ERR_BUSY
 * (something else is playing) or AUDIO_ERR_MEMORY (the converted sound is too long, or no memory).
 * ============================================================ */
#ifndef HARIS_AUDIO_H
#define HARIS_AUDIO_H

#define AUDIO_ERR_NONE -10
#define AUDIO_ERR_BUSY -11
#define AUDIO_ERR_MEMORY -12
#define AUDIO_ERR_STOPPED -13 /* audio_stop(1) was called: the sound was cut short (or not started); the request is used up */

/*   audio_stop(1);                asks the sound that plays now (or the next that starts) to stop; the player returns AUDIO_ERR_STOPPED
 *   audio_stop(0);                takes an unused request back
 *   audio_stop_requested();       1 while a request waits: the playback loops of the drivers look at it every few milliseconds */
void audio_stop(int request);
int audio_stop_requested(void);

int audio_play_wav(const unsigned char *file, unsigned long len);

/*   audio_volume();               master volume 0..100 (%), or -1 when there is no sound hardware with an output
 *   audio_set_volume(70);         sets it (clamped to 0..100); also while a sound plays; returns the new value or -1
 *   audio_volume_step(+5);        louder / quieter by that many percent points; returns the new value or -1 */
int audio_volume(void);
int audio_set_volume(int percent);
int audio_volume_step(int delta);

#endif
