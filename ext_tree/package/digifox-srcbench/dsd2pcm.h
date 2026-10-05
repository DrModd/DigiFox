/*
 * dsd2pcm — DSD64..DSD512 to PCM 352.8 kHz, multi-stage, for the Fox (Cortex-A7).
 *
 *   stage 1: table FIR on the 1-bit stream, 4 bits (DSD64) or 8 bits (DSD128+)
 *            per lookup, decimation by 4 or 8 -> 705.6 kHz .. 2822.4 kHz.
 *            Small tables (<= 9 x 256 floats per channel set) stay in L1 cache.
 *   stage 2: half-band FIRs, each /2, down to 352.8 kHz (float, NEON-friendly).
 * Pass band 0..96 kHz flat, ~100 dB stop band where it would alias into it.
 * The final low-pass to 96 kHz (for 192 kHz output) is done by soxr.
 *
 * GPL-2.0-or-later. DigiFox.
 */
#ifndef DSD2PCM_H
#define DSD2PCM_H
#include <stddef.h>
#include <stdint.h>

#define DSD2PCM_RATE 352800

typedef struct dsd2pcm dsd2pcm_t;

/* dsd_mult: 64, 128, 256, 512 */
dsd2pcm_t *dsd2pcm_new(int dsd_mult);
void       dsd2pcm_free(dsd2pcm_t *d);

/* Bytes per channel per output sample at 352.8 kHz (DSD64 0.5 ... DSD512 4),
 * times 2 = bytes consumed per output frame. */
double     dsd2pcm_bytes_per_frame(const dsd2pcm_t *d);

/* Convert one channel. in: `n` DSD bytes, MSB first. out: up to
 * n * 8 / (dsd_mult / 64 * 8) samples, written with stride `ostride`
 * (2 for interleaved stereo). Returns samples written. Keep a separate
 * dsd2pcm_t per channel. */
size_t     dsd2pcm_run(dsd2pcm_t *d, const uint8_t *in, size_t n, float *out, int ostride);

#endif
