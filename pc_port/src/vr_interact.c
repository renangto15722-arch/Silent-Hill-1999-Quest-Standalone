/*
=============================================================================
 SH1 PC Port - VR HANDS: grab, radial wheel, weapon (one priority manager)
=============================================================================

 Every frame the motion controllers are read (vr_openvr.c
 VrOvr_PollControllers), this file decides what the right grip, the right
 trigger, A and X mean, BEFORE the state becomes the virtual gamepad the
 game reads (vr_input.c). Nothing else interprets those inputs.

 A, in Full VR gameplay:
   - a tap is A for the game, delivered on release for a few frames (a tap
     cannot be told from a hold until then);
   - held VR_WHEEL_HOLD_MS it opens the radial wheel (healing items and the
     Map) and never reaches the game: select by moving the right hand or
     the right stick, release A = confirm, released with nothing selected =
     close.

 Either grip, one owner per press, decided at the press (GripPress):
   H. HOLSTER - the hand is within VR_HOLSTER_RADIUS_M of one of the six
               holsters (vr_ik.c) holding a weapon Harry owns - the left
               hand only finds the knife (VR_LEFT_WEAPON): at the equipped
               weapon's own holster the press holsters it or draws it into
               this hand (drawn in the other hand, this hand takes it); at
               another's it equips that weapon, in this hand. Holstered,
               the weapon stays equipped for the game (ammo, HUD) but sits
               in its holster, and Aim, fire and reload are blocked;
               equipping another weapon draws it.
   M. MAP    - the hand is nearer the paper map's holster (the slot set to
               Map) than any weapon's: like a weapon, a press takes the map
               into that hand and it stays there; a press at the holster
               with that hand puts it back (see HandToMapHolster).
   G. GRAB   - a pickup is within vr_grab_radius of the hand at the press:
               that map event runs, exactly as the action button would run
               it (events_main.c Event_PcVrGrabFind).
   Otherwise the press does nothing. The game never sees the right grip
   during gameplay; a left grip press that does nothing is the game's.

 Weapon, independent of the grip:
   - The trigger of the hand that holds the weapon (the right one, or the
     left for a knife the left grip took from its holster): past half it
     is Aim (the game's LT in the altcam scheme), pulled fully it also
     holds Action/Shoot (the game's RT) once the weapon is up. With a
     weapon in hand neither physical trigger reaches the game any other
     way.
   - X: manual reload, through pc_combat.c's own rules (gun equipped, clip
     not full, rounds in reserve, not already reloading). The game reloads
     only with the gun raised, so when those rules allow it Aim is held for
     the reload (until it ends, or 1.5 s if it never starts). X does
     nothing else: it never reaches the game.

 Fresh presses only: an input counts only after it was seen RELEASED during
 gameplay. A grab that opened the take screen, a menu, a cutscene or a lost
 focus therefore never leaves a grip/trigger/A/X "pressed" that acts later;
 everything is dropped (wheel closed with no action) the frame gameplay ends
 or the controllers stop reporting.

 Grab and wheel need Full VR (the wheel draws in its 2D interface layer,
 vr_wheel.c); fire and reload work whenever a headset plays the game.
=============================================================================
*/

#include "game.h"
#include "pc_config.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"

#include <math.h>
#include <string.h>

#include <SDL.h>

#include "vr_input.h"
#include "vr_interact.h"

extern int  VrTracking_HeadCamActive(void);
extern int  VrTracking_CutsceneActive(void);
extern int  VrTracking_HideCutsceneBars(void);   /* = Full VR this frame */
extern int  VrTracking_GetUiContext(void);
extern int  VrIk_GetHandWorld(int hand, VECTOR3* out);
extern int  VrIk_HolsterSlotItem(int slot);
extern int  VrIk_HolsterSlotOf(int itemId);
extern int  VrIk_HolsterSlotWorld(int slot, VECTOR3* out);
extern int  Pc_VrWheelUse(u8 id);
extern int  VrIk_GetHandInHead(int hand, float out[3]);
extern int  Pc_QuickOptions_IsOpen(void);
extern int  Pc_VrReloadPress(void);
extern int  Pc_VrWheelItems(u8* ids, s32* counts, int max);
extern s32  Event_PcVrGrabFind(s32 handX, s32 handZ, s32 radius);
extern s32  g_PcVrGrabEvent;
extern void VrOvr_Log(const char* fmt, ...);
extern int  g_PcFpsCam;

/* thresholds: press above the first, release below the second */
#define VR_GRIP_ON        0.65f
#define VR_GRIP_OFF       0.35f
#define VR_FIRE_ON        0.90f
#define VR_FIRE_OFF       0.70f
#define VR_WHEEL_DEAD_M   0.035f  /* hand travel (m, at Wheel_Size 100) before a slot is picked */
#define VR_WHEEL_STICK    0.5f
#define VR_WHEEL_HOLD_MS  2000u   /* A held this long opens the wheel */
#define VR_TAP_PULSE      4       /* frames a released A tap is held for the game */
#define VR_USE_WINDOW_MS  500u    /* a wheel pick waits this long for a safe moment */
#define VR_RELOAD_WAIT_MS 1500u   /* raised gun waiting for the reload to start */
#define VR_RELOAD_MAX_MS  4000u   /* ... and at most this long in all */

enum { GRIP_IDLE = 0, GRIP_GRAB, GRIP_MAP, GRIP_SPENT, GRIP_WHEEL };

