/* MenuScript, SubmenuScript, the buttons and CutsceneScript.
 * PSP: the resolution / fullscreen / key-rebinding entries of the settings menu are hidden. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "g_game.h"
#include "aw_gfx.h"

void g_quit(void);

#ifndef WHITE
#define WHITE 0xFFFFFFFFu   /* same value as aw_gfx.h RGBA(255, 255, 255, 255) */
#endif
#define GREEN 0xFF00FF00u
#define MAXBTN 16
#define MAXSUB 10

static int ref_i(const Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n) ? f_ref_at(s, &e[i]) : -1;
}
static int ref_n(const Script *s, uint32_t field) { int n; f_elems(s, field, &n); return n; }
static const char *f_str(const Script *s, uint32_t name)
{
    const WField *f = f_get(s, name);
    return (f && f->kind == F_STR) ? w_string(f->v.i) : "";
}

/* the PC Fullscreen toggle is the PSP's Language switch (LANG.PAK translation <-> built-in English) */
static int is_lang_button(Script *b)
{
    return b && b->type == ST_PrefButton && !strcmp(f_str(b, CRC("pref")), "fullscreen");
}
static void lang_button_text(Script *b)
{
    const char *f = lang_file_name();
    set_text(f_ref(b, CRC("buttonText")), menu_txt(66));
    set_text(f_ref(b, CRC("buttonText2")), prefs_int("lang", 1) && f ? f : "English");
}

static uint32_t f_color(const Script *s, uint32_t name, uint32_t def)
{
    const WField *f = f_get(s, name);
    return (f && f->kind == F_COLOR) ? (uint32_t)f->v.i : def;
}
static int asset_at(const Script *s, uint32_t field, int i)
{
    int n;
    const WField *e = f_elems(s, field, &n);
    return (e && i >= 0 && i < n && e[i].kind == F_ASSET) ? e[i].v.i : -1;
}
static void text_enable(int go, int on)
{
    int t = text_of(go);
    if (t < 0) return;
    if (on) W.text[t].d.flags |= WTXT_ENABLED;
    else W.text[t].d.flags &= ~WTXT_ENABLED;
}
static void img_sprite(int go, int spr)
{
    int i = img_of(go);
    if (i >= 0 && spr >= 0) W.img[i].spr = spr;
}
static void img_color(int go, uint32_t c)
{
    int i = img_of(go);
    if (i < 0) return;
    W.img[i].r = c & 255; W.img[i].g = (c >> 8) & 255; W.img[i].b = (c >> 16) & 255; W.img[i].a = c >> 24;
}

/* ================================================================== state */
typedef struct {
    Script *btn[MAXBTN]; int nbtn;
    int max_index, parent_menu;
    char refresh_type[12];
} SubS;

typedef struct {
    Script *player;
    Script *sub[MAXSUB]; int nsub;
    int current_menu, current_index, current_cheat, dirdown;
    int gallery_unlocked;
    Co delay;
} MenuS;

typedef struct {
    Script *menu;            /* MenuScript */
    Script *chapter;         /* SubmenuScript (chapter buttons, load level) */
    int state;               /* PrefButton currentState */
    float volume;            /* VolumeButton */
    int progress;            /* ProgressButton */
    int page;                /* GalleryButton */
    int hidden;              /* PSP: not available */
} BtnS;

static Script *menu_of(Script *b) { return ((BtnS *)b->self)->menu; }
static MenuS *ms(Script *menu) { return menu->self; }

static int is_button(int type)
{
    return type == ST_NextMenuButton || type == ST_ChapterButton || type == ST_LoadLevelButton || type == ST_ReturnButton ||
           type == ST_PrefButton || type == ST_VolumeButton || type == ST_ProgressButton || type == ST_GalleryButton ||
           type == ST_CreditButton || type == ST_ResolutionButton;
}
static Script *button_on(int go)
{
    if (go < 0) return 0;
    for (int i = 0; i < W.nscript; i++)
        if (W.script[i].alive && W.script[i].go == go && is_button(W.script[i].type))
            return &W.script[i];
    return 0;
}

static void btn_init(Script *b)
{
    BtnS *st = b->self;
    st->menu = script_ref(b, CRC("menuScript"), ST_MenuScript);
    if (!st->menu) st->menu = find_script(ST_MenuScript);
    st->chapter = script_ref(b, CRC("chapterScript"), ST_SubmenuScript);
    if (b->type == ST_LoadLevelButton) {
        int t2 = f_ref(b, CRC("buttonText2"));
        if (t2 >= 0 && W.obj[t2].text >= 0) {
            W.text[W.obj[t2].text].d.font = 3;   /* px 11 */
            if (W.obj[t2].rect >= 0)
                W.rect[W.obj[t2].rect].sd_x = 1800.0f;
        }
    }
}

