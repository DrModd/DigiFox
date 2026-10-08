/*
 * UAC2 → I2S Router for PureFox / PureCore
 *
 * Routes audio from USB Audio Class 2 gadget to I2S DAC on RV1106 (LuckFox Pico MAX).
 * Supports PCM up to 768 kHz and native DSD64–DSD512.
 *
 * Architecture: single-thread, blocking capture ↔ accumulate ↔ blocking playback.
 * USB capture paces the loop; I2S playback follows at matched rate.
 *
 * Key constraints of RV1106 I2S:
 *   - DMA only picks up data at period boundaries (sub-period writes are invisible)
 *   - start_threshold is ignored — DMA auto-starts on first snd_pcm_writei()
 *   - UART at 115200 baud takes ~4 ms per printf — kills DSD512 timing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <errno.h>
#include <stdint.h>
#include <sched.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <time.h>

/* DigiFox: rare events into the same log as the converter (diag.php), kept
 * on the flash so they survive a hang and the reboot after it */
#define EVENT_DIR       "/var/lib/digifox"
#define EVENT_LOG       EVENT_DIR "/events.log"
/* the host sends nothing for this long while the I2S plays: the RV1106 DMA
 * would loop its last buffer (a loud buzz) — stop the I2S, wait for data */
#define USB_STARVE_MS   250

#define EVLOG_PREFIX     "usb: "
#define EVLOG_PREFIX_ARG
#include "evlog_async.h"
/* time.h not needed — status log uses frame counter instead of time() syscall */

/* ── Device constants ─────────────────────────────────────────────── */

#define I2S_CARD        "hw:0,0"
/* DigiFox: with "digifox-srcmode fox" the stream goes through the ALSA
 * plugin "digifox" (PCM 192 kHz on the I2S port, AK4137 not needed).
 * Read on every stream start, so switching needs no restart. */
#define SRCMODE_FILE    "/etc/digifox/srcmode"

static const char *i2s_device(void) {
    char m[16] = "";
    FILE *f = fopen(SRCMODE_FILE, "r");
    if (f) { if (!fgets(m, sizeof m, f)) m[0] = 0; fclose(f); }
    return strncmp(m, "fox", 3) == 0 ? "digifox" : I2S_CARD;
}
#define SYSFS_UAC2_PATH "/sys/class/u_audio"
#define SYSFS_RATE_FILE      "rate"
#define SYSFS_FORMAT_FILE    "format"
#define SYSFS_CHANNELS_FILE  "channels"

#define I2S_FORMAT_PCM  SND_PCM_FORMAT_S32_LE
#define I2S_FORMAT_DSD  SND_PCM_FORMAT_DSD_U32_LE
#define I2S_CHANNELS    2

#define UEVENT_BUFFER_SIZE  4096
#define MAX_CONSECUTIVE_ERRORS 50
#define PERIOD_FRAMES   512

/* ── DSD rate tables ──────────────────────────────────────────────── */

#define DSD64_RATE       2822400
#define DSD128_RATE      5644800
#define DSD256_RATE     11289600
#define DSD512_RATE     22579200
#define DSD64_RATE_48    3072000
#define DSD128_RATE_48   6144000
#define DSD256_RATE_48  12288000
#define DSD512_RATE_48  24576000

/* Byte-swap macro for DSD 32-bit words.
 * USB RAW_DATA sends DSD bytes oldest-first: [B0(oldest) B1 B2 B3(newest)]
 * ALSA DSD_U32_LE stores oldest at MSB:      [B3(oldest) B2 B1 B0(newest)]
 * Without bswap → violet noise on I2S. */
#define BSWAP32(x) (((x) >> 24) | (((x) >> 8) & 0xFF00) | \
                    (((x) << 8) & 0xFF0000) | ((x) << 24))

/* ── Globals ──────────────────────────────────────────────────────── */

static volatile int running = 1;
static snd_pcm_t *pcm_capture  = NULL;
static snd_pcm_t *pcm_playback = NULL;
static char uac_card_path[256] = "";
static char uac_card_name[64]  = "";
static int  is_current_dsd = 0;
static snd_pcm_uframes_t playback_period = 1024;