static int s_gripDown, s_gripArmed, s_gripOwner = GRIP_IDLE;
static int s_lgripDown, s_lgripArmed, s_lgripOwner = GRIP_IDLE;
static int s_mapHand = -1;   /* the hand holding the paper map, -1 = in its holster */
static int    s_aDown, s_aArmed, s_aHeld, s_aPulse, s_yPulse;
static Uint32 s_aSince;
static int s_fireDown, s_fireArmed, s_firing, s_firePrevLogged;
static int s_xDown, s_xArmed, s_xHeld;
static Uint32 s_xSince;
static int    s_menuDown, s_menuArmed, s_menuHeld, s_menuPulse;
static Uint32 s_menuSince;
static int    s_menuReq;            /* a held-button menu for the game: 1 inventory, 2 options */
static Uint32 s_menuReqUntil;

#define VR_MENU_HOLD_MS    600u     /* X held: inventory (as Y held is the VR menu) */
#define VR_OPTIONS_HOLD_MS 2000u    /* Menu held: the game's options */

static void MenuRequest(int which)
{
    s_menuReq      = which;
    s_menuReqUntil = SDL_GetTicks() + 500u;
}

/* X reload: 0 = idle, 1 = gun raised waiting for the reload, 2 = reloading */
static int    s_reloadPhase;
static Uint32 s_reloadStart;

static VrWheelView s_wheel;
static float       s_wheelOrigin[3];
static int         s_wheelSelFromStick;

static int    s_pendingUse = -1;   /* inventory item id picked on the wheel */
static Uint32 s_pendingUntil;

/* Harry is under the player's control, in first person, with a headset. */
static int VrHandsGameplay(void)
{
    return VrTracking_HeadCamActive() && !VrTracking_CutsceneActive() && g_PcFpsCam &&
           g_GameWork.gameState == GameState_InGame &&
           g_SysWork.sysState == SysState_Gameplay &&
           !g_Player_DisableControl &&
           !Pc_QuickOptions_IsOpen() &&
           VrTracking_GetUiContext() == 0;
}

static int WheelOpen(void)
{
    memset(&s_wheel, 0, sizeof(s_wheel));
    s_wheel.n   = Pc_VrWheelItems(s_wheel.ids, s_wheel.counts, VR_WHEEL_MAX);
    s_wheel.sel = -1;
    if (s_wheel.n <= 0 || !VrIk_GetHandInHead(1, s_wheelOrigin))
        return 0;
    s_wheel.open        = 1;
    s_wheelSelFromStick = 0;
    return 1;
}

/* apply = A was released: use what is selected (if anything). The Map is
 * the game's own Map button (Y), so it opens - or not - by the game's rules. */
static void WheelClose(int apply)
{
    if (!s_wheel.open) return;
    if (apply && s_wheel.sel >= 0 && s_wheel.sel < s_wheel.n)
    {
        if (s_wheel.ids[s_wheel.sel] == VR_WHEEL_ID_MAP)
        {
            s_yPulse = VR_TAP_PULSE;
        }
        else
        {
            s_pendingUse   = s_wheel.ids[s_wheel.sel];
            s_pendingUntil = SDL_GetTicks() + VR_USE_WINDOW_MS;
        }
    }
    s_wheel.open = 0;
    s_wheel.sel  = -1;
}

/* Slot under a direction (x right, y up): slot 0 at the top, clockwise. */
static int WheelSlot(float x, float y)
{
    const float seg = 6.28318531f / (float)s_wheel.n;
    float       a   = atan2f(x, y);   /* 0 = up, clockwise positive */
    if (a < 0.0f) a += 6.28318531f;
    return (int)floorf((a + seg * 0.5f) / seg) % s_wheel.n;
}

static void WheelTrack(VrPadState* st)
{
    float h[3];

    if (s_wheel.n <= 0) return;

    if (fabsf(st->rx) > VR_WHEEL_STICK || fabsf(st->ry) > VR_WHEEL_STICK)
    {
        s_wheel.sel         = WheelSlot(st->rx, st->ry);
        s_wheelSelFromStick = 1;
    }
    else if (VrIk_GetHandInHead(1, h))
    {
        const float dx   = h[0] - s_wheelOrigin[0];
        const float dy   = h[1] - s_wheelOrigin[1];
        const float dead = VR_WHEEL_DEAD_M * g_PcConfig.vrWheelSize / 100.0f;

        if (dx * dx + dy * dy > dead * dead)
        {
            s_wheel.sel         = WheelSlot(dx, dy);
            s_wheelSelFromStick = 0;
        }
        else if (!s_wheelSelFromStick)
        {
            s_wheel.sel = -1;   /* hand back at the centre: nothing */
        }
    }
}

/* World pickups in VR (events_util.c, map events): Full VR first person. */
int Pc_VrPickupsInWorld(void)
{
    /* not g_PcFpsCam: the VR first-person camera is only switched on in
     * gameplay, and these run as events */
    return VrTracking_HeadCamActive() && g_PcConfig.vrGameViewMode == 2;
}

/* A pickup's take screen is up (events call this every tick it runs): the
 * world freezes from the next frame - before the take screen fades the
 * picture out - and the take screen goes to the 2D panel in front of it
 * (vr_tracking.c GetUiContext reads Pc_VrPickupHeld). Held for 150 ms, so
 * the reading does not depend on where in the frame it happens. */
#define VR_PICKUP_HOLD_MS 150u
static Uint32 s_pickupHoldMs;
static int    s_pickupHoldOn;

void Pc_VrPickupHold(void)
{
    const Uint32 now = SDL_GetTicks();
    if (!s_pickupHoldOn || now - s_pickupHoldMs > VR_PICKUP_HOLD_MS)
    {
        extern void VrOvr_MenuFreezeNow(void);
        VrOvr_MenuFreezeNow();
        VrOvr_Log("[VR-HANDS] pickup: take screen on the 2D panel\n");
    }
    s_pickupHoldOn = 1;
    s_pickupHoldMs = now;
}

int Pc_VrPickupHeld(void)
{
    return s_pickupHoldOn && SDL_GetTicks() - s_pickupHoldMs <= VR_PICKUP_HOLD_MS;
}

