/* BigStrigaScript: the finale boss (summon rotations, smash, bombard, electro wipe). */
#include <math.h>
#include "g_game.h"

enum { SUB_DOGS, SUB_MARCH, SUB_SKULL };

typedef struct {
    Script *rot;
    int anim, player;
    int wendigo, skull_index, summon_index, first_rotation;
    float cooldown;
    int big_audio;
    Co fbr;                   /* FirstBossRotation */
    int plan[4], nplan, pi;
    Co sub; int sub_kind;
    Co charge;                /* SkeliChargeSoundDelay */
    Co bomb; int bomb_i;      /* PlayerBombard */
} BossS;

static int ref_i(const Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n) ? f_ref_at(s, &e[i]) : -1;
}
static int ref_n(const Script *s, uint32_t field) { int n; f_elems(s, field, &n); return n; }
static Script *minion_i(Script *s, int i) { return script_on(ref_i(s, CRC("minions"), i), ST_MinionAiScript); }
static Script *wall_i(Script *s, int i) { return script_on(ref_i(s, CRC("skeliWalls"), i), ST_BulletSpreadScript); }
static Script *skull_i(Script *s, int i) { return script_on(ref_i(s, CRC("skulls"), i), ST_SkullbombScript); }
static Script *barrage_i(Script *s, int i) { return script_on(ref_i(s, CRC("scrapBarrage"), i), ST_ScrapBarrageScript); }
static float player_x(BossS *b) { float x, y; obj_pos(b->player, &x, &y); return x; }

static void boss_init(Script *s)
{
    BossS *b = s->self;
    b->rot = script_ref(s, CRC("rotation"), ST_RotationScript);
    b->anim = f_ref(s, CRC("animator"));
    b->player = f_ref(s, CRC("playerTransform"));
    b->wendigo = f_int(s, CRC("wendigoCountdown"), 0);
    b->first_rotation = 1;
    b->cooldown = 1;
    b->big_audio = -1;
}

static void dog_check(Script *s)
{
    BossS *b = s->self;
    int n = ref_n(s, CRC("minions")), sum = 0;
    for (int i = 0; i < n; i++) { Script *m = minion_i(s, i); if (m) sum += minion_available(m); }
    if (sum != n) b->summon_index = 2;
}

static void dog_summoner(Script *s)
{
    BossS *b = s->self;
    if (b->wendigo == 0) {
        mgr_short(snd_at(s, CRC("dogClip"), 1), 0);
        if (b->rot) rotation_ghost_intro(b->rot);
    } else {
        mgr_short(snd_at(s, CRC("dogClip"), 3), 0);
        int n = ref_n(s, CRC("minions"));
        for (int i = 0; i < n; i++) { Script *m = minion_i(s, i); if (m) ai_spawn(m); }
    }
    b->wendigo--;
}

static void charge_sound(Script *s)
{
    BossS *b = s->self;
    mgr_short(snd_pick(s, CRC("skeliSummonClip")), 0);
    co_start(&b->charge);
    co_wait(&b->charge, 1.0f, 1);
}

static void holo_cast(Script *s, int i)
{
    int h = ref_i(s, CRC("holoAnimator"), i);
    anim_set_enabled(h, 1);
    anim_trigger(h, P("cast"));
}

static void skull_spawn_next(Script *s)
{
    BossS *b = s->self;
    int n = ref_n(s, CRC("skulls"));
    if (!n) return;
    if (b->skull_index >= n) b->skull_index = 0;
    Script *k = skull_i(s, b->skull_index);
    if (k) skull_spawn(k);
    b->skull_index++;
}

void boss_start(Script *s)
{
    BossS *b = s->self;
    anim_set_enabled(b->anim, 1);
    if (b->wendigo == 0) {
        mgr_short(snd_at(s, CRC("phaseClip"), 0), 0);
        co_start(&b->fbr);
        co_wait(&b->fbr, 1.0f, 1);
    } else {
        mgr_short(snd_at(s, CRC("bigClips"), 0), 0);
    }
}

static void sub_start(Script *s, int kind)
{
    BossS *b = s->self;
    b->sub_kind = kind;
    if (kind == SUB_DOGS) {
        mgr_short(snd_at(s, CRC("dogClip"), b->wendigo == 0 ? 0 : 2), 0);
        anim_trigger(b->anim, P("dogs"));
    } else if (kind == SUB_MARCH) {
        mgr_short(snd_at(s, CRC("minionClip"), 0), 0);
        anim_trigger(b->anim, P("march"));
    } else {
        mgr_short(snd_at(s, CRC("minionClip"), 1), 0);
        anim_trigger(b->anim, P("skull"));
    }
    co_start(&b->sub);
    co_wait(&b->sub, 0.8f, 1);
}