static void sighandler(int sig) { running = 0; }

/* ── DSD helpers ──────────────────────────────────────────────────── */

static int is_dsd_rate(unsigned int rate) {
    return (rate == DSD64_RATE    || rate == DSD128_RATE    ||
            rate == DSD256_RATE   || rate == DSD512_RATE    ||
            rate == DSD64_RATE_48 || rate == DSD128_RATE_48 ||
            rate == DSD256_RATE_48|| rate == DSD512_RATE_48);
}

static const char *get_dsd_name(unsigned int rate) {
    switch (rate) {
        case DSD64_RATE:     return "DSD64/44.1";
        case DSD128_RATE:    return "DSD128/44.1";
        case DSD256_RATE:    return "DSD256/44.1";
        case DSD512_RATE:    return "DSD512/44.1";
        case DSD64_RATE_48:  return "DSD64/48";
        case DSD128_RATE_48: return "DSD128/48";
        case DSD256_RATE_48: return "DSD256/48";
        case DSD512_RATE_48: return "DSD512/48";
        default: return "Unknown";
    }
}

/* ── Sysfs / UAC helpers ─────────────────────────────────────────── */

static int find_uac_card(void) {
    char path[256];
    struct stat st;
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "%s/uac_card%d", SYSFS_UAC2_PATH, i);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(uac_card_path, path, sizeof(uac_card_path) - 1);
            snprintf(uac_card_name, sizeof(uac_card_name), "uac_card%d", i);
            printf("Found UAC card: %s (card %d)\n", uac_card_path, i);
            return i;
        }
    }
    fprintf(stderr, "ERROR: No UAC card found in %s\n", SYSFS_UAC2_PATH);
    return -1;
}

static int read_sysfs_int(const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", uac_card_path, filename);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int value = 0;
    if (fscanf(fp, "%d", &value) != 1) { fclose(fp); return -1; }
    fclose(fp);
    return value;
}

/* ── Netlink uevent socket ────────────────────────────────────────── */

static int create_uevent_socket(void) {
    int sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sock < 0) {
        fprintf(stderr, "Cannot create netlink socket: %s\n", strerror(errno));
        return -1;
    }
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_groups = 1,
        .nl_pid    = getpid(),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot bind netlink socket: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

/* ── PCM setup ────────────────────────────────────────────────────── */

