/*
=============================================================================
 SH1 PC Port - VR BODY FRAME (hands, holsters, held weapon, gun ray)
=============================================================================

 With a headset in first person the headset is the head and your hands are
 the GTA SA VR hands (vr_hands.c, drawn over each eye by vr_full.c). Harry
 is NOT drawn in first-person VR gameplay (bodyprog_bone_80044F14.c); his
 skeleton still animates, so cutscenes and every other camera show him as
 always. The IK arms (build24-150) are gone (Renan 03/11/2026).

 What stays here is the frame everything VR is placed in: the view, Harry's
 root and body yaw, his holsters, the equipped weapon in the hand, the paper
 map and the gun ray.

 Where: the end of Player_Update (player_control.c), after every animation
 of the frame.

 ONE FRAME, ONE SOURCE: everything VR hangs off the view this frame is
 rendered from (VrTracking_GetCameraMatrix): the game camera plus the
 OpenXR head pose, built by VrApplyHeadOffset in DebugCamera_Update -
 BEFORE Player_Update, from where Harry stood before this frame's move.
 The model's root is put back where Harry was when the view was built
 (VrTracking_GetCameraPlayer, SyncRootToView), so the holsters ride the
 same frame as the view.

 Spaces: the OpenXR standing space gives the head and the controllers;
 a controller relative to the head, in the head's yaw/pitch frame, is the
 player-local pose; the view (game camera + head) puts it in the world.

 SCALE IN THE SKELETON: Harry's bone world matrices carry a uniform scale
 (~0.96, times PLAYER HEIGHT); Ortho splits it off.

 VIRTUAL HAND (s_vh*): where the IK used to put Harry's hand bone - the
 wrist (grip + 7 cm along the forearm, GripToWrist), turned like the
 controller times K (his relaxed standing hand in the body frame) or, with
 a gun, like the aim ray times the gun's grip - at the skeleton's scale
 times 1/World_Scale. The held weapon is placed on it, so the VR Weapon
 Grip values tuned on the old IK hand keep their meaning.

 TORSO: left to the animation. Holding the torso and head at the standing
 pose (build48-130, PlaceTorso) while the hips and legs walked stretched
 the waist, where the chest mesh shares vertices with the hips: the lower
 chest deformed over the legs. The IK only moves the arms; the shoulders
 follow the animated torso and the arms reach the controllers from there.
 The walk lean still takes the chest forward under the camera, so the whole
 skeleton moves back with it, rigidly, on the root (KeepNeckUnderView).
 Head Height is where your eyes sit on Harry: too low, and the top of his
 chest (the V of the collar) is in the view.

 PLAYER HEIGHT (vr_player_height): people differ, so Harry's whole body is
 scaled from his feet by the root's local (SyncRootToView) to the player's
 height as they see it - height / World_Scale over Harry's own height
 (s_harryHeight, from his standing neck at the first idle). The whole body
 at once, so no seam stretches; the arm IK and Rebuild Body measure in the
 world and follow. Bone world matrices then carry that scale, so they are
 tested with IsScaledRotation (Ortho first). Head Height still places the
 camera, independently.

 HOLSTERS (vr_holster*; vr_interact.c owns the drawn/holstered state): six
 slots on Harry's body - left/right hip, left/right chest, behind the
 left/right shoulder (s_holsterSlots) - each holding the weapon the player
 picked for it. In the body's yaw frame from the root - they follow where he
 faces, not the walk animation - and scaled with PLAYER HEIGHT. A slot's
 orientation is his relaxed standing hand (s_handK, the hand's frame in the
 body frame) pitched about the body's right axis (down at the hips and
 chest, up behind the shoulders). Every slot draws its own copy of its
 weapon (world_draw.c Pc_VrHolstersDraw): the original mesh while the weapon
 is in it, all black while it is in the hand.

 WEAPON GRIP (VR Weapon Grip page, vr_item_*): the equipped weapon's model
 offset and turn in the hand, in the CONTROLLER's frame - forward is where
 it points - kept per weapon (vr_grip_<id>, GripEditSync).
 VrIk_HeldItemMatrix: the controller rotation C (the aim pose with a gun,
 else the grip pose), the hand bone's world matrix H: model = C R C^T H,
 moved by C o (o: forward, up, right, cm at your real size; C's columns
 are right, down, forward - CtrlRotToWorld).

 Right hand with a weapon: the hand bone turns with the controller's
 pointing ray (aim pose), so the held weapon points where the controller
 does. The weapon's grip in the hand bone's frame is read once per weapon
 from the game's own aim pose (after ~0.25 s of aiming) and kept for the
 session. Shots use the same ray (VrIk_GetGunRay, player_control.c).

 Matrices follow Vw_MultiplyAndTransformMatrix: world = parent * local,
 rot = Rp*Rl (Q12), t = Rp*tl + tp (world Q8 units, 256 per meter).
=============================================================================
*/

#include "game.h"
#include "pc_config.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/math/math.h"

/* bodyprog/view/vw_calc.c (its header drags in view-system types) */
extern void Vw_CoordHierarchyMatrixCompute(GsCOORDINATE2* rootCoord, MATRIX* transformMat);

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

extern int  VrTracking_HeadCamActive(void);
extern int  VrTracking_CutsceneActive(void);
extern int  VrTracking_GetCameraMatrix(MATRIX* out);
extern int  VrOvr_GetHeadPose(float pos[3], float rot[3]);
extern int  VrOvr_GetHandPose(int hand, float pos[3], float rot[3][3]);
extern int  VrOvr_GetAimPose(float pos[3], float rot[3][3]);
extern int  VrOvr_RuntimeIsOpenXR(void);
extern int  VrTracking_GetCameraPlayer(VECTOR* pos, int* yaw);
extern void VrTracking_GetHeadOffset(s32 out[3]);
extern void VrOvr_Log(const char* fmt, ...);
extern int  VrInteract_WeaponHand(void);
extern int  g_PcFpsCam;

#define VR_IK_UNITS_PER_M  256.0f   /* world Q8 units per meter */
#define VR_IK_GRIP_TO_WRIST 0.07f   /* m: grip pose (palm) -> wrist, see GripToWrist */

typedef struct { float r[3][3]; float t[3]; } Xf;

static void XfFromMatrix(const MATRIX* m, Xf* x)
{
    int i, j;
    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++) x->r[i][j] = (float)m->m[i][j] / 4096.0f;
        x->t[i] = (float)m->t[i];
    }
}

/* out = a * b (world = parent * local) */
static void XfMul(const Xf* a, const Xf* b, Xf* out)
{
    Xf o;
    int i, j;
    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
            o.r[i][j] = a->r[i][0] * b->r[0][j] + a->r[i][1] * b->r[1][j] + a->r[i][2] * b->r[2][j];
        o.t[i] = a->r[i][0] * b->t[0] + a->r[i][1] * b->t[1] + a->r[i][2] * b->t[2] + a->t[i];
    }
    *out = o;
}

static void RotVec(const float r[3][3], const float v[3], float out[3])
{
    int i;
    for (i = 0; i < 3; i++) out[i] = r[i][0] * v[0] + r[i][1] * v[1] + r[i][2] * v[2];
}

static void RotMul(const float a[3][3], const float b[3][3], float out[3][3])
{
    float o[3][3];
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(out, o, sizeof(o));
}

/* out = a^T * b (a orthonormal: its inverse is its transpose) */
static void RotMulTransA(const float a[3][3], const float b[3][3], float out[3][3])
{
    float o[3][3];
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            o[i][j] = a[0][i] * b[0][j] + a[1][i] * b[1][j] + a[2][i] * b[2][j];
    memcpy(out, o, sizeof(o));
}