/* HOLSTER (see the header) */
static int s_holstered;        /* the equipped weapon is in the holster */
static u8  s_holsterWeapon;    /* ...this one (another equipped = drawn) */
static int s_leftWeapon;       /* the equipped knife is in the LEFT hand */

/* The only weapon the left hand takes. Every other one is drawn and fired
 * with the right hand. */
#define VR_LEFT_WEAPON InvItemId_KitchenKnife

/* The hand the equipped weapon is in: 0 = left, 1 = right (world_draw.c, vr_ik.c). */
int VrInteract_WeaponHand(void)
{
    if (s_leftWeapon && g_SavegamePtr->equippedWeapon != VR_LEFT_WEAPON)
        s_leftWeapon = 0;   /* another weapon was equipped: it is in the right hand */
    return s_leftWeapon ? 0 : 1;
}

static int HolsterWeapon(void)
{
    return g_SysWork.playerCombat.weaponAttack != NO_VALUE &&
           g_SavegamePtr->equippedWeapon != InvItemId_Unequipped;
}

/* -1 = no holster this frame, 0 = weapon drawn (holster empty), 1 = holstered */
int VrInteract_HolsterState(void)
{
    VECTOR3 p;
    if (!HolsterWeapon() ||
        !VrIk_HolsterSlotWorld(VrIk_HolsterSlotOf(g_SavegamePtr->equippedWeapon), &p))
        return -1;
    if (s_holstered && s_holsterWeapon != g_SavegamePtr->equippedWeapon)
        s_holstered = 0;   /* another weapon was equipped: it is in the hand */
    return s_holstered;
}

/* GRAB BOXES (vr_holster_box.c draws them): a holster takes the hand that is
 * inside its box - Box_Size (vr_hbox_size) at the hands' size, plus a little
 * margin - in the body's frame. How far in, as the larger of the three axes
 * over the half edge: <= 1 = inside, 0 = at the centre. */
extern void  VrIk_BodyFrame(float out[3][3]);
extern float VrIk_HandScale(void);
#define VR_BOX_MARGIN_M 0.02f

static float BoxHalfM(void)
{
    return (g_PcConfig.vrBoxSize * 0.005f + VR_BOX_MARGIN_M) * VrIk_HandScale();
}

static float HandInBox(const VECTOR3* hand, const VECTOR3* centre)
{
    float body[3][3], d[3], m = 0.0f;
    const float half = BoxHalfM();
    int   j;

    VrIk_BodyFrame(body);
    d[0] = (float)(hand->vx - centre->vx) / 4096.0f;
    d[1] = (float)(hand->vy - centre->vy) / 4096.0f;
    d[2] = (float)(hand->vz - centre->vz) / 4096.0f;
    for (j = 0; j < 3; j++)
    {
        const float a = fabsf(body[0][j] * d[0] + body[1][j] * d[1] + body[2][j] * d[2]) / half;
        if (a > m) m = a;
    }
    return m;
}

/* The weapon in the holster box a hand is in, 0 for none; outDist = how far
 * in (HandInBox). The left hand only finds VR_LEFT_WEAPON. Logged per grip
 * press, so a miss in the headset log says how far off the hand was. */
static u8 HandAtHolster(int handIdx, float* outDist)
{
    VECTOR3 hand, hol;
    float   best = 1e9f;
    u8      bestId = 0;
    int     k;

    *outDist = best;
    if (!VrIk_GetHandWorld(handIdx, &hand))
        return 0;
    for (k = 0; k < 6; k++)
    {
        const int id = VrIk_HolsterSlotItem(k);
        float     d;
        if (!id || (handIdx == 0 && id != VR_LEFT_WEAPON) || !VrIk_HolsterSlotWorld(k, &hol))
            continue;
        d = HandInBox(&hand, &hol);
        if (d < best) { best = d; bestId = (u8)id; }
    }
    if (!bestId)
        return 0;
    VrOvr_Log("[VR-HANDS] grip: %s hand at %.2f of the box of weapon %d (inside <= 1)\n",
              handIdx ? "right" : "left", (double)best, (int)bestId);
    *outDist = best;
    return best <= 1.0f ? bestId : 0;
}

/* The hand (either) is in holster slot k's box: the box lights up
 * (world_draw.c). */
int VrInteract_HandInSlotBox(int slot)
{
    extern int VrIk_MapHolsterSlot(void);
    VECTOR3 hol, hand;
    int     h;

    if (!VrIk_HolsterSlotWorld(slot, &hol))
        return 0;
    for (h = 0; h < 2; h++)
    {
        const int id = VrIk_HolsterSlotItem(slot);
        if (h == 0 && id != VR_LEFT_WEAPON && slot != VrIk_MapHolsterSlot())
            continue;
        if (VrIk_GetHandWorld(h, &hand) && HandInBox(&hand, &hol) <= 1.0f)
            return 1;
    }
    return 0;
}

/* PAPER MAP: it sits in the holster slot set to Map (vr_ik.c
 * VrIk_MapHolsterSlot), like a weapon. A grip there - either hand - takes
 * the area's map out while Harry has it (pc_minimap.c) and it stays in that
 * hand; a grip at the holster with the hand holding it puts it back.
 * Distance in game meters, 1e9 when there is no map to take. */
static float HandToMapHolster(int handIdx)
{
    extern int Pc_MinimapVrMapReady(void);
    extern int VrIk_MapHolsterWorld(VECTOR3* out);
    VECTOR3 hand, hol;
    float   dx, dy, dz, d;

    if (!Pc_MinimapVrMapReady() || !VrIk_GetHandWorld(handIdx, &hand) || !VrIk_MapHolsterWorld(&hol))
        return 1e9f;
    (void)dx; (void)dy; (void)dz;
    d = HandInBox(&hand, &hol);
    VrOvr_Log("[VR-HANDS] grip: %s hand at %.2f of the map holster box (inside <= 1)\n", handIdx ? "right" : "left", (double)d);
    return d;
}

