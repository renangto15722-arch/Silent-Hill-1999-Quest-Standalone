/* VR HD WEAPONS. The inventory shows each weapon from its own close-up model
 * (ITEM\UNQxx.TMD, drawn by libgs: item_screens_cam.c func_8004BD74), far more
 * detailed than the PLM the game puts in Harry's hand (world_draw.c
 * WorldGfx_HeldItemSet). This loads the close-up model of a weapon into a
 * buffer of its own and draws it in place of the PLM - in the hand and in the
 * holsters - fitted onto the PLM: same size and same spot.
 *
 * The fit (vr_weapon_hd_fit.h) was solved offline from the two models
 * (K:\sh1vr\wpnhd\fit2.py on SH1VR_WPNDUMP's dump): PLM = M * TMD + t, M a
 * rotation times a uniform scale. Every PLM-side setting (VR Weapon Grip,
 * holster slots, the aim) still applies, since the new model hangs from the
 * same coordinate the PLM was drawn from.
 *
 * Lighting: the light the game builds for its lit characters
 * (bodyprog_80055028.c func_8005A21C: func_80057228 + func_8005A42C/478/838,
 * GTE light/colour matrices and back colour), made for the PLM's own world
 * matrix, then turned into the TMD's frame (light matrix * fit rotation), so
 * the libgs drawer's NormalColorCol lights it the game's way.
 *
 * SH1VR_WPNDUMP=1 dumps both models of every weapon to vrweapon_dump.txt in
 * the game folder (the fit's input). */
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/item_screens.h"
#include "main/fsqueue.h"
#include "pc_config.h"

extern void VrOvr_Log(const char* fmt, ...);
extern void LmHeader_FixOffsets(s_LmHeader* lmHdr);
extern void GsLinkObject4_PC(struct TMD_STRUCT* tmd, GsDOBJ2* obj);
extern void func_80057228(MATRIX* mat, s32 alpha, SVECTOR* arg2, VECTOR3* arg3);
extern void GsSetLsMatrix(MATRIX* m);
extern s_WorldEnvWork g_WorldEnvWork;

typedef struct
{
    int itemId;
    s32 tmdIdx;   /* the inventory's close-up model */
    s32 plmIdx;   /* the model the game holds */
    const char* name;
} VrWeaponHdFiles;

static const VrWeaponHdFiles s_files[] = {
    { InvItemId_KitchenKnife, FILE_ITEM_UNQ80_TMD, FILE_ITEM_KNIFE_PLM,   "knife"    },
    { InvItemId_SteelPipe,    FILE_ITEM_UNQ81_TMD, FILE_ITEM_PIPE_PLM,    "pipe"     },
    { InvItemId_Hammer,       FILE_ITEM_UNQ82_TMD, FILE_ITEM_HAMMER_PLM,  "hammer"   },
    { InvItemId_Chainsaw,     FILE_ITEM_UNQ83_TMD, FILE_ITEM_CSAW_PLM,    "chainsaw" },
    { InvItemId_Axe,          FILE_ITEM_UNQ84_TMD, FILE_ITEM_AXE_PLM,     "axe"      },
    { InvItemId_RockDrill,    FILE_ITEM_UNQ85_TMD, FILE_ITEM_DRILL_PLM,   "drill"    },
    { InvItemId_Katana,       FILE_ITEM_UNQ86_TMD, FILE_ITEM_KATANA_PLM,  "katana"   },
    { InvItemId_Handgun,      FILE_ITEM_UNQA0_TMD, FILE_ITEM_HANDGUN_PLM, "handgun"  },
    { InvItemId_HuntingRifle, FILE_ITEM_UNQA1_TMD, FILE_ITEM_RIFLE_PLM,   "rifle"    },
    { InvItemId_Shotgun,      FILE_ITEM_UNQA2_TMD, FILE_ITEM_SHOTGUN_PLM, "shotgun"  },
    { InvItemId_HyperBlaster, FILE_ITEM_UNQA3_TMD, FILE_ITEM_HPRGUN_PLM,  "hyper"    },
};
#define VR_WPN_COUNT ((int)(sizeof(s_files) / sizeof(s_files[0])))

/* PLM point = m * TMD point + t (m row-major) */
typedef struct
{
    int   itemId;
    float m[9];
    float t[3];
} VrWeaponHdFit;

