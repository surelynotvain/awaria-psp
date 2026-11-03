/* VCPE controller reading (see vcpe_pad.h). */

/* HOW THIS FILE WORKS
 *
 * Games don't test PSP buttons directly: a bind table maps PSP buttons to the game's own bits (B_JUMP, B_OK...), so
 * controls can be remapped in one place. Each update samples the pad once and derives:
 * - held: buttons down now;
 * - pressed: buttons that went down since the last time the game asked (kept until read, so a tap is never lost when
 *   the game runs no update in a display frame);
 * - released: buttons that were down at the previous update and are up now;
 * - repeat: menu-style auto-repeat (first press, then every `rate` frames after `delay` frames held). In autotest
 *   runs the buttons come from the test script instead of the pad, so runs are repeatable. */
#include <pspctrl.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include "vcpe_plat.h"
#include "vcpe_test.h"
#include "vcpe_input.h"
#include "vcpe_pad.h"

/* repeat_frames[i]: how many updates bit i has been held (0 = up). last_press: time of the last accepted press, for
 * debouncing. */
static const VcpePadBind *binds;
static int nbinds, stick_mode = VCPE_STICK_DPAD, debounce_us;
static VcpeInput edges;
static uint32_t psp_now, held_now, held_prev_update, stick_bits, repeat_frames[32];
static int sx, sy, sampling;
static int64_t last_press[32];

/* Sampling cycle 0 = sample at vblank; analog mode so the stick is read too. */
void vcpe_pad_init(const VcpePadBind *b, int n, int mode)
{
    binds = b;
    nbinds = n;
    stick_mode = mode;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    sampling = 1;
    vcpe_input_reset(&edges, 0);
}

void vcpe_pad_debounce(int ms) { debounce_us = ms * 1000; }

/* PSP button bits to game bits through the bind table (no table: PSP bits as they are). */
static uint32_t map(uint32_t psp)
{
    if (!binds)
        return psp;
    uint32_t b = 0;
    for (int i = 0; i < nbinds; i++)
        if (psp & binds[i].psp)
            b |= binds[i].bits;
    return b;
}

/* Read the pad once per update and update every derived state. */
uint32_t vcpe_pad_update(void)
{
    uint32_t psp = 0;
    if (!sampling)
        vcpe_pad_init(binds, nbinds, stick_mode);
    if (vcpe_autotest) {
        unsigned h = 0;
        vcpe_test_step(&h);
        psp = h;
        vcpe_test_stick(&sx, &sy);
        if (stick_mode == VCPE_STICK_DPAD) {
            if (sx < -76) psp |= PSP_CTRL_LEFT;
            if (sx > 76) psp |= PSP_CTRL_RIGHT;
            if (sy < -76) psp |= PSP_CTRL_UP;
            if (sy > 76) psp |= PSP_CTRL_DOWN;
        } else if (stick_mode == VCPE_STICK_DPAD_HYST) {
            if (sx < -96) stick_bits |= PSP_CTRL_LEFT; else if (sx > -40) stick_bits &= ~PSP_CTRL_LEFT;
            if (sx > 96) stick_bits |= PSP_CTRL_RIGHT; else if (sx < 40) stick_bits &= ~PSP_CTRL_RIGHT;
            if (sy < -96) stick_bits |= PSP_CTRL_UP; else if (sy > -40) stick_bits &= ~PSP_CTRL_UP;
            if (sy > 96) stick_bits |= PSP_CTRL_DOWN; else if (sy < 40) stick_bits &= ~PSP_CTRL_DOWN;
            psp |= stick_bits;
        }
    } else {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        psp = pad.Buttons;
        /* The stick reads 0..255 per axis with 128 at rest; centre it to -128..127. */
        sx = pad.Lx - 128;
        sy = pad.Ly - 128;
        /* Stick as d-pad, plain: past about 60% of the way out counts as a press. */
        if (stick_mode == VCPE_STICK_DPAD) {
            stick_bits = 0;
            if (pad.Lx < 50) stick_bits |= PSP_CTRL_LEFT;
            if (pad.Lx > 205) stick_bits |= PSP_CTRL_RIGHT;
            if (pad.Ly < 50) stick_bits |= PSP_CTRL_UP;
            if (pad.Ly > 205) stick_bits |= PSP_CTRL_DOWN;
        /* With hysteresis: a direction engages far out (96) and only lets go near the centre (40), so a stick resting
         * near the threshold doesn't flicker on and off. */
        } else if (stick_mode == VCPE_STICK_DPAD_HYST) {
            /* engages far out, lets go near the centre */
            if (sy < -96) stick_bits |= PSP_CTRL_UP; else if (sy > -40) stick_bits &= ~PSP_CTRL_UP;
            if (sy > 96) stick_bits |= PSP_CTRL_DOWN; else if (sy < 40) stick_bits &= ~PSP_CTRL_DOWN;
            if (sx < -96) stick_bits |= PSP_CTRL_LEFT; else if (sx > -40) stick_bits &= ~PSP_CTRL_LEFT;
            if (sx > 96) stick_bits |= PSP_CTRL_RIGHT; else if (sx < 40) stick_bits &= ~PSP_CTRL_RIGHT;
        } else {
            stick_bits = 0;
        }
        psp |= stick_bits;
    }
    psp_now = psp;
    uint32_t held = map(psp);
    uint32_t newly = held & ~edges.previous;
    /* Anti-mash debounce: a new press of the same button within debounce_us of the last accepted one is ignored. */
    if (debounce_us && newly) {
        int64_t now = (int64_t)vcpe_time_us();
        for (int i = 0; i < 32; i++)
            if (newly & (1u << i)) {
                if (now - last_press[i] < debounce_us)
                    edges.previous |= 1u << i;   /* too soon: not a new press */
                else
                    last_press[i] = now;
            }
    }
    vcpe_input_sample(&edges, held);
    held_prev_update = held_now;
    held_now = held;
    for (int i = 0; i < 32; i++)
        repeat_frames[i] = (held & (1u << i)) ? repeat_frames[i] + 1 : 0;
    return held;
}