/* One thing per hand: the map taken into the hand holding the drawn weapon
 * sends the weapon to its holster (it stays equipped; a grip at the holster
 * draws it again). */
static void MapTake(int handIdx)
{
    if (g_SavegamePtr->equippedWeapon != InvItemId_Unequipped && VrInteract_WeaponHand() == handIdx &&
        VrInteract_HolsterState() == 0)
    {
        s_holstered     = 1;
        s_holsterWeapon = g_SavegamePtr->equippedWeapon;
        VrOvr_Log("[VR-HANDS] map: weapon %d to its holster\n", (int)s_holsterWeapon);
    }
    s_mapHand = handIdx;
    VrOvr_Log("[VR-HANDS] map: in the %s hand\n", handIdx ? "right" : "left");
}

static void MapPutBack(void)
{
    if (s_mapHand < 0) return;
    s_mapHand = -1;
    VrOvr_Log("[VR-HANDS] map: back in its holster\n");
}

/* The equipped weapon is in a hand and free to strike (vr_melee.c): Full VR
 * gameplay, not holstered, no map in that hand. */
int VrInteract_WeaponLive(void)
{
    return VrHandsGameplay() && VrTracking_HideCutsceneBars() &&
           g_SysWork.playerCombat.weaponAttack != NO_VALUE &&
           VrInteract_HolsterState() != 1 && s_mapHand != VrInteract_WeaponHand();
}

/* A hand holds something - the drawn weapon or the map - so the VR hand is
 * drawn closed around it (vr_hands.c). */
int VrInteract_HandHolds(int hand)
{
    if (s_mapHand == hand)
        return 1;
    return VrHandsGameplay() && g_SysWork.playerCombat.weaponAttack != NO_VALUE &&
           g_SavegamePtr->equippedWeapon != InvItemId_Unequipped &&
           VrInteract_HolsterState() != 1 && VrInteract_WeaponHand() == hand;
}

/* The hand holding the paper map (pc_minimap.c draws it), -1 for none. */
int VrInteract_MapHand(void)
{
    return s_mapHand;
}

/* The upper body is in the aim pose (or attacking from it). */
static int WeaponUp(void)
{
    switch (g_SysWork.playerWork.extra.upperBodyState)
    {
        case PlayerUpperBodyState_Aim:
        case PlayerUpperBodyState_AimTargetLock:
        case PlayerUpperBodyState_AimTargetLockSwitch:
        case PlayerUpperBodyState_Attack:
            return 1;
    }
    return 0;
}

/* A grip pressed in Full VR gameplay (0 = left, 1 = right hand): the nearer
 * holster (weapon or map), else grab. GRIP_IDLE = it did nothing. */
static int GripPress(int handIdx)
{
    const char* side = handIdx ? "right" : "left";
    VECTOR3     hand;
    s32         ev;

    {
        float       weaponDist;
        const u8    at   = HandAtHolster(handIdx, &weaponDist);
        /* the map's holster: an empty hand takes it, the hand holding it puts it back */
        const float mapD = (s_mapHand < 0 || s_mapHand == handIdx) ? HandToMapHolster(handIdx) : 1e9f;

        if (mapD <= 1.0f && (!at || mapD < weaponDist))
        {
            if (s_mapHand == handIdx)
                MapPutBack();
            else
                MapTake(handIdx);
            return GRIP_MAP;
        }
        if (at && at == g_SavegamePtr->equippedWeapon && VrInteract_HolsterState() >= 0)
        {
            if (!s_holstered && VrInteract_WeaponHand() != handIdx)
            {
                /* drawn, in the other hand: this hand takes it */
                s_leftWeapon = (handIdx == 0);
                if (s_mapHand == handIdx) MapPutBack();   /* one thing per hand */
                VrOvr_Log("[VR-HANDS] holster: weapon %d to the %s hand\n", (int)at, side);
                return GRIP_SPENT;
            }
            s_holstered     = !s_holstered;
            s_holsterWeapon = at;
            s_leftWeapon    = (handIdx == 0);
            if (!s_holstered && s_mapHand == handIdx) MapPutBack();   /* one thing per hand */
            VrOvr_Log("[VR-HANDS] holster: weapon %d %s (%s hand)\n", (int)at,
                      s_holstered ? "holstered" : "drawn", side);
            return GRIP_SPENT;
        }
        if (at)
        {
            /* another weapon's holster: equip it, in this hand */
            if (Pc_VrWheelUse(at))
            {
                s_holstered  = 0;
                s_leftWeapon = (handIdx == 0);
                if (s_mapHand == handIdx) MapPutBack();   /* one thing per hand */
                VrOvr_Log("[VR-HANDS] holster: weapon %d drawn (equipped, %s hand)\n", (int)at, side);
            }
            return GRIP_SPENT;
        }
    }

    if (VrIk_GetHandWorld(handIdx, &hand))
    {
        const s32 radius = (s32)(g_PcConfig.vrGrabRadius * 4096.0f);
        ev = Event_PcVrGrabFind(hand.vx, hand.vz, radius);
        if (ev != NO_VALUE)
        {
            g_PcVrGrabEvent = ev;
            VrOvr_Log("[VR-HANDS] grab: map event %d (%s hand)\n", (int)ev, side);
            return GRIP_GRAB;
        }
    }
    return GRIP_IDLE;
}