static float Len(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static int Normalize(float v[3])
{
    float l = Len(v);
    if (l < 1e-6f) return 0;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return 1;
}

/* A real rotation (unit-ish columns, right-handed)? A bone matrix read on a
 * frame the skeleton is not posed - e.g. the first one after the quick menu
 * closes - comes back all zero, and a hand drawn with it collapses to
 * nothing (the "hand vanished after Rebuild Body" bug). */
static int IsRotation(const float r[3][3])
{
    int   j;
    float det;
    for (j = 0; j < 3; j++)
    {
        const float n = r[0][j] * r[0][j] + r[1][j] * r[1][j] + r[2][j] * r[2][j];
        if (n < 0.64f || n > 1.44f) return 0;
    }
    det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
          r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
          r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    return det > 0.5f;
}

/* A bone world matrix: a rotation times a uniform scale (Harry's ~0.96
 * times PLAYER HEIGHT). IsRotation alone wants ~1. */
static float Ortho(const float r[3][3], float out[3][3]);
static int IsScaledRotation(const float r[3][3])
{
    float q[3][3];
    const float sc = Ortho(r, q);
    return sc > 0.2f && sc < 5.0f && IsRotation(q);
}

/* The head's yaw/pitch frame (the camera never rolls - Model-B) and the
 * view the camera really used: everything a controller needs to be placed
 * in the game world. */
typedef struct
{
    float hp[3];
    float cy, sy, cp, sp;
    Xf    cam;
} HeadFrame;

static int HeadFrameGet(HeadFrame* f)
{
    float  hr[3];
    MATRIX cam;

    if (!VrOvr_GetHeadPose(f->hp, hr) || !VrTracking_GetCameraMatrix(&cam))
        return 0;
    f->cy = cosf(hr[1]); f->sy = sinf(hr[1]);
    f->cp = cosf(hr[0]); f->sp = sinf(hr[0]);
    XfFromMatrix(&cam, &f->cam);

    return 1;
}

/* Tracking-space vector -> the head's frame:
 * h = Rx(pitch)^T * Ry(yaw)^T * d, OpenVR axes */
static void ToHead(const HeadFrame* f, const float d[3], float h[3])
{
    float x, y, z;
    x = f->cy * d[0] - f->sy * d[2];
    z = f->sy * d[0] + f->cy * d[2];
    y = f->cp * d[1] + f->sp * z;
    z = -f->sp * d[1] + f->cp * z;
    h[0] = x; h[1] = y; h[2] = z;
}

/* Tracking-space direction -> game world direction (length kept).
 * OpenVR (x right, y up, z back) -> PSX camera (x right, y down, z fwd). */
static void DirToWorld(const HeadFrame* f, const float d[3], float out[3])
{
    float h[3], c[3];
    ToHead(f, d, h);
    c[0] =  h[0];
    c[1] = -h[1];
    c[2] = -h[2];
    RotVec(f->cam.r, c, out);
}

/* World units per real meter: World_Scale, like the eyes and head. The
 * hands are where the controllers are; the old Body_Size scaled this too,
 * which put them at 93% (etc.) of the real distance from your eyes. */
static float ReachScale(void)
{
    return VR_IK_UNITS_PER_M / (g_PcConfig.vrScale > 0.05f ? g_PcConfig.vrScale : 1.0f);
}

/* Uniform scale of a bone world matrix, and its pure rotation. */
static float Ortho(const float r[3][3], float out[3][3])
{
    float s = 0.0f;
    int   i, j;
    for (j = 0; j < 3; j++)
        s += sqrtf(r[0][j] * r[0][j] + r[1][j] * r[1][j] + r[2][j] * r[2][j]);
    s /= 3.0f;
    if (s < 1e-4f) s = 1.0f;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            out[i][j] = r[i][j] / s;
    return s;
}

/* Tracking-space point -> game world point, world Q8 */
static void PointToWorld(const HeadFrame* f, const float p[3], float out[3])
{
    const float d[3] = { p[0] - f->hp[0], p[1] - f->hp[1], p[2] - f->hp[2] };
    float scale = ReachScale(), w[3];
    int   i;

    DirToWorld(f, d, w);
    for (i = 0; i < 3; i++) out[i] = w[i] * scale + f->cam.t[i];
}

/* Grip pose (the palm) -> the wrist, in the CONTROLLER's frame, so the
 * point turns with the controller. Which axis runs up the forearm depends on
 * the runtime's pose convention:
 *   OpenXR grip pose: -Z runs through the fist from little finger to thumb,
 *   +X is the palm normal, so the forearm lies along +Y.
 *   OpenVR (legacy Touch pose): -Z points forward like the aim ray, so the
 *   forearm lies along +Z.
 * The old code took +Z on OpenXR too: that is across the hand toward the
 * little finger, so turning the controller swung the wrist target around
 * it and the hand slid sideways/forward. */
static void GripToWrist(const float gp[3], const float gr[3][3], float out[3])
{
    const int axis = VrOvr_RuntimeIsOpenXR() ? 1 : 2;
    int       i;
    for (i = 0; i < 3; i++) out[i] = gp[i] + gr[i][axis] * VR_IK_GRIP_TO_WRIST;
}

/* Wrist target in world Q8 for hand 0 (left) / 1 (right). */
static int HandTarget(int hand, float out[3])
{
    HeadFrame f;
    float     gp[3], gr[3][3], w[3];

    if (!HeadFrameGet(&f) || !VrOvr_GetHandPose(hand, gp, gr))
        return 0;
    GripToWrist(gp, gr, w);
    PointToWorld(&f, w, out);
    return 1;
}

/* Controller grip (the palm), world Q12 - item grabbing (vr_interact.c). */
int VrIk_GetHandWorld(int hand, VECTOR3* out)
{
    HeadFrame f;
    float     gp[3], gr[3][3], w[3];

    if (!HeadFrameGet(&f) || !VrOvr_GetHandPose(hand, gp, gr))
        return 0;
    PointToWorld(&f, gp, w);
    out->vx = (s32)lrintf(w[0] * 16.0f);
    out->vy = (s32)lrintf(w[1] * 16.0f);
    out->vz = (s32)lrintf(w[2] * 16.0f);
    return 1;
}

/* Controller grip relative to the head, in the head's yaw/pitch frame
 * (meters, OpenVR axes: x right, y up, z back) - the radial wheel picks
 * by where the hand moves (vr_interact.c). */
int VrIk_GetHandInHead(int hand, float out[3])
{
    HeadFrame f;
    float     gp[3], gr[3][3], d[3];

    if (!HeadFrameGet(&f) || !VrOvr_GetHandPose(hand, gp, gr))
        return 0;
    d[0] = gp[0] - f.hp[0];
    d[1] = gp[1] - f.hp[1];
    d[2] = gp[2] - f.hp[2];
    ToHead(&f, d, out);
    return 1;
}

/* A tracking-space point (meters, OpenVR axes) in the game world, Q12 - and
 * a tracking-space direction as a world direction, length kept (vr_melee.c). */
int VrIk_TrackingToWorld(const float p[3], VECTOR3* out)
{
    HeadFrame f;
    float     w[3];

    if (!HeadFrameGet(&f))
        return 0;
    PointToWorld(&f, p, w);
    out->vx = (s32)lrintf(w[0] * 16.0f);
    out->vy = (s32)lrintf(w[1] * 16.0f);
    out->vz = (s32)lrintf(w[2] * 16.0f);
    return 1;
}

int VrIk_TrackingDirToWorld(const float d[3], float out[3])
{
    HeadFrame f;

    if (!HeadFrameGet(&f))
        return 0;
    DirToWorld(&f, d, out);
    return 1;
}

/* Right controller pointing ray in the world: origin (world Q12) and unit
 * direction (Q12). The bullet ray of player_control.c in VR. */
int VrIk_GetGunRay(VECTOR3* pos, VECTOR3* fwd)
{
    HeadFrame f;
    float     ap[3], ar[3][3], w[3], d[3], dw[3];

    if (!VrTracking_HeadCamActive() || VrTracking_CutsceneActive() ||
        !g_PcFpsCam || !HeadFrameGet(&f) || !VrOvr_GetAimPose(ap, ar))
        return 0;

    /* the VR Weapon Grip page's Gun Aim rows (vr_aim_*), in the aim pose's own
     * frame (x right, y up, z back): the muzzle point, then the ray turned by
     * pitch (up +) and yaw (right +) off the pointing axis (-Z) */
    {
        const float* o   = g_PcConfig.vrAimOfs;
        const float  cm  = 0.01f;
        const float  p   = o[3] * (3.14159265f / 180.0f), y = o[4] * (3.14159265f / 180.0f);
        const float  kz  = cosf(p) * cosf(y), kx = cosf(p) * sinf(y), ky = sinf(p);
        int          i;
        float        m[3];
        for (i = 0; i < 3; i++)
        {
            m[i] = ap[i] + ar[i][0] * o[2] * cm + ar[i][1] * o[1] * cm - ar[i][2] * o[0] * cm;
            d[i] = -ar[i][2] * kz + ar[i][0] * kx + ar[i][1] * ky;
        }
        PointToWorld(&f, m, w);
    }
    DirToWorld(&f, d, dw);
    if (!Normalize(dw)) return 0;

    pos->vx = (s32)lrintf(w[0] * 16.0f);
    pos->vy = (s32)lrintf(w[1] * 16.0f);
    pos->vz = (s32)lrintf(w[2] * 16.0f);
    fwd->vx = (s32)lrintf(dw[0] * 4096.0f);
    fwd->vy = (s32)lrintf(dw[1] * 4096.0f);
    fwd->vz = (s32)lrintf(dw[2] * 4096.0f);
    return 1;
}

/* World rotation of the right controller's pointing frame, its axes in PSX
 * convention (x right, y down, z = pointing direction). */
static void CtrlRotToWorld(const HeadFrame* f, const float r[3][3], float out[3][3])
{
    float col[3], w[3];
    int   j, i;

    for (j = 0; j < 3; j++)
    {
        const float sgn = (j == 0) ? 1.0f : -1.0f;   /* controller y up -> down, z back -> fwd */
        for (i = 0; i < 3; i++) col[i] = r[i][j] * sgn;
        DirToWorld(f, col, w);
        for (i = 0; i < 3; i++) out[i][j] = w[i];
    }
}

static int AimWorldRotation(float out[3][3])
{
    HeadFrame f;
    float     ap[3], ar[3][3];

    if (!HeadFrameGet(&f) || !VrOvr_GetAimPose(ap, ar))
        return 0;
    CtrlRotToWorld(&f, ar, out);
    return 1;
}

/* Same, for a controller's grip pose (0 = left, 1 = right). */
static int GripWorldRotation(int hand, float out[3][3])
{
    HeadFrame f;
    float     gp[3], gr[3][3];

    if (!HeadFrameGet(&f) || !VrOvr_GetHandPose(hand, gp, gr))
        return 0;
    CtrlRotToWorld(&f, gr, out);
    return 1;
}

/* Hand bone frame in the controller frame (both hands). A hand holding a
 * controller upright - the controller's axes lined up with the body (right,
 * up, back) - hangs like a relaxed hand: arm down, palm toward the thigh.
 * That is Harry's standing idle, so K = Body^T * Hand there, read once per
 * session from the animation before anything here moves it. It is a fact
 * of Harry's skeleton, not of you, so Rebuild Body does not read it again.
 * The same holds for the OpenXR grip pose and OpenVR's Touch pose. */
static float s_handK[2][3][3];
static int   s_handKnown[2];

/* Harry's yaw in the view's frame (see ONE FRAME, ONE SOURCE): set by
 * SyncRootToView each frame, his live yaw until then. */
static int   s_viewYawValid = 0;
static int   s_viewYawQ12;

static float BodyYaw(void)
{
    const int q = s_viewYawValid ? s_viewYawQ12 : g_SysWork.playerWork.player.rotation.vy;
    return (float)q * (6.28318531f / 4096.0f);
}

/* The model's root to where Harry stood and faced when this frame's view
 * was built. Per frame and not cumulative: Player_LogicUpdate and
 * func_8007C0D8 rewrite the root from his live position every frame; only
 * the drawn body (and the VR pose built on it) moves back a frame, to meet
 * the view. Collision and logic keep using his real position. */
/* PLAYER HEIGHT (see the header) */
#define VR_NECK_TO_EYE_M 0.10f    /* eye above the neck (head bone) */
#define VR_EYE_TO_HEIGHT 0.936f   /* eye height / standing height */
static float s_harryHeight;       /* Harry's standing height, world Q8 at scale 1; 0 = not read */
static float s_bodyScaleApplied = 1.0f;

static float BodyScale(void)
{
    float s;
    if (g_PcConfig.vrPlayerHeight < 1.0f || s_harryHeight <= 0.0f)
        return 1.0f;
    /* a real meter is VR_IK_UNITS_PER_M / World_Scale world units (ReachScale) */
    s = g_PcConfig.vrPlayerHeight / 100.0f * ReachScale() / s_harryHeight;
    return (s < 0.5f) ? 0.5f : (s > 2.0f) ? 2.0f : s;
}

/* Harry's root in world Q8 without the Q12 -> Q8 truncation (SyncRootToView,
 * then KeepNeckUnderView's shift): with the exact camera (vw_calc.c), the
 * body must be exact too or it would shimmer against the view. */
static double s_rootExactT[3];

static void SyncRootToView(GsCOORDINATE2* coords)
{
    VECTOR  pos;
    int     yaw;
    SVECTOR rot;

    s_viewYawValid = 0;
    if (!VrTracking_GetCameraPlayer(&pos, &yaw))
        return;
    rot.vx = g_SysWork.playerWork.player.rotation.vx;
    rot.vy = (s16)yaw;
    rot.vz = g_SysWork.playerWork.player.rotation.vz;
    rot.pad = 0;
    Math_RotMatrixZxyNegGte(&rot, &coords[HarryBone_Root].coord);
    {
        /* PLAYER HEIGHT: the whole skeleton from the feet */
        int i, j;
        s_bodyScaleApplied = BodyScale();
        if (s_bodyScaleApplied != 1.0f)
            for (i = 0; i < 3; i++)
                for (j = 0; j < 3; j++)
                    coords[HarryBone_Root].coord.m[i][j] =
                        (s16)lrintf((float)coords[HarryBone_Root].coord.m[i][j] * s_bodyScaleApplied);
    }
    coords[HarryBone_Root].coord.t[0] = Q12_TO_Q8(pos.vx);
    coords[HarryBone_Root].coord.t[1] = Q12_TO_Q8(pos.vy);
    coords[HarryBone_Root].coord.t[2] = Q12_TO_Q8(pos.vz);
    coords[HarryBone_Root].flg = 0;
    /* the same spot before the truncation, for RootExactRegister */
    s_rootExactT[0] = (double)pos.vx / 16.0;
    s_rootExactT[1] = (double)pos.vy / 16.0;
    s_rootExactT[2] = (double)pos.vz / 16.0;
    s_viewYawQ12   = yaw;
    s_viewYawValid = 1;
}

/* Harry's body frame in the world, PSX axes: x right, y down, z forward. */
static void BodyRotation(float out[3][3])
{
    const float yaw = BodyYaw();
    out[0][0] = cosf(yaw);  out[0][1] = 0.0f; out[0][2] = sinf(yaw);
    out[1][0] = 0.0f;       out[1][1] = 1.0f; out[1][2] = 0.0f;
    out[2][0] = -sinf(yaw); out[2][1] = 0.0f; out[2][2] = cosf(yaw);
}

static void HandRefCapture(GsCOORDINATE2* coords)
{
    float  body[3][3];
    MATRIX m;
    Xf     hand;
    int    h;

    if ((s_handKnown[0] && s_handKnown[1]) ||
        g_SysWork.playerWork.extra.upperBodyState != PlayerUpperBodyState_None)
        return;

    BodyRotation(body);
    for (h = 0; h < 2; h++)
    {
        if (s_handKnown[h]) continue;
        Vw_CoordHierarchyMatrixCompute(&coords[h ? HarryBone_RightHand : HarryBone_LeftHand], &m);
        XfFromMatrix(&m, &hand);
        if (!IsScaledRotation(hand.r)) continue;   /* not posed this frame: try the next */
        Ortho(hand.r, hand.r);
        RotMulTransA(body, hand.r, s_handK[h]);
        s_handKnown[h] = 1;
        VrOvr_Log("[VR-IK] hand %c reference: K = [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f]\n",
                  h ? 'R' : 'L',
                  (double)s_handK[h][0][0], (double)s_handK[h][0][1], (double)s_handK[h][0][2],
                  (double)s_handK[h][1][0], (double)s_handK[h][1][1], (double)s_handK[h][1][2],
                  (double)s_handK[h][2][0], (double)s_handK[h][2][1], (double)s_handK[h][2][2]);
    }
}

/* The weapon's grip in the right hand bone's frame, per inventory item:
 * K = Aim^T * Hand, read from the game's aim pose (see the header). */
#define VR_IK_GRIP_ITEMS  256
#define VR_IK_GRIP_SETTLE 15     /* frames of steady aiming before reading it */
static float s_gripK[VR_IK_GRIP_ITEMS][3][3];
static u8    s_gripKnown[VR_IK_GRIP_ITEMS];

/* The aim direction the animation holds the weapon along: Harry's yaw, and
 * the aim pitch the pose leans to (field_122: 90 deg = level, more = up). */
static void AnimAimRotation(float out[3][3])
{
    const float yaw = BodyYaw();
    const float pit = (float)(g_SysWork.playerWork.player.properties.player.field_122 - 1024) *
                      (6.28318531f / 4096.0f);
    const float fwd[3]   = { sinf(yaw), 0.0f, cosf(yaw) };
    const float right[3] = { cosf(yaw), 0.0f, -sinf(yaw) };
    const float cp = cosf(pit), sp = sinf(pit);
    int i;

    for (i = 0; i < 3; i++)
    {
        const float down = (i == 1) ? 1.0f : 0.0f;
        out[i][0] = right[i];
        out[i][1] = cp * down + sp * fwd[i];   /* pitch up: the barrel rises (-Y) */
        out[i][2] = cp * fwd[i] - sp * down;
    }
}

/* The aim-pose grip (K, the hand following the aim ray) is a GUN thing. A
 * melee weapon read it too the first time it was raised - after which the
 * hand and the weapon swapped from the grip pose to the aim pose frame, and
 * a VR Weapon Grip offset tuned before no longer sat right, its values the
 * same (the knife). Melee weapons always follow the grip pose. */
static int IsGunItem(int item)
{
    return item >= InvItemId_Handgun && item <= InvItemId_HyperBlaster;
}

static void GripCapture(GsCOORDINATE2* coords)
{
    static int s_aimFrames = 0;
    const int  item = (int)g_SavegamePtr->equippedWeapon & 0xFF;
    float      aim[3][3];
    MATRIX     m;
    Xf         hand;

    if (!IsGunItem(item) || g_SysWork.playerWork.extra.upperBodyState != PlayerUpperBodyState_Aim)
    {
        s_aimFrames = 0;
        return;
    }
    if (s_gripKnown[item] || ++s_aimFrames < VR_IK_GRIP_SETTLE)
        return;

    Vw_CoordHierarchyMatrixCompute(&coords[HarryBone_RightHand], &m);
    XfFromMatrix(&m, &hand);
    if (!IsScaledRotation(hand.r))
        return;   /* not posed this frame: try the next */
    Ortho(hand.r, hand.r);
    AnimAimRotation(aim);
    RotMulTransA(aim, hand.r, s_gripK[item]);
    s_gripKnown[item] = 1;
    VrOvr_Log("[VR-IK] weapon grip read for item %d: K = [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f]\n",
              item,
              (double)s_gripK[item][0][0], (double)s_gripK[item][0][1], (double)s_gripK[item][0][2],
              (double)s_gripK[item][1][0], (double)s_gripK[item][1][1], (double)s_gripK[item][1][2],
              (double)s_gripK[item][2][0], (double)s_gripK[item][2][1], (double)s_gripK[item][2][2]);
}

/* World rotation the right hand bone should have this frame, 0 = keep the
 * animation's (no weapon, grip not read yet, no aim pose). */
static int GunHandRotation(float out[3][3])
{
    const int item = (int)g_SavegamePtr->equippedWeapon & 0xFF;
    float     aim[3][3];

    if (g_SysWork.playerCombat.weaponAttack == NO_VALUE || !IsGunItem(item) || !s_gripKnown[item] ||
        !AimWorldRotation(aim))
        return 0;
    RotMul(aim, s_gripK[item], out);
    return 1;
}

/* Rotation by angle t about unit axis a. */
static void AxisRotation(const float a[3], float t, float out[3][3])
{
    const float c = cosf(t), s = sinf(t), k = 1.0f - c;
    out[0][0] = c + a[0] * a[0] * k;        out[0][1] = a[0] * a[1] * k - a[2] * s; out[0][2] = a[0] * a[2] * k + a[1] * s;
    out[1][0] = a[1] * a[0] * k + a[2] * s; out[1][1] = c + a[1] * a[1] * k;        out[1][2] = a[1] * a[2] * k - a[0] * s;
    out[2][0] = a[2] * a[0] * k - a[1] * s; out[2][1] = a[2] * a[1] * k + a[0] * s; out[2][2] = c + a[2] * a[2] * k;
}

/* HAND SIZE: World_Scale shows the world k times bigger, so what the hand
 * holds (and the holsters) is drawn at 1 / World_Scale - your size. */
static float HandScale(void)
{
    const float w = g_PcConfig.vrScale > 0.05f ? g_PcConfig.vrScale : 1.0f;
    return 1.0f / w;
}

static int   s_ikActive;         /* defined with the holsters below */

/* VIRTUAL HAND (see the header), this frame: world rotation with scale, and
 * the wrist in world Q8 */
static float s_vhR[2][3][3];
static float s_vhT[2][3];
static int   s_vhOk[2];

/* vr_hands.c / vr_full.c: the VR hands are on this frame (first-person VR
 * gameplay, controllers tracked) */
int VrIk_HandsActive(void)
{
    return s_ikActive;
}

/* TORSO (see the header): never written here. Read once at the first idle:
 * Harry's standing height (PLAYER HEIGHT) and where his neck stands over the
 * root, in the body frame (x right, z forward; world Q8 at scale 1). */
static float s_neckRest[2];

/* Neck (head bone origin) minus root, world Q8; 0 = not posed this frame. */
static int NeckFromRoot(GsCOORDINATE2* coords, float out[3], float* rootY)
{
    MATRIX m;
    Xf     root, neck;
    int    k;

    if (coords[HarryBone_Torso].super == NULL)
        return 0;
    Vw_CoordHierarchyMatrixCompute(coords[HarryBone_Torso].super, &m);
    XfFromMatrix(&m, &root);
    if (!IsScaledRotation(root.r))
        return 0;
    Vw_CoordHierarchyMatrixCompute(&coords[HarryBone_Head], &m);
    XfFromMatrix(&m, &neck);
    for (k = 0; k < 3; k++)
        out[k] = neck.t[k] - root.t[k];
    if (rootY) *rootY = root.t[1];
    return 1;
}

static void ReadHarryHeight(GsCOORDINATE2* coords)
{
    float d[3], b[3][3];

    if (s_harryHeight > 0.0f)
        return;
    if (g_SysWork.playerWork.extra.upperBodyState != PlayerUpperBodyState_None)
        return;   /* standing idle only */
    if (!NeckFromRoot(coords, d, NULL))
        return;   /* not posed this frame */
    /* his standing neck above the root, at scale 1 */
    s_harryHeight = (-d[1] / s_bodyScaleApplied + VR_NECK_TO_EYE_M * VR_IK_UNITS_PER_M) /
                    VR_EYE_TO_HEIGHT;
    BodyRotation(b);
    s_neckRest[0] = (b[0][0] * d[0] + b[1][0] * d[1] + b[2][0] * d[2]) / s_bodyScaleApplied;
    s_neckRest[1] = (b[0][2] * d[0] + b[1][2] * d[1] + b[2][2] * d[2]) / s_bodyScaleApplied;
    VrOvr_Log("[VR-IK] Harry %.2f m tall\n", (double)(s_harryHeight / VR_IK_UNITS_PER_M));
}

/* The walk/run animation leans the torso and carries the hips' sway into it,
 * which takes the chest forward under a camera that stays put - walking put
 * the chest and shoulders inside the view. Nothing of the pose is changed:
 * the whole skeleton is moved back, as one piece on the root, by how far the
 * animated neck left its standing spot over the feet (horizontally), so the
 * chest stays under the eyes and no seam stretches. */
static void KeepNeckUnderView(GsCOORDINATE2* coords)
{
    float d[3], b[3][3], ex, ez;
    int   k;

    if (s_harryHeight <= 0.0f || !NeckFromRoot(coords, d, NULL))
        return;
    BodyRotation(b);
    ex = b[0][0] * d[0] + b[1][0] * d[1] + b[2][0] * d[2] - s_neckRest[0] * s_bodyScaleApplied;
    ez = b[0][2] * d[0] + b[1][2] * d[1] + b[2][2] * d[2] - s_neckRest[1] * s_bodyScaleApplied;
    for (k = 0; k < 3; k += 2)
    {
        const float d = b[k][0] * ex + b[k][2] * ez;
        coords[HarryBone_Root].coord.t[k] -= (s32)lrintf(d);
        s_rootExactT[k] -= (double)d;
    }
    for (k = 0; k < HarryBone_Count; k++)
        coords[k].flg = 0;
}

/* The root's exact local for PGXP (vw_calc.c Vw_PcExactLocalSet): every bone
 * of Harry hangs from it. */
static void RootExactRegister(GsCOORDINATE2* coords)
{
    extern void Vw_PcExactLocalSet(const MATRIX* rounded, const double m[9], const double t[3]);
    const MATRIX* r = &coords[HarryBone_Root].coord;
    double        m[9];
    int           i, j;

    extern void Vw_PcExactCoordSet(const GsCOORDINATE2* c, const double m[9], const double t[3]);

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            m[i * 3 + j] = (double)r->m[i][j] / 4096.0;
    Vw_PcExactLocalSet(r, m, s_rootExactT);
    /* the per-coordinate entry wins over the content slots: keep it ours */
    Vw_PcExactCoordSet(&coords[HarryBone_Root], m, s_rootExactT);
}

/* game_sys_states.c: the IK placed Harry's root this frame, leave it alone */
int VrIk_RootExactOwned(void)
{
    return s_ikActive;
}

/* HOLSTERS (see the header): each slot, in meters, in the body frame (x
 * right, y up, z forward), at Harry's own size, and the pitch that turns the
 * relaxed hand: +90 hangs it down, -90 stands it up. vrHolsterSlot order. */
static const struct { float right, up, fwd, pitch; } s_holsterSlots[6] = {
    { -0.20f, 0.90f,  0.04f,  90.0f },   /* left hip */
    {  0.20f, 0.90f,  0.04f,  90.0f },   /* right hip */
    { -0.12f, 1.28f,  0.12f,  90.0f },   /* left chest */
    {  0.12f, 1.28f,  0.12f,  90.0f },   /* right chest */
    { -0.15f, 1.45f, -0.15f, -90.0f },   /* behind the left shoulder */
    {  0.15f, 1.45f, -0.15f, -90.0f },   /* behind the right shoulder */
};

static int s_ikActive;   /* VrIk_ApplyArms posed the body this frame */

/* Harry owns this weapon (pc_combat.c, the inventory). */
static int OwnsWeapon(int itemId)
{
    extern int Pc_VrOwnsWeapon(u8 id);
    return Pc_VrOwnsWeapon((u8)itemId);
}

/* For the holster rows of the options (options.c): only owned weapons. */
int VrIk_WeaponOwned(int itemId)
{
    return itemId != 0 && OwnsWeapon(itemId);
}

/* The weapon in a slot (inventory id), 0 = none: holsters off, nothing
 * picked, not owned, or the same weapon already in an earlier slot. */
int VrIk_HolsterSlotItem(int slot)
{
    int id, k;
    if (!g_PcConfig.vrHolsterOn || slot < 0 || slot >= 6)
        return 0;
    id = PcConfig_HolsterWeapon(g_PcConfig.vrHolsterSlot[slot]);
    if (!id || !OwnsWeapon(id))
        return 0;
    for (k = 0; k < slot; k++)
        if (PcConfig_HolsterWeapon(g_PcConfig.vrHolsterSlot[k]) == id) return 0;
    return id;
}

/* The slot holding this weapon, -1 for none. */
int VrIk_HolsterSlotOf(int itemId)
{
    int k;
    for (k = 0; k < 6; k++)
        if (itemId && VrIk_HolsterSlotItem(k) == itemId) return k;
    return -1;
}

/* A slot's world matrix (rotation Q12, translation world Q8). 0 while the IK
 * body is not running or the relaxed hand is not read yet. */
int VrIk_HolsterSlotMatrix(int slot, MATRIX* out)
{
    const GsCOORDINATE2* root = &g_SysWork.playerBoneCoords[HarryBone_Root];
    float body[3][3], local[3], ofs[3], px[3][3], r[3][3];
    const float u = VR_IK_UNITS_PER_M;
    float a;
    int   i, j;

    if (slot < 0 || slot >= 6 || !s_ikActive || !s_handKnown[1])
        return 0;
    a = s_holsterSlots[slot].pitch * (3.14159265f / 180.0f);
    BodyRotation(body);

    /* PSX axes: y is down */
    local[0] = s_holsterSlots[slot].right * u * s_bodyScaleApplied;
    local[1] = -s_holsterSlots[slot].up * u * s_bodyScaleApplied;
    local[2] = s_holsterSlots[slot].fwd * u * s_bodyScaleApplied;
    RotVec(body, local, ofs);

    /* about the body's right axis, then into the world */
    px[0][0] = 1.0f; px[0][1] = 0.0f;     px[0][2] = 0.0f;
    px[1][0] = 0.0f; px[1][1] = cosf(a);  px[1][2] = -sinf(a);
    px[2][0] = 0.0f; px[2][1] = sinf(a);  px[2][2] = cosf(a);
    RotMul(px, s_handK[1], r);
    RotMul(body, r, r);

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
            out->m[i][j] = (s16)lrintf(r[i][j] * HandScale() * s_bodyScaleApplied * 4096.0f);
        out->t[i] = root->coord.t[i] + (s32)lrintf(ofs[i]);
    }
    {
        /* its float source for PGXP (vw_calc.c Vw_PcExactLocalSet): the slot
         * rides the body without whole-unit steps */
        extern void Vw_PcExactLocalSet(const MATRIX* rounded, const double m[9], const double t[3]);
        double em[9], et[3];
        for (i = 0; i < 3; i++)
        {
            for (j = 0; j < 3; j++)
                em[i * 3 + j] = (double)r[i][j] * HandScale() * s_bodyScaleApplied;
            et[i] = s_rootExactT[i] + ofs[i];   /* the root's exact spot (RootExactRegister) */
        }
        Vw_PcExactLocalSet(out, em, et);
    }
    return 1;
}

