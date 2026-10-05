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
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sched.h>
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
#define ST_ROOM     12288           /* free staging needed to convert a chunk */
#define PCM_CHUNK   1024            /* input frames per PCM conversion step  */
#define DSD_CHUNK   2048            /* bytes per channel per DSD step        */
/* what the player sends, for the amplifier display (pfrate -> "@IN ...") */
#define IN_FILE     "/tmp/digifox_in"
/* filter settings (web page I2S): phase=lin|int|min rolloff=steep|std|slow gain=0|-3
 * re-read by the pump thread, so a change is heard within a second */
#define FILTER_FILE "/etc/digifox/srcfilter"

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
    uint64_t     played;            /* reported pointer: really played       */
    uint64_t     ptr_last;          /* played at the last pointer() call     */
    snd_pcm_uframes_t prebuf;       /* staged output before the first write  */
    snd_pcm_uframes_t since_ptr;    /* consumed since the last pointer call  */
    int          running;

    int32_t     *st;                /* staging, interleaved S32              */
    size_t       st_len, st_pos;
    int          final;             /* drain: write the last partial period  */

    soxr_t       sx;
    dsd2pcm_t   *dd[CH];
    int32_t     *pin;               /* PCM chunk as S32                      */
    uint8_t     *dbytes[CH];        /* DSD chunk, one byte stream per ch     */
    float       *dpcm;              /* DSD -> PCM (352.8/384k), interleaved  */
    double       ratio;             /* input frames per output frame         */
    unsigned     sx_in;             /* soxr input rate (PCM rate / DSD mid)  */
    int          sx_float;          /* soxr input is float (DSD)             */
    int          f_phase;           /* 0 linear, 1 intermediate, 2 minimum  */
    int          f_roll;            /* 0 standard, 1 steep, 2 slow           */
    int          f_gain3;           /* 1: −3 dB headroom                     */
    time_t       f_mtime;

    /* The pump thread keeps the I2S fed while the player is busy elsewhere
     * (Qobuz preparing the next track for gapless: tens of ms without a
     * single ALSA call). All state above is guarded by `lock`. */
    pthread_mutex_t lock;
    pthread_t    thr;
    int          thr_ok, quit;
    unsigned long writes;           /* successful slave writes (progress)    */
    unsigned long xruns, stalls;    /* slave recoveries, stall starts (log)  */

    /* The player waits on this eventfd, not on the slave: "room in our ring"
     * and "room in the I2S buffer" are different things, and waiting on the
     * slave made the player spin at real-time priority whenever the I2S had
     * room but the ring did not — starving the USB driver on the single core. */
    int          efd, ev_set;
    int          kfd;               /* eventfd: wake the pump thread         */
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
    /* ~5.3 ms periods, ~85 ms buffer: data only moves while the player is
     * inside an ALSA call, so the slave must bridge the player's sleeps;
     * short enough that the extra latency stays small */
    snd_pcm_uframes_t period = 1024, buffer = 16384;

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
    d->xruns++;
    if (err == -EPIPE || err == -ESTRPIPE || err == -EBADFD) {
        int e = snd_pcm_prepare(d->slave);
        return e < 0 ? e : 0;
    }
    return err;
}

/* staging -> slave. The RV1106 I2S (see uac2_router) starts its DMA on the
 * first write whatever start_threshold says, and handles only whole periods
 * well. So: write whole periods only, and the first write after prepare only
 * once half a slave buffer is staged. Returns 1 when the slave is full. */
