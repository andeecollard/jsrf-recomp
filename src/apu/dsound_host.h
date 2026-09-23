/* The host DSOUND: JSRF's DirectSound entry points answered on the host.
 *
 * docs/jsrf/plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_DSOUND_BOUNDARY.md.
 * Instead of running the title's DSOUND against the APU model, the lifted entry
 * points drive this model and it mixes straight to the host device.
 *
 * Pure C, no guest dependencies: guest memory is reached through the resolver
 * passed to dsh_init, and objects are named by the guest handle the caller
 * allocated. Every function is safe to call from any thread; dsh_mix runs on
 * the audio device's thread and never calls guest code.
 *
 * Units are the XDK's: positions and lengths in BYTES of the buffer's own
 * data, volume and headroom in hundredths of a decibel, frequency in Hz. */
#ifndef DSOUND_HOST_H
#define DSOUND_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WAVEFORMATEX tags this model plays. */
#define DSH_TAG_PCM         0x0001u
#define DSH_TAG_XBOX_ADPCM  0x0069u

/* IDirectSoundBuffer_Play flags and GetStatus bits, as the XDK defines them. */
#define DSH_PLAY_LOOPING     0x00000001u
#define DSH_STATUS_PLAYING   0x00000001u
#define DSH_STATUS_LOOPING   0x00000004u

/* DSBHEADROOM_DEFAULT: a buffer attenuates by 6 dB until told otherwise. */
#define DSH_HEADROOM_DEFAULT 600u

/* Guest memory: a host pointer to [va, va+len), or NULL if any of it is not
 * mapped. Called with the model's lock held, from any thread. */
typedef const uint8_t *(*dsh_mem_fn)(uint32_t va, uint32_t len);

typedef struct dsh_format {
    uint16_t tag;          /* DSH_TAG_PCM or DSH_TAG_XBOX_ADPCM */
    uint16_t channels;     /* 1 or 2 */
    uint32_t rate;         /* nSamplesPerSec */
    uint16_t bits;         /* 16 for PCM, 4 for ADPCM (8-bit PCM is accepted too) */
    uint16_t block_align;  /* nBlockAlign: 36 * channels for ADPCM */
} dsh_format;

/* Returns the count of buffers the model holds, for reports. */
void     dsh_init(dsh_mem_fn mem, uint32_t out_rate);
void     dsh_reset(void);

/* Returns 0 on success, -1 if the format is one this model cannot play or the
 * table is full. `data_va`/`bytes` may be 0 and set later by SetBufferData. */
int      dsh_buffer_create(uint32_t handle, const dsh_format *fmt,
                           uint32_t data_va, uint32_t bytes);
void     dsh_buffer_release(uint32_t handle);
int      dsh_buffer_exists(uint32_t handle);

void     dsh_set_buffer_data(uint32_t handle, uint32_t data_va, uint32_t bytes);
void     dsh_set_loop_region(uint32_t handle, uint32_t start, uint32_t length);
void     dsh_set_current_position(uint32_t handle, uint32_t pos);
void     dsh_play(uint32_t handle, uint32_t flags);
void     dsh_stop(uint32_t handle);
void     dsh_set_frequency(uint32_t handle, uint32_t hz);   /* 0 = the format's rate */
void     dsh_set_volume(uint32_t handle, int32_t centibels);  /* <= 0 */
void     dsh_set_headroom(uint32_t handle, uint32_t centibels);

/* IDirectSoundBuffer_SetMixBins / DSBUFFERDESC.lpMixBins: `n` (bin, volume)
 * pairs, volume in centibels. Bins 0/4 (front/back left) feed the left
 * output, 1/5 the right, 2 (centre) both at -3 dB; the rest (LFE, crosstalk,
 * I3DL2 and FX sends) are not mixed yet. For a mono buffer every pair applies
 * to its one channel; for stereo, pair i applies to channel i % 2. n = 0
 * restores the default: mono to both sides, stereo left-to-left,
 * right-to-right. */
