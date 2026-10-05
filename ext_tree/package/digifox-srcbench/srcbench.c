/*
 * digifox-srcbench — how much CPU does the Fox need to replace the AK4137?
 *
 * Measures, on one core, the share of real time needed to
 *   - resample PCM stereo 44.1 … 768 kHz to 192 kHz with soxr (HQ and VHQ),
 *     32-bit integer in and out, as the ALSA path would do;
 *   - convert DSD64 … DSD512 to PCM: FIR decimation to 352.8 kHz
 *     (table lookup per 8 DSD bits) + soxr 352.8 -> 192 kHz.
 * 100 % = one core fully busy in real time. The player itself (Qobuz,
 * Roon, ...) and the system need their own share on top, so for steady
 * playback a figure below ~50 % is comfortable.
 *
 *   digifox-srcbench            all tests, 8 s of audio each
 *   digifox-srcbench -s 3       3 s of audio each (faster, less precise)
 *
 * GPL-2.0-or-later. DigiFox.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <soxr.h>

#define OUT_RATE   192000.0
#define CH         2
#define BLOCK      4096          /* input frames per soxr_process call */

static double cpu_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static double wall_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ PCM */

/* test signal: two sines and a little noise, full 32-bit range */
static void fill_pcm(int32_t *b, size_t frames, double rate, size_t *phase)
{
    for (size_t i = 0; i < frames; i++, (*phase)++) {
        double t = (double)*phase / rate;
        double s = 0.45 * sin(2 * M_PI * 1000.0 * t) + 0.25 * sin(2 * M_PI * 7351.0 * t)
                 + 0.02 * ((double)rand() / RAND_MAX - 0.5);
        int32_t v = (int32_t)(s * 2147483647.0);
        b[i * CH] = v;
        b[i * CH + 1] = -v;
    }
}

/* returns % of real time, or -1 on error */
static double bench_pcm(double in_rate, unsigned long quality, double seconds, const char **engine)
{
    soxr_error_t err = NULL;
    soxr_io_spec_t io = soxr_io_spec(SOXR_INT32_I, SOXR_INT32_I);
    soxr_quality_spec_t q = soxr_quality_spec(quality, 0);
    soxr_runtime_spec_t rt = soxr_runtime_spec(1);
    soxr_t s = soxr_create(in_rate, OUT_RATE, CH, &err, &io, &q, &rt);
    if (!s || err) { fprintf(stderr, "soxr_create: %s\n", err ? err : "?"); return -1; }
    *engine = soxr_engine(s);

    size_t out_cap = (size_t)(BLOCK * OUT_RATE / in_rate) + 256;
    int32_t *in = malloc(sizeof(int32_t) * CH * BLOCK);
    int32_t *out = malloc(sizeof(int32_t) * CH * out_cap);
    size_t total = (size_t)(in_rate * seconds), done = 0, phase = 0;

    /* the test signal is generated outside the timed part */
    int32_t *sig = malloc(sizeof(int32_t) * CH * BLOCK * 8);
    fill_pcm(sig, BLOCK * 8, in_rate, &phase);

    double cpu = 0;
    unsigned blk = 0;
    while (done < total) {
        memcpy(in, sig + (size_t)(blk++ % 8) * BLOCK * CH, sizeof(int32_t) * CH * BLOCK);
        size_t idone = 0, odone = 0, n = BLOCK;
        double c0 = cpu_now();
        while (n) {
            soxr_process(s, in + (BLOCK - n) * CH, n, &idone, out, out_cap, &odone);
            if (!idone) break;
            n -= idone;
        }
        cpu += cpu_now() - c0;
        done += BLOCK;
    }
    soxr_delete(s);
    free(in); free(out); free(sig);
    return 100.0 * cpu / seconds;
}

/* ------------------------------------------------------------------ DSD */

/* DSD -> PCM 352.8 kHz by FIR decimation. Taps grouped by 8: for each group
 * a 256-entry table holds the sum of the 8 taps for every bit pattern, so one
 * output sample costs taps/8 lookups. Low-pass at ~80 kHz, Kaiser window. */
typedef struct {
    int taps, groups, decim_bytes;
    float *lut;             /* groups * 256 */
    uint8_t *hist[CH];      /* last `groups` bytes per channel, ring */
    int pos;
} dsd_dec_t;

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}

static void dsd_init(dsd_dec_t *d, int dsd_mult)
{
    double fs = 2822400.0 * dsd_mult / 64.0;          /* DSD bit rate */
    int decim = (int)(fs / 352800.0 + 0.5);            /* 8, 16, 32, 64 */
    d->taps = 16 * decim;
    d->groups = d->taps / 8;
    d->decim_bytes = decim / 8;
    d->lut = calloc((size_t)d->groups * 256, sizeof(float));
    double *h = malloc(sizeof(double) * d->taps), sum = 0, beta = 8.0, fc = 80000.0 / fs;
    for (int i = 0; i < d->taps; i++) {
        double m = i - (d->taps - 1) / 2.0, r = 2.0 * i / (d->taps - 1) - 1.0;
        double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        h[i] = sinc * bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
        sum += h[i];
    }
    for (int g = 0; g < d->groups; g++)
        for (int b = 0; b < 256; b++) {
            double acc = 0;
            for (int k = 0; k < 8; k++)                /* MSB first, as in DSD */
                acc += ((b >> (7 - k)) & 1 ? 1.0 : -1.0) * h[g * 8 + k] / sum;
            d->lut[g * 256 + b] = (float)acc;
        }
    free(h);
    for (int c = 0; c < CH; c++) d->hist[c] = calloc((size_t)d->groups, 1);
    d->pos = 0;
}

