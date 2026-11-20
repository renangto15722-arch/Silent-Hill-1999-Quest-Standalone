/*
=============================================================================
 SH1 PC Port - VR MELEE: physical strikes with the melee weapons
=============================================================================

 In Full VR the melee weapon in the hand hits by itself, no button, and only
 by a real swing. The game's own swing (the trigger) lands nothing with these
 weapons (VrMelee_BlocksGameHit, bodyprog_combat func_8008B714).

 THE WEAPON. Its shape is the close-up model drawn in the hand, with the VR
 Weapon Grip placement and Scale (vr_ik.c VrIk_HeldItemMatrix,
 vr_weapon_hd.c VrWeaponHd_LengthAxis): the axis from the grip to the tip.
 Until that model is loaded, a segment along the controller (s_melee reach).

 SWUNG WEAPONS (knife, pipe, hammer, axe, katana): two spheres on the axis,
 one at the tip and one where the striking part begins (blade base, head),
 joined into a capsule and tested against the enemy's collision cylinder -
 at this frame's position and half way back to the last one, so a fast swing
 does not pass through. Speeds are measured in the tracking space (the real
 controller motion: walking and turning add nothing).

   A SWING starts when the tip moves faster than 60% of the weapon's minimum
   speed, and ends when the motion reverses (a return or back-swing is a new
   swing) or slows under 35% of it for 80 ms. It records its time and the
   length of the tip's path (the arc).
   A CONTACT (the capsule entering an enemy) is a VALID hit only inside a
   swing that has lasted the weapon's minimum time, travelled its minimum
   arc, and is at least at its minimum speed. Anything else - a touch, a push,
   a short shake - does no damage.
   Each enemy takes ONE hit per swing (both spheres and every sample count
   as one weapon), and the weapon has to leave the body before it can hit it
   again. After a hit the next swing cannot begin before the weapon's
   recovery time.
   DAMAGE is the game's own table damage for the weapon (func_8008B714),
   scaled by the swing: 25% at the minimum speed up to 100% at the weapon's
   full speed with its full arc (a fast but short swing reaches 70%). Never
   above the game's damage.

 MOTOR WEAPONS (chainsaw, rock drill): a box along the cutting part (the
 bar / the bit). While the trigger is held with the motor running, an enemy
 in the box takes the game's damage once per interval (Chainsaw_Hit_Every_ms
 / Drill_Hit_Every_ms), one timer per enemy.

 The guns are untouched (not in s_melee). Nothing here moves the body or
 reads more than the controller and head poses.
=============================================================================
*/

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/items.h"
#include "bodyprog/collision/ray.h"
#include "pc_config.h"
#include "pc_pick.h"

#include <math.h>
#include <string.h>

#include <SDL.h>

extern int    VrInteract_WeaponLive(void);
extern int    VrInteract_WeaponHand(void);
extern int    VrInteract_WeaponTriggerHeld(void);
extern int    VrOvr_GetHandPose(int hand, float pos[3], float rot[3][3]);
extern int    VrIk_TrackingToWorld(const float p[3], VECTOR3* out);
extern int    VrIk_WorldToTracking(const float* w, float* p, int n);
extern int    VrIk_HeldItemMatrix(MATRIX* out);
extern float  VrIk_UnitsPerMeter(void);
extern int    VrWeaponHd_LengthAxis(int itemId, float dir[3], float* len);
extern q19_12 Game_GasWeaponPowerTimerValue(void);
extern void   VrOvr_Log(const char* fmt, ...);

#define VR_MELEE_CAPSULE_N   5            /* samples along the capsule (both spheres included) */
#define VR_MELEE_START_K     0.60f        /* a swing starts at this x vMin */
#define VR_MELEE_REST_K      0.35f        /* ... and ends under this x vMin ... */
#define VR_MELEE_REST_MS     80.0f        /* ... held this long */
#define VR_MELEE_REVERSE     -0.20f       /* cos between the motion and the swing: below = reversed */
#define VR_MELEE_MIN_FACTOR  0.25f        /* damage of a valid hit at the minimum speed */
#define VR_MELEE_MAX_DT      0.1f         /* a longer frame (hitch, menu) measures no speed */
#define VR_MELEE_TARGETS     16