static int flush(dfx_t *d)
{
    int full = 0;
    for (int guard = 0; guard < 64; guard++) {
        snd_pcm_uframes_t pending = d->st_len - d->st_pos;
        if (!pending) break;
        snd_pcm_state_t state = snd_pcm_state(d->slave);
        if (state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_SUSPENDED) {
            if (slave_recover(d, -EPIPE) < 0) { full = 1; break; }
            continue;
        }
        if (state == SND_PCM_STATE_PREPARED && !d->final && pending < d->prebuf) {
            /* pre-buffer first — unless the player cannot give more: its
             * buffer is (nearly) all in our staging. Then start now, with a
             * little silence in front to make whole periods. */
            snd_pcm_uframes_t used = (snd_pcm_uframes_t)(d->wpos - d->played);
            int stalled = d->wpos == d->hw &&
                          d->io.buffer_size - used < d->io.period_size;
            if (!stalled) break;
            d->stalls++;
            snd_pcm_uframes_t z = (d->s_period - pending % d->s_period) % d->s_period;
            if (z && d->st_len + z <= ST_CAP) {
                memmove(d->st + (d->st_pos + z) * CH, d->st + d->st_pos * CH, pending * CH * sizeof(int32_t));
                memset(d->st + d->st_pos * CH, 0, z * CH * sizeof(int32_t));
                d->st_len += z;
                pending += z;
            }
        }
        snd_pcm_sframes_t av = snd_pcm_avail_update(d->slave);
        if (av < 0) { if (slave_recover(d, (int)av) < 0) { full = 1; break; } continue; }
        snd_pcm_uframes_t n = pending < (snd_pcm_uframes_t)av ? pending : (snd_pcm_uframes_t)av;
        if (!d->final || n < pending) n -= n % d->s_period;
        if (!n) { full = pending >= d->s_period || d->final; break; }
        snd_pcm_sframes_t w = snd_pcm_writei(d->slave, d->st + d->st_pos * CH, n);
        if (w == -EAGAIN) { full = 1; break; }
        if (w < 0) { if (slave_recover(d, (int)w) < 0) { full = 1; break; } continue; }
        d->st_pos += w;
        d->writes++;
        /* pre-buffer written: start now (the RV1106 starts by itself on the
         * first write anyway; other cards wait for start_threshold) */
        if (snd_pcm_state(d->slave) == SND_PCM_STATE_PREPARED) snd_pcm_start(d->slave);
    }
    if (d->st_pos) {                                   /* keep the remainder in front */
        size_t left = d->st_len - d->st_pos;
        if (left) memmove(d->st, d->st + d->st_pos * CH, left * CH * sizeof(int32_t));
        d->st_len = left;
        d->st_pos = 0;
    }
    return full;
}

/* Frames of the player's stream that are really out of the I2S port by now.
 * Players (Qobuz) end playback when the pointer says everything has been
 * played, so data still waiting in staging, soxr or the slave buffer must not
 * count as played. Monotonic, never ahead of what was converted. */
static void update_played(dfx_t *d)
{
    snd_pcm_sframes_t sd = 0;
    if (snd_pcm_delay(d->slave, &sd) < 0 || sd < 0) sd = 0;
    /* soxr's own filter delay is not counted: it only comes out with more
     * input, and counting it could leave the player without room to write
     * while we wait for a full pre-buffer (deadlock) */
    double down = (double)sd + (double)(d->st_len - d->st_pos);
    uint64_t lag = (uint64_t)(down * d->ratio + 0.5);
    /* A player with a tiny buffer (USB router at 768 kHz: 11 ms) must still
     * be able to keep our ~85 ms queue full: count at most half its buffer
     * as "not yet played". Players with normal buffers see the full truth. */
    if (lag > d->io.buffer_size / 2) lag = d->io.buffer_size / 2;
    uint64_t p = d->hw - (lag < d->hw - d->played ? lag : d->hw - d->played);
    /* the ioplug core sees a jump of a whole buffer as no movement */
    uint64_t cap = d->ptr_last + d->io.buffer_size - 1;
    if (p > cap) p = cap;
    if (p > d->played) d->played = p;
}

/* eventfd readable <=> the player can write at least one period */
static void signal_ready(dfx_t *d)
{
    if (d->efd < 0) return;
    int ready = 1;
    if (d->io.buffer_size && d->st) {
        snd_pcm_uframes_t used = (snd_pcm_uframes_t)(d->wpos - d->played);
        ready = d->io.buffer_size - used >= d->io.period_size;
    }
    if (ready && !d->ev_set) {
        uint64_t one = 1;
        if (write(d->efd, &one, sizeof one) == sizeof one) d->ev_set = 1;
    } else if (!ready && d->ev_set) {
        uint64_t v;
        if (read(d->efd, &v, sizeof v) == sizeof v || errno == EAGAIN) d->ev_set = 0;
    }
}

