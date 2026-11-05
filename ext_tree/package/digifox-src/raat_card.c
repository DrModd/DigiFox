/*
 * raat_card.so — LD_PRELOAD for Roon Ready (raat_app) in the "ФОКС" mode.
 *
 * raat_app asks the output PCM for its sound card (snd_pcm_info_get_card)
 * and builds "hw:<card>" from it to read the card info and probe formats.
 * The DigiFox sample-rate converter is an ALSA ioplug, and ioplugs report
 * card -1, so raat_app found no formats and aborted when Roon connected.
 * The converter always sits on card 0 (the RV1106 I2S), so say so.
 *
 * And snd_pcm_hw_params_any() on a plugin returns 1 ("params narrowed",
 * normal in alsa-lib), which raat_app takes for an error ("Operation not
 * permitted (1)") and probes no formats: report success as 0.
 *
 * Режим АК (DIGIFOX_DSD_MAX=256 от S95roonready): AK4137 принимает DSD максимум
 * DSD256, на DSD512 усилитель молчит. raat_app узнаёт форматы через
 * snd_pcm_hw_params_test_rate(): DSD-частоты выше DSD256 отвечаем «нет» —
 * Roon сам понижает DSD512 до DSD256 (как настройка Max DSD Rate).
 *
 * GPL-2.0-or-later. DigiFox.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

int snd_pcm_info_get_card(const snd_pcm_info_t *info)
{
    static int (*real)(const snd_pcm_info_t *);
    if (!real) real = (int (*)(const snd_pcm_info_t *))dlsym(RTLD_NEXT, "snd_pcm_info_get_card");
    int c = real ? real(info) : -1;
    return c < 0 ? 0 : c;
}

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    static int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *);
    if (!real) real = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *))dlsym(RTLD_NEXT, "snd_pcm_hw_params_any");
    int r = real ? real(pcm, params) : -ENOSYS;
    return r > 0 ? 0 : r;
}

/* Самая высокая частота кадров ALSA для DSD256 в данном формате, 0 — не DSD */
static unsigned int dsd256_rate(snd_pcm_format_t f)
{
    switch (f) {
    case SND_PCM_FORMAT_DSD_U8:     return 1411200;
    case SND_PCM_FORMAT_DSD_U16_LE:
    case SND_PCM_FORMAT_DSD_U16_BE: return 705600;
    case SND_PCM_FORMAT_DSD_U32_LE:
    case SND_PCM_FORMAT_DSD_U32_BE: return 352800;
    default:                        return 0;
    }
}

static int dsd_capped(const snd_pcm_hw_params_t *params, unsigned int val)
{
    static int max = -1;
    if (max < 0) {
        const char *e = getenv("DIGIFOX_DSD_MAX");
        max = e && e[0] == '2' && e[1] == '5' && e[2] == '6';
    }
    if (!max) return 0;
    snd_pcm_format_t f;
    if (snd_pcm_hw_params_get_format(params, &f) < 0) return 0;   /* формат ещё не выбран */
    unsigned int top = dsd256_rate(f);
    /* с запасом на семейство 48 кГц: DSD256/48 = 384000 (U32) */
    return top && val > top + top / 10;
}

int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
    static int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int);
    if (!real) real = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int))dlsym(RTLD_NEXT, "snd_pcm_hw_params_test_rate");
    if (dsd_capped(params, val)) return -EINVAL;
    return real ? real(pcm, params, val, dir) : -ENOSYS;
}

int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
    static int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int);
    if (!real) real = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int))dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_rate");
    if (dsd_capped(params, val)) return -EINVAL;
    return real ? real(pcm, params, val, dir) : -ENOSYS;
}