typedef struct
{
    u8    item;       /* InvItemId */
    u8    motor;      /* chainsaw / drill: box, cuts on an interval */
    float reach;      /* m, grip to tip, before the model is loaded */
    float from;       /* the striking part starts at this fraction of the length */
    float radius;     /* m: the spheres (swung) / the box's half width (motor) */
    float vMin;       /* m/s at the tip: slower is no hit */
    float vFull;      /* m/s at the tip for the game's full damage */
    float arcMin;     /* m of tip path in this swing before a hit counts */
    float arcFull;    /* m of tip path for the full damage */
    float minMs;      /* ms the swing must have lasted */
    float recoverMs;  /* ms after a hit before the next swing can begin */
} s_VrMelee;

/* Knife: short, fast, light. Pipe: medium, moderate force. Hammer / axe:
 * long, heavy, slow - the most force. Katana: long, fast, a little less force
 * than the pipe. (Reach itself is the model's real length.) */
static const s_VrMelee s_melee[] = {
    /* item                   motor reach from  radius vMin vFull arcMin arcFull minMs recover */
    /* calibrated on the headset 20/11: real swings put the tip at 15-40 m/s
     * over 1.5-4 m, so a medium swing is medium damage and only a hard, wide
     * one reaches the game's full damage */
    { InvItemId_KitchenKnife, 0,    0.22f, 0.45f, 0.035f, 3.0f,  9.0f, 0.10f, 0.40f,  30.0f, 120.0f },
    { InvItemId_SteelPipe,    0,    0.65f, 0.40f, 0.040f, 6.0f, 18.0f, 0.40f, 1.20f,  80.0f, 350.0f },
    { InvItemId_Hammer,       0,    0.50f, 0.75f, 0.060f, 8.0f, 22.0f, 0.50f, 1.50f, 110.0f, 550.0f },
    { InvItemId_Axe,          0,    0.65f, 0.70f, 0.060f, 8.0f, 22.0f, 0.50f, 1.50f, 110.0f, 550.0f },
    { InvItemId_Katana,       0,    0.90f, 0.30f, 0.035f, 5.0f, 16.0f, 0.35f, 1.10f,  60.0f, 250.0f },
    { InvItemId_Chainsaw,     1,    0.60f, 0.35f, 0.070f, 0.0f, 0.0f, 0.00f, 0.00f,   0.0f,   0.0f },
    { InvItemId_RockDrill,    1,    0.55f, 0.45f, 0.050f, 0.0f, 0.0f, 0.00f, 0.00f,   0.0f,   0.0f },
};

/* last frame */
static int    s_prevValid;
static u8     s_prevItem;
static int    s_prevHand;
static Uint64 s_prevTicks;
static float  s_prevTipT[3];               /* tip, tracking space (m) */
static float  s_prevTipW[3], s_prevBaseW[3];   /* world Q8 */
static float  s_prevSpeed;

/* the swing */
static int    s_swing;
static Uint32 s_swingT0;
static float  s_arc, s_slowMs, s_dir[3];
static u32    s_hitMask;                   /* enemies hit (or touched) by this swing */
static Uint32 s_nextSwingMs;               /* recovery: no new swing before */

/* per enemy */
static u8     s_latched[VR_MELEE_TARGETS]; /* in the body since the last contact */
static u8     s_pending[VR_MELEE_TARGETS]; /* in the body, fast, swing still too young: judged again */
static u32    s_latchSwing[VR_MELEE_TARGETS]; /* the swing that last judged it */
static u32    s_swingId;                   /* counts swings */
static Uint32 s_lastCutMs[VR_MELEE_TARGETS];

static int    s_inStrike;                  /* our own func_8008B714 call */

/* Show_Hitboxes (vr_show_hitboxes): this frame's volumes, world Q12 */
static int    s_dbgValid, s_dbgMotor, s_dbgLive;
static float  s_dbgA[3], s_dbgB[3], s_dbgR;
static Uint32 s_dbgHitMs;

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
    s_dbgValid  = 0;
    s_swing     = 0;
    s_hitMask   = 0;
    memset(s_latched, 0, sizeof(s_latched));
    memset(s_pending, 0, sizeof(s_pending));
}

/* bodyprog_combat func_8008B714: a hit by Harry on an enemy that did not come
 * from here, with a melee or motor weapon, while that weapon is live in a VR
 * hand - the game's own swing. Kick and stomp (8, 9) and the guns pass. */