/* A slot's spot in world Q12, for the grab test (vr_interact.c). */
int VrIk_HolsterSlotWorld(int slot, VECTOR3* out)
{
    MATRIX m;
    if (!VrIk_HolsterSlotMatrix(slot, &m))
        return 0;
    out->vx = m.t[0] * 16;
    out->vy = m.t[1] * 16;
    out->vz = m.t[2] * 16;
    return 1;
}

/* PAPER MAP: it lives in a holster like the weapons - the slot set to "Map"
 * (PCCFG_HOLSTER_MAP) on the VR Holsters page - shown there folded, and held
 * it is a sheet with its bottom edge in the hand. */
#define VR_MAP_SHEET_W     0.30f
#define VR_MAP_SHEET_H     0.225f   /* the paper map is 4:3 */
#define VR_MAP_SHEET_LIFT  0.02f    /* bottom edge above the grip */
#define VR_MAP_FOLDED      0.40f    /* folded in the holster: this much of the sheet */
#define VR_MAP_FOLDED_TILT 60.0f    /* deg from upright: the holstered map lies back, its face up, to be seen looking down */

/* The holster slot holding the map, -1 for none (holsters off, or no slot set to Map). */
int VrIk_MapHolsterSlot(void)
{
    int k;
    if (!g_PcConfig.vrHolsterOn)
        return -1;
    for (k = 0; k < 6; k++)
        if (g_PcConfig.vrHolsterSlot[k] == PCCFG_HOLSTER_MAP) return k;
    return -1;
}

