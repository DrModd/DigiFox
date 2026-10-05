/*
 * pfrate — instant stream rate/format reports for the DigiD D1 amplifier.
 *
 * Started by "pfctl serve" with stdout on the console UART. Reads the ALSA
 * playback hw_params every 20 ms (no process spawning) and prints
 *
 *   @RATE <Hz> <format>     e.g. @RATE 192000 S32_LE
 *   @RATE DSD<N>            e.g. @RATE DSD128
 *   @RATE STOP              nothing is playing
 *   @IN <Hz> <format> | @IN DSD<N> | @IN STOP
 *                           what the player really sends when the Fox converts
 *                           everything to 192 kHz (ALSA plugin "digifox" writes
 *                           /tmp/digifox_in); only for the amplifier display
 *
 * as soon as a new stream is opened, so the STM32 can mute the AX5689 before
 * the new stream starts. A new rate is reported immediately; STOP only after it
 * lasted STOP_HOLD_MS, so a short close between two tracks does not blink the
 * display. The text is the same as pfctl's rate_now().
 *
 * Copyright (C) 2026 DigiFox. GPL-2.0-or-later.
 */
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#define POLL_MS       20
#define GLOB_EVERY_MS 1000     /* re-scan cards (USB DAC may come and go) */
#define STOP_HOLD_MS  400
#define IN_FILE       "/tmp/digifox_in"
#ifndef PFRATE_GLOB
#define PFRATE_GLOB   "/proc/asound/card*/pcm*p/sub0/hw_params"
#endif

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Parse one hw_params file. Returns 1 and fills out[] if a stream is open. */
static int read_params(const char *path, char *out, size_t outlen)
{
    char buf[512];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    if (strncmp(buf, "closed", 6) == 0)
        return 0;

    char fmt[32] = "";
    long hz = 0;
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl)
            *nl = 0;
        if (strncmp(l, "format:", 7) == 0)
            sscanf(l + 7, " %31s", fmt);
        else if (strncmp(l, "rate:", 5) == 0)
            hz = strtol(l + 5, NULL, 10);
        l = nl ? nl + 1 : NULL;
    }
    if (hz <= 0)
        return 0;

    int bits = 0;
    if (strncmp(fmt, "DSD_U32", 7) == 0) bits = 32;
    else if (strncmp(fmt, "DSD_U16", 7) == 0) bits = 16;
    else if (strncmp(fmt, "DSD_U8", 6) == 0) bits = 8;

    if (bits)
        snprintf(out, outlen, "DSD%ld", hz * bits / 44100);
    else
        snprintf(out, outlen, "%ld %s", hz, fmt);
    return 1;
}

int main(void)
{
    glob_t g = { 0 };
    long last_glob = -GLOB_EVERY_MS, stop_since = -1;
    char last[64] = "", cur[64];
    char last_in[64] = "STOP", in[64];

    signal(SIGPIPE, SIG_DFL);
    setvbuf(stdout, NULL, _IONBF, 0);

    for (;;) {
        long t = now_ms();
        if (t - last_glob >= GLOB_EVERY_MS) {
            globfree(&g);
            memset(&g, 0, sizeof(g));
            glob(PFRATE_GLOB, 0, NULL, &g);
            last_glob = t;
        }

        int open_ = 0;
        for (size_t i = 0; i < g.gl_pathc && !open_; i++)
            open_ = read_params(g.gl_pathv[i], cur, sizeof(cur));

        if (open_) {
            stop_since = -1;
            if (strcmp(cur, last) != 0) {
                char line[80];
                int len = snprintf(line, sizeof(line), "@RATE %s\n", cur);
                if (write(1, line, len) < 0)
                    return 1;           /* UART gone: pfctl will restart us */
                strcpy(last, cur);
            }
        } else if (strcmp(last, "STOP") != 0) {
            if (stop_since < 0)
                stop_since = t;
            else if (t - stop_since >= STOP_HOLD_MS || last[0] == 0) {
                if (write(1, "@RATE STOP\n", 11) < 0)
                    return 1;
                strcpy(last, "STOP");
                stop_since = -1;
            }
        }
        /* source of the conversion plugin, only while the I2S port is open */
        strcpy(in, "STOP");
        if (open_) {
            int fd = open(IN_FILE, O_RDONLY);
            if (fd >= 0) {
                ssize_t n = read(fd, in, sizeof(in) - 1);
                close(fd);
                if (n > 0) { in[n] = 0; in[strcspn(in, "\r\n")] = 0; }
                if (n <= 0 || !in[0]) strcpy(in, "STOP");
            }
        }
        if (strcmp(in, last_in) != 0 && (open_ || strcmp(last, "STOP") == 0)) {
            char line[80];
            int len = snprintf(line, sizeof(line), "@IN %s\n", in);
            if (write(1, line, len) < 0)
                return 1;
            strcpy(last_in, in);
        }
        usleep(POLL_MS * 1000);
    }
}
