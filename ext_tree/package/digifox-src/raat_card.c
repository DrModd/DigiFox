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
 * GPL-2.0-or-later. DigiFox.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
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