static int setup_pcm(snd_pcm_t **pcm, const char *device, snd_pcm_stream_t stream,
                     unsigned int rate, snd_pcm_format_t format, unsigned int channels,
                     int is_dsd)
{
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    int err;
    snd_pcm_uframes_t period_size, buffer_size;

    if (is_dsd) {
        period_size = 512;
        buffer_size = (stream == SND_PCM_STREAM_CAPTURE) ? 65536 : 32768;
    } else {
        period_size = PERIOD_FRAMES;
        if (stream == SND_PCM_STREAM_CAPTURE) {
            /* DigiFox: 512 at every rate. The gadget's feedback PI keeps the
             * capture fill at buffer/128 = 512 frames. Reading 512-frame
             * chunks back-to-back averages ~256, below that target, so the
             * PI asks the host for more until the I2S buffer is full and
             * pushes back — the loop then follows the I2S clock. With 1024
             * (used above 192 kHz) the average sat right on the target, the
             * PI saw no error while a slow host drained the I2S buffer:
             * an underrun every few minutes at 384 kHz (~150 ppm). */
            period_size = 512;
        }
        buffer_size = period_size * 16;
        /* Capture: scale buffer to give PI controller ~180ms headroom
         * at any rate (same as 8192 frames at 44.1k). */
        if (stream == SND_PCM_STREAM_CAPTURE)
            buffer_size = 65536;
    }

    if ((err = snd_pcm_open(pcm, device, stream, 0)) < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, snd_strerror(err));
        return err;
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(*pcm, hw_params);
    snd_pcm_hw_params_set_access(*pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(*pcm, hw_params, format);
    snd_pcm_hw_params_set_channels(*pcm, hw_params, channels);
    snd_pcm_hw_params_set_rate_near(*pcm, hw_params, &rate, 0);

    /* Cap DSD playback period — RV1106 DMA ignores sub-period writes */
    if (is_dsd && stream == SND_PCM_STREAM_PLAYBACK) {
        snd_pcm_uframes_t pmax = period_size;
        snd_pcm_hw_params_set_period_size_max(*pcm, hw_params, &pmax, 0);
    }
    snd_pcm_hw_params_set_period_size_near(*pcm, hw_params, &period_size, 0);
    snd_pcm_hw_params_set_buffer_size_near(*pcm, hw_params, &buffer_size);

    if ((err = snd_pcm_hw_params(*pcm, hw_params)) < 0) {
        fprintf(stderr, "Cannot set hw params for %s: %s\n", device, snd_strerror(err));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return err;
    }

    snd_pcm_hw_params_get_period_size(hw_params, &period_size, 0);
    snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);

    snd_pcm_sw_params_alloca(&sw_params);
    snd_pcm_sw_params_current(*pcm, sw_params);
    snd_pcm_sw_params_set_start_threshold(*pcm, sw_params, buffer_size / 2);
    snd_pcm_sw_params_set_avail_min(*pcm, sw_params, period_size);
    snd_pcm_sw_params(*pcm, sw_params);

    printf("  %s: %u Hz, %s, %u ch, period %lu, buffer %lu\n",
           device, rate, snd_pcm_format_name(format), channels,
           (unsigned long)period_size, (unsigned long)buffer_size);

    return 0;
}

static void close_pcms(void) {
    if (pcm_capture)  { snd_pcm_drop(pcm_capture);  snd_pcm_close(pcm_capture);  pcm_capture  = NULL; }
    if (pcm_playback) { snd_pcm_drop(pcm_playback); snd_pcm_close(pcm_playback); pcm_playback = NULL; }
}

/* ── Audio configuration ──────────────────────────────────────────── */

static int configure_audio(unsigned int rate, int card, char **buffer, size_t *buf_size,
                           snd_pcm_uframes_t *period_size_out) {
    int is_dsd = is_dsd_rate(rate);
    snd_pcm_format_t i2s_format = is_dsd ? I2S_FORMAT_DSD : I2S_FORMAT_PCM;
    unsigned int lrck_rate = is_dsd ? rate / 32 : rate;

    is_current_dsd = is_dsd;

    if (is_dsd)
        printf("\n[CONFIG] DSD MODE: %s (%u Hz, LRCK %u)\n", get_dsd_name(rate), rate, lrck_rate);
    else
        printf("\n[CONFIG] PCM: %u Hz, 32-bit, Stereo\n", rate);

    close_pcms();

    /* UAC2 capture — always S32_LE (DSD arrives as raw 32-bit at LRCK rate) */
    char uac_device[32];
    sprintf(uac_device, "hw:%d,0", card);
    if (setup_pcm(&pcm_capture, uac_device, SND_PCM_STREAM_CAPTURE,
                  lrck_rate, I2S_FORMAT_PCM, I2S_CHANNELS, is_dsd) < 0)
        return -1;

    /* I2S playback — DSD_U32_LE for DSD, S32_LE for PCM */
    if (setup_pcm(&pcm_playback, i2s_device(), SND_PCM_STREAM_PLAYBACK,
                  lrck_rate, i2s_format, I2S_CHANNELS, is_dsd) < 0) {
        close_pcms();
        return -1;
    }

    /* Read negotiated period sizes */
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);

    snd_pcm_hw_params_current(pcm_capture, hw);
    snd_pcm_uframes_t cap_period;
    snd_pcm_hw_params_get_period_size(hw, &cap_period, 0);

    snd_pcm_hw_params_current(pcm_playback, hw);
    snd_pcm_uframes_t pb_period;
    snd_pcm_hw_params_get_period_size(hw, &pb_period, 0);

    *period_size_out = cap_period;
    playback_period = pb_period;

    /* Allocate capture buffer (large enough for max period) */
    snd_pcm_uframes_t max_period = (cap_period > pb_period) ? cap_period : pb_period;
    size_t frame_bytes = I2S_CHANNELS * 4;
    *buf_size = max_period * frame_bytes;
    *buffer = realloc(*buffer, *buf_size);
    if (!*buffer) { fprintf(stderr, "Cannot allocate buffer\n"); close_pcms(); return -1; }

    /* Prepare and start capture — USB data begins filling the buffer */
    snd_pcm_prepare(pcm_capture);
    snd_pcm_prepare(pcm_playback);
    snd_pcm_start(pcm_capture);

    printf("[CONFIG] OK, capture period=%lu, playback period=%lu\n\n",
           (unsigned long)cap_period, (unsigned long)pb_period);
    fflush(stdout);
    return 0;
}

