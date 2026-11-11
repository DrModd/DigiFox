/*
 * pa_shim.c — libpulse.so.0 for Spotify Soloist on DigiFox: the 48 PulseAudio calls
 * Soloist makes, done straight on ALSA (no PulseAudio daemon, nothing in between).
 *
 * Soloist plays only through PipeWire or PulseAudio; DigiFox has plain ALSA. The
 * shim is loaded only by Soloist (LD_LIBRARY_PATH=/usr/lib/digifox/soloist in
 * S95spotify), the rest of the system never sees it.
 *
 * How it maps:
 *   threaded mainloop   one thread + one mutex/condvar; it runs the queued callbacks
 *                       (state changes, operation results) with the lock held, like
 *                       PulseAudio, feeds ALSA and wakes waiters every 5 ms
 *   context             always "ready" (connecting → authorizing → setting name → ready)
 *   stream              ALSA PCM "plug:default" in the stream's own sample format,
 *                       non-blocking; what ALSA can't take yet waits in a small queue
 *   volume              remembered and reported back, the samples are NOT scaled:
 *                       the Fox output stays bit-perfect, volume is the amplifier's job
 *   timing info         from snd_pcm_delay(): written bytes, played bytes, latency
 *
 * DIGIFOX_PA_DEV overrides the ALSA device, DIGIFOX_PA_DEBUG=1 logs to stderr.
 */
#define _GNU_SOURCE
#include <pulse/pulseaudio.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define T() D("> %s", __func__)

/* DIGIFOX_PA_DEBUG=1: log to stderr; DIGIFOX_PA_LOG=<file>: also to a file, each line
   written through (O_SYNC) — survives a hang or a watchdog reboot */
static int dbg = -1, dbg_fd = -1;
static int detail;                  /* lines left to log in detail (after an uncork) */
static void D(const char *f, ...)
{
    if (dbg < 0) {
        const char *e = getenv("DIGIFOX_PA_DEBUG"), *l = getenv("DIGIFOX_PA_LOG");
        dbg = e && *e == '1';
        if (l && *l) { dbg_fd = open(l, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644); if (dbg_fd >= 0) dbg = 1; }
    }
    if (!dbg) return;
    char buf[256];
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    int n = snprintf(buf, sizeof buf, "pa_shim %ld.%03ld: ", (long)t.tv_sec, t.tv_nsec / 1000000);
    va_list ap; va_start(ap, f);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 1, f, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (dbg_fd >= 0) { ssize_t w = write(dbg_fd, buf, (size_t)n); (void)w; }
    else { ssize_t w = write(2, buf, (size_t)n); (void)w; }
}

/* ------------------------------------------------------------------ jobs */
typedef void (*job_fn)(void *a, void *b, void *c, intptr_t i);
struct job { struct job *next; job_fn fn; void *a, *b, *c; intptr_t i; uint64_t due; };

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

struct pa_threaded_mainloop {
    pa_mainloop_api api;            /* first: pa_context_new() gets &api, api.userdata = this */
    pthread_mutex_t mtx;
    pthread_cond_t  cond;           /* pa_threaded_mainloop_wait / signal */
    pthread_cond_t  wake;           /* loop thread: new job or stop */
    pthread_t th;
    int running, quit;
    struct job *head, *tail;
    struct pa_stream *streams;
};

/* delay_ms > 0: run not before that — like a reply from a real PulseAudio server.
   Soloist asks for timing info again as soon as the previous answer arrives: answered
   at once, that was a busy loop (its threads are real-time: the Fox watchdog rebooted it) */
static void post_at(pa_threaded_mainloop *m, unsigned delay_ms, job_fn fn, void *a, void *b, void *c, intptr_t i)
{
    struct job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->fn = fn; j->a = a; j->b = b; j->c = c; j->i = i;
    j->due = delay_ms ? now_ms() + delay_ms : 0;
    if (m->tail) m->tail->next = j; else m->head = j;
    m->tail = j;
    pthread_cond_signal(&m->wake);
}
static void post(pa_threaded_mainloop *m, job_fn fn, void *a, void *b, void *c, intptr_t i)
{
    post_at(m, 0, fn, a, b, c, i);
}

/* ------------------------------------------------------------- objects */
struct pa_operation {
    int refs;
    pa_operation_state_t st;
};

struct pa_proplist { int dummy; };

struct pa_context {
    int refs;
    pa_threaded_mainloop *m;
    pa_context_state_t st;
    pa_context_notify_cb_t state_cb; void *state_ud;
    pa_context_subscribe_cb_t sub_cb; void *sub_ud;
    int err;
};