uint32_t vcpe_pad_held(void) { return held_now; }
uint32_t vcpe_pad_pressed(void) { return vcpe_input_pressed(&edges); }
uint32_t vcpe_pad_released(void) { return held_prev_update & ~held_now; }
uint32_t vcpe_pad_psp(void) { return psp_now; }
void vcpe_pad_stick(int *x, int *y) { if (x) *x = sx; if (y) *y = sy; }

/* Auto-repeat: fires on the first frame held, then after `delay` frames, every `rate` frames. */
uint32_t vcpe_pad_repeat(uint32_t bits, int delay, int rate)
{
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t b = 1u << i;
        if (!(bits & b) || !repeat_frames[i])
            continue;
        uint32_t f = repeat_frames[i];
        if (f == 1 || (f > (uint32_t)delay && rate > 0 && (f - delay - 1) % rate == 0))
            r |= b;
    }
    return r;
}

/* Autotest script names to PSP bits; "cross+circle" is a chord. */
unsigned vcpe_pad_button(const char *name)
{
    static const struct { const char *n; unsigned b; } m[] = {
        {"up", PSP_CTRL_UP}, {"down", PSP_CTRL_DOWN}, {"left", PSP_CTRL_LEFT}, {"right", PSP_CTRL_RIGHT},
        {"cross", PSP_CTRL_CROSS}, {"x", PSP_CTRL_CROSS}, {"ok", PSP_CTRL_CROSS},
        {"circle", PSP_CTRL_CIRCLE}, {"o", PSP_CTRL_CIRCLE}, {"back", PSP_CTRL_CIRCLE},
        {"square", PSP_CTRL_SQUARE}, {"sq", PSP_CTRL_SQUARE}, {"triangle", PSP_CTRL_TRIANGLE}, {"tr", PSP_CTRL_TRIANGLE},
        {"l", PSP_CTRL_LTRIGGER}, {"r", PSP_CTRL_RTRIGGER}, {"start", PSP_CTRL_START}, {"select", PSP_CTRL_SELECT}};
    unsigned b = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", name);
    for (char *t = strtok(buf, "+"); t; t = strtok(0, "+"))
        for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
            if (!strcmp(t, m[i].n))
                b |= m[i].b;
    return b;
}
