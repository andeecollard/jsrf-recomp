/*
 * apu_wav.h -- the WAV tap on the APU's output (RECOMP_AUDIO_WAV).
 *
 * Writes exactly the samples handed to the host device -- 48 kHz, 16-bit
 * stereo, after the mixdown, tone and software mixer. That is what the APU
 * produced, not what the speaker played: when a period reaches the device
 * late, the device plays silence of its own in the meantime, and the file
 * has no hole there. Whether the output was continuous is measured from the
 * device's consumption (the existing JSRF SDL queue-depth and starvation
 * counters), never from this file alone.
 *
 * The header is valid at every point a run can end: it is rewritten with the
 * current sizes once per second of audio (so a hard kill loses at most the
 * last second), and finalised by apu_wav_close, which mcpx_apu_shutdown, an
 * atexit handler and (on Windows) the console-control handler all call. The
 * watchdog's _exit skips atexit, so it calls apu_wav_close_nowait first.
 */
#ifndef APU_WAV_H
#define APU_WAV_H

#include <stdint.h>

#define APU_WAV_RATE     48000
#define APU_WAV_CHANNELS 2

/* Start a dump at `path`. max_secs > 0 caps it (the file is closed once that
 * much audio is written); <= 0 is uncapped. Returns 1 on success. A second
 * open while one is active is refused (returns 0). */
int  apu_wav_open(const char *path, double max_secs);

/* Append `count` stereo sample frames (interleaved L/R). No-op when closed. */
void apu_wav_write(const int16_t *samples, int count);

/* Finalise the header and close. Safe to call more than once and from any
 * thread. */
void apu_wav_close(void);

/* apu_wav_close for a thread about to _exit (the watchdog): tries the lock
 * for up to wait_ms and gives up rather than block on a writer that will
 * never release it. Returns 1 if closed (or nothing was open), 0 if the
 * lock stayed held -- the last once-a-second header refresh then stands. */
int  apu_wav_close_nowait(int wait_ms);

int  apu_wav_is_open(void);

#endif /* APU_WAV_H */