static void sub_tick(Script *s, float dt)
{
    BossS *b = s->self;
    if (!co_tick(&b->sub, dt, T.udt)) return;
    if (b->sub.step == 1) {
        if (b->sub_kind == SUB_DOGS) {
            dog_summoner(s);
        } else if (b->sub_kind == SUB_MARCH) {
            charge_sound(s);
            int holo = ref_i(s, CRC("holoTransform"), 0);
            if (player_x(b) > 0) {
                Script *w = wall_i(s, 1);
                if (w) spread_wall(w);
                obj_scale(holo, 1, 1);
            } else {
                Script *w = wall_i(s, 0);
                if (w) spread_wall(w);
                obj_scale(holo, -1, 1);
            }
            holo_cast(s, 0);
        } else {
            mgr_short(snd_pick(s, CRC("skullClip")), 0);
            skull_spawn_next(s);
            if (b->skull_index > ref_n(s, CRC("skeliWalls"))) b->skull_index = 0;
        }
        co_wait(&b->sub, b->cooldown, 2);
        return;
    }
    co_stop(&b->sub);
    b->pi++;
    /* resume FirstBossRotation right away */
    b->fbr.wait = 0;
}

static void fbr_tick(Script *s, float dt)
{
    BossS *b = s->self;
    if (b->sub.on) return;
    if (!co_tick(&b->fbr, dt, T.udt)) return;
    if (b->fbr.step == 1) {
        if (b->first_rotation) b->first_rotation = 0;
        else mgr_short(snd_at(s, CRC("phaseClip"), 2), 0);
        if (b->summon_index != 2) dog_check(s);
        int k = b->summon_index;
        b->nplan = 0;
        if (k == 0) b->plan[b->nplan++] = SUB_DOGS;
        if (k == 0 || k == 2) b->plan[b->nplan++] = SUB_MARCH;
        if (k == 1 || k == 2) b->plan[b->nplan++] = SUB_SKULL;
        if (k == 1) b->plan[b->nplan++] = SUB_DOGS;
        b->pi = 0;
        b->fbr.step = 2;
    }
    if (b->fbr.step == 2) {
        if (b->pi < b->nplan) {
            sub_start(s, b->plan[b->pi]);
            return;
        }
        if (++b->summon_index == 3) b->summon_index = 0;
        mgr_short(snd_at(s, CRC("phaseClip"), 1), 0);
        co_wait(&b->fbr, 3.0f, 3);
        return;
    }
    if (b->fbr.step == 3)
        co_wait(&b->fbr, 1.0f, 1);
}

static void bomb_tick(Script *s, float dt)
{
    BossS *b = s->self;
    if (!co_tick(&b->bomb, dt, T.udt)) return;
    int n = ref_n(s, CRC("scrapBarrage"));
    switch (b->bomb.step) {
    case 1:
        b->bomb_i = 0;
        b->bomb.step = 2;
        /* fallthrough */
    case 2:
        if (b->bomb_i < n) {
            float px, py;
            obj_pos(b->player, &px, &py);
            Script *br = barrage_i(s, b->bomb_i);
            if (br) barrage_aim(br, px, py);
            co_wait(&b->bomb, 0.3f, 3);
            return;
        }
        co_stop(&b->bomb);
        return;
    case 3:
        mgr_short(f_asset(s, CRC("targetMarkClip")), 0);
        b->bomb_i++;
        b->bomb.step = 2;
        co_wait(&b->bomb, 0, 2);
        return;
    }
}

static void boss_update(Script *s, float dt)
{
    BossS *b = s->self;
    sub_tick(s, dt);
    fbr_tick(s, dt);
    bomb_tick(s, dt);
    if (co_tick(&b->charge, dt, T.udt)) {
        co_stop(&b->charge);
        mgr_short(snd_pick(s, CRC("skeliChargeClip")), 0);
    }
}

static void stop_all(BossS *b)
{
    co_stop(&b->fbr); co_stop(&b->sub); co_stop(&b->charge); co_stop(&b->bomb);
}

