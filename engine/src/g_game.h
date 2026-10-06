/* Game scripts ported from recon/decomp: shared state and cross-script calls. */
#ifndef G_GAME_H
#define G_GAME_H
#include "g_core.h"
#include "g_input.h"
#include "w_anim.h"
#include "w_phys.h"
#include "aw_audio.h"
#include "prefs.h"

/* ManagerScript (DontDestroyOnLoad: survives scene loads) */
typedef struct {
    int inited;
    int next_level, main_menu, sub_menu, intro_ready, first_transition, resetable_input;
    int requested_track;
    char music_credit[96];
} Mgr;
extern Mgr M;
int mgr_progress(void);   /* unlocked chapters (capped in the demo build) */
const char *menu_txt(int i);     /* local/m.json */
const char *chap_txt(int i);     /* local/ch.json */

/* sounds by asset id (sfx id, or 0x8000 | music) */
int mgr_short(int snd, int priority);
int mgr_short_common(int snd);
void mgr_long(int snd);
void mgr_paused(int snd);
int snd_pick(const Script *s, uint32_t field);   /* random element of an AudioClip[] field (-1) */
int snd_at(const Script *s, uint32_t field, int i);
int snd_count(const Script *s, uint32_t field);
void mgr_music_check(int snd);
void mgr_audio_pause(int stop);
void mgr_audio_remove_all(void);
void mgr_volume(const char *type, float v);    /* 0..1 */
void mgr_music_muffle(int muffle);
void mgr_level_complete(void);
void mgr_reload(void);
void mgr_load_scene(int n);

/* helpers */
float frandf(void);                               /* [0,1) */
float frange(float a, float b);                   /* Random.Range(float) */
int irange(int a, int b);                         /* Random.Range(int) [a,b) */
int P(const char *param);                         /* animator parameter id (cached by the caller) */
void obj_pos2(int o, float v[2]);
void set_text(int go, const char *s);
void set_text_color(int go, uint32_t c);
void sr_enable(int go, int on);
void camera_shake(float duration, float amount);
#define PI_F 3.14159265f

/* script types used across files */
typedef struct PlayerS PlayerS;
typedef struct RotationS RotationS;
Script *find_script(int type);                    /* first alive script of a type */

/* Player */
void player_gameover(Script *player);
void player_junk_pickup(Script *player, int junk);
void player_junk_deposit(Script *player, int slot);
int player_hand(Script *player, int slot);
int player_input_blocked(Script *player);
void player_block_input(Script *player, int on);
int player_in_menu(Script *player);
int player_in_cutscene(Script *player);
void player_set_in_menu(Script *player, int on);
void player_set_in_cutscene(Script *player, int on);
void player_input_confirm(Script *player);
int player_superdash(Script *player);
int player_dashing(Script *player);
void player_dash_dir(Script *player, float *x, float *y);
void player_material_flash(Script *player);
void player_post_eat(Script *player);
void player_eat_end(Script *player);
void player_drone_holo_appear(Script *player);
int player_go(Script *player);

/* Rotation (level loop) */
void rotation_gameover(Script *rot, int location_go, int dead, int xflip);
void rotation_score_update(Script *rot, int gen_go, int gen_anim_go, int xflip);
void rotation_repair_finished(Script *rot, Script *engine);
void rotation_dash_counter(Script *rot);
void rotation_game_starter(Script *rot);
void rotation_ghost_intro(Script *rot);
void rotation_anti_dronebug(Script *rot);
void rotation_junk_pool_add(Script *rot, int item);
int rotation_drone_state(Script *rot);
int rotation_dash_count(Script *rot);
int rotation_solo_haunt(Script *rot);
void rotation_set_solo_haunt(Script *rot, int v);
int rotation_cam_go(Script *rot);

/* Upscale animator (full screen sequences) */
void upscale_activator(Script *u, const char *command, int target_go, int xflip);
int upscale_drone_hit(Script *u);
void upscale_set_drone_hit(Script *u, int v);
int upscale_in_intro(Script *u);
void upscale_set_gen_anim(Script *u, int go);

/* Menu / cutscene / transition */
void menu_input_confirm(Script *menu);
void menu_switch(Script *menu, int next);
void menu_open_close(Script *menu, int number, int open);
void menu_next_level(Script *menu, float delay);
int menu_current(Script *menu);
int menu_gallery_unlocked(Script *menu);
void cutscene_input_confirm(Script *c);
void transition_go(Script *t);

/* engines (generators) */
void engine_defect_start(Script *e, const int want[2], float time);
void engine_highlight(Script *e, int on);
void engine_interaction(Script *e);
void aparatus_highlight(Script *a, int on);
void aparatus_interaction(Script *a);

/* ghosts (GhostAiScript / AiScript) */
void ai_spawn(Script *ai);
void ghost_wipe_event(Script *ai);
void ai_attacked(Script *ai, float dx, float dy);     /* IVulnerable */
int minion_available(Script *m);
void minion_death(Script *m);
void dogo_waved(Script *d, float x, float y);
void boss_start(Script *b);
void boss_electro_wipe(Script *b);

/* weapons */
void throwable_throw(Script *t, float ox, float oy, float dx, float dy);
void bullet_cast(Script *b);
void bullet_destruction(Script *b);
void spread_spread(Script *s, float x, float y);
void spread_wall(Script *s);
void spread_wipe(Script *s);
void spreadpool_ignite(Script *p, float x, float y);
void spreadpool_wipe(Script *p);
void shock_emit(Script *s, float ox, float oy, int advanced, float tx, float ty);
void shock_destruction(Script *s);
void thunder_bolt(Script *t);
void barrage_aim(Script *b, float x, float y);
void barrage_destruction(Script *b, int perma);
void barrage_wipe(Script *b);
void roto_recast(Script *r, float x, float y);
void icewave_throw(Script *w, float ox, float oy, float dx, float dy);
void icewave_destruction(Script *w);
void icespike_spawn(Script *sp, float x, float y, int spr);
void phase_phase(Script *p, float tx, float ty, float ox, float oy, float scx, float scy);
void skull_spawn(Script *s);
void skull_dmg(Script *s, int contact);
void skull_wipe(Script *s);
void electro_discharge(Script *e, float x, float y, const char *trigger);
void electrowipe_activate(Script *e, float x, float y);
void fireball_destruction(Script *f);
void projectile_destruction(Script *p);
#endif