static void btn_base_highlight(Script *b, int hl)
{
    obj_set_active(f_ref(b, CRC("finger")), hl);
    uint32_t c = hl ? WHITE : f_color(b, CRC("nonWhite"), GREEN);
    set_text_color(f_ref(b, CRC("buttonText")), c);
    int t2 = f_ref(b, CRC("buttonText2"));
    if (t2 >= 0) set_text_color(t2, c);
}

static void sub_sprite_refresh(Script *sub, const char *trigger, int idx);

static void btn_highlight(Script *b, int hl, int move_out)
{
    if (!b) return;
    BtnS *st = b->self;
    Script *menu = menu_of(b);
    switch (b->type) {
    case ST_GalleryButton:
        return;
    case ST_NextMenuButton:
        btn_base_highlight(b, hl);
        if (menu && ms(menu)->current_menu == 1 && hl)
            anim_trigger(f_ref(menu, CRC("animator")), P(f_str(b, CRC("aniTrigger"))));
        return;
    case ST_ChapterButton:
        btn_base_highlight(b, hl);
        if (hl) {
            if (st->chapter) sub_sprite_refresh(st->chapter, f_str(b, CRC("ggTrigger")), atoi(f_str(b, CRC("chapterSymbol"))) - 1);
            anim_trigger(f_ref(b, CRC("mapAnimator")), P("on"));
        } else {
            anim_trigger(f_ref(b, CRC("mapAnimator")), P("off"));
        }
        return;
    case ST_LoadLevelButton:
        btn_base_highlight(b, hl);
        if (menu && ms(menu)->current_menu != 0 && hl)
            anim_trigger(f_ref(b, CRC("skullAnimator")), P(f_str(b, CRC("difficulty"))));
        return;
    default:
        btn_base_highlight(b, hl);
    }
}

static void volume_text(Script *b)
{
    BtnS *st = b->self;
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", (int)lroundf(st->volume * 10));
    set_text(f_ref(b, CRC("buttonText2")), buf);
}

static void gallery_page(Script *b)
{
    BtnS *st = b->self;
    Script *menu = menu_of(b);
    int n = ref_n(b, CRC("ImageArray"));
    if (!n) return;
    int req = 0;
    {
        int c;
        const WField *e = f_elems(b, CRC("requirementArray"), &c);
        if (e && st->page < c) req = e[st->page].v.i;
    }
    char buf[64];
    int t2 = f_ref(b, CRC("buttonText2"));
    if (prefs_int("beel", 0) != 1 && prefs_int("hardCount", 0) < req) {
        snprintf(buf, sizeof buf, "\xe2\x98\x85 %d / %d", prefs_int("hardCount", 0), req);
        set_text(f_ref(b, CRC("requirementText1")), buf);
        set_text(f_ref(b, CRC("requirementText2")), menu_txt(53));
        set_text(t2, "");
        obj_set_active(f_ref(b, CRC("curtainObject")), 1);
    } else {
        set_text(t2, txt_line(TXT_G, st->page));
        obj_set_active(f_ref(b, CRC("curtainObject")), 0);
        set_text_color(t2, st->page < 14 ? GREEN : WHITE);
    }
    (void)menu;
    snprintf(buf, sizeof buf, "%d / %d", st->page + 1, n);
    set_text(f_ref(b, CRC("buttonText")), buf);
    int spr = asset_at(b, CRC("ImageArray"), st->page);
    int gi = f_ref(b, CRC("galleryImage"));
    img_sprite(gi, spr);
    int rt = f_ref(b, CRC("rectTransform"));
    if (rt >= 0 && W.obj[rt].rect >= 0 && spr >= 0) {
        /* sizeDelta = the texture size in canvas units (sprites are stored at 272/1200 scale, *4) */
        W.rect[W.obj[rt].rect].sd_x = g_sprites[spr].rw / 4.0f * 1200.0f / 272.0f;
        W.rect[W.obj[rt].rect].sd_y = g_sprites[spr].rh / 4.0f * 1200.0f / 272.0f;
    }
}

