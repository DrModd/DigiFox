/*
 * raat_card.so — LD_PRELOAD for Roon Ready (raat_app) in the "ФОКС" mode.
 *
 * raat_app asks the output PCM for its sound card (snd_pcm_info_get_card)
 * and builds "hw:<card>" from it to read the card info and probe formats.
 * The DigiFox sample-rate converter is an ALSA ioplug, and ioplugs report
 * card -1, so raat_app found no formats and aborted when Roon connected.
 * The converter always sits on card 0 (the RV1106 I2S), so say so.
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
