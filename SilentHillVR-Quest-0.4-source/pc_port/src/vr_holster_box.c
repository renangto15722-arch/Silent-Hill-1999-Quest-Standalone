/*
=============================================================================
 SH1 PC Port - VR GRAB BOX: the visible box of a holster / weapon wheel slot
=============================================================================
 Each holster slot and each weapon wheel slot shows a see-through box: the
 hand inside it takes what it holds (vr_interact.c), and the weapon in it is
 sized to fit (world_draw.c). Box_Size (vr_hbox_size) is its edge, Box_Opacity
 (vr_hbox_opacity) how solid it looks.

 World prims like the laser (vr_crosshair.c): RotTransPers into POLY_F4 from
 the GsOUT_PACKET_P arena, view-space vertices at 1/4096 m around an anchor
 (the box centre). Six faces, each in its own OT0 bucket at its own depth.
 Semi-transparent with the per-prim alpha (PsyX_SetNextPrimAlpha, as the
 bullet decals fade), blend 50/50 set by a DR_TPAGE drawn just before each
 face: an untextured prim takes the blend of whatever page was last set.
=============================================================================
*/
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/view/vw_calc.h"
#include <math.h>
#include "pc_config.h"

extern void PsyX_SetNextPrimAlpha(int a);

/* corner index bits: 1 = +right, 2 = +down, 4 = +forward */
static const unsigned char s_faces[6][4] = {
    { 0, 1, 2, 3 }, { 4, 5, 6, 7 },   /* back, front   (PSX quad order: 0 1 2 3 = TL TR BL BR) */
    { 0, 1, 4, 5 }, { 2, 3, 6, 7 },   /* top, bottom */
    { 0, 2, 4, 6 }, { 1, 3, 5, 7 },   /* left, right */
};

/* centre: world Q12. axes: world unit vectors as COLUMNS (right, down, forward).
 * halfM: half the edge in meters. hot = the hand is in it (brighter). */
void VrBox_Draw(GsOT* ot, const VECTOR3* centre, const float axes[3][3], float halfM, int hot)
{
    MATRIX   ws, m;
    float    corner[8][3];
    int      alpha, i, k, f;
    u8       r, g, b;

    alpha = (int)lrintf(g_PcConfig.vrBoxOpacity * 2.55f);
    if (hot) alpha = alpha * 2 + 40;
    if (alpha > 255) alpha = 255;
    if (alpha <= 0 || halfM <= 0.0f)
        return;
    r = hot ? 255 : 150;
    g = hot ? 220 : 170;
    b = hot ? 120 : 190;

    Vw_WorldScreenMatrixAtPositionGet(&ws, centre->vx, centre->vy, centre->vz);

    /* the corners in view space, meters from the anchor */
    for (k = 0; k < 8; k++)
    {
        float off[3];
        for (i = 0; i < 3; i++)
        {
            off[i] = axes[i][0] * ((k & 1) ? halfM : -halfM) +
                     axes[i][1] * ((k & 2) ? halfM : -halfM) +
                     axes[i][2] * ((k & 4) ? halfM : -halfM);
        }
        for (i = 0; i < 3; i++)
            corner[k][i] = (ws.m[i][0] * off[0] + ws.m[i][1] * off[1] + ws.m[i][2] * off[2]) / 4096.0f;
    }

    for (k = 0; k < 9; k++) m.m[k / 3][k % 3] = (k % 4 == 0) ? 256 : 0;
    m.t[0] = ws.t[0]; m.t[1] = ws.t[1]; m.t[2] = ws.t[2];
    SetRotMatrix(&m);
    SetTransMatrix(&m);

    for (f = 0; f < 6; f++)
    {
        POLY_F4*  poly = (POLY_F4*)GsOUT_PACKET_P;
        DR_TPAGE* tp;
        s16*      xy[4];
        s32       otzSum = 0, bucket;
        int       ok = 1;

        setPolyF4(poly);
        xy[0] = &poly->x0; xy[1] = &poly->x1; xy[2] = &poly->x2; xy[3] = &poly->x3;
        for (k = 0; k < 4; k++)
        {
            const float* c = corner[s_faces[f][k]];
            SVECTOR      v;
            long         p, flag;
            s32          otz;
            v.vx  = (s16)lrintf(c[0] * 4096.0f);
            v.vy  = (s16)lrintf(c[1] * 4096.0f);
            v.vz  = (s16)lrintf(c[2] * 4096.0f);
            v.pad = 0;
            otz = RotTransPers(&v, (int*)xy[k], &p, &flag);
            if (otz <= 8) { ok = 0; break; }   /* behind the eye / too near */
            otzSum += otz;
        }
        if (!ok)
            continue;

        bucket = ((otzSum / 4) >> 1) - 2;
        if (bucket < 1) bucket = 1;
        if (bucket >= ORDERING_TABLE_SIZE) bucket = ORDERING_TABLE_SIZE - 1;

        setRGB0(poly, r, g, b);
        setSemiTrans(poly, 1);
        PsyX_SetNextPrimAlpha(alpha);
        addPrim(&ot->org[bucket], poly);
        poly++;

        /* linked after the face = drawn before it: the 50/50 blend page */
        tp = (DR_TPAGE*)poly;
        setDrawTPage(tp, 1, 0, getTPage(0, 0, 0, 0));
        addPrim(&ot->org[bucket], tp);
        GsOUT_PACKET_P = (PACKET*)(tp + 1);
    }
}