static void pump_core(dfx_t *d);

/* move data and tell the player whether it may write */
static void pump(dfx_t *d)
{
    pump_core(d);
    if (d->running && d->st) update_played(d);
    signal_ready(d);
}

/* From the player's calls: never convert here. The USB router runs at
 * SCHED_FIFO 70, above the kernel's IRQ threads (50); converting in its
 * context kept the USB driver from taking packets (0.7 % of the audio lost).
 * Only wake the pump thread (FIFO 45, below the IRQ threads). */
static void poke(dfx_t *d)
{
    if (d->thr_ok && d->kfd >= 0) {
        uint64_t one = 1;
        if (write(d->kfd, &one, sizeof one) < 0) {}
        if (d->running && d->st) update_played(d);
        signal_ready(d);
    } else {
        pump(d);                     /* no thread: do it here */
    }
}

/* move data: staging -> slave, ring -> staging. Never blocks. */
static void pump_core(dfx_t *d)
{
    if (!d->running || !d->st) return;
    for (int guard = 0; guard < 256; guard++) {
        if (flush(d)) return;                          /* slave full */
        if (ST_CAP - d->st_len < ST_ROOM) return;      /* cannot happen, but be safe */
        if (d->st_len >= d->s_buffer) return;          /* enough staged          */
        uint64_t before = d->hw;
        convert_chunk(d);
        if (d->hw == before) return;                   /* ring empty */
    }
}

/* ------------------------------------------------------------ callbacks */

/* read FILTER_FILE; returns its mtime (0 — no file: defaults) */
static time_t read_filter(dfx_t *d)
{
    struct stat st;
    d->f_phase = 0; d->f_roll = 0; d->f_gain3 = 0;
    if (stat(FILTER_FILE, &st) < 0) return 0;
    FILE *f = fopen(FILTER_FILE, "r");
    if (!f) return 0;
    char l[64];
    while (fgets(l, sizeof l, f)) {
        if (!strncmp(l, "phase=", 6))
            d->f_phase = !strncmp(l + 6, "int", 3) ? 1 : !strncmp(l + 6, "min", 3) ? 2 : 0;
        else if (!strncmp(l, "rolloff=", 8))
            d->f_roll = !strncmp(l + 8, "steep", 5) ? 1 : !strncmp(l + 8, "slow", 4) ? 2 : 0;
        else if (!strncmp(l, "gain=", 5))
            d->f_gain3 = !strncmp(l + 5, "-3", 2);
    }
    fclose(f);
    return st.st_mtime ? st.st_mtime : 1;
}

/* (re)build the resampler for the current stream and filter settings */
static int make_soxr(dfx_t *d)
{
    if (d->sx) { soxr_delete(d->sx); d->sx = NULL; }
    if (d->mode != M_PCM && d->mode != M_DSD) return 0;
    int vhq = d->mode == M_PCM &&
              (d->quality == Q_VHQ || (d->quality == Q_AUTO && d->sx_in <= 384000));
    if (d->mode == M_DSD && d->quality == Q_VHQ) vhq = 1;
    unsigned long recipe = (vhq ? SOXR_VHQ : SOXR_HQ) |
        (d->f_phase == 1 ? SOXR_INTERMEDIATE_PHASE : d->f_phase == 2 ? SOXR_MINIMUM_PHASE : SOXR_LINEAR_PHASE) |
        (d->f_roll == 1 ? SOXR_STEEP_FILTER : 0);
    soxr_quality_spec_t q = soxr_quality_spec(recipe, 0);
    if (d->f_roll == 2) q.passband_end = 0.80;     /* gentle: 0 dB to 80 % of Nyquist */
    soxr_io_spec_t ios = soxr_io_spec(d->sx_float ? SOXR_FLOAT32_I : SOXR_INT32_I, SOXR_INT32_I);
    if (d->f_gain3) ios.scale = 0.70794578;        /* −3 dB */
    soxr_runtime_spec_t rt = soxr_runtime_spec(1);
    soxr_error_t serr = NULL;
    d->sx = soxr_create(d->sx_in, OUT_RATE, CH, &serr, &ios, &q, &rt);
    if (!d->sx || serr) {
        SNDERR("digifox: soxr: %s", serr ? serr : "?");
        if (d->sx) { soxr_delete(d->sx); d->sx = NULL; }
        return -EINVAL;
    }
    return 0;
}

