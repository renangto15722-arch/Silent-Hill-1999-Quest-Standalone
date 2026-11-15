/*
=============================================================================
 SH1 PC Port - VR MELEE: physical strikes with the melee weapons
=============================================================================

 In Full VR the melee weapon in the hand hits by itself: no button. Every
 frame the weapon is a segment along the controller that holds it (grip pose
 -Z, through the fist, as a held handle runs), sampled at a few points. A
 sample inside an enemy's collision cylinder is a contact, and a contact
 lands a hit through the game's own hit function (func_8008B714: the
 weapon's table damage and knockback, the impact sound, the enemy's hit
 reaction) with that damage and knockback scaled by how fast the point was
 moving - real controller speed in the tracking space, so walking adds
 nothing.

 Per weapon (s_melee): its reach, the speed below which a contact does no
 damage, and the speed of a full (factor 1.0) hit; faster swings go up to
 VR_MELEE_FACTOR_MAX. A contact LATCHES that enemy until the weapon has left
 its body, so a weapon resting on or pushed into an enemy never repeats the
 hit. The chainsaw and the rock drill are motor weapons: any contact cuts,
 and while they stay in the body they cut again every VR_MELEE_MOTOR_MS.

 The trigger attack (vr_interact.c) and the guns are untouched. Nothing here
 moves the body or reads more than the controller and head poses, so the
 tracking and the IK are untouched too.
=============================================================================
*/

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/items.h"
#include "bodyprog/collision/ray.h"
#include "pc_pick.h"

#include <math.h>
#include <string.h>

#include <SDL.h>

extern int  VrInteract_WeaponLive(void);
extern int  VrInteract_WeaponHand(void);
extern int  VrOvr_GetHandPose(int hand, float pos[3], float rot[3][3]);
extern int  VrIk_TrackingToWorld(const float p[3], VECTOR3* out);
extern int  VrIk_TrackingDirToWorld(const float d[3], float out[3]);
extern void VrOvr_Log(const char* fmt, ...);

#define VR_MELEE_SAMPLES     3
#define VR_MELEE_THICK       Q12(0.04f)   /* the weapon's own thickness, added to the body */
#define VR_MELEE_MIN_FACTOR  0.15f        /* below this a contact is only a touch */
#define VR_MELEE_FACTOR_MAX  1.5f
#define VR_MELEE_MOTOR_MS    300u
#define VR_MELEE_MAX_DT      0.1f         /* a longer frame (hitch, menu) measures no speed */

typedef struct
{
    u8    item;       /* InvItemId */
    float reach;      /* m from the grip to the tip, real size */
    float vMin;       /* m/s at the contact point: no damage below */
    float vFull;      /* m/s for the game's normal hit */
    u8    motor;      /* chainsaw / drill: cuts on contact */
} s_VrMelee;

static const s_VrMelee s_melee[] = {
    { InvItemId_KitchenKnife, 0.22f, 1.2f, 3.5f, 0 },
    { InvItemId_SteelPipe,    0.65f, 1.5f, 4.5f, 0 },
    { InvItemId_Hammer,       0.50f, 1.5f, 4.0f, 0 },
    { InvItemId_Katana,       0.90f, 1.5f, 5.0f, 0 },
    { InvItemId_Axe,          0.65f, 1.5f, 4.5f, 0 },
    { InvItemId_Chainsaw,     0.60f, 0.0f, 1.0f, 1 },
    { InvItemId_RockDrill,    0.55f, 0.0f, 1.0f, 1 },
};

/* where along the reach the samples sit (the handle itself does not hit) */
static const float s_sampleAt[VR_MELEE_SAMPLES] = { 0.4f, 0.7f, 1.0f };

#define VR_MELEE_TARGETS ((int)(sizeof(g_SysWork.npcs) / sizeof(g_SysWork.npcs[0])))

static float  s_prev[VR_MELEE_SAMPLES][3];   /* last frame's samples, tracking space */
static int    s_prevValid;
static u8     s_prevItem;
static int    s_prevHand;
static Uint64 s_prevTicks;
static u8     s_latched[16];
static Uint32 s_lastHitMs[16];

static const s_VrMelee* MeleeOf(u8 item)
{
    int i;
    for (i = 0; i < (int)(sizeof(s_melee) / sizeof(s_melee[0])); i++)
        if (s_melee[i].item == item) return &s_melee[i];
    return NULL;
}

static void ResetAll(void)
{
    s_prevValid = 0;
    memset(s_latched, 0, sizeof(s_latched));
}

/* A world point (Q12) inside this character's body, the weapon's thickness
 * included. The cylinder func_8008BF84 tests the game's own melee against. */