static void btn_refresh(Script *b, const char *ref)
{
    if (!b) return;
    BtnS *st = b->self;
    Script *menu = menu_of(b);
    int regular = !strcmp(ref, "regular");
    int bt = f_ref(b, CRC("buttonText"));
    int t2 = f_ref(b, CRC("buttonText2"));
    if (b->type == ST_GalleryButton) {
        st->page = 0;
        gallery_page(b);
        return;
    }
    if (b->type == ST_ChapterButton) {
        const char *sym = f_str(b, CRC("chapterSymbol"));
        if (!strcmp(ref, "chapter")) {
            char key[8], buf[16];
            snprintf(key, sizeof key, "%sH", sym);
            if (prefs_int(key, 0) >= 1) snprintf(buf, sizeof buf, "%s\xe2\x98\x85", sym);
            else snprintf(buf, sizeof buf, "%s", sym);
            set_text(bt, buf);
        } else if (strcmp(ref, "simple")) {
            set_text(bt, ref);
        }
        return;
    }
    if (regular) set_text(bt, menu_txt(f_int(b, CRC("textInt"), 0)));
    switch (b->type) {
    case ST_NextMenuButton:
        if (f_int(b, CRC("nextMenu"), 0) == 5 && menu && ms(menu)->gallery_unlocked) {
            char buf[96];
            snprintf(buf, sizeof buf, "%s \xe2\x98\x85", menu_txt(f_int(b, CRC("textInt"), 0) + 3));
            set_text(bt, buf);
        }
        break;
    case ST_LoadLevelButton:
        if (menu && ms(menu)->current_menu != 0)
            set_text(t2, menu_txt(f_int(b, CRC("textInt"), 0) + 1));
        break;
    case ST_PrefButton:
        if (is_lang_button(b)) {
            lang_button_text(b);
            break;
        }
        if (regular) {
            st->state = prefs_int(f_str(b, CRC("pref")), 0) == 1;
            set_text(t2, menu_txt(f_int(b, CRC("button2int"), 0) + st->state));
        }
        break;
    case ST_VolumeButton:
        st->volume = 10.0f;
        volume_text(b);
        break;
    case ST_ProgressButton:
        st->progress = mgr_progress();
        set_text(t2, chap_txt(st->progress - 1));
        break;
    case ST_CreditButton:
        if (regular && !strcmp(menu_txt(30), "X")) set_text(bt, "");
        break;
    case ST_ResolutionButton:
        set_text(t2, "480 x 272");
        break;
    }
}

static void menu_switch_i(Script *menu, int next);
static void menu_next_level_i(Script *menu, float delay);

static void btn_confirm(Script *b)
{
    if (!b) return;
    BtnS *st = b->self;
    Script *menu = menu_of(b);
    int confirm = f_asset(b, CRC("confirmClip"));
    if (g_autotest) printf("[aw] confirm type %d menu %d idx %d next_level %d sym %s\n", b->type, menu ? ms(menu)->current_menu : -1, menu ? ms(menu)->current_index : -1, M.next_level, f_str(b, CRC("chapterSymbol")));
    switch (b->type) {
    case ST_NextMenuButton: {
        mgr_long(confirm);
        int next = f_int(b, CRC("nextMenu"), 0);
        if (next == 666) g_quit();
        else if (next == 0) { if (menu && ms(menu)->nsub > 2) menu_switch_i(menu, ((SubS *)ms(menu)->sub[2]->self)->parent_menu); }
        else if (menu) menu_switch_i(menu, next);
        break;
    }
    case ST_ChapterButton:
        mgr_paused(confirm);
        M.next_level = atoi(f_str(b, CRC("chapterSymbol")));
        if (menu) menu_switch_i(menu, f_int(b, CRC("nextMenu"), 7));
        break;
    case ST_LoadLevelButton: {
        mgr_long(confirm);
        if (!menu) break;
        if (ms(menu)->current_menu == 0) {
            M.main_menu = 1;
            menu_next_level_i(menu, 0);
            break;
        }
        prefs_set_int("mode", atoi(f_str(b, CRC("difficulty"))));
        int nl = M.next_level;
        int num = nl >= 10 ? (nl != 13 ? 1 : 2) : 0;
        anim_trigger(f_ref(b, CRC("introAnimator")), P("start"));
        anim_set_int(f_ref(b, CRC("introAnimator2")), P("scene"), num);
        set_text(f_ref(b, CRC("textbox")), menu_txt(61 + num));
        menu_next_level_i(menu, 1.7f);
        break;
    }
    case ST_ReturnButton:
        mgr_paused(confirm);
        if (menu) menu_open_close(menu, 0, 0);
        break;
    case ST_PrefButton:
        mgr_paused(confirm);
        if (is_lang_button(b)) {
            /* switch language: reload the menu scene with the other text, back on the Settings page */
            int on = !prefs_int("lang", 1);
            prefs_set_int("lang", on);
            lang_button_text(b);
            extern void g_lang_request(int on);
            g_lang_request(on);
            M.main_menu = 1;
            M.sub_menu = 2;
            g_scene_request(0);
            break;
        }
        st->state = !st->state;
        set_text(f_ref(b, CRC("buttonText2")), menu_txt(f_int(b, CRC("button2int"), 0) + st->state));
        prefs_set_int(f_str(b, CRC("pref")), st->state);
        break;
    default:
        break;
    }
}

