/* audio_out.h -- see audio_out.c. MIT licensed, see LICENSE. */
#ifndef PB_AUDIO_OUT_H
#define PB_AUDIO_OUT_H

#include <stddef.h>
#include <stdint.h>

/* Open the Switch's audio output. Returns the rate actually used (always
 * 48000 with audout) or 0 on failure. `channels` and `samples` are the
 * frame layout SDL asked for; `samples` is per channel. */
int  pbaudio_open(int freq, int channels, int samples);
/* Blocking: hands one buffer of interleaved signed 16-bit frames to the
 * hardware and waits until a buffer is free again. */
void pbaudio_write_s16(const int16_t *frames, size_t nframes);
void pbaudio_close(void);
int  pbaudio_is_open(void);

#endif
