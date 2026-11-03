/* VCPE audio: software mixer (ADPCM SFX voices + streamed ADPCM music). Games put their own
 * sound policy (which voice plays what, volume curves) on top of these primitives. */
#ifndef VCPE_AUDIO_H
#define VCPE_AUDIO_H
#include <stdint.h>

#define VCPE_MAX_VOICES 32
/* the converters write this table layout (name, PAK offset, bytes, samples) */
typedef struct { const char *name; uint32_t off, size, samples; } VcpeSound;

int vcpe_audio_init(const char *music_pak, int voices, uint32_t sfx_off, uint32_t sfx_size,
                    const VcpeSound *sfx, int sfx_n, const VcpeSound *music);
void vcpe_audio_shutdown(void);

/* voices: start returns a handle (generation << 8 | voice) or -1 */
int vcpe_voice_start(int voice, int sfx_id, float pitch, int loop);
void vcpe_voice_stop(int voice);
int vcpe_voice_active(int voice);
void vcpe_voice_pause(int voice, int on);
void vcpe_voice_pitch(int voice, float pitch);
void vcpe_voice_gain(int voice, int gain256);
int vcpe_voice_count(void);
int vcpe_handle_voice(int handle);     /* the voice still playing that handle's sound, or -1 */
void vcpe_voices_stop_all(void);
/* play sound id on a free voice in [first, last] (a voice that is not playing, else the one started longest ago);
 * returns a handle for vcpe_handle_voice, -1 when id is invalid. Reserve the other voices for music-like loops. */
int vcpe_sfx_play(int id, float pitch, int gain256, int first, int last);

/* music: play fades the current track out, then starts the new one (-1: silence) */
void vcpe_music_play(int track);
int vcpe_music_current(void);
void vcpe_music_stop_now(void);
void vcpe_music_pause(int on);

/* 5.6 sound residency (opt-in). Instead of reading every effect at boot, pass sfx_size 0 to vcpe_audio_init, then
 * vcpe_sfx_slab(bytes) once (one fixed block, the size of the biggest set: no heap fragmentation) and, behind the loading
 * screen of each level, vcpe_sfx_keep(set) with one bit per effect id (bit i of set[i / 8]). Effects outside the set
 * are dropped unless a voice is still playing them; missing ones are read from the PAK. If the gaps run out, voices
 * playing sounds outside the set are stopped and the slab is packed. A voice started on an effect that is not resident
 * reads it on the spot (a short hitch) and counts a miss (report with vcpe_sfx_miss_hook). */
int vcpe_sfx_slab(uint32_t bytes);
int vcpe_sfx_keep(const uint8_t *set);          /* 0 ok, -1 some effects did not fit or could not be read */
uint32_t vcpe_sfx_resident_kb(void);
uint32_t vcpe_sfx_slab_kb(void);
int vcpe_sfx_misses(void);
int vcpe_sfx_resident(int id);
void vcpe_sfx_miss_hook(void (*report)(int id, const char *name));

void vcpe_sfx_gain(int gain256);
void vcpe_music_gain(int gain256);
void vcpe_music_scale(int num, int den);   /* extra music factor: ducking, muffling (1/1 = none) */
void vcpe_music_lowpass(float hz);         /* music through a 6 dB/octave low-pass at hz (0: off), e.g. heard through a wall */
uint32_t vcpe_audio_ms(void);              /* audio time mixed so far, in ms */
/* 4.5: called by the mixer thread once per output block to add stereo samples (lr[2 * frames], zeroed, 44.1 kHz,
 * mixed after the music and effect gains). vcpe_stream installs itself here. */
extern void (*vcpe_audio_mix_hook)(int *lr, int frames);

#endif
