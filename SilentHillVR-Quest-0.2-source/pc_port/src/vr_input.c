/*
=============================================================================
 SH1 PC Port - VR MOTION CONTROLLERS -> VIRTUAL GAMEPAD
=============================================================================

 The game reads input through SDL game controllers (PsyX_pad.cpp: mapping,
 rebinding, hysteresis, the controls panel). Rather than teach it about VR
 controllers, the runtime backends (vr_openxr.c / vr_openvr.c) fill a
 VrPadState each frame and this file mirrors it onto an SDL VIRTUAL game
 controller. The game sees an ordinary pad hot-plugged in, so every
 existing bind (pad_cross = a, pad_r2 = righttrigger...) applies as-is.

 Touch layout -> pad: sticks -> sticks, A/B/X/Y -> A/B/X/Y,
 triggers -> triggers, grips -> shoulders, stick clicks -> stick clicks,
 left menu -> start.
=============================================================================
*/

#include <stdio.h>
#include <string.h>

#include <math.h>

#include <SDL.h>

#include "vr_input.h"

extern void VrOvr_Log(const char* fmt, ...);   /* SilentHillVR.log */
#define printf VrOvr_Log

static SDL_Joystick* s_joy = NULL;
static int           s_failed = 0;

static Sint16 Axis(float v)
{
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return (Sint16)(v * 32767.0f);
}

/* A virtual pad's trigger is a raw joystick axis, and SDL maps the axis'
 * FULL range (-32768..32767) onto the trigger's 0..32767. Writing 0 for a
 * released trigger therefore read as half pulled: Aim (left trigger) was held
 * the whole game. Released = -32768, fully pulled = 32767. */
static Sint16 Trigger(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (Sint16)lrintf(v * 65535.0f - 32768.0f);
}

static int Attach(void)
{
    int idx;

    if (s_joy) return 1;
    if (s_failed) return 0;

    /* Axes and buttons in SDL_GameControllerAxis / SDL_GameControllerButton
     * order: SDL builds the virtual pad's mapping from exactly that order. */
    idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                    SDL_CONTROLLER_AXIS_MAX,
                                    SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1, 0);
    if (idx < 0)
    {
        printf("[VR-INPUT] virtual pad failed: %s\n", SDL_GetError());
        s_failed = 1;
        return 0;
    }
    s_joy = SDL_JoystickOpen(idx);
    if (!s_joy)
    {
        printf("[VR-INPUT] virtual pad open failed: %s\n", SDL_GetError());
        SDL_JoystickDetachVirtual(idx);
        s_failed = 1;
        return 0;
    }
    printf("[VR-INPUT] motion controllers -> virtual gamepad (device %d, instance %d)\n",
           idx, (int)SDL_JoystickInstanceID(s_joy));
    {
        int i;
        for (i = 0; i < SDL_NumJoysticks(); i++)
            printf("[VR-INPUT]   joystick %d '%s' gamecontroller=%d\n", i,
                   SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "?",
                   (int)SDL_IsGameController(i));
    }
    return 1;
}

void VrInput_Push(const VrPadState* s)
{
    static const VrPadState neutral;

    /* No pad until the controllers report once - a pad with nothing behind
     * it would only steal slot 0 from a real controller. */
    if (!s_joy && (!s || !s->valid)) return;
    if (!Attach()) return;
    if (!s || !s->valid) s = &neutral;

    {
        /* first presses only: proves the buttons reach the virtual pad */
        static int lastMask = 0, logged = 0;
        int mask = (s->a ? 1 : 0) | (s->b ? 2 : 0) | (s->x ? 4 : 0) | (s->y ? 8 : 0) |
                   (s->menu ? 16 : 0) | (s->rt > 0.5f ? 32 : 0) | (s->lt > 0.5f ? 64 : 0);
        if (mask != lastMask && logged < 20)
        {
            printf("[VR-INPUT] pad buttons 0x%02X (a b x y menu rt lt)\n", mask);
            logged++;
        }
        lastMask = mask;
    }

    /* SDL: stick Y down is positive; VR sticks report up positive. */
    SDL_JoystickSetVirtualAxis(s_joy, SDL_CONTROLLER_AXIS_LEFTX,  Axis(s->lx));
    SDL_JoystickSetVirtualAxis(s_joy, SDL_CONTROLLER_AXIS_LEFTY,  Axis(-s->ly));
    SDL_JoystickSetVirtualAxis(s_joy, SDL_CONTROLLER_AXIS_RIGHTX, Axis(s->rx));
    SDL_JoystickSetVirtualAxis(s_joy, SDL_CONTROLLER_AXIS_RIGHTY, Axis(-s->ry));
    SDL_JoystickSetVirtualAxis(s_joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT,  Trigger(s->lt));
    SDL_JoystickSetVirtualAxis(s_joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, Trigger(s->rt));

    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_A, (Uint8)(s->a != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_B, (Uint8)(s->b != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_X, (Uint8)(s->x != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_Y, (Uint8)(s->y != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_START, (Uint8)(s->menu != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_BACK, (Uint8)(s->back != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_LEFTSTICK,  (Uint8)(s->lstick != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_RIGHTSTICK, (Uint8)(s->rstick != 0));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  (Uint8)(s->lgrip > 0.5f));
    SDL_JoystickSetVirtualButton(s_joy, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, (Uint8)(s->rgrip > 0.5f));

}