static int InsideBody(const s_SubCharacter* c, const VECTOR3* p)
{
    const q19_12 sc  = Pc_Pick_CollScale(c);
    const q19_12 r   = Q12_MULT_PRECISE(c->collision.cylinder.field_2, sc) + VR_MELEE_THICK;
    const q19_12 top = Q12_MULT_PRECISE(c->collision.box.top, sc);
    const q19_12 bot = Q12_MULT_PRECISE(c->collision.box.height, sc);
    const q19_12 y0  = c->position.vy + ((top < bot) ? top : bot) - VR_MELEE_THICK;
    const q19_12 y1  = c->position.vy + ((top < bot) ? bot : top) + VR_MELEE_THICK;
    const float  dx  = (float)(p->vx - (c->position.vx + c->collision.shapeOffsets.box.vx));
    const float  dz  = (float)(p->vz - (c->position.vz + c->collision.shapeOffsets.box.vz));

    if (p->vy < y0 || p->vy > y1)
        return 0;
    return dx * dx + dz * dz <= (float)r * (float)r;
}

static int Hittable(const s_SubCharacter* c)
{
    return c->model.charaId != Chara_None && c->health >= Q12(0.0f) && c->collision.state &&
           !Pc_Pick_IsNoCollide(c);
}

/* No wall between Harry's chest and the contact, as func_8008BF84 checks. */
static int Reachable(const VECTOR3* p)
{
    static s_RayTrace trace;
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    VECTOR3 from;

    from.vx = pl->position.vx;
    from.vy = pl->position.vy + pl->collision.box.top;
    from.vz = pl->position.vz;
    return !Ray_TraceQuery(&trace, &from, p);
}

/* One hit on `t` through the game's hit function, scaled by `factor`.
 * Harry's attack state (field_44) is set up as a swing of this weapon that
 * has not hit `t` yet, and put back afterwards. */
static void Strike(s_SubCharacter* t, VECTOR3* at, const float dirWorld[3], u8 weaponId, float factor)
{
    s_SubCharacter*   pl    = &g_SysWork.playerWork.player;
    const s_SubCharacter_44 saved = pl->field_44;
    const s32         wa    = WEAPON_ATTACK(weaponId, AttackInputType_Tap);
    const s_800AD4C8* info  = &D_800AD4C8[wa];
    const q19_12      amt0  = t->damage.amount;
    const VECTOR3     kb0   = t->damage.position;
    const float       kbF   = (factor < 1.0f) ? factor : 1.0f;

    memset(&pl->field_44.field_24, 0, sizeof(pl->field_44.field_24));
    memset(&pl->field_44.field_48, 0, sizeof(pl->field_44.field_48));
    pl->field_44.field_2  = (u8)wa;
    pl->field_44.field_8  = 0;                  /* this swing has hit nobody */
    pl->field_44.field_14 = info->field_E;      /* motor weapons: inside their cutting window */
    pl->field_44.field_48[0].vx = (s32)lrintf(dirWorld[0] * 4096.0f);   /* knockback along the swing */
    pl->field_44.field_48[0].vy = (s32)lrintf(dirWorld[1] * 4096.0f);
    pl->field_44.field_48[0].vz = (s32)lrintf(dirWorld[2] * 4096.0f);

    /* blades take the PC single-hit scale func_8008A3E0 passes (Q12 0.25) */
    func_8008B714(pl, t, at, (info->field_10 == 1) ? Q12(0.25f) : Q12(1.0f));
    pl->field_44 = saved;

    t->damage.amount      = amt0 + (q19_12)lrintf((float)(t->damage.amount - amt0) * factor);
    t->damage.position.vx = kb0.vx + (s32)lrintf((float)(t->damage.position.vx - kb0.vx) * kbF);
    t->damage.position.vy = kb0.vy + (s32)lrintf((float)(t->damage.position.vy - kb0.vy) * kbF);
    t->damage.position.vz = kb0.vz + (s32)lrintf((float)(t->damage.position.vz - kb0.vz) * kbF);
}