static const VrWeaponHdFit s_fit[] = {
#include "vr_weapon_hd_fit.h"
};
#define VR_FIT_COUNT ((int)(sizeof(s_fit) / sizeof(s_fit[0])))

typedef struct
{
    u8*               buf;     /* the TMD file, kept: the object points into it */
    struct TMD_STRUCT tmd;
    GsDOBJ2           obj;
    GsCOORDINATE2     coord;   /* the fit, hung from the PLM's coordinate */
    MATRIX            local;   /* the fit rounded (the coordinate's local) */
    double            em[9], et[3];
    float             rot[3][3];   /* the fit's rotation, for the light */
    u8                ready, failed;
} VrWeaponHd;

static VrWeaponHd s_hd[VR_WPN_COUNT];

/* One file into a fresh buffer; reads are synchronous on PC once the queue
 * is pumped (world_draw.c VrHolster_Load). NULL on failure. */
static u8* VrWeaponHd_ReadFile(s32 fileIdx, s32* outSize)
{
    s32 size = Fs_GetFileSectorAlignedSize(fileIdx), pump = 0;
    u8* buf;

    *outSize = size;
    if (size <= 0 || (buf = (u8*)calloc(1, size + 4096)) == NULL)
        return NULL;
    Fs_QueueStartRead(fileIdx, buf);
    while (Fs_QueueGetLength() > 0 && pump++ < 100000)
        Fs_QueueUpdate();
    return buf;
}

static void VrWeaponHd_DumpTmd(FILE* f, const u8* buf)
{
    const u32* h    = (const u32*)buf;      /* id, flags, nobj, objects */
    const u8*  objs = buf + 12;
    u32        nobj = h[2], o;

    fprintf(f, "TMD id %08x flags %u nobj %u\n", h[0], h[1], nobj);
    if (nobj == 0 || nobj > 48) return;
    for (o = 0; o < nobj; o++)
    {
        const u32* raw = (const u32*)(objs + o * 28);
        const s16* v   = (const s16*)(objs + raw[0]);
        const u8*  p   = objs + raw[4];
        u32        i, n = raw[5];
        int        modes[256] = { 0 };

        fprintf(f, "obj %u verts %u norms %u prims %u scale %d\n", o, raw[1], raw[3], raw[5], (int)raw[6]);
        for (i = 0; i < raw[1] && i < 4096; i++)
            fprintf(f, "tv %d %d %d\n", v[i * 4 + 0], v[i * 4 + 1], v[i * 4 + 2]);
        for (i = 0; i < n && i < 8192; i++)
        {
            /* olen, ilen, flag, mode; size (1 + ilen) words */
            modes[p[3]]++;
            if (i < 4)
                fprintf(f, "prim %u olen %u ilen %u flag %02x mode %02x\n", i, p[0], p[1], p[2], p[3]);
            if (p[3] == 0x34 && p[1] == 6)
            {
                /* TG3: uv0 cba, uv1 tsb, uv2 pad, then n0 v0 n1 v1 n2 v2 */
                const u16* q  = (const u16*)(p + 16);
                const u16* uv = (const u16*)(p + 4);
                fprintf(f, "tt %u %u %u\n", q[1], q[3], q[5]);
                fprintf(f, "tx uv %d,%d %d,%d %d,%d cba %04x tsb %04x\n", p[4], p[5], p[8], p[9], p[12], p[13],
                        uv[1], uv[3]);
            }
            if (p[1] == 0) break;
            p += (1 + p[1]) * 4;
        }
        for (i = 0; i < 256; i++)
            if (modes[i]) fprintf(f, "mode %02x x%d\n", i, modes[i]);
    }
}

static void VrWeaponHd_DumpPlm(FILE* f, u8* buf)
{
    s_LmHeader*    lm = (s_LmHeader*)buf;
    s_ModelHeader* mh;
    int            m, i;

    LmHeader_FixOffsets(lm);
    fprintf(f, "PLM models %d materials %d\n", lm->modelCount, lm->materialCount);
    if (lm->modelCount < 1) return;
    mh = &lm->modelHdrs[0];
    fprintf(f, "model0 meshes %d vofs %d nofs %d B0 %d B1 %d B4 %d\n", mh->meshCount, mh->vertexOffset,
            mh->normalOffset, mh->field_B_0, mh->field_B_1, mh->field_B_4);
    for (m = 0; m < mh->meshCount; m++)
    {
        s_MeshHeader* me = &mh->meshHdrs[m];
        fprintf(f, "mesh %d prims %d verts %d norms %d\n", m, me->primitiveCount, me->vertexCount, me->normalCount);
        for (i = 0; i < me->vertexCount; i++)
            fprintf(f, "pv %d %d %d\n", me->verticesXy[i].vx, me->verticesXy[i].vy, me->verticesZ[i]);
        for (i = 0; i < me->primitiveCount; i++)
        {
            /* vertex indices (func_8005AC50 u.s_1.field_0..3; 0xFF = triangle) */
            const u8* c = me->primitives[i].field_C;
            fprintf(f, "pp %d %d %d %d\n", c[0], c[1], c[2], c[3]);
        }
    }
}

