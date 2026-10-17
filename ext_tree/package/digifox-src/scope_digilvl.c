/*
 * scope_digilvl — ALSA meter scope: levels for the amplifier's VU meters when
 * the players write straight to the I2S card (AK4137 does the rate conversion,
 * "digifox-srcmode ak4137").
 *
 * The stock "meter" PCM plugin passes the audio to the card untouched and calls
 * its scopes with what the card has just played (hardware position), so the
 * levels are in time with the sound by construction. Only PCM is measured
 * (through the stock "s16" scope); for DSD this scope stays disabled.
 *
 * asound.conf (written by digifox-srcmode when the amplifier shows VU meters):
 *   pcm_scope_type.digilvl { lib "/usr/lib/alsa-lib/libasound_scope_digilvl.so" }
 *   pcm.!default { type meter slave.pcm "hw:0,0" frequency 50 scopes.0 { type digilvl } }
 *
 * GPL-2.0-or-later. DigiFox.
 */
#include <errno.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include "dfx_level.h"

typedef struct {
    snd_pcm_t        *pcm;          /* the meter PCM */
    snd_pcm_scope_t  *s16;          /* stock pseudo scope: frames as int16 per channel */
    snd_pcm_uframes_t old;          /* position measured up to */
    dfx_lvl_t        *lvl;
    unsigned          rate, ch;
    int               on;
    int               checked;
} lv_t;

/* enable runs from the meter's thread start, before the PCM is fully set up:
 * the format is checked on the first update instead */
static int lv_enable(snd_pcm_scope_t *scope)
{
    lv_t *l = snd_pcm_scope_get_callback_private(scope);
    l->on = 0;
    l->checked = 0;
    if (!l->lvl) l->lvl = dfx_lvl_map();
    return l->lvl ? 0 : -EINVAL;
}

/* PCM only: the s16 scope cannot read DSD (and stays disabled then) */
static int lv_check(lv_t *l)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_format_t fmt;
    snd_pcm_hw_params_alloca(&hw);
    l->checked = 1;
    if (snd_pcm_hw_params_current(l->pcm, hw) < 0) return 0;
    if (snd_pcm_hw_params_get_format(hw, &fmt) < 0) return 0;
    if (!snd_pcm_format_linear(fmt) || snd_pcm_format_width(fmt) < 16) return 0;
    l->rate = snd_pcm_meter_get_rate(l->pcm);
    l->ch = snd_pcm_meter_get_channels(l->pcm);
    if (!l->rate || !l->ch) return 0;
    l->old = snd_pcm_meter_get_now(l->pcm);
    l->on = 1;
    return 1;
}

static void lv_disable(snd_pcm_scope_t *scope)
{
    lv_t *l = snd_pcm_scope_get_callback_private(scope);
    l->on = 0;
    l->checked = 0;
}

static void lv_start(snd_pcm_scope_t *scope) { (void)scope; }
static void lv_stop(snd_pcm_scope_t *scope)  { (void)scope; }

static void lv_reset(snd_pcm_scope_t *scope)
{
    lv_t *l = snd_pcm_scope_get_callback_private(scope);
    l->old = snd_pcm_meter_get_now(l->pcm);
}

static void lv_update(snd_pcm_scope_t *scope)
{
    lv_t *l = snd_pcm_scope_get_callback_private(scope);
    if (!l->checked && !lv_check(l)) return;
    if (!l->on) return;
    snd_pcm_uframes_t now = snd_pcm_meter_get_now(l->pcm);
    snd_pcm_uframes_t bufsize = snd_pcm_meter_get_bufsize(l->pcm);
    snd_pcm_uframes_t boundary = snd_pcm_meter_get_boundary(l->pcm);
    snd_pcm_sframes_t size = (snd_pcm_sframes_t)now - (snd_pcm_sframes_t)l->old;
    if (size < 0) size += boundary;
    if (size > (snd_pcm_sframes_t)bufsize) size = bufsize;
    if (size <= 0) return;
    int16_t *c0 = snd_pcm_scope_s16_get_channel_buffer(l->s16, 0);
    int16_t *c1 = snd_pcm_scope_s16_get_channel_buffer(l->s16, l->ch > 1 ? 1 : 0);
    snd_pcm_uframes_t off = l->old % bufsize;
    /* these frames have just been played: the newest one is heard now */
    int64_t t_end = dfx_now_ms();
    unsigned long blk = (unsigned long)l->rate * DFX_LVL_BLOCK_MS / 1000;
    if (!blk) blk = 1;
    unsigned long done = 0;
    while (done < (unsigned long)size) {
        unsigned long n = (unsigned long)size - done < blk ? (unsigned long)size - done : blk;
        double sum[2] = { 0, 0 };
        int pk[2] = { 0, 0 };
        for (unsigned long i = 0; i < n; i++) {
            snd_pcm_uframes_t o = (off + done + i) % bufsize;
            int v0 = c0[o], v1 = c1[o];
            sum[0] += (double)v0 * v0;
            sum[1] += (double)v1 * v1;
            if (v0 < 0) v0 = -v0;
            if (v1 < 0) v1 = -v1;
            if (v0 > pk[0]) pk[0] = v0;
            if (v1 > pk[1]) pk[1] = v1;
        }
        const double k2 = 1.0 / (32768.0 * 32768.0);
        float ms[2] = { (float)(sum[0] / n * k2), (float)(sum[1] / n * k2) };
        float pkv[2] = { pk[0] / 32768.0f, pk[1] / 32768.0f };
        int64_t t = t_end - (int64_t)(((unsigned long)size - done) * 1000UL / l->rate);
        dfx_lvl_put(l->lvl, t, ms, pkv);
        done += n;
    }
    l->old = now;
}

static void lv_close(snd_pcm_scope_t *scope)
{
    lv_t *l = snd_pcm_scope_get_callback_private(scope);
    if (l->lvl) munmap(l->lvl, sizeof(dfx_lvl_t));
    free(l);
}

static const snd_pcm_scope_ops_t lv_ops = {
    .enable  = lv_enable,
    .disable = lv_disable,
    .start   = lv_start,
    .stop    = lv_stop,
    .update  = lv_update,
    .reset   = lv_reset,
    .close   = lv_close,
};

int _snd_pcm_scope_digilvl_open(snd_pcm_t *pcm, const char *name,
                                snd_config_t *root, snd_config_t *conf)
{
    (void)root; (void)conf;
    snd_pcm_scope_t *s16 = snd_pcm_meter_search_scope(pcm, "s16");
    int err;
    if (!s16 && (err = snd_pcm_scope_s16_open(pcm, "s16", &s16)) < 0)
        return err;                         /* added before ours: updated first */
    lv_t *l = calloc(1, sizeof *l);
    if (!l) return -ENOMEM;
    l->pcm = pcm;
    l->s16 = s16;
    snd_pcm_scope_t *scope;
    if ((err = snd_pcm_scope_malloc(&scope)) < 0) { free(l); return err; }
    snd_pcm_scope_set_name(scope, name ? name : "digilvl");
    snd_pcm_scope_set_ops(scope, &lv_ops);
    snd_pcm_scope_set_callback_private(scope, l);
    return snd_pcm_meter_add_scope(pcm, scope);
}