static int dfx_hw_params_inner(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
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

    d->f_mtime = read_filter(d);
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
        d->sx_in = dsd2pcm_out_rate(d->dd[0]);
        d->sx_float = 1;
    } else if (io->rate == OUT_RATE && !d->f_gain3) {
        d->mode = M_PASS;
    } else {
        d->mode = M_PCM;
        d->pin = malloc(sizeof(int32_t) * CH * PCM_CHUNK);
        if (!d->pin) { free_conv(d); return -ENOMEM; }
        d->sx_in = io->rate;
        d->sx_float = 0;
    }
    /* 192 kHz with −3 dB: through soxr at 1:1, which only scales */
    if ((d->mode == M_PCM || d->mode == M_DSD) && make_soxr(d) < 0) {
        free_conv(d);
        return -EINVAL;
    }
    d->ratio = (double)io->rate / OUT_RATE;
    /* pre-buffer half a slave buffer (~43 ms). update_played() counts at most
     * half the player's buffer as queued, so even a player with a tiny buffer
     * can fill it; the stall check in flush() covers the rest. */
    d->prebuf = d->s_buffer / 2 - (d->s_buffer / 2) % d->s_period;
    if (!d->prebuf) d->prebuf = d->s_period;
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
    d->wpos = d->hw = d->played = d->ptr_last = DFX_POS0;
    d->since_ptr = 0;
    d->final = 0;
    d->st_len = d->st_pos = 0;
    if (d->sx) soxr_clear(d->sx);
    for (int c = 0; c < CH; c++) if (d->dd[c]) dsd2pcm_reset(d->dd[c]);
}

static int dfx_prepare(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    { uint64_t v; if (d->efd >= 0 && read(d->efd, &v, sizeof v) < 0) {} d->ev_set = 0; }
    d->running = 0;
    reset_stream(d);
    snd_pcm_drop(d->slave);
    int err = snd_pcm_prepare(d->slave);
    signal_ready(d);                 /* empty ring: the player may write */
    return err;
}

static int dfx_start(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    d->running = 1;
    poke(d);
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
        poke(d);
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
    poke(d);
    return size;
}

/* The ioplug core sees a jump of a full buffer as no movement, so between
 * two pointer calls fewer than buffer_size frames may be consumed
 * (convert_chunk honours that via since_ptr). */