struct pa_stream {
    int refs;
    struct pa_stream *next;         /* mainloop list (connected streams) */
    pa_context *ctx;
    pa_stream_state_t st;
    pa_sample_spec ss;
    pa_channel_map map;
    uint32_t idx;
    pa_stream_notify_cb_t state_cb; void *state_ud;
    pa_stream_notify_cb_t start_cb; void *start_ud;
    pa_stream_notify_cb_t under_cb; void *under_ud;
    snd_pcm_t *pcm;
    size_t frame;                   /* bytes per frame */
    snd_pcm_uframes_t bufsize;
    size_t tlen;                    /* bytes the "server" holds: ALSA buffer + queue (tlength) */
    pa_usec_t latency;
    int corked, paused_hw, started;
    int64_t written;                /* bytes accepted from the client */
    uint8_t *q; size_t qoff, qlen, qcap;  /* accepted, not yet in ALSA: q[qoff .. qoff+qlen) */
    pa_cvolume vol;
    pa_timing_info ti;
};

static uint32_t next_idx = 1;

static pa_operation *op_new(void)
{
    pa_operation *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->refs = 2;                    /* the caller's + the queued job's */
    o->st = PA_OPERATION_RUNNING;
    return o;
}
static void op_done(pa_threaded_mainloop *m, pa_operation *o)
{
    o->st = PA_OPERATION_DONE;
    pthread_cond_broadcast(&m->cond);
    if (--o->refs == 0) free(o);
}

/* ------------------------------------------------------------- mainloop */
static void stream_service(pa_stream *s);

static void *loop(void *arg)
{
    pa_threaded_mainloop *m = arg;
    pthread_mutex_lock(&m->mtx);
    while (!m->quit) {
        /* run the due jobs; callbacks may post new ones (they go after the kept ones) */
        struct job *list = m->head, *keep = NULL, **kt = &keep;
        m->head = m->tail = NULL;
        uint64_t now = now_ms();
        while (list) {
            struct job *j = list;
            list = j->next; j->next = NULL;
            if (j->due > now) { *kt = j; kt = &j->next; continue; }
            j->fn(j->a, j->b, j->c, j->i);
            free(j);
        }
        if (keep) {
            *kt = m->head;
            if (!m->head) m->tail = (struct job *)((char *)kt - offsetof(struct job, next));
            m->head = keep;
        }
        for (pa_stream *s = m->streams; s; s = s->next) stream_service(s);
        /* clients poll writable_size between waits: wake them regularly */
        pthread_cond_broadcast(&m->cond);
        /* sleep 5 ms (less if a delayed job is due sooner); new jobs wake at once */
        int due_now = 0;
        for (struct job *j = m->head; j; j = j->next) if (j->due <= now_ms()) { due_now = 1; break; }
        if (due_now) continue;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        if (!m->quit) pthread_cond_timedwait(&m->wake, &m->mtx, &ts);
    }
    pthread_mutex_unlock(&m->mtx);
    return NULL;
}

EXPORT pa_threaded_mainloop *pa_threaded_mainloop_new(void)
{
    T();
    pa_threaded_mainloop *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mtx, &a);
    pthread_mutexattr_destroy(&a);
    pthread_cond_init(&m->cond, NULL);
    pthread_cond_init(&m->wake, NULL);
    m->api.userdata = m;
    D("mainloop_new");
    return m;
}
EXPORT pa_mainloop_api *pa_threaded_mainloop_get_api(pa_threaded_mainloop *m) { return &m->api; }
EXPORT int pa_threaded_mainloop_start(pa_threaded_mainloop *m)
{
    T();
    m->quit = 0;
    if (pthread_create(&m->th, NULL, loop, m)) return -1;
    m->running = 1;
    return 0;
}
EXPORT void pa_threaded_mainloop_stop(pa_threaded_mainloop *m)
{
    T();
    if (!m->running) return;
    pthread_mutex_lock(&m->mtx);
    m->quit = 1;
    pthread_cond_signal(&m->wake);
    pthread_mutex_unlock(&m->mtx);
    pthread_join(m->th, NULL);
    m->running = 0;
}
EXPORT void pa_threaded_mainloop_free(pa_threaded_mainloop *m)
{
    T();
    if (!m) return;
    pa_threaded_mainloop_stop(m);
    while (m->head) { struct job *j = m->head; m->head = j->next; free(j); }
    pthread_mutex_destroy(&m->mtx);
    pthread_cond_destroy(&m->cond);
    pthread_cond_destroy(&m->wake);
    free(m);
}
EXPORT void pa_threaded_mainloop_lock(pa_threaded_mainloop *m)   { pthread_mutex_lock(&m->mtx); }
EXPORT void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) { pthread_mutex_unlock(&m->mtx); }
EXPORT void pa_threaded_mainloop_wait(pa_threaded_mainloop *m)   { pthread_cond_wait(&m->cond, &m->mtx); }
EXPORT void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_accept)
{
    (void)wait_for_accept;
    pthread_cond_broadcast(&m->cond);
}

