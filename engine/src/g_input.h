/* InputScript: move vector + primary / secondary / cancel presses this frame */
#ifndef G_INPUT_H
#define G_INPUT_H
enum { BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8, BTN_PRIMARY = 16, BTN_SECONDARY = 32, BTN_CANCEL = 64 };
typedef struct { float mx, my; int primary, secondary, cancel; unsigned held, pressed; } Input;
extern Input IN;
extern int g_autotest;
#endif