/* The map holster's spot in world Q12, for the grab test (vr_interact.c). */
int VrIk_MapHolsterWorld(VECTOR3* out)
{
    const int slot = VrIk_MapHolsterSlot();
    return slot >= 0 && VrIk_HolsterSlotWorld(slot, out);
}

/* The folded map in its holster (pc_minimap.c), in VrIk_MapSheetFrame's
 * format: a smaller sheet across the body at the slot, lying back so its
 * printed side faces up. Returns 1 / -1 / 0 like VrIk_MapSheetFrame. */
int VrIk_MapHolsterFrame(float centre[3], float right[3], float up[3], float front[3], float* cosView)
{
    const int   slot = VrIk_MapHolsterSlot();
    const float s    = ReachScale();
    const float a    = VR_MAP_FOLDED_TILT * (3.14159265f / 180.0f);
    float       body[3][3], up0[3], fw[3], toEye[3], facing = 0.0f, d;
    HeadFrame   f;
    MATRIX      m;
    int         i;

    if (slot < 0 || !VrIk_HolsterSlotMatrix(slot, &m) || !HeadFrameGet(&f))
        return 0;
    BodyRotation(body);   /* columns: right, down, forward */
    for (i = 0; i < 3; i++)
    {
        /* printed side up and back toward your eyes when you look down at it,
         * the image's top pointing ahead: readable the right way up */
        up0[i] =  body[i][2] * sinf(a) - body[i][1] * cosf(a);
        fw[i]  = -body[i][1] * sinf(a) - body[i][2] * cosf(a);
        right[i]  = body[i][0] * (VR_MAP_SHEET_W * 0.5f * VR_MAP_FOLDED) * s;
        up[i]     = up0[i] * (VR_MAP_SHEET_H * 0.5f * VR_MAP_FOLDED) * s;
        front[i]  = fw[i] * 0.001f * s;
        centre[i] = (float)m.t[i];
        toEye[i]  = (float)f.cam.t[i] - centre[i];
        facing   += toEye[i] * fw[i];
    }
    d = sqrtf(toEye[0] * toEye[0] + toEye[1] * toEye[1] + toEye[2] * toEye[2]);
    if (cosView) *cosView = (d > 1e-3f) ? facing / d : 1.0f;
    return (facing >= 0.0f) ? 1 : -1;
}