#define DSH_MIXBIN_MAX 8
void     dsh_set_mixbins(uint32_t handle, uint32_t n, const uint32_t *bins, const int32_t *vols);

/* 3D (G48 §2c: 161 of 231 buffers in a gameplay replay carry DSBCAPS_CTRL3D;
 * the listener moves every frame; I3DL2 reverb is always set to "off").
 * DirectSound's model: below min distance full volume, then -6 dB per
 * doubling of distance times the rolloff factor, no further falloff past max.
 * Distances are in the title's units times the distance factor. Pan is a soft
 * equal-ish split from the source's direction against the listener's right
 * vector (top x front, DirectSound's left-handed axes); the Xbox's HRTF is
 * not modelled. Mode: 0 normal, 1 head-relative, 2 disabled. Defaults are
 * the XDK's: position 0, min 1, max 1e9, mode normal. */
void     dsh_set_3d(uint32_t handle, int enabled);
void     dsh_set_3d_position(uint32_t handle, float x, float y, float z);
void     dsh_set_3d_distances(uint32_t handle, float min_d, float max_d);   /* <0 leaves one unchanged */
void     dsh_set_3d_mode(uint32_t handle, uint32_t mode);
void     dsh_set_listener_position(float x, float y, float z);
void     dsh_set_listener_orientation(float fx, float fy, float fz, float tx, float ty, float tz);
void     dsh_set_listener_factors(float distance_factor, float rolloff_factor);   /* <0 leaves one unchanged */

/* STREAM BUFFERS (G46, the intro garble). A stream's writer -- CRI ADX --
 * keeps only 10-60 ms written ahead of the play cursor, and refills once a
 * frame; a frame gap longer than that lets the cursor overtake the writer and
 * play a lap-old region. On the Xbox the cursor DSOUND reports is the APU's
 * FETCH position, which runs ahead of what is heard, so the writer's real
 * cushion is larger than it computes.
 *   dsh_set_cursor_lead: report the cursor `bytes` ahead of what the mixer has
 *     consumed (play and write both), so the writer keeps that much more
 *     written. 0 = off.
 *   dsh_stream_mark: the end of the region the writer just filled (byte offset
 *     in the buffer). The mixer counts an UNDERRUN each time its cursor
 *     crosses the latest mark -- playback overtaking the writer. */
void     dsh_set_cursor_lead(uint32_t handle, uint32_t bytes);
void     dsh_stream_mark(uint32_t handle, uint32_t end_offset);

uint32_t dsh_get_status(uint32_t handle);
/* The play cursor, and a write cursor one mix quantum ahead of it, both
 * wrapped into the buffer. Either pointer may be NULL. */
void     dsh_get_current_position(uint32_t handle, uint32_t *play, uint32_t *write);

/* Mix `frames` stereo frames at the output rate into `out`, interleaved,
 * advancing every playing voice. Overwrites `out`. */
void     dsh_mix(int16_t *out, uint32_t frames);

/* Xbox ADPCM, one block: 4-byte header per channel, then 4-byte chunks of 8
 * samples alternating channels. Produces 65 frames per block, interleaved.
 * Hardware semantics: the reserved header byte is ignored and the step index
 * clamped, never refused. Returns frames produced (65), or 0 if `in_bytes` is
 * shorter than a block. */
int      dsh_adpcm_decode_block(int16_t *out, const uint8_t *in,
                                uint32_t in_bytes, int channels);

/* Start pulling dsh_mix to the host's audio device, 48 kHz stereo. If no
 * device opens (a silenced harness run), a thread mixes into scratch at real
 * time instead, so cursors and status still advance as they would with one.
 * Returns 1 for a device, 2 for the paced thread, 0 if neither started. */
int      dsh_output_start(void);

typedef struct dsh_stats {
    unsigned long buffers, playing, created, refused_format, released;
    unsigned long plays, stops, mixes, frames_mixed, missing_data;
    unsigned long underruns;        /* cursor crossed a stream's write mark */
} dsh_stats;
void     dsh_get_stats(dsh_stats *s);

#ifdef __cplusplus
}
#endif
#endif
