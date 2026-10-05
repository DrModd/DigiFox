/*
 * pcm_digifox — ALSA plugin: everything a player sends goes out of the I2S
 * port as PCM 192 kHz / 32 bit, the job the AK4137 does on older DigiD D1.
 *
 *   PCM 44.1 … 768 kHz      -> soxr (VHQ up to 384 kHz, HQ above)
 *   PCM 192 kHz             -> passed through (format conversion only)
 *   DSD64 … DSD256 (U8, U16, U32 LE/BE, 44.1k and 48k families)
 *                           -> dsd2pcm (multi-stage, 352.8/384 kHz) -> soxr HQ
 *
 * asound.conf:
 *   pcm.!default { type digifox  slave "hw:0,0" }      # quality "auto|hq|vhq"
 *
 * Design: an ioplug. The plugin's ring buffer holds the player's data as it
 * came; frames leave the ring ("hardware pointer") when they are converted
 * into a small staging buffer, which is written to the slave without
 * blocking whenever the slave has room. Conversion happens in whichever
 * call comes first: write, pointer update or poll. Delay reports the whole
 * chain (ring + staging + slave), so AirPlay/Roon sync stays right.
 *
 * GPL-2.0-or-later. DigiFox.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <soxr.h>
#include "dsd2pcm.h"

#define OUT_RATE    192000
#define CH          2
#define ST_CAP      32768           /* staging, output frames               */
#define PCM_CHUNK   2048            /* input frames per PCM conversion step  */
#define DSD_CHUNK   4096            /* bytes per channel per DSD step        */
/* what the player sends, for the amplifier display (pfrate -> "@IN ...") */
#define IN_FILE     "/tmp/digifox_in"

enum { M_NONE, M_PASS, M_PCM, M_DSD };
enum { Q_AUTO, Q_HQ, Q_VHQ };

typedef struct {
    snd_pcm_ioplug_t io;
    char        *slave_name;
    snd_pcm_t   *slave;
    int          slave_ready;       /* hw params set */
    snd_pcm_uframes_t s_period, s_buffer;
    int          quality;

    /* stream */
    int          mode;
    unsigned     fbytes;            /* bytes per input frame                 */
    unsigned     dsd_bpc;           /* DSD: bytes per channel per frame      */
    int          dsd_rev;           /* DSD: bytes of a word stored LSB first */
    snd_pcm_format_t fmt;

    uint8_t     *ring;              /* buffer_size frames, input format      */
    /* 64-bit on purpose: snd_pcm_uframes_t is 32 bits on the Fox and would
     * wrap after ~50 min of DSD256 (x % buffer_size jumps at the wrap) */
    uint64_t     wpos, hw;          /* frames written / consumed (monotonic) */
    snd_pcm_uframes_t since_ptr;    /* consumed since the last pointer call  */
    int          running;

    int32_t     *st;                /* staging, interleaved S32              */
    size_t       st_len, st_pos;

    soxr_t       sx;
    dsd2pcm_t   *dd[CH];
    int32_t     *pin;               /* PCM chunk as S32                      */
    uint8_t     *dbytes[CH];        /* DSD chunk, one byte stream per ch     */
    float       *dpcm;              /* DSD -> PCM (352.8/384k), interleaved  */
    double       ratio;             /* input frames per output frame         */
} dfx_t;

/* ------------------------------------------------------------ helpers */

static int dsd_bits(snd_pcm_format_t f)
{
    switch (f) {
    case SND_PCM_FORMAT_DSD_U8:     return 8;
    case SND_PCM_FORMAT_DSD_U16_LE:
    case SND_PCM_FORMAT_DSD_U16_BE: return 16;
    case SND_PCM_FORMAT_DSD_U32_LE:
    case SND_PCM_FORMAT_DSD_U32_BE: return 32;
    default:                        return 0;
    }
}