/* The held map (pc_minimap.c) is a physical sheet locked to the controller
 * that holds it, like a paper held by its bottom edge: it rises out of the
 * fist along the controller's through-the-fist axis (OpenXR grip -Z, thumb
 * side), its width runs along the controller's X, and its front - the side
 * the map is printed on - is right x up, which faces you in a natural hold.
 * Tilted back toward you by VR_MAP_SHEET_TILT so a relaxed wrist reads it.
 * Turn the wrist and you see its back, like real paper.
 * World Q8 out: centre, half-width (right), half-height (up), and 1 mm along
 * the front normal (front); cosView = cosine of the view angle (signed).
 * Returns 1 when the front faces the eyes, -1 when the back does, 0 when
 * there is no pose. */
#define VR_MAP_SHEET_TILT_XR 25.0f   /* deg: the grip's fist axis already leans forward */
#define VR_MAP_SHEET_TILT_VR 60.0f   /* OpenVR's Touch pose -Z points ahead like the aim */
int VrIk_MapSheetFrame(int hand, float centre[3], float right[3], float up[3], float front[3], float* cosView)
{
    HeadFrame   f;
    float       gp[3], gr[3][3], g[3], xr[3], up0[3], n0[3], ut[3], nt[3], rw[3], uw[3], nw[3];
    const float s    = ReachScale();
    /* the VR Holsters page's Map in Hand rows (vr_map_*): pitch adds to the
     * built-in tilt, yaw turns it about its up axis, roll about its face */
    const float* mo  = g_PcConfig.vrMapOfs;
    const float  d2r = 3.14159265f / 180.0f;
    const float tilt = ((VrOvr_RuntimeIsOpenXR() ? VR_MAP_SHEET_TILT_XR : VR_MAP_SHEET_TILT_VR) + mo[3]) * d2r;
    const float ct = cosf(tilt), st = sinf(tilt);
    const float cy = cosf(mo[4] * d2r), sy = sinf(mo[4] * d2r);
    const float cr = cosf(mo[5] * d2r), sr = sinf(mo[5] * d2r);
    float       toEye[3], facing = 0.0f;
    int         i;

    if (!HeadFrameGet(&f) || !VrOvr_GetHandPose(hand, gp, gr))
        return 0;

    /* in the tracking space (OpenVR axes: x right, y up, z back) */
    for (i = 0; i < 3; i++)
    {
        xr[i]  = gr[i][0];    /* the controller's X: across the fist */
        up0[i] = -gr[i][2];   /* -Z: out of the fist, thumb side */
    }
    n0[0] = xr[1] * up0[2] - xr[2] * up0[1];   /* front = right x up */
    n0[1] = xr[2] * up0[0] - xr[0] * up0[2];
    n0[2] = xr[0] * up0[1] - xr[1] * up0[0];
    for (i = 0; i < 3; i++)
    {
        ut[i] = up0[i] * ct + n0[i] * st;      /* the top leans back toward you */
        nt[i] = n0[i] * ct - up0[i] * st;
    }
    for (i = 0; i < 3; i++)
    {
        /* yaw: right and face turn about the sheet's up */
        const float x = xr[i], n = nt[i];
        xr[i] = x * cy - n * sy;
        nt[i] = n * cy + x * sy;
    }
    for (i = 0; i < 3; i++)
    {
        /* roll: right and up turn about the face */
        const float x = xr[i], uu = ut[i];
        xr[i] = x * cr + uu * sr;
        ut[i] = uu * cr - x * sr;
    }

    PointToWorld(&f, gp, g);
    DirToWorld(&f, xr, rw);
    DirToWorld(&f, ut, uw);
    DirToWorld(&f, nt, nw);
    if (!Normalize(rw) || !Normalize(uw) || !Normalize(nw))
        return 0;

    for (i = 0; i < 3; i++)
    {
        /* the map's Scale (VR Objects page, vr_map_scale): the sheet grows
         * from its bottom edge, which stays in the hand */
        const float ms = g_PcConfig.vrMapScale / 100.0f;
        right[i]  = rw[i] * (VR_MAP_SHEET_W * 0.5f) * ms * s;
        up[i]     = uw[i] * (VR_MAP_SHEET_H * 0.5f) * ms * s;
        front[i]  = nw[i] * 0.001f * s;
        /* the Map in Hand offsets (cm): forward = away from you, along the face */
        centre[i] = g[i] + uw[i] * (VR_MAP_SHEET_H * 0.5f * ms + VR_MAP_SHEET_LIFT) * s +
                    (rw[i] * mo[2] + uw[i] * mo[1] - nw[i] * mo[0]) * 0.01f * s;
        toEye[i]  = (float)f.cam.t[i] - centre[i];
        facing   += toEye[i] * nw[i];
    }
    if (cosView)
    {
        /* cosine between the front normal and the line to the eyes (matte shading) */
        const float d = sqrtf(toEye[0] * toEye[0] + toEye[1] * toEye[1] + toEye[2] * toEye[2]);
        *cosView = (d > 1e-3f) ? facing / d : 1.0f;
    }
    return (facing >= 0.0f) ? 1 : -1;
}