/* -------------------------------------------------------------- context */
static void ctx_unref(pa_context *c) { if (c && --c->refs == 0) free(c); }

static void job_ctx_state(void *a, void *b, void *c_, intptr_t st)
{
    pa_context *c = a; (void)b; (void)c_;
    c->st = (pa_context_state_t)st;
    if (c->state_cb) c->state_cb(c, c->state_ud);
    ctx_unref(c);
}
static void ctx_state(pa_context *c, pa_context_state_t st)
{
    c->refs++;
    post(c->m, job_ctx_state, c, NULL, NULL, st);
}

EXPORT pa_context *pa_context_new(pa_mainloop_api *api, const char *name)
{
    T();
    pa_context *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->refs = 1;
    c->m = api->userdata;
    c->st = PA_CONTEXT_UNCONNECTED;
    D("context_new %s", name ? name : "");
    return c;
}
EXPORT void pa_context_unref(pa_context *c) { ctx_unref(c); }
EXPORT void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *ud)
{ c->state_cb = cb; c->state_ud = ud; }
EXPORT int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags, const pa_spawn_api *api)
{
    T();
    (void)server; (void)flags; (void)api;
    /* PulseAudio: connecting already inside pa_context_connect (callers check
       PA_CONTEXT_IS_GOOD right after it), the rest from the mainloop */
    c->st = PA_CONTEXT_CONNECTING;
    if (c->state_cb) c->state_cb(c, c->state_ud);
    ctx_state(c, PA_CONTEXT_AUTHORIZING);
    ctx_state(c, PA_CONTEXT_SETTING_NAME);
    ctx_state(c, PA_CONTEXT_READY);
    return 0;
}
EXPORT void pa_context_disconnect(pa_context *c)
{
    T();
    if (c->st == PA_CONTEXT_TERMINATED) return;
    c->st = PA_CONTEXT_TERMINATED;
    if (c->state_cb) c->state_cb(c, c->state_ud);
}
EXPORT pa_context_state_t pa_context_get_state(const pa_context *c) { return c->st; }
EXPORT int pa_context_errno(const pa_context *c) { D("> pa_context_errno %d", c->err); return c->err; }

static void job_success(void *a, void *b, void *c_, intptr_t i)
{
    pa_context *c = a; pa_operation *o = b;
    pa_context_success_cb_t cb = (pa_context_success_cb_t)(uintptr_t)i;
    if (cb) cb(c, 1, c_);
    op_done(c->m, o);
    ctx_unref(c);
}
static pa_operation *ctx_success(pa_context *c, pa_context_success_cb_t cb, void *ud)
{
    pa_operation *o = op_new();
    if (!o) return NULL;
    c->refs++;
    post(c->m, job_success, c, o, ud, (intptr_t)(uintptr_t)cb);
    return o;
}

EXPORT void pa_context_set_subscribe_callback(pa_context *c, pa_context_subscribe_cb_t cb, void *ud)
{ c->sub_cb = cb; c->sub_ud = ud; }
EXPORT pa_operation *pa_context_subscribe(pa_context *c, pa_subscription_mask_t m, pa_context_success_cb_t cb, void *ud)
{ (void)m; return ctx_success(c, cb, ud); }

/* sink input info: the stream itself; looked up when the job runs */
struct sii_req { pa_sink_input_info_cb_t cb; void *ud; uint32_t idx; };
static void job_sii(void *a, void *b, void *c_, intptr_t i)
{
    pa_context *c = a; pa_operation *o = b; struct sii_req *r = c_; (void)i;
    pa_stream *s = NULL;
    for (s = c->m->streams; s; s = s->next) if (s->idx == r->idx) break;
    if (s && r->cb) {
        pa_sink_input_info info;
        static pa_proplist pl;
        memset(&info, 0, sizeof info);
        info.index = s->idx;
        info.name = "Spotify";
        info.owner_module = PA_INVALID_INDEX;
        info.client = 0;
        info.sink = 0;
        info.sample_spec = s->ss;
        info.channel_map = s->map;
        info.volume = s->vol;
        info.buffer_usec = 0;
        info.sink_usec = s->latency;
        info.resample_method = "copy";
        info.driver = "digifox-alsa";
        info.mute = 0;
        info.proplist = &pl;
        info.corked = s->corked;
        info.has_volume = 1;
        info.volume_writable = 1;
        r->cb(c, &info, 0, r->ud);
    }
    if (r->cb) r->cb(c, NULL, 1, r->ud);
    free(r);
    op_done(c->m, o);
    ctx_unref(c);
}
EXPORT pa_operation *pa_context_get_sink_input_info(pa_context *c, uint32_t idx, pa_sink_input_info_cb_t cb, void *ud)
{
    T();
    pa_operation *o = op_new();
    struct sii_req *r = calloc(1, sizeof *r);
    if (!o || !r) { free(o); free(r); return NULL; }
    r->cb = cb; r->ud = ud; r->idx = idx;
    c->refs++;
    post(c->m, job_sii, c, o, r, 0);
    return o;
}