int VrMelee_BlocksGameHit(s32 weaponAttack)
{
    const s32 id = weaponAttack % 10;
    if (s_inStrike || weaponAttack < 0 || weaponAttack >= 30 || id > EquippedWeaponId_Axe)
        return 0;
    return MeleeOf((u8)g_SavegamePtr->equippedWeapon) != NULL && VrInteract_WeaponLive();
}

static int Hittable(const s_SubCharacter* c)
{
    return c->model.charaId != Chara_None && c->health >= Q12(0.0f) && c->collision.state &&
           !Pc_Pick_IsNoCollide(c);
}

/* The enemy's collision cylinder (the one func_8008BF84 tests the game's
 * melee against): centre x/z, radius, bottom/top y, world Q12. */
typedef struct { float x, z, r, y0, y1; } Cyl;

static void CylOf(const s_SubCharacter* c, Cyl* o)
{
    const q19_12 sc  = Pc_Pick_CollScale(c);
    const q19_12 top = Q12_MULT_PRECISE(c->collision.box.top, sc);
    const q19_12 bot = Q12_MULT_PRECISE(c->collision.box.height, sc);
    o->r  = (float)Q12_MULT_PRECISE(c->collision.cylinder.field_2, sc);
    o->y0 = (float)(c->position.vy + ((top < bot) ? top : bot));
    o->y1 = (float)(c->position.vy + ((top < bot) ? bot : top));
    o->x  = (float)(c->position.vx + c->collision.shapeOffsets.box.vx);
    o->z  = (float)(c->position.vz + c->collision.shapeOffsets.box.vz);
}

/* a sphere (world Q12 centre, radius) touches the cylinder */
static int SphereInCyl(const Cyl* c, const float p[3], float r)
{
    const float dx = p[0] - c->x, dz = p[2] - c->z, rr = c->r + r;
    if (p[1] < c->y0 - r || p[1] > c->y1 + r)
        return 0;
    return dx * dx + dz * dz <= rr * rr;
}

/* the capsule a..b (world Q12) touches the cylinder; the touching point out */
static int CapsuleInCyl(const Cyl* c, const float a[3], const float b[3], float r, float at[3])
{
    int j, k;
    for (j = VR_MELEE_CAPSULE_N - 1; j >= 0; j--)   /* the tip end first */
    {
        const float t = (float)j / (float)(VR_MELEE_CAPSULE_N - 1);
        float       p[3];
        for (k = 0; k < 3; k++) p[k] = b[k] + (a[k] - b[k]) * t;
        if (SphereInCyl(c, p, r))
        {
            for (k = 0; k < 3; k++) at[k] = p[k];
            return 1;
        }
    }
    return 0;
}

/* the box along b..a (world Q12) with a square section of half width h
 * touches the cylinder: points up the cylinder's axis, each one's distance
 * to the box against the cylinder's radius */
static int BoxInCyl(const Cyl* c, const float a[3], const float b[3], float h, float at[3])
{
    float u[3], e1[3], e2[3], len, y;
    int   k;

    for (k = 0; k < 3; k++) u[k] = a[k] - b[k];
    len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (len < 1.0f)
        return 0;
    for (k = 0; k < 3; k++) u[k] /= len;
    /* any two axes across it (the section is square) */
    if (fabsf(u[1]) < 0.9f) { e1[0] = -u[2]; e1[1] = 0.0f; e1[2] = u[0]; }
    else                    { e1[0] = 0.0f; e1[1] = u[2]; e1[2] = -u[1]; }
    {
        const float n = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
        for (k = 0; k < 3; k++) e1[k] /= n;
    }
    e2[0] = u[1] * e1[2] - u[2] * e1[1];
    e2[1] = u[2] * e1[0] - u[0] * e1[2];
    e2[2] = u[0] * e1[1] - u[1] * e1[0];

    for (y = c->y0; y <= c->y1 + 1.0f; y += Q12(0.08f))
    {
        const float p[3] = { c->x, (y > c->y1) ? c->y1 : y, c->z };
        float       d[3], l0, l1, l2, q0, q1, q2;
        for (k = 0; k < 3; k++) d[k] = p[k] - b[k];
        l0 = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
        l1 = d[0] * e1[0] + d[1] * e1[1] + d[2] * e1[2];
        l2 = d[0] * e2[0] + d[1] * e2[1] + d[2] * e2[2];
        q0 = (l0 < 0.0f) ? l0 : (l0 > len) ? l0 - len : 0.0f;
        q1 = (l1 < -h) ? l1 + h : (l1 > h) ? l1 - h : 0.0f;
        q2 = (l2 < -h) ? l2 + h : (l2 > h) ? l2 - h : 0.0f;
        if (q0 * q0 + q1 * q1 + q2 * q2 <= c->r * c->r)
        {
            /* where: the point of the box nearest the axis point */
            const float cl0 = l0 - q0, cl1 = l1 - q1, cl2 = l2 - q2;
            for (k = 0; k < 3; k++) at[k] = b[k] + u[k] * cl0 + e1[k] * cl1 + e2[k] * cl2;
            return 1;
        }
    }
    return 0;
}

