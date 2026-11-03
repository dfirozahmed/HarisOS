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

int audio_play_wav(const unsigned char *file, unsigned long len);

#endif