/* volume: kept and reported, the samples stay untouched (bit-perfect) */
static void job_sub_change(void *a, void *b, void *c_, intptr_t idx)
{
    pa_context *c = a; (void)b; (void)c_;
    if (c->sub_cb) c->sub_cb(c, PA_SUBSCRIPTION_EVENT_SINK_INPUT | PA_SUBSCRIPTION_EVENT_CHANGE, (uint32_t)idx, c->sub_ud);
    ctx_unref(c);
}
EXPORT pa_operation *pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *v, pa_context_success_cb_t cb, void *ud)
{
    T();
    for (pa_stream *s = c->m->streams; s; s = s->next)
        if (s->idx == idx && v) s->vol = *v;
    D("set volume idx %u -> %u", idx, v ? pa_cvolume_avg(v) : 0);
    pa_operation *o = ctx_success(c, cb, ud);
    c->refs++;
    post(c->m, job_sub_change, c, NULL, NULL, idx);
    return o;
}

/* ------------------------------------------------------------- stream */
static void stream_unref(pa_stream *s)
{
    if (!s || --s->refs) return;
    if (s->pcm) snd_pcm_close(s->pcm);
    free(s->q);
    ctx_unref(s->ctx);
    free(s);
}

static void job_stream_cb(void *a, void *b, void *c_, intptr_t i)
{
    pa_stream *s = a; (void)b; (void)c_;
    switch (i) {
    case 1: if (s->start_cb) s->start_cb(s, s->start_ud); break;
    case 2: if (s->under_cb) s->under_cb(s, s->under_ud); break;
    }
    stream_unref(s);
}
static void stream_notify(pa_stream *s, int what)
{
    s->refs++;
    post(s->ctx->m, job_stream_cb, s, NULL, NULL, what);
}
/* like PulseAudio: the new state is seen when its callback runs (creating, then ready) */
static void job_stream_state(void *a, void *b, void *c_, intptr_t st)
{
    pa_stream *s = a; (void)b; (void)c_;
    if (s->st != PA_STREAM_TERMINATED) s->st = (pa_stream_state_t)st;
    if (s->state_cb) s->state_cb(s, s->state_ud);
    stream_unref(s);
}
static void stream_set_state(pa_stream *s, pa_stream_state_t st)
{
    if (st == PA_STREAM_TERMINATED) s->st = st;     /* disconnect: at once */
    s->refs++;
    post(s->ctx->m, job_stream_state, s, NULL, NULL, st);
}

EXPORT pa_stream *pa_stream_new_with_proplist(pa_context *c, const char *name, const pa_sample_spec *ss,
                                             const pa_channel_map *map, pa_proplist *p)
{
    (void)p;
    pa_stream *s = calloc(1, sizeof *s);
    if (!s || !ss) { free(s); return NULL; }
    s->refs = 1;
    s->ctx = c; c->refs++;
    s->ss = *ss;
    if (map) s->map = *map; else pa_channel_map_init_auto(&s->map, ss->channels, PA_CHANNEL_MAP_DEFAULT);
    s->idx = next_idx++;
    s->st = PA_STREAM_UNCONNECTED;
    s->frame = pa_frame_size(ss);
    pa_cvolume_set(&s->vol, ss->channels, PA_VOLUME_NORM);
    D("stream_new %s: format %d, %u Hz, %u ch", name ? name : "", ss->format, ss->rate, ss->channels);
    return s;
}
EXPORT pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map)
{ return pa_stream_new_with_proplist(c, name, ss, map, NULL); }
EXPORT void pa_stream_unref(pa_stream *s) { stream_unref(s); }
EXPORT pa_stream_state_t pa_stream_get_state(const pa_stream *s) { return s->st; }
EXPORT uint32_t pa_stream_get_index(const pa_stream *s) { return s->idx; }
EXPORT int pa_stream_is_corked(const pa_stream *s) { return s->corked; }
EXPORT void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *ud) { s->state_cb = cb; s->state_ud = ud; }
EXPORT void pa_stream_set_started_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *ud) { s->start_cb = cb; s->start_ud = ud; }
EXPORT void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *ud) { s->under_cb = cb; s->under_ud = ud; }