/* No wall between the hand holding the weapon and the contact (the game's
 * melee checks from Harry's chest, func_8008BF84; in VR the weapon is where
 * the hand is, and from the chest a counter or a table between the body and
 * a hand reaching over it blocked real hits). */
static int Reachable(const float gripQ12[3], const float atQ12[3])
{
    static s_RayTrace trace;
    VECTOR3 from, to;

    from.vx = (s32)gripQ12[0];
    from.vy = (s32)gripQ12[1];
    from.vz = (s32)gripQ12[2];
    to.vx   = (s32)atQ12[0];
    to.vy   = (s32)atQ12[1];
    to.vz   = (s32)atQ12[2];
    return !Ray_TraceQuery(&trace, &from, &to);
}

/* One hit on `t` through the game's hit function, scaled by `factor` (<= 1).
 * Harry's attack state (field_44) is set up as a swing of this weapon that
 * has not hit `t` yet, and put back afterwards. */
static void Strike(s_SubCharacter* t, const float atQ12[3], const float dirWorld[3], u8 weaponId, float factor)
{
    s_SubCharacter*   pl    = &g_SysWork.playerWork.player;
    const s_SubCharacter_44 saved = pl->field_44;
    const s32         wa    = WEAPON_ATTACK(weaponId, AttackInputType_Tap);
    const s_800AD4C8* info  = &D_800AD4C8[wa];
    const q19_12      amt0  = t->damage.amount;
    const VECTOR3     kb0   = t->damage.position;
    VECTOR3           at;

    at.vx = (s32)atQ12[0];
    at.vy = (s32)atQ12[1];
    at.vz = (s32)atQ12[2];
    memset(&pl->field_44.field_24, 0, sizeof(pl->field_44.field_24));
    memset(&pl->field_44.field_48, 0, sizeof(pl->field_44.field_48));
    pl->field_44.field_2  = (u8)wa;
    pl->field_44.field_8  = 0;                  /* this swing has hit nobody */
    pl->field_44.field_14 = info->field_E;      /* motor weapons: inside their cutting window */
    pl->field_44.field_48[0].vx = (s32)lrintf(dirWorld[0] * 4096.0f);   /* knockback along the swing */
    pl->field_44.field_48[0].vy = (s32)lrintf(dirWorld[1] * 4096.0f);
    pl->field_44.field_48[0].vz = (s32)lrintf(dirWorld[2] * 4096.0f);

    /* blades take the PC single-hit scale func_8008A3E0 passes (Q12 0.25) */
    s_inStrike = 1;
    func_8008B714(pl, t, &at, (info->field_10 == 1) ? Q12(0.25f) : Q12(1.0f));
    s_inStrike = 0;
    s_dbgHitMs = SDL_GetTicks();
    pl->field_44 = saved;

    if (factor < 1.0f)
    {
        t->damage.amount      = amt0 + (q19_12)lrintf((float)(t->damage.amount - amt0) * factor);
        t->damage.position.vx = kb0.vx + (s32)lrintf((float)(t->damage.position.vx - kb0.vx) * factor);
        t->damage.position.vy = kb0.vy + (s32)lrintf((float)(t->damage.position.vy - kb0.vy) * factor);
        t->damage.position.vz = kb0.vz + (s32)lrintf((float)(t->damage.position.vz - kb0.vz) * factor);
    }
}

static void Normalize(float v[3])
{
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-6f) { v[0] /= n; v[1] /= n; v[2] /= n; }
    else v[0] = v[1] = v[2] = 0.0f;
}

/* The weapon's tip and the start of its striking part this frame: world Q8
 * and the tip in tracking space (m). 2 = from the model in the hand, 1 = the
 * controller fallback, 0 = no pose. */