/* ---- WEAPON WHEEL (Weapon_Select = Weapon_Wheel, instead of the weapon
 * holsters) --------------------------------------------------------------
 * Held right grip that nothing else took (no map holster, no pickup in
 * reach): an oval of the weapons Harry owns - only those, evenly around it,
 * slot 0 at the top, clockwise, in the inventory's weapon order - opens
 * Wheel_Distance ahead of the right controller, upright, facing the way the
 * body faces. It stays where it opened (it rides with Harry if he walks) and
 * never reorders while open. The right hand goes to a weapon: its box lights
 * up (an enlarged hit area, and the slot it is in keeps it until another is
 * clearly nearer, so a neighbour does not take over). Releasing the grip
 * takes the lit weapon into the right hand; nothing lit = nothing changes.
 * Sizes are real cm at the hands' size (1 / World_Scale). */
#define VR_WW_MAX       11
#define VR_WW_HIT_M     0.03f    /* hit area beyond the box, in the wheel's plane */
#define VR_WW_DEPTH_M   0.08f    /* ... and in front of / behind it */
#define VR_WW_STICKY_M  0.015f   /* another slot must be this much nearer to take over */

typedef struct
{
    int     open;
    int     n;
    u8      ids[VR_WW_MAX];
    int     sel;                 /* -1 = none lit */
    VECTOR3 ofs;                 /* centre - Harry's position when it opened (world Q12) */
    float   frame[3][3];         /* columns: right, down, forward (world) */
    float   slot[VR_WW_MAX][2];  /* each slot: right, down from the centre (m) */
    float   half;                /* half a slot box (m) */
} VrWeaponWheel;

static VrWeaponWheel s_ww;

static void WWheelCentre(VECTOR3* out)
{
    out->vx = g_SysWork.playerWork.player.position.vx + s_ww.ofs.vx;
    out->vy = g_SysWork.playerWork.player.position.vy + s_ww.ofs.vy;
    out->vz = g_SysWork.playerWork.player.position.vz + s_ww.ofs.vz;
}

static int WWheelOpen(void)
{
    extern int Pc_VrOwnsWeapon(u8 id);
    const float hs = VrIk_HandScale();
    VECTOR3     hand;
    float       minGap = 1e9f, halfCm;
    int         i;

    memset(&s_ww, 0, sizeof(s_ww));
    for (i = 1; i <= 11 && s_ww.n < VR_WW_MAX; i++)
    {
        const int id = PcConfig_HolsterWeapon(i);
        if (id && Pc_VrOwnsWeapon((u8)id))
            s_ww.ids[s_ww.n++] = (u8)id;
    }
    if (s_ww.n <= 0 || !VrIk_GetHandWorld(1, &hand))
    {
        VrOvr_Log("[VR-HANDS] weapon wheel: nothing to show (%d weapons)\n", s_ww.n);
        return 0;
    }

    VrIk_BodyFrame(s_ww.frame);
    {
        const float d = g_PcConfig.vrWWheelDist * 0.01f * hs * 4096.0f;
        s_ww.ofs.vx = hand.vx + (s32)lrintf(s_ww.frame[0][2] * d) - g_SysWork.playerWork.player.position.vx;
        s_ww.ofs.vy = hand.vy + (s32)lrintf(s_ww.frame[1][2] * d) - g_SysWork.playerWork.player.position.vy;
        s_ww.ofs.vz = hand.vz + (s32)lrintf(s_ww.frame[2][2] * d) - g_SysWork.playerWork.player.position.vz;
    }
    for (i = 0; i < s_ww.n; i++)
    {
        const float a = 6.28318531f * (float)i / (float)s_ww.n;
        s_ww.slot[i][0] = 0.5f * g_PcConfig.vrWWheelWidth * 0.01f * hs * sinf(a);
        s_ww.slot[i][1] = -0.5f * g_PcConfig.vrWWheelHeight * 0.01f * hs * cosf(a);
    }
    /* the boxes never overlap: at most 90% of the nearest neighbour's gap */
    for (i = 0; i < s_ww.n && s_ww.n > 1; i++)
    {
        const int   j  = (i + 1) % s_ww.n;
        const float dx = s_ww.slot[i][0] - s_ww.slot[j][0], dy = s_ww.slot[i][1] - s_ww.slot[j][1];
        const float g  = sqrtf(dx * dx + dy * dy);
        if (g < minGap) minGap = g;
    }
    halfCm    = g_PcConfig.vrBoxSize * 0.5f;
    s_ww.half = halfCm * 0.01f * hs;
    if (s_ww.n > 1 && s_ww.half > 0.45f * minGap)
        s_ww.half = 0.45f * minGap;
    s_ww.sel  = -1;
    s_ww.open = 1;
    VrOvr_Log("[VR-HANDS] weapon wheel: open, %d weapons, box %.1f cm\n", s_ww.n, (double)(s_ww.half * 200.0f / hs));
    return 1;
}

static void WWheelTrack(void)
{
    VECTOR3 hand, c;
    float   d[3], p[3], best = 1e9f, cur = 1e9f;
    int     i, j, bestI = -1;

    if (!VrIk_GetHandWorld(1, &hand))
        return;
    WWheelCentre(&c);
    d[0] = (float)(hand.vx - c.vx) / 4096.0f;
    d[1] = (float)(hand.vy - c.vy) / 4096.0f;
    d[2] = (float)(hand.vz - c.vz) / 4096.0f;
    for (j = 0; j < 3; j++)
        p[j] = s_ww.frame[0][j] * d[0] + s_ww.frame[1][j] * d[1] + s_ww.frame[2][j] * d[2];

    {
        const float hs    = VrIk_HandScale();
        const float reach = s_ww.half + VR_WW_HIT_M * hs;
        const float depth = s_ww.half + VR_WW_DEPTH_M * hs;
        for (i = 0; i < s_ww.n; i++)
        {
            const float dx = p[0] - s_ww.slot[i][0], dy = p[1] - s_ww.slot[i][1];
            float       q;
            if (fabsf(dx) > reach || fabsf(dy) > reach || fabsf(p[2]) > depth)
                continue;
            q = sqrtf(dx * dx + dy * dy);
            if (i == s_ww.sel) cur = q;
            if (q < best) { best = q; bestI = i; }
        }
        if (s_ww.sel >= 0 && cur < 1e8f && bestI != s_ww.sel && best > cur - VR_WW_STICKY_M * hs)
            bestI = s_ww.sel;   /* still in the lit box: a neighbour must be clearly nearer */
    }
    if (bestI != s_ww.sel)
    {
        s_ww.sel = bestI;
        if (bestI >= 0)
            VrOvr_Log("[VR-HANDS] weapon wheel: weapon %d lit\n", (int)s_ww.ids[bestI]);
    }
}