static void boss_event(Script *s, int fn, int iarg, float farg)
{
    BossS *b = s->self;
    float x, y;
    obj_pos(s->go, &x, &y);
    switch (fn) {
    case EV_AniBossRotation: {
        float px = player_x(b);
        if (px < 4 && px > -4) {
            anim_trigger(b->anim, P("smash"));
            b->big_audio = mgr_short(snd_at(s, CRC("bigClips"), 2), 0);
            if (px > 2) obj_set_pos(s->go, 2, y);
            else if (px < -2) obj_set_pos(s->go, -2, y);
        } else {
            anim_trigger(b->anim, P("bomb"));
            b->big_audio = mgr_short(snd_pick(s, CRC("bigcannonClip")), 0);
            co_start(&b->bomb);
            co_wait(&b->bomb, b->cooldown, 1);
        }
        break;
    }
    case EV_AniRecovery:
        obj_set_pos(s->go, 0, y);
        break;
    case EV_AniDoubleSummon: {
        if (b->summon_index != 2) dog_check(s);
        b->big_audio = mgr_short(snd_at(s, CRC("bigClips"), 1), 0);
        int k = b->summon_index;
        if (k == 0) dog_summoner(s);
        if (k == 0 || k == 2) {
            charge_sound(s);
            int n = ref_n(s, CRC("skeliWalls"));
            for (int i = 0; i < n; i++) {
                Script *w = wall_i(s, i);
                if (w) spread_wall(w);
                holo_cast(s, i);
            }
        }
        if (k == 1 || k == 2) {
            mgr_short(snd_pick(s, CRC("skullClip")), 0);
            skull_spawn_next(s);
            skull_spawn_next(s);
            if (b->skull_index > ref_n(s, CRC("skeliWalls"))) b->skull_index = 0;
        }
        if (k == 1) dog_summoner(s);
        if (++b->summon_index == 3) b->summon_index = 0;
        break;
    }
    case EV_AniSmashKill:
        camera_shake(0.3f, 0.3f);
        if (b->first_rotation) {
            b->first_rotation = 0;
            mgr_short(snd_at(s, CRC("bigClips"), 3), 0);
            obj_set_active(f_ref(s, CRC("glassCracks")), 1);
        }
        mgr_short(snd_pick(s, CRC("bigslamClip")), 0);
        {
            Script *pool = script_ref(s, CRC("spreadPool"), ST_BulletSpreadPoolScript);
            if (pool) spreadpool_ignite(pool, x, -2);
        }
        break;
    case EV_AniSmallShake:
        camera_shake(0.2f, 0.2f);
        break;
    case EV_LastScream:
        mgr_long(snd_at(s, CRC("bigClips"), 4));
        break;
    }
}

void boss_electro_wipe(Script *s)
{
    BossS *b = s->self;
    if (b->big_audio >= 0) { snd_stop(b->big_audio); b->big_audio = -1; }
    int n = ref_n(s, CRC("minions"));
    for (int i = 0; i < n; i++) { Script *m = minion_i(s, i); if (m) minion_death(m); }
    n = ref_n(s, CRC("skulls"));
    for (int i = 0; i < n; i++) { Script *k = skull_i(s, i); if (k) skull_dmg(k, 0); }
    n = ref_n(s, CRC("skeliWalls"));
    for (int i = 0; i < n; i++) spread_wipe(wall_i(s, i));
    n = ref_n(s, CRC("scrapBarrage"));
    for (int i = 0; i < n; i++) { Script *br = barrage_i(s, i); if (br) barrage_destruction(br, 0); }
    Script *dog = script_ref(s, CRC("wendogo"), ST_DogoAiScript);
    float px, py;
    obj_pos(b->player, &px, &py);
    if (dog) dogo_waved(dog, px, py);
    Script *pool = script_ref(s, CRC("spreadPool"), ST_BulletSpreadPoolScript);
    if (pool) spreadpool_wipe(pool);
    stop_all(b);
    camera_shake(0.2f, 0.2f);
    anim_trigger(b->anim, P("stun"));
}

void boss_wipe_event(Script *s)
{
    BossS *b = s->self;
    int n = ref_n(s, CRC("minions"));
    for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("minions"), i), 0);
    n = ref_n(s, CRC("skulls"));
    for (int i = 0; i < n; i++) { Script *k = skull_i(s, i); if (k) skull_wipe(k); }
    n = ref_n(s, CRC("skeliWalls"));
    for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("skeliWalls"), i), 0);
    n = ref_n(s, CRC("holoTransform"));
    for (int i = 0; i < n; i++) obj_set_active(ref_i(s, CRC("holoTransform"), i), 0);
    n = ref_n(s, CRC("scrapBarrage"));
    for (int i = 0; i < n; i++) { Script *br = barrage_i(s, i); if (br) barrage_destruction(br, 1); }
    obj_set_active(f_ref(s, CRC("spreadPool")), 0);
    stop_all(b);
    anim_trigger(b->anim, P("end"));
}

static void boss_disable(Script *s) { stop_all(s->self); }
const ScriptVT vt_BigStrigaScript = {sizeof(BossS), boss_init, 0, boss_update, 0, boss_event, 0, 0, 0, boss_disable};