static int WeaponPoints(const s_VrMelee* w, u8 item, int hand, float tipW[3], float baseW[3], float tipT[3],
                        float gripW[3])
{
    MATRIX hm;
    float  u[3], L;
    int    i;

    if (VrIk_HeldItemMatrix(&hm) && VrWeaponHd_LengthAxis(item, u, &L))
    {
        float lt[3], lb[3];
        for (i = 0; i < 3; i++) { lt[i] = u[i] * L; lb[i] = u[i] * L * w->from; }
        for (i = 0; i < 3; i++)
        {
            tipW[i]  = (hm.m[i][0] * lt[0] + hm.m[i][1] * lt[1] + hm.m[i][2] * lt[2]) / 4096.0f + (float)hm.t[i];
            baseW[i] = (hm.m[i][0] * lb[0] + hm.m[i][1] * lb[1] + hm.m[i][2] * lb[2]) / 4096.0f + (float)hm.t[i];
            gripW[i] = (float)hm.t[i];
        }
        return VrIk_WorldToTracking(tipW, tipT, 1) ? 2 : 0;
    }
    {
        /* the weapon runs out of the fist: grip pose -Z */
        float   gp[3], gr[3][3], bT[3];
        VECTOR3 a, b, g;
        if (!VrOvr_GetHandPose(hand, gp, gr))
            return 0;
        for (i = 0; i < 3; i++)
        {
            tipT[i] = gp[i] - gr[i][2] * w->reach;
            bT[i]   = gp[i] - gr[i][2] * w->reach * w->from;
        }
        if (!VrIk_TrackingToWorld(tipT, &a) || !VrIk_TrackingToWorld(bT, &b) || !VrIk_TrackingToWorld(gp, &g))
            return 0;
        gripW[0] = g.vx / 16.0f; gripW[1] = g.vy / 16.0f; gripW[2] = g.vz / 16.0f;
        tipW[0]  = a.vx / 16.0f; tipW[1]  = a.vy / 16.0f; tipW[2]  = a.vz / 16.0f;
        baseW[0] = b.vx / 16.0f; baseW[1] = b.vy / 16.0f; baseW[2] = b.vz / 16.0f;
        return 1;
    }
}