static void report_in(const char *s)
{
    if (!s) { unlink(IN_FILE); return; }
    char tmp[64];
    snprintf(tmp, sizeof tmp, IN_FILE ".%d", (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
    rename(tmp, IN_FILE);
}

static void free_conv(dfx_t *d)
{
    if (d->mode != M_NONE) report_in(NULL);
    if (d->sx) { soxr_delete(d->sx); d->sx = NULL; }
    for (int c = 0; c < CH; c++) {
        if (d->dd[c]) { dsd2pcm_free(d->dd[c]); d->dd[c] = NULL; }
        free(d->dbytes[c]); d->dbytes[c] = NULL;
    }
    free(d->ring); d->ring = NULL;
    free(d->st); d->st = NULL;
    free(d->pin); d->pin = NULL;
    free(d->dpcm); d->dpcm = NULL;
    d->mode = M_NONE;
}

/* one slave setup for the whole life of the plugin: S32_LE, 2 ch, 192 kHz */
static int setup_slave(dfx_t *d)
{
    if (d->slave_ready) return 0;
    snd_pcm_hw_params_t *hp;
    snd_pcm_sw_params_t *sp;
    snd_pcm_hw_params_alloca(&hp);
    snd_pcm_sw_params_alloca(&sp);
    int err;
    unsigned rate = OUT_RATE;
    /* ~10.7 ms periods, ~85 ms buffer: data only moves while the player is
     * inside an ALSA call, so the slave must bridge the player's sleeps */
    snd_pcm_uframes_t period = 2048, buffer = 16384;

    if ((err = snd_pcm_hw_params_any(d->slave, hp)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_access(d->slave, hp, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_format(d->slave, hp, SND_PCM_FORMAT_S32_LE)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_channels(d->slave, hp, CH)) < 0) return err;
    if ((err = snd_pcm_hw_params_set_rate(d->slave, hp, rate, 0)) < 0) return err;
    snd_pcm_hw_params_set_period_size_near(d->slave, hp, &period, NULL);
    snd_pcm_hw_params_set_buffer_size_near(d->slave, hp, &buffer);
    if ((err = snd_pcm_hw_params(d->slave, hp)) < 0) return err;
    snd_pcm_hw_params_get_period_size(hp, &d->s_period, NULL);
    snd_pcm_hw_params_get_buffer_size(hp, &d->s_buffer);

    snd_pcm_sw_params_current(d->slave, sp);
    snd_pcm_sw_params_set_start_threshold(d->slave, sp, d->s_buffer / 2);
    snd_pcm_sw_params_set_avail_min(d->slave, sp, d->s_period);
    if ((err = snd_pcm_sw_params(d->slave, sp)) < 0) return err;
    d->slave_ready = 1;
    return 0;
}

/* ------------------------------------------------------------ conversion */

/* PCM frames from the ring (any supported format) -> S32 interleaved */
static void pcm_to_s32(dfx_t *d, const uint8_t *src, int32_t *dst, size_t frames)
{
    size_t n = frames * CH;
    switch (d->fmt) {
    case SND_PCM_FORMAT_S16_LE: {
        const int16_t *s = (const int16_t *)src;
        for (size_t i = 0; i < n; i++) dst[i] = (int32_t)((uint32_t)(int32_t)s[i] << 16);
        break; }
    case SND_PCM_FORMAT_S24_LE: {             /* 24 bits in the low part of 32 */
        const int32_t *s = (const int32_t *)src;
        for (size_t i = 0; i < n; i++) dst[i] = (int32_t)((uint32_t)s[i] << 8);
        break; }
    case SND_PCM_FORMAT_S24_3LE:
        for (size_t i = 0; i < n; i++, src += 3)
            dst[i] = (int32_t)(((uint32_t)src[0] << 8) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 24));
        break;
    case SND_PCM_FORMAT_FLOAT_LE: {
        const float *s = (const float *)src;
        for (size_t i = 0; i < n; i++) {
            float v = s[i] * 2147483648.0f;
            dst[i] = v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (int32_t)lrintf(v);
        }
        break; }
    default:                                  /* S32_LE */
        memcpy(dst, src, n * sizeof(int32_t));
    }
}

/* DSD frames -> one byte stream per channel, oldest bit first (MSB) */
static void dsd_split(dfx_t *d, const uint8_t *src, size_t frames)
{
    const unsigned b = d->dsd_bpc;
    uint8_t *o0 = d->dbytes[0], *o1 = d->dbytes[1];
    for (size_t f = 0; f < frames; f++, src += 2 * b) {
        for (unsigned k = 0; k < b; k++) {
            unsigned j = d->dsd_rev ? b - 1 - k : k;
            *o0++ = src[j];
            *o1++ = src[b + j];
        }
    }
}

/* put soxr output for `in` frames into staging */
static void run_soxr(dfx_t *d, const void *in, size_t n)
{
    size_t idone = 0, odone = 0;
    size_t room = ST_CAP - d->st_len;
    soxr_process(d->sx, in, n, &idone, d->st + d->st_len * CH, room, &odone);
    d->st_len += odone;
}

/* convert up to one chunk from the ring into the (empty) staging buffer */
static void convert_chunk(dfx_t *d)
{
    snd_pcm_ioplug_t *io = &d->io;
    snd_pcm_uframes_t fill = (snd_pcm_uframes_t)(d->wpos - d->hw);
    snd_pcm_uframes_t lim = io->buffer_size - 1 - d->since_ptr;   /* see pointer() */
    if (fill > lim) fill = lim;
    if (!fill) return;

    snd_pcm_uframes_t max = d->mode == M_DSD ? DSD_CHUNK / d->dsd_bpc : PCM_CHUNK;
    snd_pcm_uframes_t off = (snd_pcm_uframes_t)(d->hw % io->buffer_size);
    snd_pcm_uframes_t n = fill < max ? fill : max;
    if (off + n > io->buffer_size) n = io->buffer_size - off;      /* no wrap inside */
    const uint8_t *src = d->ring + (size_t)off * d->fbytes;

    switch (d->mode) {
    case M_PASS:
        pcm_to_s32(d, src, d->st + d->st_len * CH, n);
        d->st_len += n;
        break;
    case M_PCM:
        pcm_to_s32(d, src, d->pin, n);
        run_soxr(d, d->pin, n);
        break;
    case M_DSD: {
        dsd_split(d, src, n);
        size_t bytes = (size_t)n * d->dsd_bpc, np = 0;
        for (int c = 0; c < CH; c++)
            np = dsd2pcm_run(d->dd[c], d->dbytes[c], bytes, d->dpcm + c, CH);
        run_soxr(d, d->dpcm, np);
        break; }
    }
    d->hw += n;
    d->since_ptr += n;
}

static int slave_recover(dfx_t *d, int err)
{
    if (err == -EPIPE || err == -ESTRPIPE || err == -EBADFD) {
        int e = snd_pcm_prepare(d->slave);
        return e < 0 ? e : 0;
    }
    return err;
}

/* move data: staging -> slave, ring -> staging. Never blocks. */
static void pump(dfx_t *d)
{
    if (!d->running || !d->st) return;
    for (int guard = 0; guard < 256; guard++) {
        while (d->st_pos < d->st_len) {
            snd_pcm_sframes_t av = snd_pcm_avail_update(d->slave);
            if (av < 0) { if (slave_recover(d, (int)av) < 0) return; continue; }
            if (av == 0) return;
            snd_pcm_uframes_t n = d->st_len - d->st_pos;
            if ((snd_pcm_uframes_t)av < n) n = av;
            snd_pcm_sframes_t w = snd_pcm_writei(d->slave, d->st + d->st_pos * CH, n);
            if (w == -EAGAIN) return;
            if (w < 0) { if (slave_recover(d, (int)w) < 0) return; continue; }
            d->st_pos += w;
            if (w == 0) return;
        }
        d->st_len = d->st_pos = 0;
        uint64_t before = d->hw;
        convert_chunk(d);
        if (d->hw == before && !d->st_len) return;   /* nothing left to do */
    }
}

/* ------------------------------------------------------------ callbacks */

static int dfx_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
    dfx_t *d = io->private_data;
    (void)params;
    int err = setup_slave(d);
    if (err < 0) { SNDERR("digifox: slave %s: %s", d->slave_name, snd_strerror(err)); return err; }

    free_conv(d);
    d->fmt = io->format;
    d->fbytes = (unsigned)(snd_pcm_format_physical_width(io->format) / 8 * CH);
    if (io->format == SND_PCM_FORMAT_S24_3LE) d->fbytes = 3 * CH;
    d->ring = malloc((size_t)io->buffer_size * d->fbytes);
    d->st = malloc(sizeof(int32_t) * CH * ST_CAP);
    if (!d->ring || !d->st) { free_conv(d); return -ENOMEM; }

    soxr_error_t serr = NULL;
    soxr_runtime_spec_t rt = soxr_runtime_spec(1);
    int bits = dsd_bits(io->format);
    if (bits) {
        unsigned long bitrate = (unsigned long)io->rate * bits;
        int base = bitrate % 44100 == 0 ? 44100 : bitrate % 48000 == 0 ? 48000 : 0;
        int mult = base ? (int)(bitrate / base) : 0;
        if (!base || (mult != 64 && mult != 128 && mult != 256)) {
            SNDERR("digifox: DSD %lu Hz is not supported (DSD64..DSD256)", bitrate);
            free_conv(d);
            return -EINVAL;
        }
        d->mode = M_DSD;
        d->dsd_bpc = bits / 8;
        d->dsd_rev = io->format == SND_PCM_FORMAT_DSD_U16_LE || io->format == SND_PCM_FORMAT_DSD_U32_LE;
        for (int c = 0; c < CH; c++) {
            d->dd[c] = dsd2pcm_new_base(base, mult);
            d->dbytes[c] = malloc(DSD_CHUNK);
            if (!d->dd[c] || !d->dbytes[c]) { free_conv(d); return -ENOMEM; }
        }
        d->dpcm = malloc(sizeof(float) * CH * (DSD_CHUNK * 2 + 64));
        if (!d->dpcm) { free_conv(d); return -ENOMEM; }
        unsigned mid = dsd2pcm_out_rate(d->dd[0]);
        soxr_io_spec_t ios = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT32_I);
        soxr_quality_spec_t q = soxr_quality_spec(d->quality == Q_VHQ ? SOXR_VHQ : SOXR_HQ, 0);
        d->sx = soxr_create(mid, OUT_RATE, CH, &serr, &ios, &q, &rt);
    } else if (io->rate == OUT_RATE) {
        d->mode = M_PASS;
    } else {
        d->mode = M_PCM;
        d->pin = malloc(sizeof(int32_t) * CH * PCM_CHUNK);
        if (!d->pin) { free_conv(d); return -ENOMEM; }
        int vhq = d->quality == Q_VHQ || (d->quality == Q_AUTO && io->rate <= 384000);
        soxr_io_spec_t ios = soxr_io_spec(SOXR_INT32_I, SOXR_INT32_I);
        soxr_quality_spec_t q = soxr_quality_spec(vhq ? SOXR_VHQ : SOXR_HQ, 0);
        d->sx = soxr_create(io->rate, OUT_RATE, CH, &serr, &ios, &q, &rt);
    }
    if ((d->mode == M_PCM || d->mode == M_DSD) && (!d->sx || serr)) {
        SNDERR("digifox: soxr: %s", serr ? serr : "?");
        free_conv(d);
        return -EINVAL;
    }
    d->ratio = (double)io->rate / OUT_RATE;
    char in[48];
    if (d->mode == M_DSD) {
        unsigned long bitrate = (unsigned long)io->rate * dsd_bits(io->format);
        snprintf(in, sizeof in, "DSD%lu", bitrate % 44100 == 0 ? bitrate / 44100 : bitrate / 48000);
    } else {
        snprintf(in, sizeof in, "%u %s", io->rate, snd_pcm_format_name(io->format));
    }
    report_in(in);
    return 0;
}

