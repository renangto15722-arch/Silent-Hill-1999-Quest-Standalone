/*
=============================================================================
 SH1 PC Port - COMBAT DIFFICULTY (VR): Normal / Hard / Hell on Earth
=============================================================================

 Combat_Difficulty (vr_difficulty, VR_Holsters page), live - to balance the
 stronger VR melee. Stacks on the game's own Easy/Normal/Hard.

                       Normal   Hard   Hell on Earth
   enemy speed          1.0x    1.2x      1.4x
   enemy damage         1.0x    1.5x      2.0x
   hit recovery         1.0x    0.7x      0.4x   (time an enemy takes to recover)
   aggressiveness       game    high      max

 NORMAL touches nothing: every hook returns before changing a value.

 SPEED: a monster's AI tick (its movement, animations, attack wind-ups, its
 waits between actions) runs on g_DeltaTime; during that tick only, the
 frame time it sees is multiplied (npc_main.c, VrDiff_NpcTickBegin/End). The
 enemy moves and acts faster - existing ones at once, new ones as they
 spawn - with the same HP, animations and AI.
 RECOVERY: for VR_DIFF_STAGGER_MS after a hit lands on a monster, its clock
 runs a further 1 / recovery faster, so the hit reaction (stagger, knockdown)
 is over in recovery x its time and it cannot be kept stunned by chaining
 hits. Hell: 1.4 / 0.4 = 3.5x during that window.
 AGGRESSIVENESS: SH1 has no aggression value - every monster's behaviour is
 its own state machine. The faster clock is what makes them more aggressive:
 shorter waits between attacks, earlier attacks, less time to react.
 DAMAGE: what reaches Harry, from any monster attack, is scaled where the
 game takes it off his health (player_control.c, after its own difficulty).

 Only monsters (Air Screamer .. Monster Cybil), only in gameplay: the
 people, the cat and anything in a cutscene keep their timing.
=============================================================================
*/

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "pc_config.h"

#include <SDL.h>

#define VR_DIFF_STAGGER_MS 900u
#define VR_DIFF_SLOTS      16

static const float s_speed[3]    = { 1.0f, 1.2f, 1.4f };
static const float s_damage[3]   = { 1.0f, 1.5f, 2.0f };
static const float s_recovery[3] = { 1.0f, 0.7f, 0.4f };

static Uint32 s_hurtUntil[VR_DIFF_SLOTS];
static q19_12 s_savedDt;
static int    s_scaled;

static int Mode(void)
{
    const int m = g_PcConfig.vrDifficulty;
    return (m < 0 || m > 2) ? 0 : m;
}

static int IsMonster(const s_SubCharacter* c)
{
    return c->model.charaId >= Chara_AirScreamer && c->model.charaId <= Chara_MonsterCybil;
}

/* npc_main.c, right before a character's AI tick */
void VrDiff_NpcTickBegin(void* npcPtr, int slot)
{
    const s_SubCharacter* c = (const s_SubCharacter*)npcPtr;
    const int             m = Mode();
    const Uint32          now = SDL_GetTicks();
    float                 k;

    s_scaled = 0;
    if (m == 0 || slot < 0 || slot >= VR_DIFF_SLOTS || !IsMonster(c) ||
        g_SysWork.sysState != SysState_Gameplay)
        return;

    /* a hit waiting for this tick: its reaction is about to start */
    if (c->damage.amount > Q12(0.0f))
        s_hurtUntil[slot] = now + VR_DIFF_STAGGER_MS;

    k = s_speed[m];
    if ((s32)(s_hurtUntil[slot] - now) > 0)
        k /= s_recovery[m];

    s_savedDt   = g_DeltaTime;
    g_DeltaTime = (q19_12)((float)g_DeltaTime * k + 0.5f);
    s_scaled    = 1;
}

/* npc_main.c, right after it: the frame time back for everyone else */
void VrDiff_NpcTickEnd(void)
{
    if (s_scaled)
    {
        g_DeltaTime = s_savedDt;
        s_scaled    = 0;
    }
}

/* player_control.c: the damage about to come off Harry's health */
q19_12 VrDiff_PlayerDamage(q19_12 amount)
{
    const int m = Mode();
    if (m == 0 || amount <= Q12(0.0f))
        return amount;
    return (q19_12)((float)amount * s_damage[m] + 0.5f);
}