static int VrWeaponHd_Index(int itemId)
{
    int w;
    for (w = 0; w < VR_WPN_COUNT; w++)
        if (s_files[w].itemId == itemId) return w;
    return -1;
}

static const VrWeaponHdFit* VrWeaponHd_FitOf(int itemId)
{
    int i;
    for (i = 0; i < VR_FIT_COUNT; i++)
        if (s_fit[i].itemId == itemId) return &s_fit[i];
    return NULL;
}

/* The close-up model into its own buffer, its object built straight on it
 * (not through GsMapModelingData: that cache is a ring the inventory cycles
 * through, and would free these under the hand). */
static void VrWeaponHd_Load(int w)
{
    VrWeaponHd*          h   = &s_hd[w];
    const VrWeaponHdFit* fit = VrWeaponHd_FitOf(s_files[w].itemId);
    const u32*           raw;
    u8*                  objs;
    s32                  size;
    float                sc;
    int                  i, j;

    if (fit == NULL || (h->buf = VrWeaponHd_ReadFile(s_files[w].tmdIdx, &size)) == NULL)
    {
        h->failed = 1;
        return;
    }
    /* id, flags, nobj, then 7-word objects; offsets from the object table */
    objs = h->buf + 12;
    raw  = (const u32*)objs;
    if (((u32*)h->buf)[2] < 1 || raw[1] == 0 || raw[5] == 0 || raw[0] >= (u32)size || raw[4] >= (u32)size)
    {
        free(h->buf);
        h->buf    = NULL;
        h->failed = 1;
        return;
    }
    h->tmd.vertop  = (u_long*)(objs + raw[0]);
    h->tmd.vern    = raw[1];
    h->tmd.nortop  = (u_long*)(objs + raw[2]);
    h->tmd.norn    = raw[3];
    h->tmd.primtop = (u_long*)(objs + raw[4]);
    h->tmd.primn   = raw[5];
    h->tmd.scale   = raw[6];
    memset(&h->obj, 0, sizeof(h->obj));
    GsLinkObject4_PC(&h->tmd, &h->obj);
    h->obj.coord2 = &h->coord;

    /* the fit: rounded for the game, exact for PGXP, its rotation for light */
    sc = cbrtf(fit->m[0] * (fit->m[4] * fit->m[8] - fit->m[5] * fit->m[7]) -
               fit->m[1] * (fit->m[3] * fit->m[8] - fit->m[5] * fit->m[6]) +
               fit->m[2] * (fit->m[3] * fit->m[7] - fit->m[4] * fit->m[6]));
    if (sc <= 0.0f) sc = 1.0f;
    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            h->local.m[i][j]  = (s16)lrintf(fit->m[i * 3 + j] * 4096.0f);
            h->em[i * 3 + j]  = fit->m[i * 3 + j];
            h->rot[i][j]      = fit->m[i * 3 + j] / sc;
        }
        h->local.t[i] = (s32)lrintf(fit->t[i]);
        h->et[i]      = fit->t[i];
    }
    h->ready = 1;
    VrOvr_Log("[VR-WPNHD] %s: close-up model loaded (%u verts, %u prims, fit scale %.4f)\n",
              s_files[w].name, (unsigned)raw[1], (unsigned)raw[5], sc);
}

/* The light the game gives its lit characters (func_8005A21C), for the PLM's
 * world matrix, turned into the TMD's frame. black: unlit (an equipped
 * weapon's holster). */