/* apply = the grip was released: the lit weapon into the right hand. */
static void WWheelClose(int apply)
{
    if (!s_ww.open) return;
    if (apply && s_ww.sel >= 0)
    {
        const u8 id = s_ww.ids[s_ww.sel];
        if (id == g_SavegamePtr->equippedWeapon)
        {
            s_leftWeapon = 0;
            VrOvr_Log("[VR-HANDS] weapon wheel: weapon %d already in hand\n", (int)id);
        }
        else if (Pc_VrWheelUse(id))
        {
            s_holstered  = 0;
            s_leftWeapon = 0;
            if (s_mapHand == 1) MapPutBack();   /* one thing per hand */
            VrOvr_Log("[VR-HANDS] weapon wheel: weapon %d drawn (right hand)\n", (int)id);
        }
    }
    s_ww.open = 0;
    s_ww.sel  = -1;
}

/* For the drawing (world_draw.c): the wheel's centre (world Q12) and frame,
 * each slot's weapon and centre (world Q12), the half box (m), the lit slot. */
int VrInteract_WeaponWheel(VECTOR3* centre, float frame[3][3], u8* ids, VECTOR3* slots, float* half, int* sel)
{
    VECTOR3 c;
    int     i, j;

    if (!s_ww.open)
        return 0;
    WWheelCentre(&c);
    *centre = c;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) frame[i][j] = s_ww.frame[i][j];
    for (i = 0; i < s_ww.n; i++)
    {
        ids[i] = s_ww.ids[i];
        slots[i].vx = c.vx + (s32)lrintf((s_ww.frame[0][0] * s_ww.slot[i][0] + s_ww.frame[0][1] * s_ww.slot[i][1]) * 4096.0f);
        slots[i].vy = c.vy + (s32)lrintf((s_ww.frame[1][0] * s_ww.slot[i][0] + s_ww.frame[1][1] * s_ww.slot[i][1]) * 4096.0f);
        slots[i].vz = c.vz + (s32)lrintf((s_ww.frame[2][0] * s_ww.slot[i][0] + s_ww.frame[2][1] * s_ww.slot[i][1]) * 4096.0f);
    }
    *half = s_ww.half;
    *sel  = s_ww.sel;
    return s_ww.n;
}

/* The weapons the wheel can show (owned), for loading their models ahead. */
int VrInteract_WheelMode(void)
{
    return g_PcConfig.vrWeaponSelect == 1;
}

static void VrInteract_ProcessHands(VrPadState* st);

/* The right trigger and grip as the controller reports them, before any of
 * the rules below: the VR pointer's clicks (vr_full.c). While its ray is on
 * a menu panel they are its own, so the game does not also get them. */
static float s_rawRt, s_rawRgrip, s_rawLt, s_rawLgrip;

void VrInteract_RawRight(float* trigger, float* grip)
{
    *trigger = s_rawRt;
    *grip    = s_rawRgrip;
}

/* Either hand's raw trigger and grip (0 = left, 1 = right): the VR hands'
 * fingers (vr_hands.c). */
void VrInteract_RawHand(int hand, float* trigger, float* grip)
{
    *trigger = hand ? s_rawRt : s_rawLt;
    *grip    = hand ? s_rawRgrip : s_rawLgrip;
}

static void PointerTake(VrPadState* st)
{
    extern int VrFull_PointerOnPanel(void);

    s_rawRt    = st->valid ? st->rt : 0.0f;
    s_rawRgrip = st->valid ? st->rgrip : 0.0f;
    s_rawLt    = st->valid ? st->lt : 0.0f;
    s_rawLgrip = st->valid ? st->lgrip : 0.0f;
    if (VrFull_PointerOnPanel())
        st->rt = st->rgrip = 0.0f;
}

void VrInteract_Process(VrPadState* st)
{
    PointerTake(st);
    VrInteract_ProcessHands(st);
}

