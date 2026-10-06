/* Mixer for the Awaria port: Manager's long / paused / short sound pools, streamed music. */
#ifndef AW_AUDIO_H
#define AW_AUDIO_H
int audio_init(const char *music_pak);
void audio_shutdown(void);
int snd_long(int id);                /* Manager.PlayLongSound */
int snd_paused(int id);              /* Manager.PlayPausedSound */
int snd_short(int id, int priority); /* Manager.PlayShortSound -> handle (-1) */
int snd_short_common(int id);        /* PlayShortCommonSound (10 ms rate limit) */
int snd_short_pitch(int id, float pitch);
void snd_stop(int handle);           /* Object.Destroy(source.gameObject) */
int snd_playing(int handle);
void snd_pause_short(int on);        /* Manager.AudioPause */
void snd_remove_all(void);           /* AudioRemoveAll */
void snd_background(int enabled);    /* backgroundSoundDisabled = !enabled */
int snd_background_disabled(void);
void snd_stop_all(void);
void music_play(int track);          /* SongChanger (fade out, then the new track) */
int music_current(void);
void music_stop_now(void);
void music_set_volume10(float v);    /* 0..10 */
void sfx_set_volume10(float v);
void music_muffle(int muffle);       /* MusicMuffle */
#endif