static void VrWeaponHd_Light(MATRIX* plmWorld, const float rot[3][3], int black)
{
    extern void func_8005A42C(s_GteScratchData* scratchData, q19_12 alpha);
    extern void func_8005A478(s_GteScratchData* scratchData, q19_12 alpha);
    extern void func_8005A838(s_GteScratchData* scratchData, s32 scale);
    extern void ReadLightMatrix(MATRIX* m);
    s_GteScratchData* scratch = PSX_SCRATCH_ADDR(0);
    MATRIX            zero, l, lt;
    int               i, j;

    memset(&zero, 0, sizeof(zero));
    if (black)
    {
        SetLightMatrix(&zero);
        SetColorMatrix(&zero);
        SetBackColor(0, 0, 0);
        return;
    }
    if (g_WorldEnvWork.field_0 != 0)
        func_80057228(plmWorld, g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);
    switch (g_WorldEnvWork.field_0)
    {
        case 1:
            func_8005A478(scratch, 256 << 4);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            break;
        case 2:
            func_8005A838(scratch, 256 << 4);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            break;
        default:
            /* flat: the character colour (field_3D8). Out = colour * RBK / 4096
             * with RBK = rbk << 4, and the drawer's colour is 0x80: rbk = 2c */
            func_8005A42C(scratch, 256 << 4);
            SetLightMatrix(&zero);
            SetColorMatrix(&zero);
            SetBackColor(scratch->field_3D8.r * 2, scratch->field_3D8.g * 2, scratch->field_3D8.b * 2);
            return;
    }
    /* a normal n of the TMD is rot*n on the PLM: L_tmd = L_plm * rot */
    ReadLightMatrix(&l);
    memset(&lt, 0, sizeof(lt));
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            lt.m[i][j] = (s16)lrintf(l.m[i][0] * rot[0][j] + l.m[i][1] * rot[1][j] + l.m[i][2] * rot[2][j]);
    SetLightMatrix(&lt);
}

/* Set by world_draw.c around a weapon drawn in a grab box: the box already
 * sized it, so the weapon's own Scale is not applied on top. */
int g_VrWeaponHdNoScale = 0;

/* Draws the close-up model of itemId where its PLM would be drawn from
 * plmCoord (the hand's VR grip, a holster slot, or the hand bone itself when
 * the weapon has no grip offset). 0 = not drawn (not a weapon, not loaded):
 * draw the PLM. */
int VrWeaponHd_Draw(int itemId, GsCOORDINATE2* plmCoord, int black)
{
    extern void Vw_PcExactCoordSet(const GsCOORDINATE2* c, const double m[9], const double t[3]);
    const int   w = VrWeaponHd_Index(itemId);
    VrWeaponHd* h;
    MATRIX      plmWorld, plmView, world, view;

    if (w < 0)
        return 0;
    h = &s_hd[w];
    if (!h->ready)
        return 0;

    plmCoord->flg = 0;
    Vw_CoordToWorldAndViewMatrices(plmCoord, &plmWorld, &plmView);

    {
        /* the weapon's own Scale (VR Objects page, vrGripScale): the fit
         * scaled about the PLM's origin - the point the weapon hangs from the
         * hand by - so it grows in the grip */
        const int    slot = PcConfig_GripSlot(itemId);
        const double k    = (g_VrWeaponHdNoScale || slot < 0) ? 1.0 : g_PcConfig.vrGripScale[slot] / 100.0;
        double       em[9], et[3];
        int          i;
        for (i = 0; i < 9; i++)
        {
            em[i] = h->em[i] * k;
            h->coord.coord.m[i / 3][i % 3] = (s16)lrint(em[i] * 4096.0);
        }
        for (i = 0; i < 3; i++)
        {
            et[i] = h->et[i] * k;
            h->coord.coord.t[i] = (s32)lrint(et[i]);
        }
        h->coord.super = plmCoord;
        h->coord.flg   = 0;
        plmCoord->flg  = 0;
        Vw_PcExactCoordSet(&h->coord, em, et);
    }
    Vw_CoordToWorldAndViewMatrices(&h->coord, &world, &view);

    VrWeaponHd_Light(&plmWorld, h->rot, black);
    GsSetLsMatrix(&view);
    {
        /* [VR-WPNHD] the first draw of each weapon: what reached the OT */
        extern int g_PcTmdWorldDepth;   /* libgs_stub.c: bucket by world depth */
        static u32 s_logged;
        u8* before = (u8*)GsOUT_PACKET_P;
        /* shift 1 = the characters' OT bucket scale (func_80057090 arg2) */
        extern int g_PsyX_ActorVertexDepth;   /* PsyX_GPU.cpp: per-pixel against the hand */
        const int  prevVd = g_PsyX_ActorVertexDepth;
        g_PcTmdWorldDepth       = 1;
        g_PsyX_ActorVertexDepth = 2;   /* held item: per-vertex depth + close near plane */
        GsSortObject4J(&h->obj, &g_OrderingTable0[g_ActiveBufferIdx], 1, (u_long*)PSX_SCRATCH);
        g_PcTmdWorldDepth       = 0;
        g_PsyX_ActorVertexDepth = prevVd;
        if (!(s_logged & (1u << w)))
        {
            s_logged |= 1u << w;
            VrOvr_Log("[VR-WPNHD] %s drawn: %d bytes of prims, view t %d %d %d, light mode %d\n", s_files[w].name,
                      (int)((u8*)GsOUT_PACKET_P - before), (int)view.t[0], (int)view.t[1], (int)view.t[2],
                      (int)g_WorldEnvWork.field_0);
        }
    }
    return 1;
}