/* WEAPON GRIP (see the header): keep the page's values and the equipped
 * weapon's saved ones in step - load on a weapon change, save on an edit. */
static int   s_gripEditSlot = -2;
static float s_gripEditLast[6];

static void GripEditSync(void)
{
    const int item = (int)g_SavegamePtr->equippedWeapon & 0xFF;
    const int w    = PcConfig_GripSlot(item);
    float*    e    = g_PcConfig.vrGripEdit;
    char      key[24], val[96];

    if (w != s_gripEditSlot)
    {
        if (w >= 0) memcpy(e, g_PcConfig.vrGrip[w], sizeof(s_gripEditLast));
        else        memset(e, 0, sizeof(s_gripEditLast));
        memcpy(s_gripEditLast, e, sizeof(s_gripEditLast));
        s_gripEditSlot = w;
        return;
    }
    if (w < 0 || memcmp(e, s_gripEditLast, sizeof(s_gripEditLast)) == 0)
        return;
    memcpy(g_PcConfig.vrGrip[w], e, sizeof(s_gripEditLast));
    memcpy(s_gripEditLast, e, sizeof(s_gripEditLast));
    (void)key; (void)val;
    PcConfig_SaveGrip(item);   /* keeps the weapon's scale on the line */
}

/* Reset This Weapon (VR Weapon Grip page): the next sync saves the zeros. */
void VrIk_GripReset(void)
{
    memset(g_PcConfig.vrGripEdit, 0, sizeof(g_PcConfig.vrGripEdit));
}