static snd_pcm_format_t alsa_fmt(pa_sample_format_t f)
{
    switch (f) {
    case PA_SAMPLE_U8:        return SND_PCM_FORMAT_U8;
    case PA_SAMPLE_S16LE:     return SND_PCM_FORMAT_S16_LE;
    case PA_SAMPLE_S16BE:     return SND_PCM_FORMAT_S16_BE;
    case PA_SAMPLE_S24LE:     return SND_PCM_FORMAT_S24_3LE;
    case PA_SAMPLE_S24BE:     return SND_PCM_FORMAT_S24_3BE;
    case PA_SAMPLE_S24_32LE:  return SND_PCM_FORMAT_S24_LE;
    case PA_SAMPLE_S24_32BE:  return SND_PCM_FORMAT_S24_BE;
    case PA_SAMPLE_S32LE:     return SND_PCM_FORMAT_S32_LE;
    case PA_SAMPLE_S32BE:     return SND_PCM_FORMAT_S32_BE;
    case PA_SAMPLE_FLOAT32LE: return SND_PCM_FORMAT_FLOAT_LE;
    case PA_SAMPLE_FLOAT32BE: return SND_PCM_FORMAT_FLOAT_BE;
    default:                  return SND_PCM_FORMAT_UNKNOWN;
    }
}

static int pcm_open(pa_stream *s, const pa_buffer_attr *attr)
{
    const char *dev = getenv("DIGIFOX_PA_DEV");
    if (!dev || !*dev) dev = "plug:default";
    snd_pcm_format_t fmt = alsa_fmt(s->ss.format);
    if (fmt == SND_PCM_FORMAT_UNKNOWN) return -1;
    int r = snd_pcm_open(&s->pcm, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (r < 0) { D("open %s: %s", dev, snd_strerror(r)); s->pcm = NULL; return -1; }
    /* like a PulseAudio server: hold tlength bytes (client's, else 2 s; 100 ms..4 s) —
       200 ms of it in the ALSA buffer, the rest queued here. Soloist keeps ~1 s in the
       sink and writes only in big chunks: with just the 200 ms ALSA buffer it ran dry */
    unsigned lat = 200000;
    pa_usec_t tl = 2000000;
    if (attr && attr->tlength != (uint32_t)-1 && attr->tlength > 0) tl = pa_bytes_to_usec(attr->tlength, &s->ss);
    if (tl < 100000) tl = 100000;
    if (tl > 4000000) tl = 4000000;
    s->tlen = pa_usec_to_bytes(tl, &s->ss);
    D("buffer attr: maxlength %d tlength %d prebuf %d minreq %d -> hold %zu bytes (%llu ms)",
      attr ? (int)attr->maxlength : -2, attr ? (int)attr->tlength : -2, attr ? (int)attr->prebuf : -2,
      attr ? (int)attr->minreq : -2, s->tlen, (unsigned long long)(tl / 1000));
    r = snd_pcm_set_params(s->pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED, s->ss.channels, s->ss.rate, 0, lat);
    if (r < 0) {
        D("set_params %u Hz: %s", s->ss.rate, snd_strerror(r));
        snd_pcm_close(s->pcm); s->pcm = NULL; return -1;
    }
    snd_pcm_uframes_t buf = 0, per = 0;
    snd_pcm_get_params(s->pcm, &buf, &per);
    s->bufsize = buf;
    s->latency = (pa_usec_t)buf * 1000000ULL / s->ss.rate;
    /* start at half the buffer (set_params waits for a full one): short tails still play */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(s->pcm, sw) == 0) {
        snd_pcm_sw_params_set_start_threshold(s->pcm, sw, buf / 2 ? buf / 2 : 1);
        snd_pcm_sw_params(s->pcm, sw);
    }
    D("pcm %s: %u Hz, buffer %lu frames (%llu us)", dev, s->ss.rate, (unsigned long)buf,
      (unsigned long long)s->latency);
    return 0;
}

EXPORT int pa_stream_connect_playback(pa_stream *s, const char *dev, const pa_buffer_attr *attr,
                                      pa_stream_flags_t flags, const pa_cvolume *volume, pa_stream *sync)
{
    (void)dev; (void)sync;
    if (volume) s->vol = *volume;
    s->corked = (flags & PA_STREAM_START_CORKED) ? 1 : 0;
    D("connect_playback flags 0x%x tlength %d prebuf %d", (unsigned)flags,
      attr ? (int)attr->tlength : -2, attr ? (int)attr->prebuf : -2);
    /* creating — at once, like PulseAudio (PA_STREAM_IS_GOOD right after connect) */
    s->st = PA_STREAM_CREATING;
    if (s->state_cb) s->state_cb(s, s->state_ud);
    if (pcm_open(s, attr) < 0) {
        s->ctx->err = PA_ERR_NOTSUPPORTED;
        stream_set_state(s, PA_STREAM_FAILED);
        return 0;
    }
    pa_threaded_mainloop *m = s->ctx->m;
    s->refs++;                      /* the mainloop list's */
    s->next = m->streams; m->streams = s;
    stream_set_state(s, PA_STREAM_READY);
    return 0;
}