/* The close-up model's triangles in the game world as the hand holds it
 * (plmWorld = VrIk_HeldItemMatrix, no parent), for the VR hand's depth test
 * (vr_hands.c): the hand then shows where it is in front of the weapon and
 * hides where the weapon is in front of it. Every model is TG3 only (mode
 * 0x34: uv/clut/tpage words, then n0 v0 n1 v1 n2 v2). A weapon without a
 * close-up model gives none: the hand is then drawn whole over it. */
#define VR_WPN_OCC_MAXV 1024
void VrWeaponHd_HandOccluder(int itemId, const MATRIX* plmWorld)
{
    extern void VrHands_OccluderSet(int slot, const float* worldPts, int nVerts);
    static float pts[VR_WPN_OCC_MAXV * 3];
    const int    w = VrWeaponHd_Index(itemId);
    const VrWeaponHd* h;
    const s16*   verts;
    const u8*    p;
    float        pm[3][3], pt[3], lm[9], lt[3];
    double       k;
    u32          i;
    int          n = 0, slot, a, b, c;

    if (w < 0 || !s_hd[w].ready)
    {
        VrHands_OccluderSet(0, NULL, 0);
        return;
    }
    h     = &s_hd[w];
    slot  = PcConfig_GripSlot(itemId);
    k     = ((slot >= 0) ? g_PcConfig.vrGripScale[slot] : 100.0f) / 100.0;
    for (a = 0; a < 9; a++) lm[a] = (float)(h->em[a] * k);
    for (a = 0; a < 3; a++)
    {
        lt[a] = (float)(h->et[a] * k);
        pt[a] = (float)plmWorld->t[a];
        for (b = 0; b < 3; b++) pm[a][b] = (float)plmWorld->m[a][b] / 4096.0f;
    }
    verts = (const s16*)h->tmd.vertop;
    p     = (const u8*)h->tmd.primtop;
    for (i = 0; i < (u32)h->tmd.primn && n + 3 <= VR_WPN_OCC_MAXV; i++)
    {
        if (p[3] == 0x34 && p[1] == 6)
        {
            const u16* q = (const u16*)(p + 16);
            const u16  vi[3] = { q[1], q[3], q[5] };
            if (vi[0] < h->tmd.vern && vi[1] < h->tmd.vern && vi[2] < h->tmd.vern)
            {
                for (c = 0; c < 3; c++)
                {
                    const s16* v = verts + vi[c] * 4;
                    float      l[3];
                    /* TMD -> PLM (the fit, with the weapon's Scale) -> world */
                    for (a = 0; a < 3; a++)
                        l[a] = lm[a * 3 + 0] * v[0] + lm[a * 3 + 1] * v[1] + lm[a * 3 + 2] * v[2] + lt[a];
                    for (a = 0; a < 3; a++)
                        pts[n * 3 + a] = pm[a][0] * l[0] + pm[a][1] * l[1] + pm[a][2] * l[2] + pt[a];
                    n++;
                }
            }
        }
        if (p[1] == 0) break;
        p += (1 + p[1]) * 4;
    }
    VrHands_OccluderSet(0, pts, n);
}

/* Test view (SH1VR_WPNHD_SHOW=<item id>, sim checks): that weapon's close-up
 * model half a metre in front of the camera, side-on, barrel to the right,
 * grip down - the whole model, with no hand in the way. */