static int dfx_hw_free(snd_pcm_ioplug_t *io)
{
    free_conv(io->private_data);
    return 0;
}

#ifndef DFX_POS0          /* tests start near 2^32 to check the wrap */
#define DFX_POS0 0
#endif

static void reset_stream(dfx_t *d)
{
    d->wpos = d->hw = DFX_POS0;
    d->since_ptr = 0;
    d->st_len = d->st_pos = 0;
    if (d->sx) soxr_clear(d->sx);
    for (int c = 0; c < CH; c++) if (d->dd[c]) dsd2pcm_reset(d->dd[c]);
}

static int dfx_prepare(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    d->running = 0;
    reset_stream(d);
    snd_pcm_drop(d->slave);
    return snd_pcm_prepare(d->slave);
}

static int dfx_start(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    d->running = 1;
    pump(d);
    return 0;
}

static int dfx_stop(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    d->running = 0;
    snd_pcm_drop(d->slave);
    return 0;
}

static int dfx_pause(snd_pcm_ioplug_t *io, int enable)
{
    dfx_t *d = io->private_data;
    if (enable) {
        d->running = 0;
        if (snd_pcm_pause(d->slave, 1) < 0) snd_pcm_drop(d->slave);
    } else {
        if (snd_pcm_state(d->slave) == SND_PCM_STATE_PAUSED) snd_pcm_pause(d->slave, 0);
        else snd_pcm_prepare(d->slave);
        d->running = 1;
        pump(d);
    }
    return 0;
}