static snd_pcm_sframes_t dfx_pointer(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    poke(d);
    if (d->running) update_played(d);
    d->ptr_last = d->played;
    d->since_ptr = 0;
    return (snd_pcm_sframes_t)(d->played % io->buffer_size);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Blocking even for non-blocking players (MPD): at most the ring + slave
 * buffer, i.e. well under a second, and bounded by time in any case. */
/* called WITHOUT the lock (see dfx_cb): waits must not block the pump thread */
static int dfx_drain(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    pthread_mutex_lock(&d->lock);
    if (!d->running) d->running = 1;
    double limit = now_s() + 2.0 + (double)io->buffer_size / io->rate;
    /* everything the player wrote, through the converters, into staging/slave */
    while (now_s() < limit) {
        d->since_ptr = 0;            /* nobody reads the pointer meanwhile */
        pump(d);
        if (d->hw == d->wpos) break;
        pthread_mutex_unlock(&d->lock);
        usleep(2000);
        pthread_mutex_lock(&d->lock);
    }
    /* soxr tail, then pad the last period with silence */
    if (d->sx && ST_CAP - d->st_len > 4096) {
        size_t odone = 0;
        soxr_process(d->sx, NULL, 0, NULL, d->st + d->st_len * CH, ST_CAP - d->st_len - 4096, &odone);
        d->st_len += odone;
    }
    size_t part = d->st_len % d->s_period;
    if (part && d->st_len + (d->s_period - part) <= ST_CAP) {
        memset(d->st + d->st_len * CH, 0, (d->s_period - part) * CH * sizeof(int32_t));
        d->st_len += d->s_period - part;
    }
    d->final = 1;
    while (d->st_pos < d->st_len && now_s() < limit) {
        if (!flush(d) && d->st_pos >= d->st_len) break;
        if (d->st_len == 0) break;
        pthread_mutex_unlock(&d->lock);
        usleep(2000);
        pthread_mutex_lock(&d->lock);
    }
    d->final = 0;
    d->running = 0;                  /* the thread leaves the slave alone now */
    if (snd_pcm_state(d->slave) == SND_PCM_STATE_PREPARED) snd_pcm_start(d->slave);
    snd_pcm_nonblock(d->slave, 0);
    snd_pcm_drain(d->slave);
    snd_pcm_nonblock(d->slave, 1);
    d->played = d->ptr_last = d->hw;
    pthread_mutex_unlock(&d->lock);
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

static int dfx_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds,
                            unsigned short *revents)
{
    dfx_t *d = io->private_data;
    (void)pfd; (void)nfds;
    poke(d);
    snd_pcm_uframes_t free_ = io->buffer_size - (snd_pcm_uframes_t)(d->wpos - d->played);
    unsigned short ev = free_ >= io->period_size ? POLLOUT : 0;
    /* a slave that is broken for good must not leave the player waiting */
    snd_pcm_state_t st = snd_pcm_state(d->slave);
    if (st == SND_PCM_STATE_DISCONNECTED || st == SND_PCM_STATE_OPEN) ev |= POLLERR;
    *revents = ev;
    return 0;
}

static int dfx_close(snd_pcm_ioplug_t *io)
{
    dfx_t *d = io->private_data;
    if (d->thr_ok) {
        pthread_mutex_lock(&d->lock);
        d->quit = 1;
        pthread_mutex_unlock(&d->lock);
        pthread_join(d->thr, NULL);
    }
    pthread_mutex_destroy(&d->lock);
    if (d->efd >= 0) close(d->efd);
    if (d->kfd >= 0) close(d->kfd);
    free_conv(d);
    if (d->slave) snd_pcm_close(d->slave);
    free(d->slave_name);
    free(d);
    return 0;
}

/* ------------------------------------------------------------ pump thread */

static void *pump_thread(void *arg)
{
    dfx_t *d = arg;
    /* real-time, but below the kernel's IRQ threads (FIFO 50): converting a
     * chunk must never hold up the USB driver */
    struct sched_param sp = { .sched_priority = 45 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    struct pollfd pfd[8];
    double next_log = 0, next_cfg = 0;
    for (;;) {
        pthread_mutex_lock(&d->lock);
        if (d->quit) { pthread_mutex_unlock(&d->lock); break; }
        /* touch /tmp/digifox_debug to get a state line every 0.2 s */
        double t = now_s();
        if (t >= next_log) {
            next_log = t + 0.2;
            if (access("/tmp/digifox_debug", F_OK) == 0) {
                FILE *f = fopen("/tmp/digifox_src.log", "a");
                if (f) {
                    snd_pcm_sframes_t sd = -1;
                    if (d->running) snd_pcm_delay(d->slave, &sd);
                    fprintf(f, "%.3f run=%d rate=%u ring=%lu st=%lu slave=%s delay=%ld xruns=%lu stalls=%lu ev=%d\n",
                            t, d->running, d->io.rate, (unsigned long)(d->wpos - d->hw),
                            (unsigned long)(d->st_len - d->st_pos), snd_pcm_state_name(snd_pcm_state(d->slave)),
                            (long)sd, d->xruns, d->stalls, d->ev_set);
                    fclose(f);
                }
            }
        }
        int nf = 0, active = d->running && d->st;
        /* filter settings changed on the web page: rebuild the resampler */
        if (d->st && (d->mode == M_PCM || d->mode == M_DSD) && t >= next_cfg) {
            next_cfg = t + 0.5;
            struct stat cs;
            time_t m = stat(FILTER_FILE, &cs) == 0 ? (cs.st_mtime ? cs.st_mtime : 1) : 0;
            if (m != d->f_mtime) {
                int g = d->f_gain3;
                d->f_mtime = read_filter(d);
                if (g == d->f_gain3 || d->mode != M_PASS) make_soxr(d);
            }
        }
        if (active) {
            pump(d);
            /* data waiting for room in the I2S buffer: also wake on the slave */
            int waiting = d->wpos != d->hw || d->st_len - d->st_pos >= d->s_period;
            if (waiting && snd_pcm_state(d->slave) == SND_PCM_STATE_RUNNING)
                nf = snd_pcm_poll_descriptors(d->slave, pfd, 7);
            if (nf < 0) nf = 0;
        }
        pthread_mutex_unlock(&d->lock);
        /* sleep until the player writes (kick), the I2S has room, or 20 ms */
        pfd[nf].fd = d->kfd;
        pfd[nf].events = POLLIN;
        pfd[nf].revents = 0;
        poll(pfd, nf + 1, active ? 20 : 50);
        uint64_t v;
        if (read(d->kfd, &v, sizeof v) < 0) {}
    }
    return NULL;
}

#define LOCKED(ret, name, params, args)                 \
    static ret name##_l params                          \
    {                                                   \
        dfx_t *d_ = io->private_data;                   \
        pthread_mutex_lock(&d_->lock);                  \
        ret r_ = name args;                             \
        pthread_mutex_unlock(&d_->lock);                \
        return r_;                                      \
    }
LOCKED(int, dfx_start, (snd_pcm_ioplug_t *io), (io))
LOCKED(int, dfx_stop, (snd_pcm_ioplug_t *io), (io))
LOCKED(snd_pcm_sframes_t, dfx_pointer, (snd_pcm_ioplug_t *io), (io))
LOCKED(snd_pcm_sframes_t, dfx_transfer, (snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *a,
       snd_pcm_uframes_t o, snd_pcm_uframes_t n), (io, a, o, n))
LOCKED(int, dfx_hw_params_inner, (snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *p), (io, p))

/* Building the filters (soxr, DSD decimator) takes tens of ms on the Fox.
 * The USB router calls this at SCHED_FIFO 70 — above the USB driver — so
 * drop to normal priority for the setup and restore afterwards. */
static int dfx_hw_params_l(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *p)
{
    int pol;
    struct sched_param sp, normal = { .sched_priority = 0 };
    int rt = pthread_getschedparam(pthread_self(), &pol, &sp) == 0 && pol != SCHED_OTHER;
    if (rt) pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
    int r = dfx_hw_params_inner_l(io, p);
    if (rt) pthread_setschedparam(pthread_self(), pol, &sp);
    return r;
}
LOCKED(int, dfx_hw_free, (snd_pcm_ioplug_t *io), (io))
LOCKED(int, dfx_prepare, (snd_pcm_ioplug_t *io), (io))
LOCKED(int, dfx_pause, (snd_pcm_ioplug_t *io, int e), (io, e))
LOCKED(int, dfx_delay, (snd_pcm_ioplug_t *io, snd_pcm_sframes_t *dp), (io, dp))
LOCKED(int, dfx_poll_revents, (snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int n,
       unsigned short *rv), (io, pfd, n, rv))

static const snd_pcm_ioplug_callback_t dfx_cb = {
    .start = dfx_start_l,
    .stop = dfx_stop_l,
    .pointer = dfx_pointer_l,
    .transfer = dfx_transfer_l,
    .close = dfx_close,
    .hw_params = dfx_hw_params_l,
    .hw_free = dfx_hw_free_l,
    .prepare = dfx_prepare_l,
    .drain = dfx_drain,
    .pause = dfx_pause_l,
    .delay = dfx_delay_l,
    .poll_revents = dfx_poll_revents_l,
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

    d->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    d->kfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    d->ev_set = 0;
    pthread_mutex_init(&d->lock, NULL);
    d->thr_ok = pthread_create(&d->thr, NULL, pump_thread, d) == 0;

    d->io.version = SND_PCM_IOPLUG_VERSION;
    d->io.name = "DigiFox SRC 192k";
    d->io.callback = &dfx_cb;
    d->io.private_data = d;
    d->io.mmap_rw = 0;
    d->io.poll_fd = d->efd;
    d->io.poll_events = POLLIN;

    err = snd_pcm_ioplug_create(&d->io, name, stream, mode);
    if (err < 0) {
        dfx_close(&d->io);                     /* stops the thread, frees all */
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