static void list_remove(pa_stream *s)
{
    pa_threaded_mainloop *m = s->ctx->m;
    for (pa_stream **p = &m->streams; *p; p = &(*p)->next)
        if (*p == s) { *p = s->next; s->next = NULL; stream_unref(s); return; }
}

EXPORT int pa_stream_disconnect(pa_stream *s)
{
    T();
    if (s->pcm) { snd_pcm_drop(s->pcm); snd_pcm_close(s->pcm); s->pcm = NULL; }
    s->qlen = s->qoff = 0;
    s->refs++;                      /* list_remove may drop the last list ref */
    list_remove(s);
    stream_set_state(s, PA_STREAM_TERMINATED);
    stream_unref(s);
    return 0;
}

/* push queued bytes into ALSA; returns frames ALSA can still take */
static snd_pcm_sframes_t pcm_feed(pa_stream *s)
{
    if (!s->pcm) return 0;
    snd_pcm_state_t st = snd_pcm_state(s->pcm);
    if (st == SND_PCM_STATE_XRUN) {
        D("underrun");
        snd_pcm_prepare(s->pcm);
        s->started = 0;
        stream_notify(s, 2);
    } else if (st == SND_PCM_STATE_SETUP) {
        snd_pcm_prepare(s->pcm);
    } else if (st == SND_PCM_STATE_SUSPENDED) {
        if (snd_pcm_resume(s->pcm) < 0) snd_pcm_prepare(s->pcm);
    }
    if (s->corked) return 0;
    while (s->qlen >= s->frame) {
        snd_pcm_sframes_t n = snd_pcm_writei(s->pcm, s->q + s->qoff, s->qlen / s->frame);
        if (n == -EAGAIN || n == 0) break;
        if (n < 0) {
            if (snd_pcm_recover(s->pcm, (int)n, 1) < 0) { s->qlen = s->qoff = 0; break; }
            if (n == -EPIPE) { s->started = 0; stream_notify(s, 2); }
            continue;
        }
        size_t b = (size_t)n * s->frame;
        s->qoff += b;           /* no memmove per write: up to 2 s queued */
        s->qlen -= b;
        if (!s->qlen) s->qoff = 0;
    }
    if (!s->started && snd_pcm_state(s->pcm) == SND_PCM_STATE_RUNNING) {
        D("pcm running, written %lld bytes", (long long)s->written);
        s->started = 1;
        stream_notify(s, 1);
    }
    snd_pcm_sframes_t av = snd_pcm_avail_update(s->pcm);
    if (av < 0) {
        snd_pcm_recover(s->pcm, (int)av, 1);
        av = snd_pcm_avail_update(s->pcm);
        if (av < 0) av = 0;
    }
    return av;
}

static void stream_service(pa_stream *s) { if (s->st == PA_STREAM_READY) pcm_feed(s); }

EXPORT size_t pa_stream_writable_size(const pa_stream *cs)
{
    pa_stream *s = (pa_stream *)cs;
    if (s->st != PA_STREAM_READY || !s->pcm) return (size_t)-1;
    if (!s->corked) pcm_feed(s);
    /* free room = tlength - (queued here + still in the ALSA buffer) */
    snd_pcm_sframes_t d = 0;
    snd_pcm_state_t st = snd_pcm_state(s->pcm);
    if ((st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_PREPARED) && snd_pcm_delay(s->pcm, &d) < 0) d = 0;
    if (d < 0) d = 0;
    size_t held = s->qlen + (size_t)d * s->frame;
    size_t w = held < s->tlen ? s->tlen - held : 0;
    if (detail > 0) { detail--; D("writable %zu (queued %zu, alsa %ld fr, pcm %d)", w, s->qlen, (long)d, (int)st); }
    return w;
}