static void btn_switch(Script *b, int additive)
{
    if (!b) return;
    BtnS *st = b->self;
    int confirm = f_asset(b, CRC("confirmClip"));
    switch (b->type) {
    case ST_PrefButton:
        btn_confirm(b);
        break;
    case ST_VolumeButton: {
        if (additive) { st->volume += 1; if (st->volume > 10) st->volume = 0; }
        else { st->volume -= 1; if (st->volume < 0) st->volume = 10; }
        if (st->volume != 0) mgr_paused(confirm);
        st->volume = roundf(st->volume);
        const char *type = f_str(b, CRC("soundType"));
        prefs_set_float(type, st->volume);
        volume_text(b);
        mgr_volume(type, st->volume / 10);
        break;
    }
    case ST_ProgressButton:
        if (additive) { if (++st->progress == 13) st->progress = 1; }
        else { if (--st->progress < 1) st->progress = 12; }
        prefs_set_int("lvl", st->progress);
        set_text(f_ref(b, CRC("buttonText2")), chap_txt(st->progress - 1));
        break;
    case ST_GalleryButton: {
        mgr_paused(confirm);
        int n = ref_n(b, CRC("ImageArray"));
        if (!n) break;
        if (additive) { if (++st->page == n) st->page = 0; }
        else { if (--st->page < 0) st->page = n - 1; }
        gallery_page(b);
        break;
    }
    case ST_ResolutionButton:
        mgr_paused(confirm);
        break;
    default:
        break;
    }
}

#define BTN_VT {sizeof(BtnS), btn_init}
const ScriptVT vt_NextMenuButton = BTN_VT;
const ScriptVT vt_ChapterButton = BTN_VT;
const ScriptVT vt_LoadLevelButton = BTN_VT;
const ScriptVT vt_ReturnButton = BTN_VT;
const ScriptVT vt_PrefButton = BTN_VT;
const ScriptVT vt_VolumeButton = BTN_VT;
const ScriptVT vt_ProgressButton = BTN_VT;
const ScriptVT vt_GalleryButton = BTN_VT;
const ScriptVT vt_CreditButton = BTN_VT;
const ScriptVT vt_ResolutionButton = BTN_VT;

/* ================================================================== SubmenuScript */
static int psp_hidden(Script *b)
{
    if (!b) return 1;
    if (b->type == ST_ResolutionButton) return 1;
    if (b->type == ST_CreditButton && !strcmp(menu_txt(30), "X")) return 1;   /* no translator: close the gap */
    if (b->type == ST_VolumeButton) return 1;                 /* audio stays at 100% */
    if (is_lang_button(b)) return !lang_file_name();            /* Fullscreen -> Language */
    if (b->type == ST_NextMenuButton) {
        int n = f_int(b, CRC("nextMenu"), 0);
        if (n == 3 || n == 8) return 1;          /* key / gamepad rebinding */
        if (n == 666) return 1;                  /* main menu Quit */
        /* Settings: only Language is left in it, so only in the main menu and only with a LANG.PAK */
        if (n == 2) return !(f_str(b, CRC("aniTrigger"))[0] && lang_file_name());
    }
    return 0;
}

