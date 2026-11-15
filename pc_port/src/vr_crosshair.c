/*
=============================================================================
 SH1 PC Port - VR CROSSHAIR: a laser dot where the gun points
=============================================================================

 With a gun in the hand (Full VR, drawn, not holstered), a small red dot sits
 where the shot would land: the same ray player_control.c fires in VR
 (vr_ik.c VrIk_GetGunRay), traced like a bullet (Ray_CharaTraceQuery with the
 bullet hit cylinders, g_PcBulletHitActive) against the world and the
 characters. The dot faces the eyes and keeps about the same apparent size at
 any distance. Nothing shows when the ray hits nothing within VR_XH_RANGE.

 World prims like pc_decals.c: RotTransPers, POLY_F4 from the GsOUT_PACKET_P
 arena, an OT0 bucket just in front of the surface.
=============================================================================
*/

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/collision/ray.h"
#include "bodyprog/view/vw_calc.h"

#include <math.h>

extern int VrInteract_WeaponLive(void);
extern int VrIk_GetGunRay(VECTOR3* pos, VECTOR3* fwd);

#define VR_XH_RANGE_M 60.0f     /* the shot's own aim range (player_control.c) */
#define VR_XH_ANGLE   0.0070f   /* dot radius per meter of distance (~0.4 deg) */
#define VR_XH_MIN_M   0.006f
#define VR_XH_PULL    0.03f     /* drawn this fraction of the way back toward the gun */

static int IsGun(u8 item)
{
    return item == InvItemId_Handgun || item == InvItemId_HuntingRifle ||
           item == InvItemId_Shotgun || item == InvItemId_HyperBlaster;
}

/* Where a shot along the gun ray lands - shared by the dot and by the shot
 * itself (player_control.c aims the bullet at it), so the two always agree.
 * The trace is a 2D one (XZ against walls and character cylinders): its
 * target Y is not the ray's. So the distance ALONG the ray comes from the XZ
 * hit, and the ray stops at the floor Harry stands on if it reaches that
 * first (the trace never hits floors). Returns 1 on a wall, character or
 * floor; 0 = nothing within range (out = the far end of the ray). */
int VrGun_AimPoint(const VECTOR3* from, const VECTOR3* fwd, float rangeM, VECTOR3* out, float* distM)
{
    extern s32 g_PcBulletHitActive;
    static s_RayTrace trace;
    const float fx = fwd->vx / 4096.0f, fy = fwd->vy / 4096.0f, fz = fwd->vz / 4096.0f;
    const float hxz = sqrtf(fx * fx + fz * fz);
    const float floorY = (float)g_SysWork.playerWork.player.position.vy / 4096.0f;
    VECTOR3     off;
    float       t = rangeM;
    int         hit;

    off.vx = (s32)lrintf(fx * rangeM * 4096.0f);
    off.vy = (s32)lrintf(fy * rangeM * 4096.0f);
    off.vz = (s32)lrintf(fz * rangeM * 4096.0f);
    g_PcBulletHitActive = 1;   /* the bullet's own hit cylinders */
    hit = Ray_CharaTraceQuery(&trace, from, &off, &g_SysWork.playerWork.player);
    g_PcBulletHitActive = 0;

    if (hit && hxz >= 0.05f)
    {
        const float dx = (float)(trace.target.vx - from->vx) / 4096.0f;
        const float dz = (float)(trace.target.vz - from->vz) / 4096.0f;
        t = sqrtf(dx * dx + dz * dz) / hxz;
    }
    else
    {
        hit = 0;
    }
    if (fy > 0.0f)
    {
        const float tFloor = (floorY - (float)from->vy / 4096.0f) / fy;   /* +Y is down */
        if (tFloor > 0.0f && tFloor < t)
        {
            t   = tFloor;
            hit = 1;
        }
    }
    out->vx = from->vx + (s32)lrintf(fx * t * 4096.0f);
    out->vy = from->vy + (s32)lrintf(fy * t * 4096.0f);
    out->vz = from->vz + (s32)lrintf(fz * t * 4096.0f);
    if (distM) *distM = t;
    return hit;
}

/* world_draw.c WorldGfx_HeldItemDraw, every gameplay frame */
void VrCrosshair_Draw(GsOT* ot)
{
    VECTOR3   from, fwd, hitAt, at;
    MATRIX    ws, m;
    POLY_F4*  poly;
    float     dist, r;
    s32       otz = 0, bucket;
    long      p, flag;
    int       k;

    if (!IsGun((u8)g_SavegamePtr->equippedWeapon) || !VrInteract_WeaponLive() ||
        !VrIk_GetGunRay(&from, &fwd))
        return;
    if (!VrGun_AimPoint(&from, &fwd, VR_XH_RANGE_M, &hitAt, &dist))
        return;
    at.vx = hitAt.vx - (s32)lrintf((float)(hitAt.vx - from.vx) * VR_XH_PULL);
    at.vy = hitAt.vy - (s32)lrintf((float)(hitAt.vy - from.vy) * VR_XH_PULL);
    at.vz = hitAt.vz - (s32)lrintf((float)(hitAt.vz - from.vz) * VR_XH_PULL);

    /* view-aligned: identity rotation, vertices in view space at 1/16 of a
     * world Q8 unit (Q12 256 on the diagonal), so a few-mm dot keeps its shape */
    Vw_WorldScreenMatrixAtPositionGet(&ws, at.vx, at.vy, at.vz);
    for (k = 0; k < 9; k++) m.m[k / 3][k % 3] = (k % 4 == 0) ? 256 : 0;
    m.t[0] = ws.t[0]; m.t[1] = ws.t[1]; m.t[2] = ws.t[2];
    SetRotMatrix(&m);
    SetTransMatrix(&m);

    r = dist * VR_XH_ANGLE;
    if (r < VR_XH_MIN_M) r = VR_XH_MIN_M;
    r *= 256.0f * 16.0f;   /* meters -> the units above */

    /* projected straight into the prim: PGXP keys a vertex's precise depth on
     * the address the GTE stored it to (see pc_minimap.c vrmap_project) */
    poly = (POLY_F4*)GsOUT_PACKET_P;
    setPolyF4(poly);
    {
        s16* xy[4] = { &poly->x0, &poly->x1, &poly->x2, &poly->x3 };
        for (k = 0; k < 4; k++)
        {
            /* a diamond: top, right, left, bottom (PSX quad order) */
            static const s8 dx[4] = { 0, 1, -1, 0 }, dy[4] = { -1, 0, 0, 1 };
            SVECTOR v;
            v.vx = (s16)lrintf(dx[k] * r);
            v.vy = (s16)lrintf(dy[k] * r);
            v.vz = 0;
            v.pad = 0;
            otz = RotTransPers(&v, (int*)xy[k], &p, &flag);
            if (otz <= 8)
                return;
        }
    }

    bucket = (otz >> 1) - 2;
    if (bucket < 1) bucket = 1;
    if (bucket >= ORDERING_TABLE_SIZE) bucket = ORDERING_TABLE_SIZE - 1;

    setRGB0(poly, 255, 32, 24);
    addPrim(&ot->org[bucket], poly);
    poly++;
    GsOUT_PACKET_P = (PACKET*)poly;
}