/* Game thread, every frame (pc_combat.c Pc_ExtraActionsUpdate). */
void VrMelee_Update(void)
{
    const u8         item = (u8)g_SavegamePtr->equippedWeapon;
    const s_VrMelee* w    = MeleeOf(item);
    const int        hand = VrInteract_WeaponHand();
    const Uint64     now  = SDL_GetPerformanceCounter();
    const Uint32     ms   = SDL_GetTicks();
    const u8         wid  = (u8)(item - InvItemId_KitchenKnife);
    float            tipW[3], baseW[3], tipT[3], gripW[3], gQ[3], aQ[3], bQ[3], maQ[3], mbQ[3], vel[3] = { 0, 0, 0 };
    float            speed = 0.0f, dt, rQ, kbW[3];
    int              src, inside[VR_MELEE_TARGETS];
    int              i, k;

    /* once a frame (it runs from Harry's draw, world_draw.c) */
    {
        static Uint64 s_lastCall;
        if (s_lastCall != 0 && now - s_lastCall < SDL_GetPerformanceFrequency() / 1000)
            return;
        s_lastCall = now;
    }
    if (w == NULL || !VrInteract_WeaponLive())
    {
        ResetAll();
        return;
    }
    src = WeaponPoints(w, item, hand, tipW, baseW, tipT, gripW);
    if (!src)
    {
        ResetAll();
        return;
    }

    dt = (float)(now - s_prevTicks) / (float)SDL_GetPerformanceFrequency();
    if (item != s_prevItem || hand != s_prevHand || dt <= 0.0f || dt > VR_MELEE_MAX_DT)
    {
        s_prevValid = 0;
        s_swing     = 0;
    }

    /* world Q12; the sweep: half way back to last frame */
    for (k = 0; k < 3; k++)
    {
        gQ[k] = gripW[k] * 16.0f;
        aQ[k] = tipW[k] * 16.0f;
        bQ[k] = baseW[k] * 16.0f;
        maQ[k] = s_prevValid ? (tipW[k] + s_prevTipW[k]) * 8.0f : aQ[k];
        mbQ[k] = s_prevValid ? (baseW[k] + s_prevBaseW[k]) * 8.0f : bQ[k];
    }
    rQ = w->radius * VrIk_UnitsPerMeter() * 16.0f;

    /* the tip's real motion and the swing it belongs to */
    if (s_prevValid)
    {
        for (k = 0; k < 3; k++) vel[k] = (tipT[k] - s_prevTipT[k]) / dt;
        speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    }
    if (!w->motor && s_prevValid)
    {
        float vn[3] = { vel[0], vel[1], vel[2] };
        Normalize(vn);
        if (s_swing)
        {
            const float c = vn[0] * s_dir[0] + vn[1] * s_dir[1] + vn[2] * s_dir[2];
            if (speed > 0.05f && c < VR_MELEE_REVERSE)
                s_swing = 0;                    /* reversed: the return is a new swing */
            else
            {
                s_arc += speed * dt;
                for (k = 0; k < 3; k++) s_dir[k] = s_dir[k] * 0.75f + vn[k] * 0.25f;
                Normalize(s_dir);
                s_slowMs = (speed < w->vMin * VR_MELEE_REST_K) ? s_slowMs + dt * 1000.0f : 0.0f;
                if (s_slowMs > VR_MELEE_REST_MS)
                    s_swing = 0;                /* stopped */
            }
        }
        if (!s_swing && speed >= w->vMin * VR_MELEE_START_K && (s32)(ms - s_nextSwingMs) >= 0)
        {
            s_swing   = 1;
            s_swingId++;
            s_swingT0 = ms;
            s_arc     = 0.0f;
            s_slowMs  = 0.0f;
            s_hitMask = 0;
            for (k = 0; k < 3; k++) s_dir[k] = vn[k];
        }
    }

    /* knockback along the tip's motion in the world, else away from Harry */
    for (k = 0; k < 3; k++) kbW[k] = s_prevValid ? tipW[k] - s_prevTipW[k] : 0.0f;
    Normalize(kbW);

    for (i = 0; i < VR_MELEE_TARGETS; i++)
    {
        s_SubCharacter* t = &g_SysWork.npcs[i];
        Cyl             cyl;
        float           at[3], dir[3];
        int             hit;

        inside[i] = 0;
        if (!Hittable(t))
            continue;
        CylOf(t, &cyl);
        if (w->motor)
            hit = BoxInCyl(&cyl, aQ, bQ, rQ, at);
        else
            hit = CapsuleInCyl(&cyl, aQ, bQ, rQ, at) || CapsuleInCyl(&cyl, maQ, mbQ, rQ, at);
        if (!hit)
            continue;
        inside[i] = 1;

        for (k = 0; k < 3; k++) dir[k] = kbW[k];
        if (dir[0] == 0.0f && dir[1] == 0.0f && dir[2] == 0.0f)
        {
            dir[0] = cyl.x - (float)g_SysWork.playerWork.player.position.vx;
            dir[2] = cyl.z - (float)g_SysWork.playerWork.player.position.vz;
            Normalize(dir);
        }

        if (w->motor)
        {
            /* cuts while the trigger holds it flat out, once per interval */
            const float every = (item == InvItemId_Chainsaw) ? g_PcConfig.vrSawHitMs : g_PcConfig.vrDrillHitMs;
            if (!VrInteract_WeaponTriggerHeld() || Game_GasWeaponPowerTimerValue() == Q12(0.0f))
                continue;
            if (s_latched[i] && (float)(ms - s_lastCutMs[i]) < every)
                continue;
            if (!Reachable(gQ, at))
            {
                VrOvr_Log("[VR-MELEE] cut blocked: a wall between the hand and chara %d\n", (int)t->model.charaId);
                continue;
            }
            s_latched[i]   = 1;
            s_lastCutMs[i] = ms;
            {
                const q19_12 before = t->damage.amount;
                Strike(t, at, dir, wid, 1.0f);
                VrOvr_Log("[VR-MELEE] cut: weapon %d on chara %d, damage %d\n", (int)item,
                          (int)t->model.charaId, (int)((t->damage.amount - before) >> 12));
            }
            continue;
        }

        /* swung: entering the body, once per swing. A contact that is fast
         * enough but whose swing has only just begun (time / arc not yet
         * reached) stays PENDING and is judged again the next frames while
         * the weapon is still in; anything else is decided at once. */
        /* the same swing never judges an enemy twice; a NEW swing (a real one:
         * the speed, time and arc below) may hit it even if the weapon never
         * left its body - standing close, or walking in, it often does not */
        if ((s_latched[i] && !s_pending[i] && s_latchSwing[i] == s_swingId) || (s_hitMask & (1u << i)))
        {
            s_latched[i] = 1;
            continue;
        }
        s_latched[i]    = 1;
        s_latchSwing[i] = s_swingId;
        {
            const float v      = (speed > s_prevSpeed) ? speed : s_prevSpeed;
            const float swingMs = s_swing ? (float)(ms - s_swingT0) : 0.0f;
            const char* why    = !s_prevValid ? "no motion yet"
                               : !s_swing ? "not a swing"
                               : swingMs < w->minMs ? "swing too short"
                               : s_arc < w->arcMin ? "arc too small"
                               : v < w->vMin ? "too slow" : NULL;
            float speedEff, arcEff, factor;

            if (why && s_swing && v >= w->vMin && (swingMs < w->minMs || s_arc < w->arcMin))
            {
                s_pending[i] = 1;   /* fast, just started: look again next frame */
                continue;
            }
            s_pending[i] = 0;
            if (s_swing)
                s_hitMask |= 1u << i;
            if (why)
            {
                VrOvr_Log("[VR-MELEE] touch: weapon %d on chara %d - %s (%.2f m/s, arc %.2f m, %.0f ms)\n",
                          (int)item, (int)t->model.charaId, why, (double)v, (double)s_arc, (double)swingMs);
                continue;
            }
            if (!Reachable(gQ, at))
            {
                VrOvr_Log("[VR-MELEE] blocked: a wall between the hand and chara %d (%.2f m/s)\n",
                          (int)t->model.charaId, (double)v);
                continue;
            }
            speedEff = (v - w->vMin) / (w->vFull - w->vMin);
            arcEff   = (s_arc - w->arcMin) / (w->arcFull - w->arcMin);
            speedEff = (speedEff < 0.0f) ? 0.0f : (speedEff > 1.0f) ? 1.0f : speedEff;
            arcEff   = (arcEff < 0.0f) ? 0.0f : (arcEff > 1.0f) ? 1.0f : arcEff;
            factor   = VR_MELEE_MIN_FACTOR + (1.0f - VR_MELEE_MIN_FACTOR) * speedEff * (0.6f + 0.4f * arcEff);
            s_nextSwingMs = ms + (Uint32)w->recoverMs;
            {
                const q19_12 before = t->damage.amount;
                Strike(t, at, dir, wid, factor);
                VrOvr_Log("[VR-MELEE] hit: weapon %d on chara %d at %.2f m/s, arc %.2f m, %.0f ms -> x%.2f, damage %d (%s)\n",
                          (int)item, (int)t->model.charaId, (double)v, (double)s_arc, (double)swingMs,
                          (double)factor, (int)((t->damage.amount - before) >> 12), (src == 2) ? "model" : "controller");
            }
        }
    }

    /* out of a body: that enemy can be hit again */
    for (i = 0; i < VR_MELEE_TARGETS; i++)
        if (!inside[i]) s_latched[i] = s_pending[i] = 0;

    for (k = 0; k < 3; k++)
    {
        s_prevTipT[k]  = tipT[k];
        s_prevTipW[k]  = tipW[k];
        s_prevBaseW[k] = baseW[k];
    }
    /* what Show_Hitboxes draws this frame */
    for (k = 0; k < 3; k++) { s_dbgA[k] = aQ[k]; s_dbgB[k] = bQ[k]; }
    s_dbgR     = rQ;
    s_dbgMotor = w->motor;
    s_dbgLive  = w->motor ? (VrInteract_WeaponTriggerHeld() && Game_GasWeaponPowerTimerValue() != Q12(0.0f)) : s_swing;
    s_dbgValid = 1;

    s_prevSpeed = speed;
    s_prevValid = 1;
    s_prevItem  = item;
    s_prevHand  = hand;
    s_prevTicks = now;
}

