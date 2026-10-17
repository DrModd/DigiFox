/*
 * dfx_level.h — signal levels for the amplifier's VU meters.
 *
 * Whatever writes the audio to the I2S port (pcm_digifox, uac2_router, the
 * "digilvl" ALSA meter scope) pushes short blocks of levels into a small ring
 * in /tmp (shared memory, tmpfs). Each block carries the moment its first
 * frame is heard (CLOCK_MONOTONIC, ms), so pfrate can hand the levels to the
 * amplifier exactly when that sound comes out — the needles move in time with
 * the music although players keep a long buffer in front of the I2S.
 *
 * Values are normalised to digital full scale: mean square (RMS²) and peak
 * per channel. Only one producer is active at a time (one stream on the I2S).
 *
 * The same file is kept in digifox-src, uac2_router and pfrate.
 * GPL-2.0-or-later. DigiFox.
 */
#ifndef DFX_LEVEL_H
#define DFX_LEVEL_H

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define DFX_LVL_FILE   "/tmp/digifox_lvl"
#define DFX_LVL_MAGIC  0x4C564C31u          /* "LVL1" */
#define DFX_LVL_N      128                  /* ring entries (~1.3 s of 10 ms blocks) */
#define DFX_LVL_BLOCK_MS 10                 /* producers cut their data into blocks this long */

typedef struct {
    int64_t  t_ms;                          /* when the block starts to be heard */
    float    ms[2];                         /* mean square, full scale = 1 */
    float    pk[2];                         /* peak |x|, full scale = 1 */
} dfx_lvl_e;

typedef struct {
    uint32_t magic;
    volatile uint32_t head;                 /* entries written so far (index = head % N) */
    dfx_lvl_e e[DFX_LVL_N];
} dfx_lvl_t;

static inline int64_t dfx_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* map the ring (create it if needed); NULL on failure — levels are optional */
static inline dfx_lvl_t *dfx_lvl_map(void)
{
    int fd = open(DFX_LVL_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return NULL;
    (void)fchmod(fd, 0666);                 /* players run as other users */
    if (ftruncate(fd, sizeof(dfx_lvl_t)) < 0) { close(fd); return NULL; }
    void *p = mmap(NULL, sizeof(dfx_lvl_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    dfx_lvl_t *m = (dfx_lvl_t *)p;
    if (m->magic != DFX_LVL_MAGIC) { memset(m, 0, sizeof *m); m->magic = DFX_LVL_MAGIC; }
    return m;
}

static inline void dfx_lvl_put(dfx_lvl_t *m, int64_t t_ms, const float ms[2], const float pk[2])
{
    uint32_t h = m->head;
    dfx_lvl_e *e = &m->e[h % DFX_LVL_N];
    e->t_ms = t_ms;
    e->ms[0] = ms[0]; e->ms[1] = ms[1];
    e->pk[0] = pk[0]; e->pk[1] = pk[1];
    __sync_synchronize();
    m->head = h + 1;
}

/*
 * Levels of interleaved stereo S32 frames that start to be heard at t0_ms,
 * cut into DFX_LVL_BLOCK_MS blocks. At high rates only every step-th frame is
 * looked at (cheap enough for the real-time USB path; a VU meter does not
 * need every sample).
 */
static inline void dfx_lvl_s32(dfx_lvl_t *m, int64_t t0_ms, const int32_t *s,
                               unsigned long frames, unsigned rate)
{
    if (!m || !frames || !rate) return;
    unsigned step = rate > 96000 ? rate / 48000 : 1;
    unsigned long blk = (unsigned long)rate * DFX_LVL_BLOCK_MS / 1000;
    const float k = 1.0f / 2147483648.0f;
    for (unsigned long f0 = 0; f0 < frames; f0 += blk) {
        unsigned long n = frames - f0 < blk ? frames - f0 : blk;
        double sum[2] = { 0, 0 };
        int32_t pk[2] = { 0, 0 };
        unsigned cnt = 0;
        for (unsigned long i = f0; i < f0 + n; i += step) {
            for (int c = 0; c < 2; c++) {
                int32_t v = s[i * 2 + c];
                float x = (float)v * k;
                sum[c] += (double)x * x;
                if (v < 0) v = v == INT32_MIN ? INT32_MAX : -v;
                if (v > pk[c]) pk[c] = v;
            }
            cnt++;
        }
        if (!cnt) continue;
        float msv[2] = { (float)(sum[0] / cnt), (float)(sum[1] / cnt) };
        float pkv[2] = { (float)pk[0] * k, (float)pk[1] * k };
        dfx_lvl_put(m, t0_ms + (int64_t)(f0 * 1000 / rate), msv, pkv);
    }
}

#endif