void VrWeaponHd_DebugShow(void)
{
    extern MATRIX D_800C3868;   /* the camera: world position in t */
    extern MATRIX VbWvsMatrix;  /* world -> view rotation */
    static int s_item = -2;
    static GsCOORDINATE2 s_coord;
    /* PLM axes in view: x -> back (-z), y (down) -> down, z (barrel) -> right */
    static const int M[3][3] = { { 0, 0, 1 }, { 0, 1, 0 }, { -1, 0, 0 } };
    const int v[3] = { 0, 0, 110 };   /* ~0.5 m ahead at World_Scale 1.45 */
    int i, j, k, w;

    if (s_item == -2)
    {
        const char* e = getenv("SH1VR_WPNHD_SHOW");
        s_item = e ? atoi(e) : -1;
    }
    if (s_item < 0 || (w = VrWeaponHd_Index(s_item)) < 0)
        return;
    if (!s_hd[w].ready && !s_hd[w].failed)
        VrWeaponHd_Load(w);
    for (i = 0; i < 3; i++)
    {
        /* rotation = Vb^T * M; position = camera + Vb^T * v */
        for (j = 0; j < 3; j++)
        {
            int acc = 0;
            for (k = 0; k < 3; k++) acc += VbWvsMatrix.m[k][i] * M[k][j];
            s_coord.coord.m[i][j] = (s16)acc;
        }
        s_coord.coord.t[i] = D_800C3868.t[i] +
            ((VbWvsMatrix.m[0][i] * v[0] + VbWvsMatrix.m[1][i] * v[1] + VbWvsMatrix.m[2][i] * v[2]) >> 12);
    }
    s_coord.super = NULL;
    VrWeaponHd_Draw(s_item, &s_coord, 0);

    /* the old PLM in the same pose, 15 cm higher, to compare */
    {
        extern void Bone_ModelAssign(s_Bone* bone, s_LmHeader* lmHdr, s32 modelHdrIdx);
        extern void Lm_MaterialFsImageApply1(s_LmHeader* lmHdr, char* newStr, s_FsImageDesc* image, s32 blendMode);
        extern void Lm_MaterialFlagsApply(s_LmHeader* lmHdr);
        extern void func_80057090(s_ModelInfo* modelInfo, GsOT* arg1, s32 arg2, MATRIX* viewMat, MATRIX* worldMat, u16 arg5);
        static u8*           s_plm;
        static s_Bone        s_bone;
        static s_FsImageDesc s_desc;
        static GsCOORDINATE2 s_up;
        MATRIX               wm, vm;
        if (s_plm == NULL)
        {
            s32 sz;
            if ((s_plm = VrWeaponHd_ReadFile(s_files[w].plmIdx, &sz)) == NULL) return;
            LmHeader_FixOffsets((s_LmHeader*)s_plm);
            s_desc.tPage[1] = 27; s_desc.tPage[0] = 0; s_desc.u = 0; s_desc.v = 0;
            s_desc.clutX = 736; s_desc.clutY = 480;   /* Harry's page, as the held item */
            Lm_MaterialFsImageApply1((s_LmHeader*)s_plm, "HERO", &s_desc, BlendMode_Additive);
            Lm_MaterialFlagsApply((s_LmHeader*)s_plm);
            Bone_ModelAssign(&s_bone, (s_LmHeader*)s_plm, 0);
        }
        s_up = s_coord;
        for (i = 0; i < 3; i++)
            s_up.coord.t[i] -= (VbWvsMatrix.m[1][i] * 35) >> 12;   /* view up = -y */
        s_up.flg = 0;
        Vw_CoordToWorldAndViewMatrices(&s_up, &wm, &vm);
        func_80057090(&s_bone.modelInfo, &g_OrderingTable0[g_ActiveBufferIdx], 1, &vm, &wm, 0);
    }
}

static void VrWeaponHd_Dump(void);

/* Game update (world_draw.c Pc_VrHolsterModelsUpdate): the equipped weapon's
 * close-up first, then the holstered ones, one load a frame. */