/* Show_Hitboxes (world_draw.c, with the holsters): the weapon's hit volume -
 * the two spheres and the bar between them (swung) or the box (motor) - and
 * each enemy's body cylinder (green), see-through. The weapon is white while
 * idle, yellow during a swing (motor: trigger held, motor on), red for a
 * moment after a hit. */
void VrMelee_DebugDraw(GsOT* ot)
{
    extern void VrBox_DrawEx(GsOT * ot, const VECTOR3* centre, const float axes[3][3], const float half[3],
                             u8 r, u8 g, u8 b, int alpha);
    static const float ident[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    float   u[3], e1[3], e2[3], axes[3][3], half[3], len, n, r;
    VECTOR3 c;
    u8      cr, cg, cb;
    int     i, k;

    if (!g_PcConfig.vrShowHitboxes || !s_dbgValid || !VrInteract_WeaponLive())
        return;

    /* the enemies' bodies */
    for (i = 0; i < VR_MELEE_TARGETS; i++)
    {
        const s_SubCharacter* t = &g_SysWork.npcs[i];
        Cyl                   cyl;
        if (!Hittable(t))
            continue;
        CylOf(t, &cyl);
        c.vx = (s32)cyl.x;
        c.vy = (s32)((cyl.y0 + cyl.y1) * 0.5f);
        c.vz = (s32)cyl.z;
        half[0] = half[2] = cyl.r / 4096.0f;
        half[1] = (cyl.y1 - cyl.y0) * 0.5f / 4096.0f;
        VrBox_DrawEx(ot, &c, ident, half, 60, 220, 90, 60);
    }

    /* the weapon's frame: u along it (base -> tip), e1 / e2 across */
    for (k = 0; k < 3; k++) u[k] = s_dbgA[k] - s_dbgB[k];
    len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (len < 1.0f)
        return;
    for (k = 0; k < 3; k++) u[k] /= len;
    if (fabsf(u[1]) < 0.9f) { e1[0] = -u[2]; e1[1] = 0.0f; e1[2] = u[0]; }
    else                    { e1[0] = 0.0f; e1[1] = u[2]; e1[2] = -u[1]; }
    n = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (k = 0; k < 3; k++) e1[k] /= n;
    e2[0] = u[1] * e1[2] - u[2] * e1[1];
    e2[1] = u[2] * e1[0] - u[0] * e1[2];
    e2[2] = u[0] * e1[1] - u[1] * e1[0];
    for (k = 0; k < 3; k++) { axes[k][0] = e1[k]; axes[k][1] = e2[k]; axes[k][2] = u[k]; }

    if (SDL_GetTicks() - s_dbgHitMs < 250u) { cr = 255; cg = 40;  cb = 40;  }
    else if (s_dbgLive)                     { cr = 255; cg = 220; cb = 40;  }
    else                                    { cr = 230; cg = 230; cb = 255; }
    r = s_dbgR / 4096.0f;

    if (s_dbgMotor)
    {
        /* the box along the cutting part */
        c.vx = (s32)((s_dbgA[0] + s_dbgB[0]) * 0.5f);
        c.vy = (s32)((s_dbgA[1] + s_dbgB[1]) * 0.5f);
        c.vz = (s32)((s_dbgA[2] + s_dbgB[2]) * 0.5f);
        half[0] = half[1] = r;
        half[2] = len * 0.5f / 4096.0f;
        VrBox_DrawEx(ot, &c, axes, half, cr, cg, cb, 110);
        return;
    }

    /* the two spheres (drawn as cubes of the sphere's radius) ... */
    half[0] = half[1] = half[2] = r;
    c.vx = (s32)s_dbgA[0]; c.vy = (s32)s_dbgA[1]; c.vz = (s32)s_dbgA[2];
    VrBox_DrawEx(ot, &c, axes, half, cr, cg, cb, 130);
    c.vx = (s32)s_dbgB[0]; c.vy = (s32)s_dbgB[1]; c.vz = (s32)s_dbgB[2];
    VrBox_DrawEx(ot, &c, axes, half, cr, cg, cb, 130);
    /* ... and the capsule between them, thinner */
    c.vx = (s32)((s_dbgA[0] + s_dbgB[0]) * 0.5f);
    c.vy = (s32)((s_dbgA[1] + s_dbgB[1]) * 0.5f);
    c.vz = (s32)((s_dbgA[2] + s_dbgB[2]) * 0.5f);
    half[0] = half[1] = r * 0.5f;
    half[2] = len * 0.5f / 4096.0f;
    VrBox_DrawEx(ot, &c, axes, half, cr, cg, cb, 60);
}
