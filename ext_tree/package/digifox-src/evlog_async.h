/*
 * evlog_async.h — event log for real-time audio threads (DigiFox).
 *
 * evlog() only formats a line into memory: no file I/O in the caller.
 * A normal-priority thread writes the lines to EVENT_LOG on the flash
 * (append, fsync, 64 KB + .old) about once a second. Writing from the
 * audio threads themselves stalled them for tens of ms per line (flash
 * fsync) — and each stall caused the next underrun and the next line.
 *
 * Define EVENT_DIR, EVENT_LOG and EVLOG_PREFIX before including.
 * evlog_start() / evlog_stop() are reference counted (an ALSA plugin may
 * be opened several times in one process); stop flushes and joins.
 *
 * GPL-2.0-or-later. DigiFox.
 */
#ifndef EVLOG_ASYNC_H
#define EVLOG_ASYNC_H

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t ev_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ev_cv = PTHREAD_COND_INITIALIZER;
static char            ev_buf[8192];
static size_t          ev_len;
static unsigned        ev_dropped;
static int             ev_refs, ev_quit, ev_thr_ok;
static pthread_t       ev_thr;

static void ev_write(const char *s, size_t n)
{
    struct stat st;
    mkdir(EVENT_DIR, 0755);
    if (stat(EVENT_LOG, &st) == 0 && st.st_size > 65536) rename(EVENT_LOG, EVENT_LOG ".old");
    FILE *f = fopen(EVENT_LOG, "a");
    if (!f) return;
    fwrite(s, 1, n, f);
    fflush(f);
    fsync(fileno(f));            /* survives a hang and the hard reset after it */
    fclose(f);
}

static void *ev_thread(void *arg)
{
    (void)arg;
    static char local[sizeof ev_buf + 64];
    pthread_mutex_lock(&ev_mx);
    for (;;) {
        while (!ev_len && !ev_quit) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&ev_cv, &ev_mx, &ts);
        }
        size_t n = ev_len;
        memcpy(local, ev_buf, n);
        ev_len = 0;
        if (ev_dropped) {
            n += (size_t)snprintf(local + n, 64, "... %u lines dropped (log busy)\n", ev_dropped);
            ev_dropped = 0;
        }
        int quit = ev_quit;
        pthread_mutex_unlock(&ev_mx);
        if (n) ev_write(local, n);
        if (quit) return NULL;
        usleep(500000);          /* batch: at most ~2 flash writes a second */
        pthread_mutex_lock(&ev_mx);
    }
}

/* call from a non-real-time context if possible (thread is created
 * explicitly at SCHED_OTHER, so a real-time caller is fine too) */
static void evlog_start(void)
{
    pthread_mutex_lock(&ev_mx);
    if (ev_refs++ == 0) {
        ev_quit = 0;
        pthread_attr_t a;
        struct sched_param sp = { .sched_priority = 0 };
        pthread_attr_init(&a);
        pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&a, SCHED_OTHER);
        pthread_attr_setschedparam(&a, &sp);
        ev_thr_ok = pthread_create(&ev_thr, &a, ev_thread, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    pthread_mutex_unlock(&ev_mx);
}

static __attribute__((unused)) void evlog_stop(void)
{
    pthread_mutex_lock(&ev_mx);
    int last = --ev_refs == 0;
    if (last) { ev_quit = 1; pthread_cond_signal(&ev_cv); }
    int join = last && ev_thr_ok;
    if (last) ev_thr_ok = 0;
    pthread_mutex_unlock(&ev_mx);
    if (join) pthread_join(ev_thr, NULL);
}

static void evlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void evlog(const char *fmt, ...)
{
    /* at most 30 lines a minute: a fault that repeats must not flood the flash */
    static time_t win;
    static int cnt;
    char line[320];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int n = snprintf(line, sizeof line, "%02d.%02d %02d:%02d:%02d " EVLOG_PREFIX,
                     tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min, tm.tm_sec EVLOG_PREFIX_ARG);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';

    pthread_mutex_lock(&ev_mx);
    if (t - win >= 60) { win = t; cnt = 0; }
    if (++cnt > 30 || ev_len + (size_t)n > sizeof ev_buf) ev_dropped++;
    else { memcpy(ev_buf + ev_len, line, (size_t)n); ev_len += (size_t)n; }
    pthread_cond_signal(&ev_cv);
    pthread_mutex_unlock(&ev_mx);
}

#endif
