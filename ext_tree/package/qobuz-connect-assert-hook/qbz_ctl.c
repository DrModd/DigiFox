/*
 * DigiFox: кнопки плеера (пульт усилителя) для Qobuz Connect без исходников плеера.
 *
 * Плеер (qobuz_connect_sample_app из SDK) сам не вызывает qbz_connect_next_track и т. п.
 * Эта библиотека подгружается к нему через LD_PRELOAD (вместе с assert_hook) и:
 *   1) перехватывает несколько функций SDK, которые плеер вызывает с указателем
 *      на сессию первым аргументом, — запоминает этот указатель;
 *   2) при первом перехвате запускает поток, читающий команды из FIFO /tmp/qobuz_ctl:
 *        toggle | next | prev | pause | resume
 *      и вызывает соответствующие функции SDK.
 *
 * Аргументы функций SDK (ARM EABI, по коду libqobuz_connect.so 1.0.0):
 *   qbz_connect_next_track(h), _previous_track(h), _pause_playback(h), _resume_playback(h)
 *   qbz_connect_get_playback_state(h, int *state)  -> 0 при успехе
 *   qbz_playback_state_to_string(int) -> "Playing" | "Paused" | "Stopped" | "Idle"
 * Перехватываемые функции принимают не больше 4 аргументов в регистрах r0..r3 и
 * возвращают код ошибки в r0, поэтому обёртка с четырьмя int передаёт их без изменений.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CTL_FIFO "/tmp/qobuz_ctl"

typedef long long (*fn4_t)(int, int, int, int);
typedef int (*fn1_t)(void *);
typedef int (*state_t)(void *, int *);
typedef const char *(*state_str_t)(int);

static void *volatile g_conn;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void log_line(const char *s)
{
    (void)write(STDERR_FILENO, s, strlen(s));
}

static void *sdk(const char *name)
{
    void *f = dlsym(RTLD_DEFAULT, name);
    return f ? f : dlsym(RTLD_NEXT, name);
}

static void do_cmd(const char *c)
{
    void *h = g_conn;
    char msg[96];
    if (!h) { log_line("[qbz_ctl] no session yet\n"); return; }

    const char *fn = NULL;
    if (!strcmp(c, "next"))        fn = "qbz_connect_next_track";
    else if (!strcmp(c, "prev"))   fn = "qbz_connect_previous_track";
    else if (!strcmp(c, "pause"))  fn = "qbz_connect_pause_playback";
    else if (!strcmp(c, "resume")) fn = "qbz_connect_resume_playback";
    else if (!strcmp(c, "toggle")) {
        state_t gs = (state_t)sdk("qbz_connect_get_playback_state");
        state_str_t ss = (state_str_t)sdk("qbz_playback_state_to_string");
        int st = -1;
        const char *name = "?";
        if (gs && gs(h, &st) == 0 && ss) name = ss(st);
        fn = (name && !strcmp(name, "Playing")) ? "qbz_connect_pause_playback"
                                                : "qbz_connect_resume_playback";
    } else {
        snprintf(msg, sizeof msg, "[qbz_ctl] unknown command '%s'\n", c);
        log_line(msg);
        return;
    }

    fn1_t f = (fn1_t)sdk(fn);
    int r = f ? f(h) : -1;
    snprintf(msg, sizeof msg, "[qbz_ctl] %s -> %s = %d\n", c, fn, r);
    log_line(msg);
}

static void *ctl_thread(void *arg)
{
    (void)arg;
    char buf[64];
    size_t n = 0;

    unlink(CTL_FIFO);
    if (mkfifo(CTL_FIFO, 0666) != 0 && errno != EEXIST) {
        log_line("[qbz_ctl] mkfifo failed\n");
        return NULL;
    }
    chmod(CTL_FIFO, 0666);

    for (;;) {
        /* O_RDWR: у FIFO всегда есть «писатель» (мы сами) — read не получает EOF
         * после каждого echo, а запись извне не блокируется, пока плеер жив */
        int fd = open(CTL_FIFO, O_RDWR);
        if (fd < 0) { sleep(1); continue; }
        for (;;) {
            char ch;
            ssize_t r = read(fd, &ch, 1);
            if (r <= 0) break;
            if (ch == '\n' || ch == '\r') {
                buf[n] = 0;
                if (n) do_cmd(buf);
                n = 0;
            } else if (n < sizeof buf - 1) {
                buf[n++] = ch;
            }
        }
        close(fd);
    }
    return NULL;
}

static void start_thread(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&t, &a, ctl_thread, NULL) == 0)
        log_line("[qbz_ctl] control FIFO " CTL_FIFO " ready\n");
    pthread_attr_destroy(&a);
}

static void remember(int h)
{
    if (!h) return;
    g_conn = (void *)h;
    pthread_once(&g_once, start_thread);
}

#define QBZ_HOOK(name)                                                     \
    long long name(int a, int b, int c, int d)                             \
    {                                                                      \
        static fn4_t real;                                                 \
        if (!real) real = (fn4_t)dlsym(RTLD_NEXT, #name);                  \
        remember(a);                                                       \
        return real ? real(a, b, c, d) : -1;                               \
    }

/* вызываются плеером с сессией первым аргументом (h + не больше 3 слов) */
QBZ_HOOK(qbz_connect_get_device_uuid)
QBZ_HOOK(qbz_connect_get_session_id)
QBZ_HOOK(qbz_connect_notify_playback_initiated)
QBZ_HOOK(qbz_connect_notify_playback_paused)
QBZ_HOOK(qbz_connect_notify_playback_resumed)
QBZ_HOOK(qbz_connect_notify_playback_stopped)