static void sub_init(Script *s)
{
    SubS *st = s->self;
    st->parent_menu = f_int(s, CRC("parentMenu"), 0);
    st->max_index = f_int(s, CRC("maxIndex"), 0);
    snprintf(st->refresh_type, sizeof st->refresh_type, "%s", f_str(s, CRC("refreshType")));
    int n = ref_n(s, CRC("buttonArray"));
    float slot_y[MAXBTN];
    int nslot = 0;
    for (int i = 0; i < n && st->nbtn < MAXBTN; i++) {
        int go = ref_i(s, CRC("buttonArray"), i);
        Script *b = button_on(go);
        if (!b) continue;
        if (!b->awake) {
            /* buttons under inactive submenus wake up later; their state is needed now */
            if (!b->self) b->self = calloc(1, sizeof(BtnS));
            btn_init(b);
        }
        if (go >= 0) slot_y[nslot++] = W.obj[go].rect >= 0 ? W.rect[W.obj[go].rect].ap_y : W.obj[go].ly;
        if (psp_hidden(b)) {
            ((BtnS *)b->self)->hidden = 1;
            obj_set_active(go, 0);
            /* value labels (volume %, resolution, on/off) are separate objects: hide them too */
            int t2 = f_ref(b, CRC("buttonText2"));
            if (t2 >= 0 && !obj_is_descendant(t2, go)) obj_set_active(t2, 0);
            continue;
        }
        st->btn[st->nbtn++] = b;
    }
    /* close the gaps left by hidden buttons */
    if (st->nbtn < n && st->nbtn <= nslot)
        for (int i = 0; i < st->nbtn; i++) {
            int go = st->btn[i]->go;
            Obj *bo = &W.obj[go];
            float *y = bo->rect >= 0 ? &W.rect[bo->rect].ap_y : &bo->ly;   /* UI: RectTransform position */
            float dy = slot_y[i] - *y;
            *y = slot_y[i];
            /* a value label outside the button moves with it */
            int t2 = f_ref(st->btn[i], CRC("buttonText2"));
            if (t2 >= 0 && dy != 0 && !obj_is_descendant(t2, go)) {
                if (W.obj[t2].rect >= 0) W.rect[W.obj[t2].rect].ap_y += dy;
                else W.obj[t2].ly += dy;
            }
        }

    if (f_int(s, CRC("chapterSelect"), 0)) {
        int bt = f_ref(s, CRC("bossText"));
        int tt = f_ref(s, CRC("talkText"));
        int ht = f_ref(s, CRC("hardText"));
        if (bt >= 0 && W.obj[bt].text >= 0) {
            W.text[W.obj[bt].text].d.font = 4;   /* px 14 */
            if (W.obj[bt].rect >= 0) {
                W.rect[W.obj[bt].rect].ap_x = 180.0f;
                W.rect[W.obj[bt].rect].sd_x = 1020.0f;
                W.rect[W.obj[bt].rect].ap_y = -250.0f;
            }
        }
        if (tt >= 0 && W.obj[tt].text >= 0) {
            W.text[W.obj[tt].text].d.font = 3;   /* px 11 */
            if (W.obj[tt].rect >= 0) {
                W.rect[W.obj[tt].rect].ap_x = 180.0f;
                W.rect[W.obj[tt].rect].sd_x = 1020.0f;
                W.rect[W.obj[tt].rect].ap_y = -360.0f;
            }
        }
        /* portrait + widened briefing (-690..690 canvas units) centred on the screen */
        int pt = f_ref(s, CRC("ggAnimator"));
        if (pt >= 0 && W.obj[pt].rect >= 0)
            W.rect[W.obj[pt].rect].ap_x = -520.0f;
        if (ht >= 0 && W.obj[ht].text >= 0) {
            W.text[W.obj[ht].text].d.font = 3;   /* px 11 */
            if (W.obj[ht].rect >= 0) {
                W.rect[W.obj[ht].rect].ap_x = 180.0f;
                W.rect[W.obj[ht].rect].sd_x = 1020.0f;
            }
        }
    }
}

static void sub_menu_refresh(Script *s)
{
    SubS *st = s->self;
    int chapter = f_int(s, CRC("chapterSelect"), 0);
    if (chapter) {
        set_text(f_ref(s, CRC("bossText")), chap_txt(13));
        char buf[128];
        snprintf(buf, sizeof buf, "\xe2\x98\x85 %s", chap_txt(27));
        set_text(f_ref(s, CRC("hardText")), buf);
        st->max_index = mgr_progress();
        strcpy(st->refresh_type, "chapter");
    } else if (!strcmp(st->refresh_type, "regular")) {
        st->max_index = st->nbtn;
    }
    for (int i = 0; i < st->nbtn; i++) {
        if (chapter && st->max_index == i) st->refresh_type[0] = 0;
        btn_refresh(st->btn[i], st->refresh_type);
    }
    strcpy(st->refresh_type, "simple");
}

static void sub_sprite_refresh(Script *s, const char *trigger, int idx)
{
    anim_trigger(f_ref(s, CRC("frameAnimator")), P("switch"));
    anim_trigger(f_ref(s, CRC("ggAnimator")), P(trigger));
    set_text(f_ref(s, CRC("talkText")), chap_txt(idx + 14));
    set_text(f_ref(s, CRC("chapterText")), chap_txt(idx));
    set_text(f_ref(s, CRC("chapterText2")), chap_txt(idx));
    char key[8];
    snprintf(key, sizeof key, "%dH", idx + 1);
    text_enable(f_ref(s, CRC("hardText")), prefs_int(key, 0) >= 1);
}
const ScriptVT vt_SubmenuScript = {sizeof(SubS), sub_init};

/* ================================================================== MenuScript */
static SubS *cur_sub(MenuS *m) { return m->sub[m->current_menu] ? m->sub[m->current_menu]->self : 0; }
static Script *cur_btn(MenuS *m)
{
    SubS *s = cur_sub(m);
    return (s && m->current_index >= 0 && m->current_index < s->nbtn) ? s->btn[m->current_index] : 0;
}
static int sub_input(MenuS *m) { return m->sub[m->current_menu] && f_int(m->sub[m->current_menu], CRC("inputMenu"), 0); }

static void menu_total_refresh(Script *menu, int needed, int open)
{
    MenuS *m = menu->self;
    Script *sub = m->sub[m->current_menu];
    if (open && sub) sub_menu_refresh(sub);
    int bar = f_ref(menu, CRC("barObject"));
    if (needed && open && sub) {
        set_text(f_ref(menu, CRC("musicCredit")), M.music_credit);
        obj_set_active(bar, 1);
        img_color(f_ref(menu, CRC("barImage")), f_color(sub, CRC("barColor"), WHITE));
        set_text(f_ref(menu, CRC("barText")), menu_txt(f_int(sub, CRC("barName"), 0)));
    } else {
        obj_set_active(bar, 0);
    }
}