static snd_pcm_sframes_t dfx_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas,
                                      snd_pcm_uframes_t offset, snd_pcm_uframes_t size)
{
    dfx_t *d = io->private_data;
    const uint8_t *src = (const uint8_t *)areas[0].addr + (areas[0].first + areas[0].step * offset) / 8;
    snd_pcm_uframes_t left = size;
    /* rewind/forward/reset are not supported: never let the writer pass the
     * reader, drop what does not fit (cannot happen with normal writes) */
    snd_pcm_uframes_t room = io->buffer_size - (snd_pcm_uframes_t)(d->wpos - d->hw);
    if (left > room) left = room;
    while (left) {
        snd_pcm_uframes_t off = (snd_pcm_uframes_t)(d->wpos % io->buffer_size);
        snd_pcm_uframes_t n = io->buffer_size - off;
        if (n > left) n = left;
        memcpy(d->ring + (size_t)off * d->fbytes, src, (size_t)n * d->fbytes);
        src += (size_t)n * d->fbytes;
        d->wpos += n;
        left -= n;
    }
    pump(d);
    return size;
}

/* The ioplug core sees a jump of a full buffer as no movement, so between
 * two pointer calls fewer than buffer_size frames may be consumed
 * (convert_chunk honours that via since_ptr). */
static snd_pcm_sframes_t dfx_pointer(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    pump(d);
    d->since_ptr = 0;
    return (snd_pcm_sframes_t)(d->hw % io->buffer_size);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Blocking even for non-blocking players (MPD): at most the ring + slave
 * buffer, i.e. well under a second, and bounded by time in any case. */
static int dfx_drain(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    if (!d->running) d->running = 1;
    double limit = now_s() + 2.0 + (double)io->buffer_size / io->rate;
    /* everything the player wrote, through the converters, into the slave */
    while (now_s() < limit) {
        d->since_ptr = 0;            /* nobody reads the pointer meanwhile */
        pump(d);
        if (d->hw == d->wpos && d->st_pos >= d->st_len) break;
        if (snd_pcm_state(d->slave) == SND_PCM_STATE_PREPARED) snd_pcm_start(d->slave);
        snd_pcm_wait(d->slave, 100);
    }
    /* soxr tail */
    if (d->sx) {
        d->st_len = d->st_pos = 0;
        size_t odone = 0;
        soxr_process(d->sx, NULL, 0, NULL, d->st, ST_CAP, &odone);
        d->st_len = odone;
        while (d->st_pos < d->st_len && now_s() < limit) {
            pump(d);
            if (d->st_pos >= d->st_len) break;
            if (snd_pcm_state(d->slave) == SND_PCM_STATE_PREPARED) snd_pcm_start(d->slave);
            snd_pcm_wait(d->slave, 100);
        }
    }
    if (snd_pcm_state(d->slave) == SND_PCM_STATE_PREPARED) snd_pcm_start(d->slave);
    snd_pcm_nonblock(d->slave, 0);
    snd_pcm_drain(d->slave);
    snd_pcm_nonblock(d->slave, 1);
    d->running = 0;
    return 0;
}

static int dfx_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
    dfx_t *d = io->private_data;
    snd_pcm_sframes_t sd = 0;
    if (snd_pcm_delay(d->slave, &sd) < 0 || sd < 0) sd = 0;
    double out = (double)sd + (double)(d->st_len - d->st_pos);
    if (d->sx) out += soxr_delay(d->sx);
    *delayp = (snd_pcm_sframes_t)(d->wpos - d->hw) + (snd_pcm_sframes_t)(out * d->ratio + 0.5);
    return 0;
}