/* ── Pre-buffer: accumulate real data before first writei ─────────
 *
 * RV1106 I2S DMA auto-starts on the first snd_pcm_writei(), ignoring
 * start_threshold.  If we write before enough data is available, DMA
 * drains the buffer faster than we refill it → XRUN.
 *
 * Solution: read from USB capture into a pre-buffer until we have
 * enough data to fill half the playback buffer, THEN burst-write it
 * all.  DMA starts and immediately has ~23 ms of headroom.
 *
 * Returns: number of frames pre-buffered, or -1 on error.
 */
static int prebuffer_from_capture(char *cap_buf, snd_pcm_uframes_t cap_period,
                                  snd_pcm_uframes_t pb_period,
                                  char *prebuf, size_t prebuf_max_frames,
                                  snd_pcm_uframes_t target_frames)
{
    const size_t frame_bytes = I2S_CHANNELS * 4;
    snd_pcm_uframes_t collected = 0;
    int errors = 0;

    while (collected < target_frames && running) {
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, cap_buf, cap_period);
        if (frames > 0) {
            errors = 0;
            /* DSD byte-swap */
            if (is_current_dsd) {
                uint32_t *w = (uint32_t *)cap_buf;
                size_t nwords = frames * I2S_CHANNELS;
                for (size_t i = 0; i < nwords; i++)
                    w[i] = BSWAP32(w[i]);
            }
            /* Append to pre-buffer */
            snd_pcm_uframes_t to_copy = frames;
            if (collected + to_copy > prebuf_max_frames)
                to_copy = prebuf_max_frames - collected;
            memcpy(prebuf + collected * frame_bytes, cap_buf, to_copy * frame_bytes);
            collected += to_copy;
        } else if (frames == -EPIPE) {
            /* DigiFox: an overrun that repeats at once (host not streaming)
             * spun here at FIFO 70 and froze the whole Fox — count and pause */
            snd_pcm_prepare(pcm_capture);
            snd_pcm_start(pcm_capture);
            usleep(1000);
            if (++errors >= MAX_CONSECUTIVE_ERRORS) {
                evlog("pre-buffer: USB capture overruns keep repeating, reopening");
                return -1;
            }
        } else if (frames == 0) {
            usleep(1000);
            if (++errors >= MAX_CONSECUTIVE_ERRORS * 20) {
                evlog("pre-buffer: USB capture returns nothing, reopening");
                return -1;
            }
        } else {
            usleep(1000);
            if (++errors >= MAX_CONSECUTIVE_ERRORS) {
                evlog("pre-buffer: USB capture error %s, reopening", snd_strerror((int)frames));
                return -1;
            }
        }
    }
    return (int)collected;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    char *buffer = NULL;
    size_t buffer_size = 0;
    unsigned int current_rate = 0;
    snd_pcm_uframes_t period_size = PERIOD_FRAMES;
    int uevent_sock = -1;
    char uevent_buf[UEVENT_BUFFER_SIZE];
    int uac_card = -1;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Real-time scheduling */
    {
        struct sched_param sp = { .sched_priority = 70 };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
            printf("[RT] SCHED_FIFO priority %d\n", sp.sched_priority);
        else
            fprintf(stderr, "[RT] WARNING: Cannot set SCHED_FIFO: %s\n", strerror(errno));
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
        printf("[RT] Memory locked\n");

    printf("═══════════════════════════════════════════\n");
    printf("  UAC2 -> I2S Router\n");
    printf("═══════════════════════════════════════════\n\n");

    uac_card = find_uac_card();
    if (uac_card < 0) return 1;

    int format_bytes = read_sysfs_int(SYSFS_FORMAT_FILE);
    int channels = read_sysfs_int(SYSFS_CHANNELS_FILE);
    if (format_bytes < 0 || channels < 0) {
        fprintf(stderr, "ERROR: Cannot read UAC2 configuration\n");
        return 1;
    }
    printf("UAC2: %d-bit, %d ch\n\n", format_bytes * 8, channels);

    uevent_sock = create_uevent_socket();
    if (uevent_sock < 0) return 1;

    printf("Waiting for rate changes...\n\n");

    /* Initial rate */
    int rate = read_sysfs_int(SYSFS_RATE_FILE);
    if (rate > 0) {
        printf("Initial rate: %d Hz\n", rate);
        if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0)
            current_rate = rate;
    }

    /* ── Main loop state ─────────────────────────────────────────── */

    const size_t frame_bytes = I2S_CHANNELS * 4;
    snd_pcm_uframes_t pb_period = playback_period;

    char *accum_buf = malloc(pb_period * frame_bytes);
    snd_pcm_uframes_t accum_pos = 0;

    /* Pre-buffer for burst-writing to I2S before DMA starts.
     * 16 playback periods = half of 32768 buffer = ~23 ms at DSD512. */
    #define PREBUF_PERIODS 16
    size_t prebuf_max = pb_period * PREBUF_PERIODS;
    char *prebuf = malloc(prebuf_max * frame_bytes);

    int play_started = 0;
    int need_prebuffer = 1;  /* Start with pre-buffering phase */

    int consecutive_errors = 0;
    unsigned long write_count = 0;
    unsigned long xrun_count = 0;
    unsigned long cap_xrun_count = 0;
    unsigned long cap_frames_total = 0;
    unsigned long last_status_frames = 0;

    fflush(stdout);

    evlog_start();               /* writer thread: no file I/O at FIFO 70 */
    evlog("router started (output %s)", i2s_device());
    time_t hb_t = time(NULL), spin_t = time(NULL);
    unsigned long spin_n = 0;

    while (running) {
        /* DigiFox: this loop runs at SCHED_FIFO 70, above the network and USB
         * interrupt threads. If it ever spins without blocking, the whole Fox
         * freezes. Count passes per second and back off when it spins. */
        spin_n++;
        time_t now_t = time(NULL);
        if (now_t != spin_t) {
            if (spin_n > 20000) {
                evlog("loop spinning (%lu passes/s, play=%d prebuf=%d): backing off", spin_n, play_started, need_prebuffer);
                usleep(20000);
            }
            spin_n = 0;
            spin_t = now_t;
        }
        /* a line a minute while playing: shows when and in what state it stopped */
        if (play_started && now_t - hb_t >= 60) {
            hb_t = now_t;
            /* feedback pitch (1000000 = nominal): how far the host is pushed */
            int fb = read_sysfs_int("feedback");
            snd_pcm_sframes_t pb_delay = 0;
            if (pcm_playback) snd_pcm_delay(pcm_playback, &pb_delay);
            evlog("playing %u Hz: writes %lu, I2S underruns %lu, USB overruns %lu, feedback %+d ppm, I2S queue %ld",
                  current_rate, write_count, xrun_count, cap_xrun_count,
                  fb > 0 ? fb - 1000000 : 0, (long)pb_delay);
        }
        /* ── Uevent: rate change detection ───────────────────────── */
        ssize_t len = recv(uevent_sock, uevent_buf, sizeof(uevent_buf) - 1, MSG_DONTWAIT);
        if (len > 0) {
            uevent_buf[len] = '\0';
            if (strstr(uevent_buf, "u_audio") && strstr(uevent_buf, uac_card_name)) {
                rate = read_sysfs_int(SYSFS_RATE_FILE);
                if (rate > 0 && rate != (int)current_rate) {
                    printf("\n[CHANGE] %u -> %u Hz (w=%lu x=%lu)\n",
                           current_rate, rate, write_count, xrun_count);
                    evlog("rate %u -> %d Hz, output %s (writes %lu, xruns %lu)",
                          current_rate, rate, i2s_device(), write_count, xrun_count);
                    if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
                        current_rate = rate;
                        pb_period = playback_period;
                        /* Reallocate accumulation and pre-buffer */
                        free(accum_buf);
                        accum_buf = malloc(pb_period * frame_bytes);
                        accum_pos = 0;
                        prebuf_max = pb_period * PREBUF_PERIODS;
                        free(prebuf);
                        prebuf = malloc(prebuf_max * frame_bytes);
                        /* Reset counters, enter pre-buffer phase */
                        play_started = 0;
                        need_prebuffer = 1;
                        write_count = 0;
                        xrun_count = 0;
                        cap_xrun_count = 0;
                    }
                }
            }
        }

        if (!pcm_capture || !pcm_playback) {
            usleep(100000);
            if (current_rate > 0) {
                printf("[REOPEN] Reconfiguring at %u Hz\n", current_rate);
                if (configure_audio(current_rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
                    pb_period = playback_period;
                    free(accum_buf);
                    accum_buf = malloc(pb_period * frame_bytes);
                    accum_pos = 0;
                    prebuf_max = pb_period * PREBUF_PERIODS;
                    free(prebuf);
                    prebuf = malloc(prebuf_max * frame_bytes);
                    play_started = 0;
                    need_prebuffer = 1;
                    write_count = 0;
                    xrun_count = 0;
                    cap_xrun_count = 0;
                }
            }
            continue;
        }

        /* ── Pre-buffer phase: accumulate real data before first write ─ */
        if (need_prebuffer) {
            snd_pcm_uframes_t target = pb_period * PREBUF_PERIODS;
            int got = prebuffer_from_capture(buffer, period_size, pb_period,
                                             prebuf, prebuf_max, target);
            if (got < 0) {
                fprintf(stderr, "[PREBUF] Capture failed, reopening\n");
                close_pcms();
                play_started = 0;
                accum_pos = 0;
                usleep(500000);
                continue;
            }
            /* Burst-write all pre-buffered data to I2S.
             * DMA auto-starts on first writei; subsequent writes fill the buffer. */
            snd_pcm_uframes_t written = 0;
            while (written + pb_period <= (snd_pcm_uframes_t)got) {
                snd_pcm_sframes_t wr = snd_pcm_writei(pcm_playback,
                    prebuf + written * frame_bytes, pb_period);
                if (wr < 0) break;
                written += wr;
            }
            cap_frames_total += got;
            need_prebuffer = 0;
            play_started = 1;
            last_status_frames = cap_frames_total;  /* Defer first STAT */
            continue;
        }

        /* ── Steady state: capture → byte-swap → accumulate → write ── */
        /* watchdog: no data from the host -> stop the I2S instead of a buzz */
        if (play_started && snd_pcm_wait(pcm_capture, USB_STARVE_MS) == 0) {
            evlog("no data from the host for %d ms at %u Hz: I2S stopped, waiting (writes %lu)",
                  USB_STARVE_MS, current_rate, write_count);
            snd_pcm_drop(pcm_playback);
            snd_pcm_prepare(pcm_playback);
            need_prebuffer = 1;
            play_started = 0;
            accum_pos = 0;
            continue;
        }
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, buffer, period_size);

        if (frames > 0) {
            consecutive_errors = 0;
            cap_frames_total += frames;

            /* DSD byte-swap */
            if (is_current_dsd) {
                uint32_t *w = (uint32_t *)buffer;
                size_t nwords = frames * I2S_CHANNELS;
                for (size_t i = 0; i < nwords; i++)
                    w[i] = BSWAP32(w[i]);
            }

            /* Accumulate into full playback periods */
            snd_pcm_uframes_t src_off = 0;
            snd_pcm_uframes_t remaining = frames;

            while (remaining > 0) {
                snd_pcm_uframes_t to_copy = remaining;
                if (accum_pos + to_copy > pb_period)
                    to_copy = pb_period - accum_pos;
                memcpy(accum_buf + accum_pos * frame_bytes,
                       buffer + src_off * frame_bytes, to_copy * frame_bytes);
                accum_pos += to_copy;
                src_off += to_copy;
                remaining -= to_copy;

                if (accum_pos >= pb_period) {
                    snd_pcm_sframes_t wr = snd_pcm_writei(pcm_playback, accum_buf, pb_period);
                    accum_pos = 0;

                    if (wr > 0) {
                        write_count++;
                    } else if (wr == -EPIPE) {
                        /* XRUN recovery: re-enter pre-buffer phase */
                        xrun_count++;
                        fprintf(stderr, "[XRUN] Playback underrun #%lu at w=%lu\n", xrun_count, write_count);
                        evlog("I2S underrun #%lu at %u Hz (writes %lu)", xrun_count, current_rate, write_count);
                        snd_pcm_prepare(pcm_playback);
                        need_prebuffer = 1;
                        play_started = 0;
                        break;
                    } else if (wr == -ENODEV || wr == -EBADF) {
                        evlog("I2S output gone (%s), reopening", snd_strerror((int)wr));
                        close_pcms();
                        play_started = 0;
                        break;
                    } else if (wr < 0) {
                        /* anything else (ESTRPIPE, EIO, ...): nothing would reach the
                         * I2S while it keeps running — start over */
                        evlog("I2S write error %s at %u Hz: restarting output", snd_strerror((int)wr), current_rate);
                        snd_pcm_drop(pcm_playback);
                        snd_pcm_prepare(pcm_playback);
                        need_prebuffer = 1;
                        play_started = 0;
                        break;
                    }
                }
            }


            /* Status log disabled to reduce spam */
            /* if (cap_frames_total - last_status_frames >= current_rate / 32 * 10) {
                last_status_frames = cap_frames_total;
                printf("[S] w=%lu x=%lu cx=%lu cf=%lu\n",
                       write_count, xrun_count, cap_xrun_count, cap_frames_total);
                fflush(stdout);
            } */
        } else if (frames == -EPIPE) {
            cap_xrun_count++;
            fprintf(stderr, "[XRUN] Capture overrun #%lu\n", cap_xrun_count);
            evlog("USB capture overrun #%lu at %u Hz", cap_xrun_count, current_rate);
            accum_pos = 0;
            snd_pcm_prepare(pcm_capture);
            snd_pcm_start(pcm_capture);
            usleep(1000);
        } else if (frames == -ENODEV || frames == -EBADF) {
            close_pcms();
            play_started = 0;
            accum_pos = 0;
            usleep(500000);
        } else if (frames == 0) {
            usleep(1000);            /* nothing read: never spin at FIFO 70 */
        } else if (frames < 0) {
            usleep(1000);
            if (++consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                fprintf(stderr, "[ERROR] Too many capture errors (last=%ld), reopening\n", (long)frames);
                evlog("too many USB capture errors (%s), reopening", snd_strerror((int)frames));
                close_pcms();
                consecutive_errors = 0;
                play_started = 0;
                accum_pos = 0;
                usleep(500000);
            }
        }
    }

    /* ── Cleanup ──────────────────────────────────────────────────── */

    free(accum_buf);
    free(prebuf);
    free(buffer);
    if (uevent_sock >= 0) close(uevent_sock);
    close_pcms();

    printf("\nStopped (w=%lu x=%lu)\n", write_count, xrun_count);
    return 0;
}
