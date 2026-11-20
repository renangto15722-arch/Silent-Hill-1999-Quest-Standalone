/*
=============================================================================
 SH1 PC Port - TRAINING DUMMY (VR melee testing)
=============================================================================

 Training_Dummy (vr_training_dummy, VR_Holsters page). While it is on, the
 first live enemy within VR_DUMMY_CATCH_M of Harry is CAUGHT and PLACED
 VR_DUMMY_AHEAD_M in front of him with its position VR_DUMMY_UP_M above his
 floor - wherever it was, a flying Air Screamer high in the air included.

   placing: for VR_DUMMY_PLACE_TICKS ticks its AI still runs, but it is held
            on that spot before and after each tick (VrDummy_Pin, npc_main.c),
            so the pose the AI builds is built there;
   frozen:  then the console FREEZE (no AI tick: it neither moves nor
            attacks, the bones keep that pose).

 It cannot die: every hit is read off its pending damage and cleared before
 anything consumes it, so its health never drops. Each hit shows on screen
 (the last one, the total, the count) and goes to the log as [VR-DUMMY].

 Nothing is spawned and no story flag is touched: in the cafe (map0_s01) the
 dummy is the Air Screamer once it has broken in. Turning the option off lets
 it go (it fights normally again).
=============================================================================
*/

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "bodyprog/text/text_draw.h"
#include "pc_config.h"
#include "pc_pick.h"

#include <math.h>
#include <stdio.h>
#include <SDL.h>

extern float VrIk_UnitsPerMeter(void);
extern void  VrOvr_Log(const char* fmt, ...);

#define VR_DUMMY_CATCH_M      30.0f   /* m from Harry (flat): the whole room */
#define VR_DUMMY_AHEAD_M      1.6f    /* placed this far in front of Harry */
#define VR_DUMMY_UP_M         1.1f    /* its position this high above his floor */
#define VR_DUMMY_PLACE_TICKS  20
#define VR_DUMMY_SHOW_MS      8000u   /* the readout stays this long after a hit */

static int     s_slot = -1;
static s32     s_chara;
static int     s_placeTicks;            /* AI ticks left before it freezes */
static VECTOR3 s_spot;                  /* world Q12 */
static q3_12   s_rotY;
static s32     s_last, s_total, s_hits;
static Uint32  s_lastMs;

static void Release(void)
{
    if (s_slot >= 0)
    {
        Pc_Pick_SetFrozen(s_slot, 0);
        VrOvr_Log("[VR-DUMMY] released chara %d (slot %d)\n", (int)s_chara, s_slot);
    }
    s_slot = -1;
}

static int Live(const s_SubCharacter* c)
{
    return c->model.charaId != Chara_None && c->model.charaId != Chara_Harry && c->health > Q12(0.0f);
}

static void Hold(s_SubCharacter* c)
{
    c->position     = s_spot;
    c->rotation.vy  = s_rotY;
    c->moveSpeed    = Q12(0.0f);
    c->damage.amount      = Q12(0.0f);
    c->damage.position.vx = 0;
    c->damage.position.vy = 0;
    c->damage.position.vz = 0;
}

/* npc_main.c, around each NPC's AI tick: the dummy being placed stays on its
 * spot, facing Harry. */
void VrDummy_Pin(void* npc, int slot)
{
    if (slot != s_slot || s_slot < 0 || s_placeTicks <= 0)
        return;
    Hold((s_SubCharacter*)npc);
}

/* Game thread, every frame of gameplay (pc_combat.c Pc_ExtraActionsUpdate),
 * after the VR melee. */