static void sub_core(Script *sub, int on) { if (sub) obj_set_active(f_ref(sub, CRC("submenuCore")), on); }
static int sub_bar(Script *sub) { return sub ? f_int(sub, CRC("bar"), 0) : 0; }

void menu_open_close(Script *menu, int number, int open)
{
    MenuS *m = menu->self;
    if (number < 0 || number >= m->nsub || !m->sub[number]) return;
    m->current_menu = number;
    if (m->player) player_set_in_menu(m->player, open);
    menu_total_refresh(menu, sub_bar(m->sub[number]), open);
    sub_core(m->sub[number], open);
    if (open) m->current_index = 0;
    if (g_autotest) { int c = f_ref(m->sub[number], CRC("submenuCore")); printf("[aw] open %d core %d act %d ah %d nbtn %d\n", number, c, c >= 0 ? W.obj[c].active : -1, c >= 0 ? W.obj[c].ah : -1, ((SubS *)m->sub[number]->self)->nbtn); }
    btn_highlight(cur_btn(m), open, 0);
    T.scale = open ? 0 : 1;
    mgr_audio_pause(open);
}

static void menu_switch_i(Script *menu, int next)
{
    MenuS *m = menu->self;
    if (next < 0 || next >= m->nsub || !m->sub[next]) return;
    sub_core(m->sub[m->current_menu], 0);
    btn_highlight(cur_btn(m), 0, 0);
    m->current_index = 0;
    m->current_menu = next;
    Script *sub = m->sub[next];
    menu_total_refresh(menu, sub_bar(sub), 1);
    const char *trig = f_str(sub, CRC("aniTrigger"));
    if (trig[0]) anim_trigger(f_ref(menu, CRC("animator")), P(trig));
    sub_core(sub, 1);
    SubS *ss = sub->self;
    if (g_autotest) printf("[aw] switch to %d next_level %d\n", next, M.next_level);
    if (next == 4) {
        m->current_index = M.next_level != 0 ? M.next_level - 1 : ss->max_index - 1;
    } else if (next == 7) {
        m->current_index = prefs_int("mode", 1);
        int spr = asset_at(menu, CRC("chapterImageArray"), M.next_level - 1);
        img_sprite(f_ref(menu, CRC("chapterImage")), spr);
    } else if (next == 1) {
        M.next_level = 0;
    }
    if (m->current_index >= ss->nbtn) m->current_index = ss->nbtn ? ss->nbtn - 1 : 0;
    if (m->current_index < 0) m->current_index = 0;
    btn_highlight(cur_btn(m), 1, 0);
}
void menu_switch(Script *menu, int next) { menu_switch_i(menu, next); }
int menu_current(Script *menu) { return ms(menu)->current_menu; }
int menu_gallery_unlocked(Script *menu) { return ms(menu)->gallery_unlocked; }

static void menu_next_level_i(Script *menu, float delay)
{
    MenuS *m = menu->self;
    if (m->player) player_block_input(m->player, 1);
    co_start(&m->delay);
    co_wait(&m->delay, delay, 1);
}
void menu_next_level(Script *menu, float delay) { menu_next_level_i(menu, delay); }

void menu_input_confirm(Script *menu)
{
    MenuS *m = menu->self;
    if (m->player && player_input_blocked(m->player)) return;
    if (sub_input(m)) return;
    btn_confirm(cur_btn(m));
}

static void hover(Script *menu, MenuS *m)
{
    int i = m->current_menu == 7 ? 2 : m->current_menu == 4 ? 1 : 0;
    mgr_paused(snd_at(menu, CRC("hoverClip"), i));
}

static void next_button(Script *menu, int additive)
{
    MenuS *m = menu->self;
    if (g_autotest) printf("[aw] next_button add %d mx %.2f my %.2f menu %d idx %d\n", additive, IN.mx, IN.my, m->current_menu, m->current_index);
    SubS *s = cur_sub(m);
    if (!s || s->max_index == 1) return;
    int maxi = s->max_index < s->nbtn ? s->max_index : s->nbtn;
    if (maxi <= 0) return;
    btn_highlight(cur_btn(m), 0, 1);
    if (additive) {
        if (m->current_menu == 1 && m->current_index == 0 && !m->gallery_unlocked) m->current_index = 2;
        else if (++m->current_index >= maxi) m->current_index = 0;
    } else if (m->current_menu == 1 && m->current_index == 2 && !m->gallery_unlocked) {
        m->current_index = 0;
    } else if (--m->current_index < 0) {
        m->current_index = maxi - 1;
    }
    btn_highlight(cur_btn(m), 1, 0);
    hover(menu, m);
}