EXPORT int pa_stream_write(pa_stream *s, const void *data, size_t nbytes, pa_free_cb_t free_cb,
                           int64_t offset, pa_seek_mode_t seek)
{
    (void)offset; (void)seek;
    if (s->st != PA_STREAM_READY) { if (free_cb) free_cb((void *)data); return -PA_ERR_BADSTATE; }
    if (s->qoff + s->qlen + nbytes > s->qcap && s->qoff) {
        memmove(s->q, s->q + s->qoff, s->qlen);     /* compact now and then */
        s->qoff = 0;
    }
    if (s->qlen + nbytes > s->qcap) {
        size_t cap = (s->qlen + nbytes) * 2;
        uint8_t *q = realloc(s->q, cap);
        if (!q) { if (free_cb) free_cb((void *)data); return -PA_ERR_INTERNAL; }
        s->q = q; s->qcap = cap;
    }
    memcpy(s->q + s->qoff + s->qlen, data, nbytes);
    s->qlen += nbytes;
    s->written += (int64_t)nbytes;
    static unsigned nw;
    if (detail > 0) { detail--; D("write %zu (total %lld, queued %zu, pcm %d)", nbytes, (long long)s->written, s->qlen, s->pcm ? (int)snd_pcm_state(s->pcm) : -1); }
    else if (nw++ % 200 == 0)
        D("write #%u: %zu bytes, total %lld, queued %zu, pcm state %d", nw, nbytes,
          (long long)s->written, s->qlen, s->pcm ? (int)snd_pcm_state(s->pcm) : -1);
    if (free_cb) free_cb((void *)data);
    pcm_feed(s);
    return 0;
}

static void job_stream_success(void *a, void *b, void *c_, intptr_t i)
{
    pa_stream *s = a; pa_operation *o = b;
    pa_stream_success_cb_t cb = (pa_stream_success_cb_t)(uintptr_t)i;
    if (cb) cb(s, 1, c_);
    op_done(s->ctx->m, o);
    stream_unref(s);
}
static pa_operation *stream_success_at(pa_stream *s, unsigned delay_ms, pa_stream_success_cb_t cb, void *ud)
{
    pa_operation *o = op_new();
    if (!o) return NULL;
    s->refs++;
    post_at(s->ctx->m, delay_ms, job_stream_success, s, o, ud, (intptr_t)(uintptr_t)cb);
    return o;
}
static pa_operation *stream_success(pa_stream *s, pa_stream_success_cb_t cb, void *ud)
{
    return stream_success_at(s, 0, cb, ud);
}

EXPORT pa_operation *pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *ud)
{
    T();
    b = !!b;
    if (s->pcm && b != s->corked) {
        snd_pcm_state_t st = snd_pcm_state(s->pcm);
        if (b) {
            /* no snd_pcm_pause: not every I2S driver pauses safely; the ≤200 ms in the
               buffer are dropped (Spotify resumes from its own position anyway) */
            if (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_PREPARED) {
                snd_pcm_drop(s->pcm); snd_pcm_prepare(s->pcm);
            }
            s->paused_hw = 0;
        } else {
            if (s->paused_hw) { if (snd_pcm_pause(s->pcm, 0) < 0) snd_pcm_prepare(s->pcm); }
            s->paused_hw = 0;
            s->started = 0;
        }
    }
    s->corked = b;
    D("cork %d", b);
    if (!b) detail = 150;
    if (!b) pcm_feed(s);
    return stream_success(s, cb, ud);
}

EXPORT pa_operation *pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *ud)
{
    T();
    if (s->pcm) {
        snd_pcm_drop(s->pcm);
        snd_pcm_prepare(s->pcm);
        s->paused_hw = 0;
        s->started = 0;
    }
    s->qlen = s->qoff = 0;
    D("flush");
    return stream_success(s, cb, ud);
}

static void fill_timing(pa_stream *s)
{
    pa_timing_info *t = &s->ti;
    snd_pcm_sframes_t d = 0;
    int playing = 0;
    if (s->pcm) {
        snd_pcm_state_t st = snd_pcm_state(s->pcm);
        playing = st == SND_PCM_STATE_RUNNING;
        if ((playing || st == SND_PCM_STATE_PAUSED || st == SND_PCM_STATE_PREPARED)
            && snd_pcm_delay(s->pcm, &d) < 0) d = 0;
        if (d < 0) d = 0;
    }
    gettimeofday(&t->timestamp, NULL);
    t->synchronized_clocks = 1;
    t->sink_usec = (pa_usec_t)d * 1000000ULL / (s->ss.rate ? s->ss.rate : 1);
    t->source_usec = 0;
    t->transport_usec = 0;
    t->playing = playing && !s->corked;
    t->write_index_corrupt = 0;
    t->write_index = s->written;
    t->read_index_corrupt = 0;
    /* PulseAudio meaning: read_index = what the sink has taken from the server buffer
       (here: handed to ALSA); what ALSA still holds is sink_usec. Counting the ALSA
       delay in both made Soloist see 0.37 s more audio buffered than there was */
    t->read_index = s->written - (int64_t)s->qlen;
    if (t->read_index < 0) t->read_index = 0;
    t->configured_sink_usec = s->latency;
    t->configured_source_usec = 0;
    t->since_underrun = 0;
    if (detail > 0) { detail--; D("timing w %lld r %lld sink %llu us playing %d", (long long)t->write_index,
                                  (long long)t->read_index, (unsigned long long)t->sink_usec, t->playing); }
}