static void VrInteract_ProcessHands(VrPadState* st)
{
    const int play = st->valid && VrHandsGameplay();
    const int full = play && VrTracking_HideCutsceneBars();
    const int gripNow = s_gripDown ? (st->rgrip > VR_GRIP_OFF) : (st->rgrip > VR_GRIP_ON);
    const int xNow    = st->x != 0;
    const int aNow    = st->a != 0;
    const float physLt = st->lt;        /* the left trigger itself: the left-hand knife's */

    /* ---- A: a tap for the game, a hold for the wheel ---- */
    if (!full)
    {
        if (s_wheel.open)
            WheelClose(0);              /* cancelled: nothing is used */
        s_aHeld = 0;                    /* A is the game's again, as it is */
    }
    else
    {
        const Uint32 now = SDL_GetTicks();

        if (aNow && !s_aDown && s_aArmed)
        {
            s_aHeld  = 1;
            s_aSince = now;
        }
        if (s_aHeld && aNow && !s_wheel.open && now - s_aSince >= VR_WHEEL_HOLD_MS)
        {
            if (WheelOpen())
                VrOvr_Log("[VR-HANDS] wheel: open, %d entries\n", s_wheel.n);
            else
            {
                s_aHeld = 2;            /* nothing to show: the hold is spent */
                VrOvr_Log("[VR-HANDS] wheel: nothing to show (%d entries: no healing item, no map)\n", s_wheel.n);
            }
        }
        if (s_wheel.open)
        {
            WheelTrack(st);
            st->rx = st->ry = 0.0f;     /* no turning while picking */
            st->rstick = 0;
        }
        if (s_aHeld && !aNow)
        {
            if (s_wheel.open)
                WheelClose(1);
            else if (s_aHeld == 1 && now - s_aSince < VR_WHEEL_HOLD_MS)
                s_aPulse = VR_TAP_PULSE;
            s_aHeld = 0;
        }
        if (s_aHeld)
            st->a = 0;                  /* held: not the game's (yet) */
    }
    s_aArmed = full ? (s_aArmed || !aNow) : 0;
    s_aDown  = aNow;
    if (s_aPulse > 0)
    {
        st->a = 1;
        s_aPulse--;
    }
    if (s_yPulse > 0)
    {
        st->y = 1;                      /* the wheel's Map: the game's Map button */
        s_yPulse--;
    }

    /* ---- right grip: holster (weapon or map) > grab > weapon wheel, decided
     * per press ---- */
    if (!full)
    {
        if (s_gripOwner != GRIP_IDLE)
            s_gripOwner = GRIP_SPENT;   /* the rest of this press is dead */
        WWheelClose(0);                 /* cancelled: nothing changes */
    }
    else if (gripNow && !s_gripDown)
    {
        s_gripOwner = s_gripArmed ? GripPress(1) : GRIP_SPENT;
        if (s_gripOwner == GRIP_IDLE && g_PcConfig.vrWeaponSelect == 1 && WWheelOpen())
            s_gripOwner = GRIP_WHEEL;
    }
    if (s_ww.open && full && gripNow)
        WWheelTrack();

    if (!gripNow && s_gripDown)
    {
        WWheelClose(1);
        s_gripOwner = GRIP_IDLE;        /* the map stays in the hand: the holster puts it back */
    }
    if (full)
        st->rgrip = 0.0f;               /* the game never sees it in gameplay */
    s_gripArmed = full ? (s_gripArmed || !gripNow) : 0;
    s_gripDown  = gripNow;

    /* ---- left grip: the same as the right one; a press that does nothing
     * (GRIP_IDLE) is the game's, as before ---- */
    {
        const int lgripNow = s_lgripDown ? (st->lgrip > VR_GRIP_OFF) : (st->lgrip > VR_GRIP_ON);

        if (!full)
        {
            if (s_lgripOwner != GRIP_IDLE)
                s_lgripOwner = GRIP_SPENT;
        }
        else if (lgripNow && !s_lgripDown)
        {
            s_lgripOwner = s_lgripArmed ? GripPress(0) : GRIP_IDLE;
        }
        if (!lgripNow && s_lgripDown)
            s_lgripOwner = GRIP_IDLE;
        if (s_lgripOwner != GRIP_IDLE)
            st->lgrip = 0.0f;           /* this press is ours */
        s_lgripArmed = full ? (s_lgripArmed || !lgripNow) : 0;
        s_lgripDown  = lgripNow;
    }
    /* no map to hold any more (another area without one, holster slot
     * changed): it goes back */
    {
        extern int Pc_MinimapVrMapReady(void);
        extern int VrIk_MapHolsterSlot(void);
        if (s_mapHand >= 0 && play && (!Pc_MinimapVrMapReady() || VrIk_MapHolsterSlot() < 0))
            MapPutBack();
    }

    /* holstered, or the map in the weapon's hand: no Aim, no fire, no reload */
    if (VrInteract_HolsterState() == 1 || s_mapHand == VrInteract_WeaponHand())
    {
        st->lt = 0.0f;
        s_reloadPhase = 0;
    }

    /* ---- the weapon's trigger: the trigger of the hand that holds it ---- */
    {
        const int   weapon  = play && g_SysWork.playerCombat.weaponAttack != NO_VALUE &&
                              VrInteract_HolsterState() != 1 && s_mapHand != VrInteract_WeaponHand();
        const float wt      = !weapon ? 0.0f : (VrInteract_WeaponHand() == 0) ? physLt : st->rt;
        const int   fireNow = s_fireDown ? (wt > VR_FIRE_OFF) : (wt > VR_FIRE_ON);

        if (weapon)
        {
            /* the game's Aim is LT and its Shoot RT: both come from here */
            st->lt = wt;
            st->rt = 0.0f;
        }

        if (fireNow && !s_fireDown && s_fireArmed && weapon)
            s_firing = 1;
        if (!fireNow || !weapon)
            s_firing = 0;
        /* Action/Shoot only once the weapon is up: pressed with Aim in one
         * pull it would land before the aim pose, where it is the plain
         * action button (and the free-aim FSM takes a shoot already held
         * on entry as no press at all). */
        if (s_firing && WeaponUp())
            st->rt = 1.0f;
        s_fireArmed = play ? (s_fireArmed || !fireNow) : 0;
        s_fireDown  = fireNow;
        if (fireNow && !s_firePrevLogged)
        {
            /* one line per full pull: why a shot did or did not go */
            VrOvr_Log("[VR-HANDS] fire: weapon %d, raised %d, rounds in the gun %d\n",
                      weapon ? (int)g_SavegamePtr->equippedWeapon : 0, WeaponUp(),
                      (int)g_SysWork.playerCombat.currentWeaponAmmo);
        }
        s_firePrevLogged = fireNow;
    }

    /* ---- X in the inventory: it closes it again (the game's item button,
     * Select, which the controllers lack). Cancel only moves to Exit. ---- */
    {
        static int s_xInvDown, s_xInvArmed, s_backPulse;
        const int  inInv = g_GameWork.gameState == GameState_InventoryScreen;

        if (inInv)
        {
            /* a fresh press: the hold that opened it does not close it */
            if (xNow && !s_xInvDown && s_xInvArmed)
                s_backPulse = VR_TAP_PULSE;
            st->x = 0;
        }
        s_xInvArmed = inInv && (s_xInvArmed || !xNow);
        s_xInvDown  = inInv && xNow;
        if (s_backPulse > 0)
        {
            st->back = 1;
            s_backPulse--;
        }
    }

    /* ---- X: a tap reloads (on release), held it opens the inventory ---- */
    {
        const Uint32 now = SDL_GetTicks();

        if (xNow && !s_xDown && s_xArmed && play)
        {
            s_xHeld  = 1;
            s_xSince = now;
        }
        if (s_xHeld == 1 && xNow && now - s_xSince >= VR_MENU_HOLD_MS)
        {
            MenuRequest(1);
            s_xHeld = 2;                  /* spent: no reload on release */
        }
        if (s_xHeld && !xNow)
        {
            if (s_xHeld == 1 && play && VrInteract_HolsterState() != 1 && s_mapHand != VrInteract_WeaponHand() &&
                Pc_VrReloadPress())
            {
                s_reloadPhase = 1;
                s_reloadStart = now;
            }
            s_xHeld = 0;
        }
        if (!play)
            s_xHeld = 0;

        if (s_reloadPhase != 0)
        {
            const int reloading = g_SysWork.playerWork.extra.upperBodyState == PlayerUpperBodyState_Reload;

            if (!play || g_SysWork.playerCombat.weaponAttack == NO_VALUE ||
                now - s_reloadStart > VR_RELOAD_MAX_MS ||
                (s_reloadPhase == 1 && !reloading && now - s_reloadStart > VR_RELOAD_WAIT_MS) ||
                (s_reloadPhase == 2 && !reloading))
            {
                s_reloadPhase = 0;
            }
            else
            {
                if (reloading)
                    s_reloadPhase = 2;
                else
                    Pc_VrReloadPress();   /* re-asked until it starts: the rules refuse it after */
                st->lt = 1.0f;            /* Aim held: the game reloads a raised gun */
            }
        }

        s_xArmed = play ? (s_xArmed || !xNow) : 0;
        s_xDown  = xNow;
        st->x    = 0;
    }

    /* ---- Menu (the game's Start / pause): a tap pauses (on release),
     * held VR_OPTIONS_HOLD_MS it opens the options ---- */
    {
        const Uint32 now     = SDL_GetTicks();
        const int    menuNow = st->menu != 0;

        if (!full)
        {
            s_menuHeld = 0;               /* menus, events: the game's, as it is */
        }
        else
        {
            if (menuNow && !s_menuDown && s_menuArmed)
            {
                s_menuHeld  = 1;
                s_menuSince = now;
            }
            if (s_menuHeld == 1 && menuNow && now - s_menuSince >= VR_OPTIONS_HOLD_MS)
            {
                MenuRequest(2);
                s_menuHeld = 2;
            }
            if (s_menuHeld && !menuNow)
            {
                if (s_menuHeld == 1)
                    s_menuPulse = VR_TAP_PULSE;
                s_menuHeld = 0;
            }
            if (s_menuHeld)
                st->menu = 0;
        }
        s_menuArmed = full ? (s_menuArmed || !menuNow) : 0;
        s_menuDown  = menuNow;
        if (s_menuPulse > 0)
        {
            st->menu = 1;
            s_menuPulse--;
        }
    }
}