void VrWeaponHd_Update(void)
{
    extern int VrIk_HolsterSlotItem(int slot);
    int        k, w;

    VrWeaponHd_Dump();
    w = VrWeaponHd_Index(g_SavegamePtr->equippedWeapon);
    if (w >= 0 && !s_hd[w].ready && !s_hd[w].failed)
    {
        VrWeaponHd_Load(w);
        return;
    }
    for (k = 0; k < 6; k++)
    {
        w = VrWeaponHd_Index(VrIk_HolsterSlotItem(k));
        if (w >= 0 && !s_hd[w].ready && !s_hd[w].failed)
        {
            VrWeaponHd_Load(w);
            return;
        }
    }
    {
        /* the weapon wheel shows every weapon Harry owns */
        extern int VrInteract_WheelMode(void);
        extern int Pc_VrOwnsWeapon(u8 id);
        if (VrInteract_WheelMode())
            for (w = 0; w < VR_WPN_COUNT; w++)
                if (Pc_VrOwnsWeapon((u8)s_files[w].itemId) && !s_hd[w].ready && !s_hd[w].failed)
                {
                    VrWeaponHd_Load(w);
                    return;
                }
    }
}

static void VrWeaponHd_Dump(void)
{
    static int s_dumped = 0;
    FILE*      f;
    int        w;

    if (s_dumped || getenv("SH1VR_WPNDUMP") == NULL)
        return;
    s_dumped = 1;
    if ((f = fopen("vrweapon_dump.txt", "w")) == NULL)
        return;
    for (w = 0; w < VR_WPN_COUNT; w++)
    {
        s32 sz;
        u8* tmd = VrWeaponHd_ReadFile(s_files[w].tmdIdx, &sz);
        u8* plm;
        fprintf(f, "=== %s item %d\n", s_files[w].name, s_files[w].itemId);
        if (tmd) { VrWeaponHd_DumpTmd(f, tmd); free(tmd); }
        plm = VrWeaponHd_ReadFile(s_files[w].plmIdx, &sz);
        if (plm) { VrWeaponHd_DumpPlm(f, plm); free(plm); }
    }
    {
        /* Harry's own models (the hand variants) and what each bone draws now */
        s_LmHeader* lm = (s_LmHeader*)HARRY_LM_BUFFER;
        int k;
        if (lm != NULL && lm->modelHdrs != NULL)
        {
            fprintf(f, "=== harry lm models %d\n", lm->modelCount);
            for (k = 0; k < lm->modelCount; k++)
            {
                const s_ModelHeader* mh = &lm->modelHdrs[k];
                int m, i, n = 0, lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
                for (m = 0; m < mh->meshCount; m++)
                {
                    const s_MeshHeader* me = &mh->meshHdrs[m];
                    for (i = 0; i < me->vertexCount; i++, n++)
                    {
                        const int v[3] = { me->verticesXy[i].vx, me->verticesXy[i].vy, me->verticesZ[i] };
                        int a;
                        for (a = 0; a < 3; a++)
                        {
                            if (v[a] < lo[a]) lo[a] = v[a];
                            if (v[a] > hi[a]) hi[a] = v[a];
                        }
                    }
                }
                fprintf(f, "hm %d %.8s meshes %d verts %d box %d..%d %d..%d %d..%d\n", k, mh->name.str, mh->meshCount, n,
                        lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            }
        }
    }
    {
        /* the inventory's texture files: where each lands in VRAM */
        static const struct { s32 idx; const char* name; } tims[] = {
            { FILE_ITEM_TIM00_TIM, "TIM00" }, { FILE_ITEM_TIM07_TIM, "TIM07" },
            { FILE_ITEM_TIM01_TIM, "TIM01" }, { FILE_ITEM_TIM02_TIM, "TIM02" },
        };
        int t;
        for (t = 0; t < (int)(sizeof(tims) / sizeof(tims[0])); t++)
        {
            s32 sz;
            u8* b = VrWeaponHd_ReadFile(tims[t].idx, &sz);
            if (b == NULL) continue;
            {
                const u32* w32  = (const u32*)b;
                const u32  flag = w32[1];
                const u8*  p    = b + 8;
                fprintf(f, "=== tim %s size %d flag %x\n", tims[t].name, (int)sz, flag);
                if (flag & 8)
                {
                    const u16* c = (const u16*)(p + 4);
                    fprintf(f, "clut x %u y %u w %u h %u\n", c[0], c[1], c[2], c[3]);
                    p += *(const u32*)p;
                }
                {
                    const u16* c = (const u16*)(p + 4);
                    fprintf(f, "image x %u y %u w %u h %u\n", c[0], c[1], c[2], c[3]);
                }
            }
            free(b);
        }
    }
    fclose(f);
    VrOvr_Log("[VR-WPNHD] dumped %d weapons to vrweapon_dump.txt\n", VR_WPN_COUNT);
}