EXPORT const pa_timing_info *pa_stream_get_timing_info(pa_stream *s)
{
    fill_timing(s);
    return &s->ti;
}
EXPORT pa_operation *pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *ud)
{
    fill_timing(s);
    return stream_success_at(s, 20, cb, ud);      /* a server round trip, not a busy loop */
}

/* ------------------------------------------------------------ operation */
EXPORT pa_operation_state_t pa_operation_get_state(const pa_operation *o) { return o->st; }
EXPORT void pa_operation_unref(pa_operation *o) { if (o && --o->refs == 0) free(o); }

/* ---------------------------------------------------------------- misc */
EXPORT const char *pa_strerror(int e)
{
    T();
    switch (e < 0 ? -e : e) {
    case PA_OK:               return "OK";
    case PA_ERR_NOTSUPPORTED: return "Not supported (ALSA device refused the format)";
    case PA_ERR_BADSTATE:     return "Bad state";
    default:                  return "Error (DigiFox ALSA shim)";
    }
}
EXPORT pa_proplist *pa_proplist_new(void) { return calloc(1, sizeof(pa_proplist)); }
EXPORT void pa_proplist_free(pa_proplist *p) { free(p); }
EXPORT int pa_proplist_sets(pa_proplist *p, const char *k, const char *v) { (void)p; (void)k; (void)v; return 0; }

EXPORT pa_cvolume *pa_cvolume_set(pa_cvolume *a, unsigned channels, pa_volume_t v)
{
    T();
    if (channels > PA_CHANNELS_MAX) channels = PA_CHANNELS_MAX;
    a->channels = (uint8_t)channels;
    for (unsigned i = 0; i < channels; i++) a->values[i] = v;
    return a;
}
EXPORT pa_volume_t pa_cvolume_avg(const pa_cvolume *a)
{
    T();
    uint64_t sum = 0;
    if (!a || !a->channels) return PA_VOLUME_MUTED;
    for (unsigned i = 0; i < a->channels; i++) sum += a->values[i];
    return (pa_volume_t)(sum / a->channels);
}
EXPORT struct timeval *pa_gettimeofday(struct timeval *tv) { gettimeofday(tv, NULL); return tv; }
EXPORT pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b)
{
    T();
    int64_t ua = (int64_t)a->tv_sec * 1000000 + a->tv_usec;
    int64_t ub = (int64_t)b->tv_sec * 1000000 + b->tv_usec;
    return (pa_usec_t)(ua > ub ? ua - ub : ub - ua);
}
EXPORT size_t pa_usec_to_bytes(pa_usec_t t, const pa_sample_spec *spec)
{
    T();
    return (size_t)((t * spec->rate) / 1000000ULL) * pa_frame_size(spec);
}

/* used above; real libpulse exports them too, cheap to have */
EXPORT size_t pa_frame_size(const pa_sample_spec *spec)
{
    T();
    size_t b;
    switch (spec->format) {
    case PA_SAMPLE_U8: case PA_SAMPLE_ALAW: case PA_SAMPLE_ULAW: b = 1; break;
    case PA_SAMPLE_S16LE: case PA_SAMPLE_S16BE: b = 2; break;
    case PA_SAMPLE_S24LE: case PA_SAMPLE_S24BE: b = 3; break;
    default: b = 4; break;
    }
    return b * spec->channels;
}
EXPORT size_t pa_sample_size(const pa_sample_spec *spec)
{
    T();
    return spec->channels ? pa_frame_size(spec) / spec->channels : 0;
}
EXPORT pa_usec_t pa_bytes_to_usec(uint64_t length, const pa_sample_spec *spec)
{
    T();
    size_t fs = pa_frame_size(spec);
    if (!fs || !spec->rate) return 0;
    return (pa_usec_t)((length / fs) * 1000000ULL / spec->rate);
}
EXPORT pa_channel_map *pa_channel_map_init_auto(pa_channel_map *m, unsigned channels, pa_channel_map_def_t def)
{
    T();
    (void)def;
    memset(m, 0, sizeof *m);
    m->channels = (uint8_t)channels;
    if (channels == 1) m->map[0] = PA_CHANNEL_POSITION_MONO;
    else for (unsigned i = 0; i < channels && i < PA_CHANNELS_MAX; i++)
        m->map[i] = i == 0 ? PA_CHANNEL_POSITION_FRONT_LEFT : i == 1 ? PA_CHANNEL_POSITION_FRONT_RIGHT
                  : (pa_channel_position_t)(PA_CHANNEL_POSITION_AUX0 + i - 2);
    return m;
}
EXPORT const char *pa_get_library_version(void) { return "17.0.0"; }