void VrDummy_Update(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    s_SubCharacter*       t;
    char                  line[64];

    if (!g_PcConfig.vrTrainingDummy || g_GameWork.gameState != GameState_InGame)
    {
        Release();
        return;
    }
    {
        static int s_onLogged;
        if (!s_onLogged)
        {
            s_onLogged = 1;
            VrOvr_Log("[VR-DUMMY] training dummy on (map %d room %d)\n", (int)g_SavegamePtr->mapIdx,
                      (int)g_SavegamePtr->mapRoomIdx);
        }
    }
    if (s_slot >= 0 && (g_SysWork.npcs[s_slot].model.charaId != s_chara || !Live(&g_SysWork.npcs[s_slot])))
        s_slot = -1;   /* gone (room change, despawn) */

    /* catch one and choose its spot */
    if (s_slot < 0)
    {
        const float upm = VrIk_UnitsPerMeter() * 16.0f;   /* world Q12 per m */
        int         i;
        if (g_SysWork.sysState != SysState_Gameplay)
            return;
        for (i = 0; i < (int)(sizeof(g_SysWork.npcs) / sizeof(g_SysWork.npcs[0])); i++)
        {
            const s_SubCharacter* c  = &g_SysWork.npcs[i];
            const float           dx = (float)(c->position.vx - pl->position.vx) / upm;
            const float           dz = (float)(c->position.vz - pl->position.vz) / upm;
            if (!Live(c) || !c->collision.state)
                continue;
            if (dx * dx + dz * dz > VR_DUMMY_CATCH_M * VR_DUMMY_CATCH_M)
                continue;
            {
                const float a = (float)pl->rotation.vy * (6.2831853f / 4096.0f);
                s_spot.vx = pl->position.vx + (s32)(sinf(a) * VR_DUMMY_AHEAD_M * upm);
                s_spot.vz = pl->position.vz + (s32)(cosf(a) * VR_DUMMY_AHEAD_M * upm);
                s_spot.vy = pl->position.vy - (s32)(VR_DUMMY_UP_M * upm);   /* PSX y is down */
                s_rotY    = (q3_12)((pl->rotation.vy + Q12_ANGLE(180.0f)) & 0xFFF);
            }
            s_slot       = i;
            s_chara      = c->model.charaId;
            s_placeTicks = VR_DUMMY_PLACE_TICKS;
            s_last = s_total = s_hits = 0;
            s_lastMs = SDL_GetTicks();
            Pc_Pick_SetFrozen(i, 0);
            Hold(&g_SysWork.npcs[i]);
            VrOvr_Log("[VR-DUMMY] caught chara %d (slot %d), was %.1f m away, health %d - placing it %.1f m ahead\n",
                      (int)s_chara, i, (double)sqrtf(dx * dx + dz * dz), (int)(c->health >> 12),
                      (double)VR_DUMMY_AHEAD_M);
            break;
        }
        if (s_slot < 0)
        {
            /* nothing to catch: say what is there, now and then */
            static Uint32 s_listMs;
            if (SDL_GetTicks() - s_listMs > 5000u)
            {
                s_listMs = SDL_GetTicks();
                for (i = 0; i < (int)(sizeof(g_SysWork.npcs) / sizeof(g_SysWork.npcs[0])); i++)
                {
                    const s_SubCharacter* c = &g_SysWork.npcs[i];
                    if (c->model.charaId == Chara_None)
                        continue;
                    VrOvr_Log("[VR-DUMMY] none caught: slot %d chara %d health %d coll %d at %.1f m\n", i,
                              (int)c->model.charaId, (int)(c->health >> 12), (int)c->collision.state,
                              (double)(sqrtf((float)(c->position.vx - pl->position.vx) * (float)(c->position.vx - pl->position.vx) +
                                             (float)(c->position.vz - pl->position.vz) * (float)(c->position.vz - pl->position.vz)) / upm));
                }
            }
            return;
        }
    }
    t = &g_SysWork.npcs[s_slot];

    /* placing: count the ticks, then freeze it there */
    if (s_placeTicks > 0)
    {
        Hold(t);
        if (--s_placeTicks == 0)
        {
            Pc_Pick_SetFrozen(s_slot, 1);
            VrOvr_Log("[VR-DUMMY] frozen in place\n");
        }
    }

    /* every hit lands in its pending damage: count it and clear it, so it
     * neither dies nor reacts */
    if (t->damage.amount > Q12(0.0f))
    {
        s_last = t->damage.amount;
        s_total += s_last;
        s_hits++;
        s_lastMs = SDL_GetTicks();
        VrOvr_Log("[VR-DUMMY] hit %d: damage %.1f (weapon %d), total %.1f\n", (int)s_hits,
                  (double)s_last / 4096.0, (int)(u8)g_SavegamePtr->equippedWeapon, (double)s_total / 4096.0);
    }
    t->damage.amount      = Q12(0.0f);
    t->damage.position.vx = 0;
    t->damage.position.vy = 0;
    t->damage.position.vz = 0;

    /* the readout (game strings: '_' is a space) */
    if (g_SysWork.sysState == SysState_Gameplay && SDL_GetTicks() - s_lastMs < VR_DUMMY_SHOW_MS)
    {
        Gfx_StringSetColor(StringColorId_Gold);
        Gfx_StringSetPosition(96, -80);
        Gfx_StringDraw("TRAINING_DUMMY", 20);
        Gfx_StringSetColor(StringColorId_White);
        if (s_hits > 0)
        {
            snprintf(line, sizeof(line), "LAST_HIT_%d.%d", (int)(s_last >> 12), (int)(((s_last & 0xFFF) * 10) >> 12));
            Gfx_StringSetPosition(96, -62);
            Gfx_StringDraw(line, 40);
            snprintf(line, sizeof(line), "TOTAL_%d__HITS_%d", (int)(s_total >> 12), (int)s_hits);
            Gfx_StringSetPosition(96, -44);
            Gfx_StringDraw(line, 40);
        }
        else
        {
            Gfx_StringSetPosition(96, -62);
            Gfx_StringDraw("HIT_IT", 20);
        }
    }
}
