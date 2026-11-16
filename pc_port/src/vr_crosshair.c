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

 Gun Sight (vr_gun_sight): 0 none, 1 the dot, 2 (default) a laser beam from
 the muzzle to the dot as well - or to VR_XH_RANGE when nothing is hit. The
 beam is the very ray the bullet takes (player_control.c fires from the same
 muzzle at VrGun_AimPoint, bodyprog_combat drops the random scatter while
 VrCrosshair_GunActive), so whatever the laser touches is what gets shot.

 World prims like pc_decals.c: RotTransPers, POLY_F4 from the GsOUT_PACKET_P
 arena, an OT0 bucket just in front of the surface. The beam is a ribbon
 facing the eye, cut in pieces (each its own anchor, so its vertices stay in
 s16 at 1/4096 m - fine enough for a 2 mm beam), each piece sorted at its own
 depth.
=============================================================================
*/

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/collision/ray.h"
#include "bodyprog/view/vw_calc.h"

#include <math.h>

#include "pc_config.h"

extern int VrInteract_WeaponLive(void);
extern int VrIk_GetGunRay(VECTOR3* pos, VECTOR3* fwd);

#define VR_XH_RANGE_M 60.0f     /* the shot's own aim range (player_control.c) */
#define VR_XH_ANGLE   0.0070f   /* dot radius per meter of distance (~0.4 deg) */
#define VR_XH_MIN_M   0.006f
#define VR_XH_PULL    0.03f     /* drawn this fraction of the way back toward the gun */

#define VR_LZ_START_M 0.01f     /* the beam starts this far past the muzzle */
#define VR_LZ_HALF    0.0006f   /* beam half width per meter from the eye (~0.07 deg) */
#define VR_LZ_HALF_MIN 0.0008f  /* never thinner than 1.6 mm */
#define VR_LZ_PIECE_MAX 6.0f    /* m; a piece's vertices must fit s16 at 1/4096 m */

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

/* A gun is in the VR hand, drawn and pointing (the laser's own condition):
 * the shot then follows the laser exactly (bodyprog_combat: no scatter). */
int VrCrosshair_GunActive(void)
{
    VECTOR3 from, fwd;
    return g_SavegamePtr != NULL && IsGun((u8)g_SavegamePtr->equippedWeapon) &&
           VrInteract_WeaponLive() && VrIk_GetGunRay(&from, &fwd);
}

