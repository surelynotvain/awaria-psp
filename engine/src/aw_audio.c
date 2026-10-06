/* Awaria's sound policy (Manager.cs) on the VCPE mixer. Voices:
 * 0..2   Manager.audioLongArray   (ring of 3: a new long sound cuts the oldest)
 * 3..5   Manager.audioShortArray  (PlayPausedSound: plays while the game is paused)
 * 6..    PlayShortSound instances (pausable as a group, stoppable by handle)       */
#include <stdint.h>
#include "aw_audio.h"
#include "awdata.h"
#include "vcpe.h"

#define VOICES 26
#define LONG0 0
#define PAUSED0 3
#define SHORT0 6
static int next_long, next_paused, next_short;
static volatile int bg_disabled;
static uint32_t common_until;

int audio_init(const char *music_pak)
{
    int r = vcpe_audio_init(music_pak, VOICES, SFX_OFF, g_sfx[NUM_SFX - 1].off + g_sfx[NUM_SFX - 1].size,
                            (const VcpeSound *)g_sfx, NUM_SFX, (const VcpeSound *)g_music);
    vcpe_sfx_gain(179);
    vcpe_music_gain(128);
    return r;
}

void audio_shutdown(void) { vcpe_audio_shutdown(); }

int snd_long(int id)
{
    int vi = LONG0 + next_long;
    next_long = (next_long + 1) % 3;
    return vcpe_voice_start(vi, id, 1.0f, 0);
}

int snd_paused(int id)
{
    int vi = PAUSED0 + next_paused;
    next_paused = (next_paused + 1) % 3;
    return vcpe_voice_start(vi, id, 1.0f, 0);
}

int snd_short(int id, int priority)
{
    if (bg_disabled && priority == 0)
        return -1;
    /* oldest finished voice first, else round robin */
    int vi = -1;
    for (int k = 0; k < VOICES - SHORT0; k++) {
        int c = SHORT0 + (next_short + k) % (VOICES - SHORT0);
        if (!vcpe_voice_active(c)) { vi = c; break; }
    }
    if (vi < 0)
        vi = SHORT0 + next_short;
    next_short = (vi - SHORT0 + 1) % (VOICES - SHORT0);
    return vcpe_voice_start(vi, id, 1.0f, 0);
}

int snd_short_common(int id)
{
    /* PlayShortCommonSound: at most one per 10 ms (one mixer chunk) */
    if (bg_disabled || (int32_t)(vcpe_audio_ms() - common_until) < 0)
        return -1;
    common_until = vcpe_audio_ms() + 10;
    return snd_short(id, 0);
}

int snd_short_pitch(int id, float pitch)
{
    int h = snd_short(id, 0);
    if (h >= 0)
        vcpe_voice_pitch(h & 0xFF, pitch);
    return h;
}

void snd_stop(int h) { vcpe_voice_stop(vcpe_handle_voice(h)); }
int snd_playing(int h) { int vi = vcpe_handle_voice(h); return vi >= 0 && vcpe_voice_active(vi); }

void snd_pause_short(int on)
{
    for (int i = SHORT0; i < VOICES; i++)
        vcpe_voice_pause(i, on);
}

void snd_remove_all(void)
{
    /* Manager.AudioRemoveAll: forget the short sounds (they keep playing) and block new background sounds */
    bg_disabled = 1;
}

void snd_background(int enabled) { bg_disabled = !enabled; }
int snd_background_disabled(void) { return bg_disabled; }
void snd_stop_all(void) { vcpe_voices_stop_all(); }

void music_play(int track) { vcpe_music_play(track); }
int music_current(void) { return vcpe_music_current(); }
void music_stop_now(void) { vcpe_music_stop_now(); }

/* Unity mixer: 20*log10(v) dB == linear amplitude v (settings are 0..10, /10) */
void music_set_volume10(float v) { vcpe_music_gain((int)(v * 25.6f)); }
void sfx_set_volume10(float v) { vcpe_sfx_gain((int)(v * 25.6f)); }
/* MusicMuffle(muffle): music volume = setting / muffle (normal = /10) */
void music_muffle(int muffle) { vcpe_music_scale(muffle > 0 ? (int)(256.0f * 10.0f / muffle) : 256, 256); }
