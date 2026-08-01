/* audio.h -- ALSA device handling and the audio thread. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>

/* Open and configure the PCM device.
 * `device` may be the literal string "none", in which case no device is
 * opened at all and the audio thread free-runs off the monotonic clock --
 * useful over ssh when you are only streaming to a laptop.
 * Returns 0 on success, -1 with a message in `err`. */
int  audio_open(const char *device, int rate, int allow_resample,
                char *err, size_t errsz);

/* Start the audio thread. Attempts SCHED_FIFO; if the kernel says no
 * (the usual case without rtprio limits configured) it degrades to normal
 * scheduling and writes an explanation into `warn`. Never fatal. */
int  audio_start(char *warn, size_t warnsz);

void audio_stop(void);
void audio_close(void);

/* Reconfigure the device for a new sample rate. Called from the UI thread.
 * Internally parks the audio thread first -- see the long comment in
 * audio.c for why the reconfiguration deliberately does NOT happen on the
 * audio thread. Returns 0 on success. */
int  audio_retune(int rate, char *err, size_t errsz);

/* Display helpers. */
const char *audio_device_name(void);
int         audio_period_frames(void);
int         audio_buffer_frames(void);
int         audio_channels(void);
int         audio_have_device(void);

/* Headless invariants for the clocked delay and master phrase looper. This
 * never opens a device or starts a thread; main.c calls it from --self-test. */
int         audio_self_test(char *err, size_t errsz);

#endif /* AUDIO_H */