static const char cheatsheet[12] = {'d', 'u', 'r', 'u', 'l', 'd', 'd', 'l', 'u', 'd', 'r', 'u'};

static int cheat_check(Script *menu, char dir)
{
    MenuS *m = menu->self;
    if (m->dirdown) return 1;
    m->dirdown = 1;
    if (m->current_menu != 6) return 0;
    if (dir == cheatsheet[m->current_cheat]) m->current_cheat++;
    else if (dir == 'd') m->current_cheat = 1;
    else m->current_cheat = 0;
    if (m->current_cheat == 12) {
        obj_set_active(f_ref(menu, CRC("pentaObject")), 1);
        mgr_paused(f_asset(menu, CRC("openClip")));
        prefs_set_int("gallery", 1);
        m->gallery_unlocked = 1;
        prefs_set_int("beel", 1);
        prefs_set_int("lvl", 13);
        m->current_cheat = 0;
    } else {
        mgr_paused(snd_at(menu, CRC("hoverClip"), 0));
    }
    return 1;
}

static void menu_init(Script *s)
{
    MenuS *m = s->self;
    m->player = script_ref(s, CRC("player"), ST_PlayerScript);
    m->current_menu = f_int(s, CRC("currentMenu"), 0);
    m->gallery_unlocked = f_int(s, CRC("galleryUnlocked"), 0);
    int n = ref_n(s, CRC("submenuArray"));
    for (int i = 0; i < n && i < MAXSUB; i++) {
        Script *sub = script_on(ref_i(s, CRC("submenuArray"), i), ST_SubmenuScript);
        if (sub && !sub->awake) {
            if (!sub->self) sub->self = calloc(1, sizeof(SubS));
            sub->awake = 1;
            sub_init(sub);
        }
        m->sub[i] = sub;
        m->nsub = i + 1;
    }
}

static void menu_start(Script *s)
{
    MenuS *m = s->self;
    if (g_autotest) printf("[aw] menu_start main %d sub %d nsub %d player %p\n", M.main_menu, M.sub_menu, m->nsub, (void *)m->player);
    M.resetable_input = 0;
    if (M.main_menu) {
        if (prefs_int("gallery", 0) == 1) m->gallery_unlocked = 1;
        M.intro_ready = 1;
        M.main_menu = 0;
        if (m->nsub > 2 && m->sub[2]) ((SubS *)m->sub[2]->self)->parent_menu = 1;
        menu_open_close(s, 1, 1);
        int sub = M.sub_menu;
        if (sub != 1) {
            menu_switch_i(s, sub);
            M.sub_menu = 1;
        } else {
            M.next_level = 0;
        }
    }
}

static void menu_update(Script *s, float dt)
{
    MenuS *m = s->self;
    if (co_tick(&m->delay, dt, T.udt)) {
        co_stop(&m->delay);
        Script *tr = script_ref(s, CRC("tranScript"), ST_TransitionScript);
        if (tr) transition_go(tr);
    }
    if (!m->player || player_input_blocked(m->player)) return;
    if (!player_in_menu(m->player)) {
        if (IN.cancel) {
            menu_open_close(s, 0, 1);
            mgr_paused(f_asset(s, CRC("openClip")));
        }
        return;
    }
    if (IN.cancel || IN.secondary) {
        if (m->current_menu == 0) {
            menu_open_close(s, 0, 0);
        } else if (m->current_menu != 1) {
            menu_switch_i(s, ((SubS *)m->sub[m->current_menu]->self)->parent_menu);
        }
        mgr_paused(f_asset(s, CRC("closeClip")));
        if (!player_in_menu(m->player)) return;
    }
    if (sub_input(m)) return;
    Script *sub = m->sub[m->current_menu];
    int unusual = sub ? f_int(sub, CRC("unusualNavigation"), 0) : 0;
    if (IN.my < -0.1f) {
        if (!cheat_check(s, 'd')) next_button(s, 1);
    } else if (IN.my > 0.1f) {
        if (!cheat_check(s, 'u')) next_button(s, 0);
    } else if (IN.mx < -0.1f) {
        if (!cheat_check(s, 'l')) { if (unusual) next_button(s, 0); else btn_switch(cur_btn(m), 0); }
    } else if (IN.mx > 0.1f) {
        if (!cheat_check(s, 'r')) { if (unusual) next_button(s, 1); else btn_switch(cur_btn(m), 1); }
    } else {
        m->dirdown = 0;
    }
}
const ScriptVT vt_MenuScript = {sizeof(MenuS), menu_init, menu_start, menu_update};

/* ================================================================== CutsceneScript */
typedef struct {
    Script *player;
    int txt_file, scene_ready, current;
} CutS;

