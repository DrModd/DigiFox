/*
 * dsd2pcm — DSD64..DSD512 to PCM 352.8 kHz, multi-stage. See dsd2pcm.h.
 * GPL-2.0-or-later. DigiFox.
 */
#include "dsd2pcm.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PASS_HZ   96000.0      /* must stay clean: 0..96 kHz (192 kHz output)  */
#define ATTEN_DB  100.0
#define MAX_HB    3
#define CHUNK     4096         /* symbols / samples processed per inner step  */

typedef struct {
    int    q;                  /* pairs of non-zero side taps                 */
    int    c;                  /* centre index = 2q+1                         */
    float *a;                  /* a[j]: tap at centre +-(2j+1)                */
    float *buf;                /* history (2c) + CHUNK input samples          */
    int    have;               /* samples in buf after the history            */
    int    odd;                /* phase: next output after one more input     */
} halfband_t;

struct dsd2pcm {
    int     mult;              /* 64, 128, 256, 512                           */
    int     sbits;             /* bits per lookup: 4 or 8                     */
    int     groups;            /* lookups per output                          */
    float  *lut;               /* groups x (1 << sbits)                       */
    uint8_t *sym;              /* history (groups-1) + CHUNK symbols          */
    float  *s1;                /* stage-1 output for one chunk                */
    int     nhb;
    halfband_t hb[MAX_HB];
};

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}

static double kaiser_beta(double a)
{
    return a > 50 ? 0.1102 * (a - 8.7) : 0.5842 * pow(a - 21, 0.4) + 0.07886 * (a - 21);
}

/* windowed-sinc low-pass, fc as a fraction of the sample rate, DC gain 1 */
static void design_lp(double *h, int n, double fc, double beta)
{
    double sum = 0;
    for (int i = 0; i < n; i++) {
        double m = i - (n - 1) / 2.0, r = 2.0 * i / (n - 1) - 1.0;
        double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        h[i] = s * bessel_i0(beta * sqrt(fmax(0, 1 - r * r))) / bessel_i0(beta);
        sum += h[i];
    }
    for (int i = 0; i < n; i++) h[i] /= sum;
}

static int hb_init(halfband_t *b, double rin)
{
    /* transition: PASS_HZ .. rin/2 - PASS_HZ (only that part aliases into 0..96k) */
    double dw = (rin / 2 - 2 * PASS_HZ) / rin;
    int n = (int)ceil((ATTEN_DB - 8) / (2.285 * 2 * M_PI * dw)) + 1;
    int q = (n - 3 + 3) / 4;                      /* length 4q+3 >= n */
    if (4 * q + 3 < n) q++;
    if (q < 1) q = 1;
    n = 4 * q + 3;
    double *h = malloc(sizeof(double) * n);
    if (!h) return -1;
    design_lp(h, n, 0.25, kaiser_beta(ATTEN_DB));
    b->q = q + 1;                                  /* side pairs at 1,3,...,2q+1 */
    b->c = (n - 1) / 2;
    b->a = malloc(sizeof(float) * b->q);
    b->buf = calloc((size_t)2 * b->c + CHUNK + 2, sizeof(float));
    if (!b->a || !b->buf) { free(h); return -1; }
    /* centre tap normalised to exactly 0.5, side taps so that the sum is 1 */
    double side = 0;
    for (int j = 0; j < b->q; j++) side += 2 * h[b->c + 2 * j + 1];
    for (int j = 0; j < b->q; j++) b->a[j] = (float)(h[b->c + 2 * j + 1] * 0.5 / side);
    b->have = 0;
    b->odd = 0;
    free(h);
    return 0;
}

/* in -> out at half rate; returns outputs. Processes in pieces of CHUNK. */
static size_t hb_run(halfband_t *b, const float *in, size_t n, float *out)
{
    size_t o = 0;
    const int H = 2 * b->c;                        /* history length */
    while (n) {
        size_t take = n > CHUNK ? CHUNK : n;
        memcpy(b->buf + H + b->have, in, take * sizeof(float));
        int total = b->have + (int)take;
        int i = b->odd ? 0 : 1;                    /* index (after history) of the newest sample */
        for (; i < total; i += 2) {
            const float *x = b->buf + H + i - b->c;   /* centre sample */
            float acc = 0.5f * x[0];
            for (int j = 0; j < b->q; j++)
                acc += b->a[j] * (x[-(2 * j + 1)] + x[2 * j + 1]);
            out[o++] = acc;
        }
        /* keep the last H samples as history; output phase continues */
        b->odd = (i - total) == 0 ? 1 : 0;
        memmove(b->buf, b->buf + total, H * sizeof(float));
        b->have = 0;
        in += take;
        n -= take;
    }
    return o;
}

