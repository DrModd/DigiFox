/*
 * dsd2pcm — DSD64..DSD512 to PCM 352.8 kHz, multi-stage. See dsd2pcm.h.
 *
 * Tuned for the in-order Cortex-A7 of the RV1106:
 *   - stage 1 sums integer tables (Q28), one table at a time over a whole
 *     chunk (see stage1), so the table stays in L1 and nothing stalls;
 *   - half-bands run on the polyphase split: all side taps of a half-band
 *     fall on the odd phase, so each output is one contiguous dot product
 *     that the compiler turns into NEON (-O3 -ffast-math -mfpu=neon).
 *
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
#define LUT_SHIFT 28           /* stage-1 tables in Q28                       */

typedef struct {
    int    Q;                  /* side taps per side: offsets 1,3,..,2Q-1     */
    float *b;                  /* 2Q taps on the odd phase, symmetric         */
    float *e;                  /* even phase: history Q-1 ... + new           */
    float *o;                  /* odd phase:  history 2Q-1 ... + new          */
    float  pend;               /* even sample waiting for its odd partner     */
    int    has_pend;
} halfband_t;

struct dsd2pcm {
    int      mult;             /* 64, 128, 256, 512                           */
    int      groups;           /* lookups (bytes) per stage-1 output          */
    int      dbytes;           /* bytes per stage-1 output (1)                */
    int32_t *lut;              /* groups x 256, Q28                           */
    uint8_t *sym;              /* history (groups-1) + CHUNK bytes            */
    float   *s1;               /* stage-1 output for one chunk                */
    int32_t *acc;              /* stage-1 integer accumulators (CHUNK)        */
    int      nhb;
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

/* ------------------------------------------------------------ half-band */

static int hb_init(halfband_t *b, double rin)
{
    /* transition: PASS_HZ .. rin/2 - PASS_HZ (only that part aliases into 0..96k) */
    double dw = (rin / 2 - 2 * PASS_HZ) / rin;
    int n = (int)ceil((ATTEN_DB - 8) / (2.285 * 2 * M_PI * dw)) + 1;
    int q = (n - 3 + 3) / 4;
    if (4 * q + 3 < n) q++;
    if (q < 1) q = 1;
    n = 4 * q + 3;                                 /* length 4q+3, centre c = 2q+1 */
    double *h = malloc(sizeof(double) * n);
    if (!h) return -1;
    design_lp(h, n, 0.25, kaiser_beta(ATTEN_DB));
    int c = 2 * q + 1;
    b->Q = q + 1;                                  /* side offsets 1,3,...,2q+1 */
    b->b = malloc(sizeof(float) * 2 * b->Q);
    b->e = calloc((size_t)b->Q + CHUNK, sizeof(float));
    b->o = calloc((size_t)2 * b->Q + CHUNK, sizeof(float));
    if (!b->b || !b->e || !b->o) { free(h); return -1; }
    /* centre tap exactly 0.5, side taps scaled so that DC gain is exactly 1 */
    double side = 0;
    for (int j = 0; j < b->Q; j++) side += 2 * h[c + 2 * j + 1];
    for (int j = 0; j < b->Q; j++) {
        float a = (float)(h[c + 2 * j + 1] * 0.5 / side);
        b->b[b->Q - 1 - j] = a;                    /* offset -(2j+1) */
        b->b[b->Q + j] = a;                        /* offset +(2j+1) */
    }
    b->has_pend = 0;
    free(h);
    return 0;
}

/* Input x[t]; E[k] = x[2k], O[k] = x[2k+1]. Output m (when O[m] arrives):
 *   y[m] = 0.5 * E[m - (Q-1)] + sum_{i<2Q} b[i] * O[m - 2Q + 1 + i]   */
static size_t hb_run(halfband_t *b, const float *in, size_t n, float *out)
{
    const int Q = b->Q, HE = Q - 1, HO = 2 * Q - 1;
    size_t o = 0;
    while (n) {
        /* split into phases, at most CHUNK pairs per pass */
        int k = 0;
        float *E = b->e + HE, *O = b->o + HO;
        if (b->has_pend && n) { E[0] = b->pend; O[0] = *in++; n--; k = 1; b->has_pend = 0; }
        while (n >= 2 && k < CHUNK) { E[k] = in[0]; O[k] = in[1]; in += 2; n -= 2; k++; }
        if (n == 1 && k < CHUNK) { b->pend = *in++; n--; b->has_pend = 1; }

        const float *bb = b->b;
        for (int m = 0; m < k; m++) {
            const float *op = O + m - 2 * Q + 1;
            float acc = 0;
            for (int i = 0; i < 2 * Q; i++) acc += bb[i] * op[i];
            out[o++] = acc + 0.5f * E[m - (Q - 1)];
        }
        memmove(b->e, b->e + k, (size_t)HE * sizeof(float));
        memmove(b->o, b->o + k, (size_t)HO * sizeof(float));
    }
    return o;
}

/* --------------------------------------------------------------- stage 1 */

dsd2pcm_t *dsd2pcm_new(int mult)
{
    if (mult != 64 && mult != 128 && mult != 256 && mult != 512) return NULL;
    dsd2pcm_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    double fd = 44100.0 * mult;
    double r1 = fd / 8;                            /* 352.8k .. 2822.4k */
    d->mult = mult;
    d->dbytes = 1;
    /* DSD64 goes to 352.8 kHz in one step: transition 96..256.8 kHz needs a
     * long filter; above, the half-bands take over and stage 1 can be short */
    d->groups = mult == 64 ? 16 : mult == 128 ? 9 : mult == 256 ? 8 : 7;
    int ntap = d->groups * 8;

    double *h = malloc(sizeof(double) * ntap);
    d->lut = malloc(sizeof(int32_t) * d->groups * 256);
    d->sym = calloc((size_t)d->groups - 1 + CHUNK, 1);
    d->s1 = malloc(sizeof(float) * CHUNK);
    d->acc = malloc(sizeof(int32_t) * CHUNK);
    if (!h || !d->lut || !d->sym || !d->s1 || !d->acc) { free(h); dsd2pcm_free(d); return NULL; }
    /* cut-off in the middle of 96 kHz .. r1-96 kHz */
    design_lp(h, ntap, (r1 / 2) / fd, kaiser_beta(ATTEN_DB));
    for (int g = 0; g < d->groups; g++)
        for (int s = 0; s < 256; s++) {
            double acc = 0;
            for (int j = 0; j < 8; j++)            /* bit j (from LSB) = tap g*8+j */
                acc += ((s >> j) & 1 ? 1.0 : -1.0) * h[g * 8 + j];
            d->lut[g * 256 + s] = (int32_t)lrint(acc * (double)(1 << LUT_SHIFT));
        }
    free(h);
    /* idle DSD pattern 0x69 as history: silence instead of a click */
    memset(d->sym, 0x69, d->groups - 1);

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
    for (int i = 0; i < d->nhb; i++) { free(d->hb[i].b); free(d->hb[i].e); free(d->hb[i].o); }
    free(d->lut); free(d->sym); free(d->s1); free(d->acc);
    free(d);
}

double dsd2pcm_bytes_per_frame(const dsd2pcm_t *d)
{
    return 44100.0 * d->mult / 8.0 / DSD2PCM_RATE;
}

/* stage 1 for `m` new bytes already placed after the history.
 * Group-outer order: each pass reads one 1 KB table and adds it into an
 * integer accumulator row. The table stays hot in L1, the inner loop has no
 * dependency chain, so the in-order A7 can issue a lookup every few cycles.
 * (Sample-outer order touched all tables per sample: ~15 cycles per lookup.) */
static void stage1(dsd2pcm_t *d, int m)
{
    const int G = d->groups;
    const uint8_t *s = d->sym + (G - 1);
    int32_t *acc = d->acc;
    memset(acc, 0, sizeof(int32_t) * (size_t)m);
    for (int g = 0; g < G; g++) {
        const int32_t *L = d->lut + g * 256;
        const uint8_t *p = s - g;
        int i = 0;
        for (; i + 3 < m; i += 4) {
            acc[i]     += L[p[i]];
            acc[i + 1] += L[p[i + 1]];
            acc[i + 2] += L[p[i + 2]];
            acc[i + 3] += L[p[i + 3]];
        }
        for (; i < m; i++) acc[i] += L[p[i]];
    }
    const float scale = 1.0f / (float)(1 << LUT_SHIFT);
    float *o = d->s1;
    for (int i = 0; i < m; i++) o[i] = (float)acc[i] * scale;
    memmove(d->sym, d->sym + m, (size_t)(G - 1));
}

size_t dsd2pcm_run(dsd2pcm_t *d, const uint8_t *in, size_t n, float *out, int ostride)
{
    size_t written = 0;
    const int G = d->groups;
    float tmp_a[CHUNK], tmp_b[CHUNK];

    while (n) {
        size_t take = n > CHUNK ? CHUNK : n;
        memcpy(d->sym + (G - 1), in, take);
        in += take; n -= take;
        stage1(d, (int)take);

        /* half-band chain */
        const float *x = d->s1;
        size_t k = take;
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