static int dfx_poll_count(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    return snd_pcm_poll_descriptors_count(d->slave);
}

static int dfx_poll_desc(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space)
{
    dfx_t *d = io->private_data;
    return snd_pcm_poll_descriptors(d->slave, pfd, space);
}

static int dfx_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds,
                            unsigned short *revents)
{
    dfx_t *d = io->private_data;
    unsigned short r = 0;
    snd_pcm_poll_descriptors_revents(d->slave, pfd, nfds, &r);
    pump(d);
    snd_pcm_uframes_t free_ = io->buffer_size - (snd_pcm_uframes_t)(d->wpos - d->hw);
    unsigned short ev = free_ >= io->period_size ? POLLOUT : 0;
    /* a slave that is broken for good must not leave the player spinning */
    if (r & (POLLERR | POLLNVAL)) {
        snd_pcm_state_t st = snd_pcm_state(d->slave);
        if (st == SND_PCM_STATE_DISCONNECTED || st == SND_PCM_STATE_OPEN)
            ev |= POLLERR;
    }
    *revents = ev;
    return 0;
}

static int dfx_close(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    free_conv(d);
    if (d->slave) snd_pcm_close(d->slave);
    free(d->slave_name);
    free(d);
    return 0;
}

static const snd_pcm_ioplug_callback_t dfx_cb = {
    .start = dfx_start,
    .stop = dfx_stop,
    .pointer = dfx_pointer,
    .transfer = dfx_transfer,
    .close = dfx_close,
    .hw_params = dfx_hw_params,
    .hw_free = dfx_hw_free,
    .prepare = dfx_prepare,
    .drain = dfx_drain,
    .pause = dfx_pause,
    .delay = dfx_delay,
    .poll_descriptors_count = dfx_poll_count,
    .poll_descriptors = dfx_poll_desc,
    .poll_revents = dfx_poll_revents,
};