static const WField *member(const Script *s, const WField *st, uint32_t name)
{
    const WField *c = f_child(s, st);
    for (int i = 0; c && i < st->count; i++)
        if (c[i].name == name) return &c[i];
    return 0;
}
static int member_int(const Script *s, const WField *st, const char *n)
{
    const WField *f = member(s, st, CRC(n));
    return f && f->kind == F_INT ? f->v.i : 0;
}
static const char *member_str(const Script *s, const WField *st, const char *n)
{
    const WField *f = member(s, st, CRC(n));
    return f && f->kind == F_STR ? w_string(f->v.i) : "";
}

static void cut_init(Script *s)
{
    CutS *c = s->self;
    c->player = script_ref(s, CRC("playerScript"), ST_PlayerScript);
    c->scene_ready = 1;
    const char *dir = f_str(s, CRC("txtDir"));   /* "/local/N.json" */
    const char *p = strrchr(dir, '/');
    int n = atoi(p ? p + 1 : dir);
    c->txt_file = (n >= 1 && n <= 13) ? TXT_F1 + n - 1 : TXT_F1;

    int co = obj_find(CRC("cutsceneObject"));
    if (co >= 0 && W.obj[co].rect >= 0)
        W.rect[W.obj[co].rect].ap_y = 10.0f;

    int nt = f_ref(s, CRC("nameText"));
    int dt = f_ref(s, CRC("dialogueText"));
    if (nt >= 0 && W.obj[nt].text >= 0) {
        W.text[W.obj[nt].text].d.font = 4;   /* px 14 */
        if (W.obj[nt].rect >= 0)
            W.rect[W.obj[nt].rect].ap_y = -240.0f;
    }
    if (dt >= 0 && W.obj[dt].text >= 0) {
        W.text[W.obj[dt].text].d.font = 11;  /* px 13 */
        if (W.obj[dt].rect >= 0) {
            W.rect[W.obj[dt].rect].sd_x = 1720.0f;
            W.rect[W.obj[dt].rect].ap_y = -300.0f;
        }
    }
    int bp = obj_find(CRC("boomper"));
    if (bp >= 0 && W.obj[bp].rect >= 0)
        W.rect[W.obj[bp].rect].ap_y = -480.0f;

    int kt = obj_find(CRC("kissText"));
    if (kt >= 0 && W.obj[kt].rect >= 0)
        W.rect[W.obj[kt].rect].ap_y = 112.0f;
}

void cutscene_input_confirm(Script *s)
{
    CutS *c = s->self;
    if (!c->scene_ready) return;
    c->scene_ready = 0;
    int n;
    const WField *arr = f_elems(s, CRC("sceneArray"), &n);
    if (c->current == n) {
        mgr_level_complete();
        Script *tr = script_ref(s, CRC("tranScript"), ST_TransitionScript);
        if (tr) transition_go(tr);
    } else if (arr) {
        const WField *e = &arr[c->current];
        const char *type = member_str(s, e, "type");
        if (!strcmp(type, "prekiss")) {
            mgr_short(snd_at(s, CRC("kissingClip"), 0), 1);
        } else {
            int ta = f_ref(s, CRC("textAnimator"));
            if (obj_active(ta)) anim_trigger(ta, P("appear"));
            set_text(f_ref(s, CRC("nameText")), txt_line(c->txt_file, member_int(s, e, "nameLine")));
            set_text(f_ref(s, CRC("dialogueText")), txt_line(c->txt_file, member_int(s, e, "dialogueLine")));
            const WField *sp = member(s, e, CRC("sceneSprite"));
            if (sp && sp->kind == F_ASSET) img_sprite(f_ref(s, CRC("scene")), sp->v.i);
            obj_set_active(f_ref(s, CRC("cutsceneAnim")), 0);
            int kiss = f_ref(s, CRC("kissText"));
            if (strcmp(type, "next")) {
                set_text(kiss, menu_txt(atoi(type)));
                text_enable(kiss, 1);
                mgr_short(snd_at(s, CRC("kissingClip"), !strcmp(type, "17") ? 1 : 2), 1);
            } else {
                mgr_short(f_asset(s, CRC("swishClip")), 1);
                text_enable(kiss, 0);
            }
        }
        anim_trigger(s->go, P(type));
        if (!strcmp(member_str(s, e, "achiev"), "anim"))
            obj_set_active(f_ref(s, CRC("cutsceneAnim")), 1);
    }
    c->current++;
}

static void cut_event(Script *s, int fn, int iarg, float farg)
{
    CutS *c = s->self;
    if (fn == EV_CutsceneReady) {
        c->scene_ready = 1;
    } else if (fn == EV_CutsceneStart) {
        cutscene_input_confirm(s);
        if (c->player) {
            player_block_input(c->player, 0);
            player_set_in_cutscene(c->player, 1);
        }
    }
}
const ScriptVT vt_CutsceneScript = {sizeof(CutS), cut_init, 0, 0, 0, cut_event};