/* The held weapon's matrix: the virtual hand turned by adj (rotation r) and
 * moved by ow, rounded into a MATRIX; its float source goes to vw_calc.c
 * (Vw_PcExactLocalSet), so PGXP draws it moving with the controller
 * continuously instead of in whole world units. */
static void HeldItemOut(int hand, const float r[3][3], const float ow[3], MATRIX* out)
{
    extern void Vw_PcExactLocalSet(const MATRIX* rounded, const double m[9], const double t[3]);
    double m[9], t[3];
    int    i, j;

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            out->m[i][j]  = (s16)lrintf(r[i][j] * 4096.0f);
            m[i * 3 + j] = (double)r[i][j];
        }
        t[i]      = (double)s_vhT[hand][i] + (double)ow[i];
        out->t[i] = (s32)lrint(t[i]);
    }
    Vw_PcExactLocalSet(out, m, t);
}

/* The knife in the LEFT hand: on the left virtual hand, with the VR Weapon
 * Grip offset mirrored (right and yaw/roll flip sides). */
static int LeftHeldItemMatrix(MATRIX* out)
{
    const int    item = (int)g_SavegamePtr->equippedWeapon & 0xFF;
    const int    w    = PcConfig_GripSlot(item);
    const float  d2r  = 3.14159265f / 180.0f;
    const float  cm   = ReachScale() / 100.0f;
    static const float zero[6] = { 0 };
    static const float ax[3] = { 1.0f, 0.0f, 0.0f }, ay[3] = { 0.0f, 1.0f, 0.0f }, az[3] = { 0.0f, 0.0f, 1.0f };
    const float* a = (w >= 0) ? g_PcConfig.vrGrip[w] : zero;
    float c[3][3], ct[3][3], rx[3][3], ry[3][3], rz[3][3], rc[3][3], adj[3][3], r[3][3], o[3], ow[3];
    int    i, j;

    if (!s_ikActive || !s_vhOk[0] || !GripWorldRotation(0, c))
        return 0;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) ct[i][j] = c[j][i];
    AxisRotation(ay, -a[4] * d2r, ry);
    AxisRotation(ax, a[3] * d2r, rx);
    AxisRotation(az, -a[5] * d2r, rz);
    RotMul(ry, rx, rc);
    RotMul(rc, rz, rc);
    RotMul(c, rc, adj);
    RotMul(adj, ct, adj);
    RotMul(adj, s_vhR[0], r);

    o[0] = -a[2] * cm;
    o[1] = -a[1] * cm;
    o[2] = a[0] * cm;
    RotVec(c, o, ow);

    (void)i; (void)j;
    HeldItemOut(0, r, ow, out);
    return 1;
}

/* The equipped weapon's model matrix in the hand, with its grip offset
 * (world_draw.c WorldGfx_HeldItemDraw). 0 = no VR hands this frame: the game
 * draws it on Harry's hand bone (cutscenes, flat play). */