/* Game thread, every frame (pc_combat.c Pc_ExtraActionsUpdate). */
void VrMelee_Update(void)
{
    const u8         item = (u8)g_SavegamePtr->equippedWeapon;
    const s_VrMelee* w    = MeleeOf(item);
    const int        hand = VrInteract_WeaponHand();
    const Uint64     now  = SDL_GetPerformanceCounter();
    const Uint32     ms   = SDL_GetTicks();
    float            gp[3], gr[3][3], dir[3], cur[VR_MELEE_SAMPLES][3], speed[VR_MELEE_SAMPLES], dt;
    VECTOR3          world[VR_MELEE_SAMPLES];
    int              inside[16];
    int              k, i, j;

    if (w == NULL || !VrInteract_WeaponLive() || !VrOvr_GetHandPose(hand, gp, gr))
    {
        ResetAll();
        return;
    }

    /* the weapon runs out of the fist: grip pose -Z (OpenXR) / the Touch pose's -Z (OpenVR) */
    for (k = 0; k < 3; k++) dir[k] = -gr[k][2];

    dt = (float)(now - s_prevTicks) / (float)SDL_GetPerformanceFrequency();
    if (item != s_prevItem || hand != s_prevHand || dt <= 0.0f || dt > VR_MELEE_MAX_DT)
        s_prevValid = 0;

    for (j = 0; j < VR_MELEE_SAMPLES; j++)
    {
        for (k = 0; k < 3; k++)
            cur[j][k] = gp[k] + dir[k] * w->reach * s_sampleAt[j];
        speed[j] = 0.0f;
        if (s_prevValid)
        {
            const float dx = cur[j][0] - s_prev[j][0], dy = cur[j][1] - s_prev[j][1], dz = cur[j][2] - s_prev[j][2];
            speed[j] = sqrtf(dx * dx + dy * dy + dz * dz) / dt;
        }
        if (!VrIk_TrackingToWorld(cur[j], &world[j]))
        {
            ResetAll();
            return;
        }
    }

    for (i = 0; i < VR_MELEE_TARGETS && i < 16; i++)
    {
        s_SubCharacter* t = &g_SysWork.npcs[i];
        int             hitSample = -1;

        inside[i] = 0;
        if (!Hittable(t))
            continue;
        for (j = VR_MELEE_SAMPLES - 1; j >= 0 && hitSample < 0; j--)   /* the tip first */
            if (InsideBody(t, &world[j]))
                hitSample = j;
        if (hitSample < 0)
            continue;
        inside[i] = 1;

        /* motor weapons cut again while they stay in; the rest need to leave first */
        if (s_latched[i] && !(w->motor && ms - s_lastHitMs[i] >= VR_MELEE_MOTOR_MS))
            continue;
        if (!s_prevValid || !Reachable(&world[hitSample]))
            continue;

        s_latched[i]   = 1;
        s_lastHitMs[i] = ms;
        {
            const float v = speed[hitSample];
            float factor  = w->motor ? 1.0f : (v - w->vMin) / (w->vFull - w->vMin);
            float vel[3], vw[3], n;

            if (factor > VR_MELEE_FACTOR_MAX) factor = VR_MELEE_FACTOR_MAX;
            if (factor < VR_MELEE_MIN_FACTOR)
            {
                VrOvr_Log("[VR-MELEE] touch: weapon %d on chara %d at %.2f m/s - too slow\n",
                          (int)item, (int)t->model.charaId, (double)v);
                continue;
            }

            /* knockback along the swing (the point's motion), else away from Harry */
            for (k = 0; k < 3; k++) vel[k] = cur[hitSample][k] - s_prev[hitSample][k];
            if (!VrIk_TrackingDirToWorld(vel, vw)) vw[0] = vw[1] = vw[2] = 0.0f;
            n = sqrtf(vw[0] * vw[0] + vw[1] * vw[1] + vw[2] * vw[2]);
            if (n > 1e-6f) { vw[0] /= n; vw[1] /= n; vw[2] /= n; }
            else
            {
                vw[0] = (float)(t->position.vx - g_SysWork.playerWork.player.position.vx);
                vw[1] = 0.0f;
                vw[2] = (float)(t->position.vz - g_SysWork.playerWork.player.position.vz);
                n = sqrtf(vw[0] * vw[0] + vw[2] * vw[2]);
                if (n > 1e-6f) { vw[0] /= n; vw[2] /= n; }
            }

            {
                const q19_12 before = t->damage.amount;
                Strike(t, &world[hitSample], vw, (u8)(item - InvItemId_KitchenKnife), factor);
                VrOvr_Log("[VR-MELEE] hit: weapon %d on chara %d at %.2f m/s, x%.2f, damage %d\n",
                          (int)item, (int)t->model.charaId, (double)v, (double)factor,
                          (int)((t->damage.amount - before) >> 12));
            }
        }
    }

    /* out of a body: that enemy can be hit again */
    for (i = 0; i < VR_MELEE_TARGETS && i < 16; i++)
        if (!inside[i]) s_latched[i] = 0;

    memcpy(s_prev, cur, sizeof(s_prev));
    s_prevValid = 1;
    s_prevItem  = item;
    s_prevHand  = hand;
    s_prevTicks = now;
}
