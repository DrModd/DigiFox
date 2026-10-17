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
 *   @LVL <L> <R> <pk>       VU meters (only while the amplifier shows them,
 *                           /tmp/amp_vu = 1): needle positions 0..250 on the VU
 *                           scale (-20..+3 VU, 0 VU = -14 dBFS or /etc/digifox/vu_ref), pk bit 0/1 —
 *                           left/right peak at -1 dBFS or above; 25 times a second
 *                           while playing, "@LVL 0 0 0" once when it stops.
 *                           Levels come from dfx_level.h with the time each block
 *                           is heard, so the needles follow the sound.
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
#include <math.h>
#include "dfx_level.h"

#define POLL_MS       20
#define GLOB_EVERY_MS 1000     /* re-scan cards (USB DAC may come and go) */
#define STOP_HOLD_MS  400
#define IN_FILE       "/tmp/digifox_in"
#define VU_FLAG       "/tmp/amp_vu"     /* "1": the amplifier shows VU meters (pfctl) */
#define LVL_EVERY_MS  40
#define LVL_REF_DBFS  (-14.0)           /* 0 VU by default: mastered music sits around 0 VU */
#define VU_REF_FILE   "/etc/digifox/vu_ref"  /* other 0 VU, dBFS, e.g. "-18" */
#define LVL_IDLE_MS   300               /* no new levels this long: silence */
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

/* VU scale position 0..250 (-20 VU .. +3 VU); the scale is linear in voltage */
static double vu_ref = LVL_REF_DBFS;

static int vu_pos(double ms)
{
    if (ms <= 1e-12) return 0;
    double vu = 10.0 * log10(ms) + 3.0103 - vu_ref;   /* sine RMS: full scale = 0 dBFS */
    double a = pow(10.0, -20.0 / 20.0), b = pow(10.0, 3.0 / 20.0);
    double p = (pow(10.0, vu / 20.0) - a) / (b - a);
    if (p < 0) p = 0;
    if (p > 1.02) p = 1.02;
    return (int)(p * 250.0 + 0.5);
}

static int read_flag(const char *path)
{
    char c = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    if (read(fd, &c, 1) != 1) c = 0;
    close(fd);
    return c == '1';
}

int main(void)
{
    glob_t g = { 0 };
    long last_glob = -GLOB_EVERY_MS, stop_since = -1;
    char last[64] = "", cur[64];
    char last_in[64] = "STOP", in[64];
    dfx_lvl_t *lvl = NULL;
    uint32_t lvl_seen = 0;
    long lvl_last = 0, vu_check = -1000, lvl_new = -100000;
    int vu_on = 0, lvl_sent_zero = 1;

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
        /* VU meters: levels whose sound is coming out now */
        if (t - vu_check >= 1000) {
            vu_check = t;
            vu_on = read_flag(VU_FLAG);
            {
                char b[16] = "";
                int fd = open(VU_REF_FILE, O_RDONLY);
                vu_ref = LVL_REF_DBFS;
                if (fd >= 0) {
                    ssize_t n = read(fd, b, sizeof b - 1);
                    close(fd);
                    if (n > 0) { b[n] = 0; double r = atof(b); if (r <= -1 && r >= -30) vu_ref = r; }
                }
            }
            if (vu_on && !lvl) { lvl = dfx_lvl_map(); if (lvl) lvl_seen = lvl->head; }
        }
        if (vu_on && lvl && t - lvl_last >= LVL_EVERY_MS) {
            lvl_last = t;
            uint32_t head = lvl->head;
            if (head - lvl_seen > DFX_LVL_N) lvl_seen = head - DFX_LVL_N;
            double ms[2] = { 0, 0 };
            float pk[2] = { 0, 0 };
            int n = 0;
            int64_t tn = dfx_now_ms();
            while (lvl_seen != head) {
                dfx_lvl_e *e = &lvl->e[lvl_seen % DFX_LVL_N];
                if (e->t_ms > tn) break;             /* not heard yet */
                for (int c = 0; c < 2; c++) {
                    ms[c] += e->ms[c];
                    if (e->pk[c] > pk[c]) pk[c] = e->pk[c];
                }
                n++;
                lvl_seen++;
            }
            if (n) {
                char line[40];
                int len = snprintf(line, sizeof line, "@LVL %d %d %d\n",
                                   vu_pos(ms[0] / n), vu_pos(ms[1] / n),
                                   (pk[0] >= 0.891f ? 1 : 0) | (pk[1] >= 0.891f ? 2 : 0));
                if (write(1, line, len) < 0)
                    return 1;
                lvl_new = t;
                lvl_sent_zero = 0;
            } else if (!lvl_sent_zero && t - lvl_new >= LVL_IDLE_MS) {
                if (write(1, "@LVL 0 0 0\n", 11) < 0)
                    return 1;
                lvl_sent_zero = 1;
            }
        } else if (!vu_on) {
            lvl_sent_zero = 1;
        }
        usleep(POLL_MS * 1000);
    }
}