dsd2pcm_t *dsd2pcm_new(int mult)
{
    if (mult != 64 && mult != 128 && mult != 256 && mult != 512) return NULL;
    dsd2pcm_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    double fd = 44100.0 * mult;
    d->mult = mult;
    d->sbits = mult == 64 ? 4 : 8;
    double r1 = fd / d->sbits;                     /* 705.6k .. 2822.4k */
    d->groups = mult == 64 ? 9 : mult == 128 ? 9 : mult == 256 ? 8 : 7;
    int ntap = d->groups * d->sbits, nsym = 1 << d->sbits;

    double *h = malloc(sizeof(double) * ntap);
    d->lut = malloc(sizeof(float) * d->groups * nsym);
    d->sym = calloc((size_t)d->groups - 1 + CHUNK, 1);
    d->s1 = malloc(sizeof(float) * CHUNK);
    if (!h || !d->lut || !d->sym || !d->s1) { free(h); dsd2pcm_free(d); return NULL; }
    /* cut-off in the middle of 96 kHz .. r1-96 kHz */
    design_lp(h, ntap, (r1 / 2) / fd, kaiser_beta(ATTEN_DB));
    for (int g = 0; g < d->groups; g++)
        for (int s = 0; s < nsym; s++) {
            double acc = 0;
            for (int j = 0; j < d->sbits; j++)     /* bit j (from LSB) = tap g*S+j */
                acc += ((s >> j) & 1 ? 1.0 : -1.0) * h[g * d->sbits + j];
            d->lut[g * nsym + s] = (float)acc;
        }
    free(h);
    /* idle DSD pattern 0x69 as history: silence instead of a click */
    memset(d->sym, mult == 64 ? 0x9 : 0x69, d->groups - 1);

    double r = r1;
    while (r > DSD2PCM_RATE + 1 && d->nhb < MAX_HB) {
        if (hb_init(&d->hb[d->nhb], r)) { dsd2pcm_free(d); return NULL; }
        d->nhb++;
        r /= 2;
    }
    return d;
}

void dsd2pcm_free(dsd2pcm_t *d)
{
    if (!d) return;
    for (int i = 0; i < d->nhb; i++) { free(d->hb[i].a); free(d->hb[i].buf); }
    free(d->lut); free(d->sym); free(d->s1);
    free(d);
}

double dsd2pcm_bytes_per_frame(const dsd2pcm_t *d)
{
    return 44100.0 * d->mult / 8.0 / DSD2PCM_RATE;
}

/* stage 1 for `m` new symbols already placed after the history */
static void stage1(dsd2pcm_t *d, int m)
{
    const int G = d->groups, nsym = 1 << d->sbits;
    const uint8_t *s = d->sym + (G - 1);
    const float *lut = d->lut;
    float *o = d->s1;
    for (int i = 0; i < m; i++) {
        const uint8_t *p = s + i;
        float acc = 0;
        for (int g = 0; g < G; g++) acc += lut[g * nsym + p[-g]];
        o[i] = acc;
    }
    memmove(d->sym, d->sym + m, (size_t)(G - 1));
}

size_t dsd2pcm_run(dsd2pcm_t *d, const uint8_t *in, size_t n, float *out, int ostride)
{
    size_t written = 0;
    const int G = d->groups;
    float tmp_a[CHUNK], tmp_b[CHUNK];

    while (n) {
        /* symbols for this pass */
        int m;
        uint8_t *dst = d->sym + (G - 1);
        if (d->sbits == 4) {
            size_t take = n > CHUNK / 2 ? CHUNK / 2 : n;
            for (size_t i = 0; i < take; i++) { dst[2 * i] = in[i] >> 4; dst[2 * i + 1] = in[i] & 15; }
            m = (int)take * 2; in += take; n -= take;
        } else {
            size_t take = n > CHUNK ? CHUNK : n;
            memcpy(dst, in, take);
            m = (int)take; in += take; n -= take;
        }
        stage1(d, m);

        /* half-band chain */
        const float *x = d->s1;
        size_t k = (size_t)m;
        float *bufs[2] = { tmp_a, tmp_b };
        for (int st = 0; st < d->nhb; st++) {
            float *y = bufs[st & 1];
            k = hb_run(&d->hb[st], x, k, y);
            x = y;
        }
        for (size_t i = 0; i < k; i++) out[(written + i) * ostride] = x[i];
        written += k;
    }
    return written;
}