int VrIk_HeldItemMatrix(MATRIX* out)
{
    const int    item = (int)g_SavegamePtr->equippedWeapon & 0xFF;
    const int    w    = PcConfig_GripSlot(item);
    static const float zero[6] = { 0 };
    const float* a;
    const float  d2r  = 3.14159265f / 180.0f;
    const float  cm   = ReachScale() / 100.0f;
    static const float ax[3] = { 1.0f, 0.0f, 0.0f }, ay[3] = { 0.0f, 1.0f, 0.0f }, az[3] = { 0.0f, 0.0f, 1.0f };
    float c[3][3], ct[3][3], rx[3][3], ry[3][3], rz[3][3], rc[3][3], adj[3][3], r[3][3], o[3], ow[3];
    int    i, j;

    if (VrInteract_WeaponHand() == 0)
        return LeftHeldItemMatrix(out);
    if (!s_ikActive || !s_vhOk[1])
        return 0;
    a = (w >= 0) ? g_PcConfig.vrGrip[w] : zero;

    /* the controller frame the hand follows: the aim pose with a gun, the
     * grip pose with anything else */
    if (!(g_SysWork.playerCombat.weaponAttack != NO_VALUE && IsGunItem(item) && s_gripKnown[item] &&
          AimWorldRotation(c)) &&
        !GripWorldRotation(1, c))
        return 0;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) ct[i][j] = c[j][i];

    /* R = yaw (about up) * pitch (about right) * roll (about the pointing axis) */
    AxisRotation(ay, a[4] * d2r, ry);
    AxisRotation(ax, a[3] * d2r, rx);
    AxisRotation(az, a[5] * d2r, rz);
    RotMul(ry, rx, rc);
    RotMul(rc, rz, rc);
    RotMul(c, rc, adj);
    RotMul(adj, ct, adj);             /* C R C^T: the turn in the world */
    RotMul(adj, s_vhR[1], r);

    /* the controller frame as CtrlRotToWorld builds it: x right, y down,
     * z forward (PSX axes) */
    o[0] = a[2] * cm;
    o[1] = -a[1] * cm;
    o[2] = a[0] * cm;
    RotVec(c, o, ow);

    HeldItemOut(1, r, ow, out);
    return 1;
}

/* Player_Update tail (player_control.c). */
void VrIk_ApplyArms(GsCOORDINATE2* coords)
{
    float tl[3], tr[3], gun[3][3], hw[2][3][3], sc;
    int   haveL = 0, haveR = 0, haveGun, haveHand[2], b, h;

    if (coords != g_SysWork.playerBoneCoords)
        return;
    {
        /* SH1VR_POSLOG (test runs, tools\sh-walkdir.ps1): Harry's position
         * and facing a few times a second */
        static int s_posLog = -1, s_posTick = 0;
        if (s_posLog < 0) s_posLog = (getenv("SH1VR_POSLOG") != NULL);
        if (s_posLog && ++s_posTick >= 10)
        {
            s_posTick = 0;
            VrOvr_Log("[VR-POS] %.3f %.3f yaw %d\n", (double)g_SysWork.playerWork.player.position.vx / 4096.0,
                      (double)g_SysWork.playerWork.player.position.vz / 4096.0,
                      (int)g_SysWork.playerWork.player.rotation.vy);
        }
    }
    s_ikActive = 0;
    s_vhOk[0] = s_vhOk[1] = 0;
    {
        /* why the VR hands are off this frame, logged when it changes */
        static const char* const s_why[] = { "on", "", "no headset view",
                                             "cutscene", "not first person", "not in gameplay",
                                             "no controller tracked" };
        static int s_lastWhy = -1;
        int why = !VrTracking_HeadCamActive() ? 2 :
                  VrTracking_CutsceneActive() ? 3 :
                  !g_PcFpsCam ? 4 :
                  (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay) ? 5 : 0;
        if (why == 0)
        {
            haveL = HandTarget(0, tl);
            haveR = HandTarget(1, tr);
            if (!haveL && !haveR)
                why = 6;
        }
        if (why != s_lastWhy)
        {
            VrOvr_Log("[VR-IK] hands %s (sysState %d)\n", s_why[why], (int)g_SysWork.sysState);
            s_lastWhy = why;
        }
        if (why != 0)
            return;
    }

    /* the model in the view's frame, before anything reads it */
    SyncRootToView(coords);

    /* world matrices recompose from the locals below: drop this frame's
     * cached ones along the arm chains first */
    for (b = HarryBone_Root; b <= HarryBone_RightHand; b++)
        coords[b].flg = 0;

    /* the animation's references, before anything here moves the body */
    GripCapture(coords);
    HandRefCapture(coords);
    /* a knife in the LEFT hand (vr_interact.c): the right hand is empty */
    haveGun = haveR && VrInteract_WeaponHand() == 1 && GunHandRotation(gun);

    /* each hand: the controller's rotation times the fixed offset K */
    for (h = 0; h < 2; h++)
    {
        float g[3][3];
        haveHand[h] = s_handKnown[h] && GripWorldRotation(h, g);
        if (haveHand[h]) RotMul(g, s_handK[h], hw[h]);
    }

    ReadHarryHeight(coords);
    KeepNeckUnderView(coords);
    RootExactRegister(coords);
    s_ikActive = 1;
    GripEditSync();
    {
        /* SH1VR_EXEC (test runs, K:\sh1vr\xrsim64): one console line, once, at the
         * first frame of play - e.g. "give allweapons" for the holster tests */
        static int s_execDone = 0;
        if (!s_execDone)
        {
            extern void Pc_ConsoleExec(const char* line);
            const char* e = getenv("SH1VR_EXEC");
            s_execDone = 1;
            /* several lines separated by ';' */
            while (e && e[0])
            {
                char        line[160];
                const char* semi = strchr(e, ';');
                size_t      n    = semi ? (size_t)(semi - e) : strlen(e);
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, e, n);
                line[n] = '\0';
                if (line[0]) Pc_ConsoleExec(line);
                e = semi ? semi + 1 : NULL;
            }
            /* SH1VR_TP="x,z" (meters): put Harry there */
            e = getenv("SH1VR_TP");
            if (e && e[0])
            {
                float tx = 0.0f, tz = 0.0f;
                if (sscanf(e, "%f,%f", &tx, &tz) == 2)
                {
                    g_SysWork.playerWork.player.position.vx = (s32)(tx * 4096.0f);
                    g_SysWork.playerWork.player.position.vz = (s32)(tz * 4096.0f);
                }
            }
        }
    }
    {
        /* the holstered weapons' own models, one load a frame (world_draw.c) */
        extern void Pc_VrHolsterModelsUpdate(void);
        Pc_VrHolsterModelsUpdate();
    }

    /* VIRTUAL HANDS (see the header): the wrist, turned like the controller
     * times K (or the gun's grip on the aim ray), at the skeleton's scale
     * (the shoulder's) times 1 / World_Scale - where the IK put the hand bone */
    {
        MATRIX m;
        Xf     ws;
        float  q[3][3];
        Vw_CoordHierarchyMatrixCompute(&coords[HarryBone_RightShoulder], &m);
        XfFromMatrix(&m, &ws);
        sc = IsScaledRotation(ws.r) ? Ortho(ws.r, q) : s_bodyScaleApplied;
    }
    for (h = 0; h < 2; h++)
    {
        const float* t   = h ? tr : tl;
        const int    has = h ? haveR : haveL;
        float        g[3][3];
        const float (*rot)[3] = (h == 1 && haveGun) ? (const float (*)[3])gun :
                                haveHand[h]           ? (const float (*)[3])hw[h] : NULL;
        int i, j;
        if (!has) continue;
        if (rot == NULL)
        {
            /* K not read yet (no idle frame so far): the controller's own frame */
            if (!GripWorldRotation(h, g)) continue;
            rot = (const float (*)[3])g;
        }
        for (i = 0; i < 3; i++)
        {
            for (j = 0; j < 3; j++) s_vhR[h][i][j] = rot[i][j] * sc * HandScale();
            s_vhT[h][i] = t[i];
        }
        s_vhOk[h] = 1;
    }

    for (b = HarryBone_Root; b <= HarryBone_RightHand; b++)
        coords[b].flg = 0;
}