static void dsd_free(dsd_dec_t *d)
{
    free(d->lut);
    for (int c = 0; c < CH; c++) free(d->hist[c]);
}

/* in: planar DSD bytes per channel, `nbytes` each; out: interleaved float */
static size_t dsd_run(dsd_dec_t *d, uint8_t *const in[CH], size_t nbytes, float *out)
{
    size_t n = 0;
    for (size_t i = 0; i + d->decim_bytes <= nbytes; i += d->decim_bytes) {
        for (int k = 0; k < d->decim_bytes; k++) {
            for (int c = 0; c < CH; c++) d->hist[c][d->pos] = in[c][i + k];
            if (++d->pos == d->groups) d->pos = 0;
        }
        for (int c = 0; c < CH; c++) {
            const uint8_t *hb = d->hist[c];
            const float *lut = d->lut;
            float acc = 0;
            int p = d->pos;
            for (int g = 0; g < d->groups; g++) {
                acc += lut[g * 256 + hb[p]];
                if (++p == d->groups) p = 0;
            }
            out[n * CH + c] = acc;
        }
        n++;
    }
    return n;
}

static double bench_dsd(int mult, unsigned long quality, double seconds, const char **engine)
{
    dsd_dec_t d;
    dsd_init(&d, mult);

    soxr_error_t err = NULL;
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT32_I);
    soxr_quality_spec_t q = soxr_quality_spec(quality, 0);
    soxr_runtime_spec_t rt = soxr_runtime_spec(1);
    soxr_t s = soxr_create(352800.0, OUT_RATE, CH, &err, &io, &q, &rt);
    if (!s || err) { fprintf(stderr, "soxr_create: %s\n", err ? err : "?"); dsd_free(&d); return -1; }
    *engine = soxr_engine(s);

    double byte_rate = 2822400.0 * mult / 64.0 / 8.0;     /* per channel */
    size_t chunk = 8192;                                  /* bytes per channel per step */
    uint8_t *in[CH];
    srand(1);
    for (int c = 0; c < CH; c++) {                        /* random-ish 1-bit stream */
        in[c] = malloc(chunk);
        for (size_t i = 0; i < chunk; i++) in[c][i] = (uint8_t)(rand() & 0xFF);
    }
    size_t pcm_cap = chunk / d.decim_bytes + 16;
    float *pcm = malloc(sizeof(float) * CH * pcm_cap);
    size_t out_cap = (size_t)(pcm_cap * OUT_RATE / 352800.0) + 256;
    int32_t *out = malloc(sizeof(int32_t) * CH * out_cap);

    size_t total = (size_t)(byte_rate * seconds), done = 0;
    double cpu = 0;
    while (done < total) {
        double c0 = cpu_now();
        size_t np = dsd_run(&d, in, chunk, pcm), off = 0;
        while (off < np) {
            size_t idone = 0, odone = 0;
            soxr_process(s, pcm + off * CH, np - off, &idone, out, out_cap, &odone);
            if (!idone) break;
            off += idone;
        }
        cpu += cpu_now() - c0;
        done += chunk;
    }
    soxr_delete(s);
    dsd_free(&d);
    for (int c = 0; c < CH; c++) free(in[c]);
    free(pcm); free(out);
    return 100.0 * cpu / seconds;
}

/* ------------------------------------------------------------------ main */

static void row(const char *name, double hq, double vhq)
{
    printf("  %-12s %7.1f %%   %7.1f %%   %s\n", name, hq, vhq,
           vhq < 0 ? "ошибка" : vhq < 35 ? "легко" : vhq < 60 ? "можно" : hq < 60 ? "только HQ" : hq < 85 ? "на грани" : "не хватит");
}

int main(int argc, char **argv)
{
    double sec = 8;
    if (argc > 2 && strcmp(argv[1], "-s") == 0) sec = atof(argv[2]);
    if (sec < 1) sec = 1;

    const char *eng = "?";
    long mhz = 0;
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f) { if (fscanf(f, "%ld", &mhz) == 1) mhz /= 1000; fclose(f); }

    printf("DigiFox: пересчёт в 192 кГц вместо AK4137 (стерео, %g с звука на тест)\n", sec);
    printf("Процессор: %ld ядро(а), %ld МГц\n", sysconf(_SC_NPROCESSORS_ONLN), mhz);
    printf("Загрузка одного ядра (100 %% = всё время ядра):\n\n");
    printf("  %-12s %9s   %9s\n", "вход", "soxr HQ", "soxr VHQ");

    static const double rates[] = { 44100, 48000, 88200, 96000, 176400, 352800, 384000, 705600, 768000 };
    double w0 = wall_now();
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        char name[16];
        snprintf(name, sizeof name, "%.1f kHz", rates[i] / 1000);
        double hq = bench_pcm(rates[i], SOXR_HQ, sec, &eng);
        double vhq = bench_pcm(rates[i], SOXR_VHQ, sec, &eng);
        row(name, hq, vhq);
        fflush(stdout);
    }
    static const int dsd[] = { 64, 128, 256, 512 };
    for (unsigned i = 0; i < sizeof dsd / sizeof dsd[0]; i++) {
        char name[16];
        snprintf(name, sizeof name, "DSD%d", dsd[i]);
        double hq = bench_dsd(dsd[i], SOXR_HQ, sec, &eng);
        double vhq = bench_dsd(dsd[i], SOXR_VHQ, sec, &eng);
        row(name, hq, vhq);
        fflush(stdout);
    }
    printf("\nДвижок soxr: %s. Время теста: %.0f с.\n", eng, wall_now() - w0);
    printf("192 кГц не пересчитывается (идёт как есть). DSD: КИХ-фильтр до 352,8 кГц + soxr.\n");
    return 0;
}