/* Game thread (game_sys_states.c, the gameplay update's menu buttons): a
 * held-button menu from above, 1 = inventory, 2 = options. Taken once; it
 * goes stale if gameplay does not take it soon. */
int Pc_VrMenuRequestTake(int which)
{
    if (s_menuReq != which)
        return 0;
    s_menuReq = 0;
    if ((Sint32)(SDL_GetTicks() - s_menuReqUntil) > 0)
        return 0;
    VrOvr_Log("[VR-HANDS] %s opened (button held)\n",
              which == 1 ? "inventory" : which == 2 ? "options" : "PC options (last page)");
    {
        /* the world as it is now stays around the menu (vr_openvr.c) */
        extern void VrOvr_MenuFreezeNow(void);
        VrOvr_MenuFreezeNow();
    }
    return 1;
}

/* Y held (vr_openvr.c): quick settings - PC Options straight on the page last
 * shown, back to the game on leaving (options.c Pc_Options_RequestDirectPc).
 * Replaces the VR quick menu panel for now (Renan, 28/10). */
void VrInteract_RequestPcOptions(void)
{
    MenuRequest(3);
}

/* game_sys_states.c, with the options request: 3 opens the options menu as 2
 * does, marked to go straight to the PC pages */
int Pc_VrPcOptionsRequestTake(void)
{
    extern void Pc_Options_RequestDirectPc(void);
    if (!Pc_VrMenuRequestTake(3))
        return 0;
    Pc_Options_RequestDirectPc();
    return 1;
}

/* Game thread, each frame (pc_combat.c Pc_ExtraActionsUpdate): use the
 * wheel's pick once it is safe, or drop it. */
void VrInteract_GameUpdate(void)
{
    if (s_pendingUse < 0) return;
    if ((Sint32)(SDL_GetTicks() - s_pendingUntil) > 0)
    {
        s_pendingUse = -1;
        return;
    }
    if (Pc_VrWheelUse((u8)s_pendingUse))
        s_pendingUse = -1;
}

int VrInteract_GetWheel(VrWheelView* out)
{
    if (!s_wheel.open) return 0;
    *out = s_wheel;
    return 1;
}
