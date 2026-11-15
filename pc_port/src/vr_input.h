#ifndef VR_INPUT_H
#define VR_INPUT_H

/* Motion-controller state, filled by a VR runtime backend each frame.
 * Sticks -1..1 (up/right positive), triggers and grips 0..1. */
typedef struct
{
    int   valid;
    float lx, ly, rx, ry;
    float lt, rt, lgrip, rgrip;
    int   a, b, x, y;
    int   lstick, rstick, menu;
    int   back;   /* no controller button of its own: vr_interact.c sets it (the game's Select) */
} VrPadState;

/* Mirror the state onto the SDL virtual game controller (vr_input.c). */
void VrInput_Push(const VrPadState* s);

#endif