/* ------------------------------------------------------------ open */

static const unsigned int acc_list[] = {
    SND_PCM_ACCESS_RW_INTERLEAVED, SND_PCM_ACCESS_MMAP_INTERLEAVED,
};
static const unsigned int fmt_list[] = {
    SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_FLOAT_LE,
    SND_PCM_FORMAT_DSD_U8, SND_PCM_FORMAT_DSD_U16_LE, SND_PCM_FORMAT_DSD_U16_BE,
    SND_PCM_FORMAT_DSD_U32_LE, SND_PCM_FORMAT_DSD_U32_BE,
};
/* PCM 44.1 … 768 kHz; DSD frame rates fall into the same list
 * (DSD64 = 352800 U8 / 176400 U16 / 88200 U32, … DSD256 = 1411200 U8) */
static const unsigned int rate_list[] = {
    44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000,
    705600, 768000, 1411200, 1536000,
};

SND_PCM_PLUGIN_DEFINE_FUNC(digifox)
{
    snd_config_iterator_t i, next;
    const char *slave = "hw:0,0", *quality = "auto";
    int err;

    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0) continue;
        if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint")) continue;
        if (!strcmp(id, "slave")) {
            if (snd_config_get_string(n, &slave) < 0) { SNDERR("digifox: slave must be a string"); return -EINVAL; }
            continue;
        }
        if (!strcmp(id, "quality")) {
            if (snd_config_get_string(n, &quality) < 0) { SNDERR("digifox: quality must be a string"); return -EINVAL; }
            continue;
        }
        SNDERR("digifox: unknown field %s", id);
        return -EINVAL;
    }
    if (stream != SND_PCM_STREAM_PLAYBACK) {
        SNDERR("digifox: playback only");
        return -EINVAL;
    }

    dfx_t *d = calloc(1, sizeof *d);
    if (!d) return -ENOMEM;
    d->slave_name = strdup(slave);
    d->quality = !strcmp(quality, "hq") ? Q_HQ : !strcmp(quality, "vhq") ? Q_VHQ : Q_AUTO;
    err = snd_pcm_open(&d->slave, slave, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        SNDERR("digifox: cannot open %s: %s", slave, snd_strerror(err));
        free(d->slave_name); free(d);
        return err;
    }

    d->io.version = SND_PCM_IOPLUG_VERSION;
    d->io.name = "DigiFox SRC 192k";
    d->io.callback = &dfx_cb;
    d->io.private_data = d;
    d->io.mmap_rw = 0;
    d->io.poll_fd = -1;
    d->io.poll_events = POLLOUT;

    err = snd_pcm_ioplug_create(&d->io, name, stream, mode);
    if (err < 0) {
        snd_pcm_close(d->slave);
        free(d->slave_name); free(d);
        return err;
    }
    if ((err = snd_pcm_ioplug_set_param_list(&d->io, SND_PCM_IOPLUG_HW_ACCESS,
                    sizeof acc_list / sizeof acc_list[0], acc_list)) < 0 ||
        (err = snd_pcm_ioplug_set_param_list(&d->io, SND_PCM_IOPLUG_HW_FORMAT,
                    sizeof fmt_list / sizeof fmt_list[0], fmt_list)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(&d->io, SND_PCM_IOPLUG_HW_CHANNELS, CH, CH)) < 0 ||
        (err = snd_pcm_ioplug_set_param_list(&d->io, SND_PCM_IOPLUG_HW_RATE,
                    sizeof rate_list / sizeof rate_list[0], rate_list)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(&d->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 256, 1 << 20)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(&d->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1024, 4 << 20)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(&d->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0) {
        snd_pcm_ioplug_delete(&d->io);
        return err;
    }
    *pcmp = d->io.pcm;
    return 0;
}

SND_PCM_PLUGIN_SYMBOL(digifox);