/* One beam piece from a to b (meters along the ray from 'from'). */
static void BeamPiece(GsOT* ot, const VECTOR3* from, const float dir[3], float a, float b)
{
    MATRIX   ws, m;
    VECTOR3  anc;
    POLY_F4* poly;
    float    vd[3], va[3], vb[3], side[3], len;
    s32      otz, otzMin = 0x7FFFFFFF, bucket;
    long     p, flag;
    int      i, k;

    /* anchor = the piece's start, on the Q12 grid */
    anc.vx = from->vx + (s32)lrintf(dir[0] * a * 4096.0f);
    anc.vy = from->vy + (s32)lrintf(dir[1] * a * 4096.0f);
    anc.vz = from->vz + (s32)lrintf(dir[2] * a * 4096.0f);
    Vw_WorldScreenMatrixAtPositionGet(&ws, anc.vx, anc.vy, anc.vz);

    /* view-space: the anchor (Q8 -> m), the beam direction, both ends */
    for (i = 0; i < 3; i++)
        vd[i] = (ws.m[i][0] * dir[0] + ws.m[i][1] * dir[1] + ws.m[i][2] * dir[2]) / 4096.0f;
    for (i = 0; i < 3; i++)
    {
        const float t0 = (float)ws.t[i] / 256.0f;
        /* the anchor's own rounding: 'a' is exactly on it, so va = anchor */
        va[i] = t0;
        vb[i] = t0 + vd[i] * (b - a);
    }
    if (va[2] <= 0.05f && vb[2] <= 0.05f)
        return;

    /* ribbon facing the eye: across the beam and the line of sight */
    {
        const float mx = (va[0] + vb[0]) * 0.5f, my = (va[1] + vb[1]) * 0.5f, mz = (va[2] + vb[2]) * 0.5f;
        side[0] = vd[1] * mz - vd[2] * my;
        side[1] = vd[2] * mx - vd[0] * mz;
        side[2] = vd[0] * my - vd[1] * mx;
        len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        if (len < 1e-6f)
            return;   /* looking straight down the beam */
        for (i = 0; i < 3; i++) side[i] /= len;
    }

    for (k = 0; k < 9; k++) m.m[k / 3][k % 3] = (k % 4 == 0) ? 256 : 0;
    m.t[0] = ws.t[0]; m.t[1] = ws.t[1]; m.t[2] = ws.t[2];
    SetRotMatrix(&m);
    SetTransMatrix(&m);

    poly = (POLY_F4*)GsOUT_PACKET_P;
    setPolyF4(poly);
    {
        s16* xy[4] = { &poly->x0, &poly->x1, &poly->x2, &poly->x3 };
        for (k = 0; k < 4; k++)
        {
            /* PSX quad order: a+, a-, b+, b- */
            const float* e   = (k < 2) ? va : vb;
            const float  sg  = (k & 1) ? -1.0f : 1.0f;
            const float  dE  = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            float        hw  = dE * VR_LZ_HALF;
            float        off[3];
            SVECTOR      v;
            if (hw < VR_LZ_HALF_MIN) hw = VR_LZ_HALF_MIN;
            for (i = 0; i < 3; i++)
                off[i] = ((k < 2) ? 0.0f : vd[i] * (b - a)) + side[i] * hw * sg;
            /* vertex units: 1/16 Q8 = 1/4096 m (the 256 diagonal above) */
            v.vx  = (s16)lrintf(off[0] * 4096.0f);
            v.vy  = (s16)lrintf(off[1] * 4096.0f);
            v.vz  = (s16)lrintf(off[2] * 4096.0f);
            v.pad = 0;
            otz = RotTransPers(&v, (int*)xy[k], &p, &flag);
            if (otz <= 8)
                return;   /* behind the eye / too near: skip this piece */
            if (otz < otzMin) otzMin = otz;
        }
    }

    bucket = (otzMin >> 1) - 2;
    if (bucket < 1) bucket = 1;
    if (bucket >= ORDERING_TABLE_SIZE) bucket = ORDERING_TABLE_SIZE - 1;

    setRGB0(poly, 255, 24, 16);
    addPrim(&ot->org[bucket], poly);
    poly++;
    GsOUT_PACKET_P = (PACKET*)poly;
}

/* The laser from the muzzle to 'end' meters along the ray: short pieces near
 * the gun, longer ones far away (each piece still sorts at its own depth). */
static void BeamDraw(GsOT* ot, const VECTOR3* from, const VECTOR3* fwd, float end)
{
    float dir[3], a = VR_LZ_START_M;
    int   guard = 0;

    dir[0] = fwd->vx / 4096.0f;
    dir[1] = fwd->vy / 4096.0f;
    dir[2] = fwd->vz / 4096.0f;
    {
        const float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (l < 1e-4f) return;
        dir[0] /= l; dir[1] /= l; dir[2] /= l;
    }
    while (a < end && guard++ < 48)
    {
        float piece = a * 0.4f;
        float b;
        if (piece < 0.15f) piece = 0.15f;
        if (piece > VR_LZ_PIECE_MAX) piece = VR_LZ_PIECE_MAX;
        b = a + piece;
        if (b > end) b = end;
        BeamPiece(ot, from, dir, a, b);
        a = b;
    }
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

    if (g_PcConfig.vrGunSight <= 0)
        return;
    if (!IsGun((u8)g_SavegamePtr->equippedWeapon) || !VrInteract_WeaponLive() ||
        !VrIk_GetGunRay(&from, &fwd))
        return;
    k = VrGun_AimPoint(&from, &fwd, VR_XH_RANGE_M, &hitAt, &dist);
    if (g_PcConfig.vrGunSight >= 2)
        BeamDraw(ot, &from, &fwd, k ? dist * (1.0f - VR_XH_PULL) : VR_XH_RANGE_M);
    if (!k)
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
