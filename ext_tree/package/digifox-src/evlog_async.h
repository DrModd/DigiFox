/*
 * evlog_async.h — event log for real-time audio threads (DigiFox).
 *
 * evlog() appends one line to EVENT_TMP in /tmp (tmpfs: RAM, microseconds,
 * no flash, no fsync), at most 30 lines a minute. S98digifox-guard moves
 * the lines to the flash log (/var/lib/digifox/events.log) every 5 s, so
 * they survive a hang and the reboot after it, minus the last seconds.
 *
 * Writing to the flash from the audio threads (fsync) stalled them for
 * tens of ms per line; a writer thread inside the plugin did not run in
 * every player. Plain tmpfs appends need neither.
 *
 * Define EVLOG_PREFIX / EVLOG_PREFIX_ARG before including.
 * GPL-2.0-or-later. DigiFox.
 */
#ifndef EVLOG_ASYNC_H
#define EVLOG_ASYNC_H

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EVENT_TMP "/tmp/digifox_events.log"

static inline void evlog_start(void) {}
static inline void evlog_stop(void) {}

static void evlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void evlog(const char *fmt, ...)
{
    static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
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

    pthread_mutex_lock(&mx);
    if (t - win >= 60 || t < win) { win = t; cnt = 0; }
    int ok = ++cnt <= 30;
    pthread_mutex_unlock(&mx);
    if (!ok) return;
    /* one write() with O_APPEND: whole lines, no stdio buffers, no malloc */
    int fd = open(EVENT_TMP, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, line, (size_t)n) < 0) {}
    close(fd);
}

#endif
