/*
=============================================================================
 SH1 PC Port - VR HEAD TRACKING v2.6  (config-driven "VR options")
=============================================================================

 Drop into:  pc_port/src/vr_tracking.c
 Requires the small pc_config patch - see VR_OPTIONS_PATCH.md
 Hook unchanged: VrApplyHeadOffset() in vwSetViewInfoDirectMatrix.

 Bridge file: C:\SH1VR\vr_tracking_data.txt (override: SH1VR_FILE env var)

 CONFIG KEYS (config.cfg, live-adjustable, auto-saved):
   vr_enabled, vr_positional, vr_roomscale,
   vr_scale, vr_roomscale_scale, vr_track_cutscenes

 HOTKEYS (game window focused):
   NUMPAD 1 - toggle VR tracking            NUMPAD 2 - recenter
   NUMPAD 3 - toggle roomscale              NUMPAD 4 - toggle 3DOF mode
   NUMPAD 7/8 - positional scale -/+        NUMPAD 5/6 - roomscale scale -/+
=============================================================================
*/

#include "game.h"          /* g_SysWork */
#include "bodyprog/screen/cutscene_border.h" /* CutsceneBorderState_* enum */
#include "control_style.h" /* g_ControlStyle */
#include "pc_config.h"     /* g_PcConfig - the VR options live here */
#include "bodyprog/math/fixed_point.h"
#include "libgte.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

extern void PcConfig_SaveKeyValue(const char* cfgKey, const char* cfgValue);

/* ---- fixed tuning (rarely touched - the live options are in config) ---- */

/* One real meter in game units (Q12): the game's own scale - trigger boxes
 * are "meter steps" shifted by Q12_SHIFT, Harry's head sits at Q12(1.55).
 * Was 3300, which made the eye separation and head movement ~19% short:
 * 64 mm of IPD became 52 mm in the world, so everything read ~24% too big.
 * vr_scale multiplies this (1.0 = true scale). */
#define VR_UNITS_PER_METER 4096
#define VR_READ_INTERVAL_MS 8
#define VR_MAX_OFFSET_METERS 1.5f
#define VR_ROOMSCALE_MAX_STEP 0.25f
#define VR_SCALE_STEP 0.25f       /* hotkey increment for the scale values */

/* Position axis mapping - CONFIRMED by in-game testing, don't touch! */
#define VR_SIGN_X (-1)
#define VR_SIGN_Y (-1)
#define VR_SIGN_Z (-1)
#define VR_SWAP_XZ 1
#define VR_MAT_TRANSPOSE 0

/* Rotation axis signs - confirmed in testing */
#define VR_SIGN_PITCH (+1)
#define VR_SIGN_YAW   (-1)
#define VR_SIGN_ROLL  (+1)

/* MODEL-B ROLL: 1 = the game camera never rolls (pitch/yaw only); head
 * roll is instead applied to the FULL-mode quad, which counter-rotates
 * in your view (vr_full.c). Kills BOTH roll-warp sources: the FILL
 * stretch-field sweep AND the port's widescreen horScale not commuting
 * with rolled renders. 0 = classic camera roll. */
#define VR_ROLL_TO_QUAD 1

/* Current head roll (radians, signed) for the quad counter-rotation. */
static float s_quadRollRad = 0.0f;
float VrTracking_GetHeadRollRad(void)
{
    return s_quadRollRad;
}

/* Applied camera pitch/yaw THIS frame (radians, post-sign - the values
 * actually baked into the game render). vr_full.c uses these to
 * rotation-compensate the STALE eye's quad in alternating-eye stereo:
 * the eye captured last frame gets shifted by the rotation delta since
 * its capture, re-aligning both eyes angularly (the ghosting fix). */
static float s_appliedPitch = 0.0f;
static float s_appliedYaw   = 0.0f;
static s32 s_headOffsetQ8[3];   /* see VrTracking_GetHeadOffset */

void VrTracking_GetAppliedAngles(float* pitch, float* yaw)
{
    *pitch = s_appliedPitch;
    *yaw   = s_appliedYaw;
}

/* Roomscale signs (Classic confirmed; FPS eye rides root - same sign) */
#define VR_ROOMSCALE_SIGN     (+1)
#define VR_ROOMSCALE_SIGN_FPS (+1)

/* PLAYSPACE MAPPING - LIVE CALIBRATION MODE (v2.8)
 * Root cause of the recurring axis crossings: the SteamVR playspace is
 * calibrated rotated vs the actual facing direction, and the old SWAP_XZ
 * absorbed that as a reflection, which fights yaw compensation.
 * v2.8 uses a pure ROTATION mapping instead, with 8 possible sign
 * combinations. Press NUMPAD . (decimal) in-game to cycle combos live;
 * lean around after each press until forward=forward and right=right at
 * ANY recenter facing. Note the combo number from the console, set it
 * below, done forever. */
#define VR_MAPPING_COMBO_DEFAULT 4  /* CALIBRATED: rot+ x+ z- (native OpenVR poses, HMD-verified) */

/* ---- state ------------------------------------------------------------- */

static int    s_haveOrigin  = 0;
static float  s_origin[3]   = {0, 0, 0};
static float  s_originRot[3]= {0, 0, 0};

/* Head yaw handed over to the body so far (radians, view-yaw space - see
 * VrTracking_TakeBodyTurn). Subtracted from the camera's head yaw while the
 * body's base yaw adds the same amount, so the view never moves. */
static float  s_bodyYawXfer = 0.0f;

/* The last view matrix VrApplyHeadOffset produced (VrTracking_GetCameraMatrix) */
static MATRIX s_lastCamMat;
static VECTOR s_lastCamPlayerPos;   /* Harry's root when that view was built */
static int    s_lastCamPlayerYaw;
static int    s_lastCamValid = 0;
static float  s_headPos[3]  = {0, 0, 0};
static float  s_headRot[3]  = {0, 0, 0};
static float  s_prevPos[3]  = {0, 0, 0};
static int    s_havePrev    = 0;
static int    s_newSample   = 0;
static double s_lastStamp   = 0.0;

/* Playspace mapping combo: bit0 = rotation sign, bit1 = X sign, bit2 = Z sign
 * (cycled live with NUMPAD . - see VR_MAPPING_COMBO_DEFAULT) */
static int    s_mappingCombo = VR_MAPPING_COMBO_DEFAULT;

static const char* VrFilePath(void)
{
    static char path[512];
    static int  init = 0;

    if (!init)
    {
        const char* env = getenv("SH1VR_FILE");
        if (env != NULL && env[0] != '\0')
        {
            strncpy(path, env, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
        else
        {
            strcpy(path, "C:\\SH1VR\\vr_tracking_data.txt");
        }
        init = 1;
    }
    return path;
}

static void VrRecenter(void)
{
    s_origin[0] = s_headPos[0];
    s_origin[1] = s_headPos[1];
    s_origin[2] = s_headPos[2];
    /* Yaw only. The headset's space is already level (gravity-aligned);
     * taking pitch/roll here baked whatever tilt the head had at recenter -
     * including the automatic one at startup - into the horizon for the
     * whole session: the world sat tilted, worst looking down. */
    s_originRot[0] = 0.0f;
    s_originRot[1] = s_headRot[1];
    s_originRot[2] = 0.0f;
    s_bodyYawXfer  = 0.0f;   /* head straight ahead of the body again */
    s_haveOrigin = 1;
    s_prevPos[0] = s_headPos[0];
    s_prevPos[1] = s_headPos[1];
    s_prevPos[2] = s_headPos[2];
    printf("[VR] recentered\n");
}

/* ---- live options: hotkeys adjust g_PcConfig + persist to config.cfg --- */

static void VrSaveInt(const char* key, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    PcConfig_SaveKeyValue(key, buf);
}

static void VrSaveFloat(const char* key, float v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f", (double)v);
    PcConfig_SaveKeyValue(key, buf);
}

static float VrAdjustScale(float v, int dir)
{
    v += dir * VR_SCALE_STEP;
    if (v < 0.25f) v = 0.25f;
    if (v > 4.0f)  v = 4.0f;
    return v;
}

static void VrPollHotkeys(void)
{
    static int p1=0,p2=0,p3=0,p4=0,p5=0,p6=0,p7=0,p8=0;

    const Uint8* ks = SDL_GetKeyboardState(NULL);
    int n1 = ks[SDL_SCANCODE_KP_1] != 0;
    int n2 = ks[SDL_SCANCODE_KP_2] != 0;
    int n3 = ks[SDL_SCANCODE_KP_3] != 0;
    int n4 = ks[SDL_SCANCODE_KP_4] != 0;
    int n5 = ks[SDL_SCANCODE_KP_5] != 0;
    int n6 = ks[SDL_SCANCODE_KP_6] != 0;
    int n7 = ks[SDL_SCANCODE_KP_7] != 0;
    int n8 = ks[SDL_SCANCODE_KP_8] != 0;

    if (n1 && !p1)
    {
        g_PcConfig.vrEnabled = !g_PcConfig.vrEnabled;
        VrSaveInt("vr_enabled", g_PcConfig.vrEnabled);
        printf("[VR] tracking %s\n", g_PcConfig.vrEnabled ? "ON" : "OFF");
    }
    if (n2 && !p2) { VrRecenter(); }
    if (n3 && !p3)
    {
        g_PcConfig.vrRoomscale = !g_PcConfig.vrRoomscale;
        VrSaveInt("vr_roomscale", g_PcConfig.vrRoomscale);
        printf("[VR] roomscale %s\n", g_PcConfig.vrRoomscale ? "ON" : "OFF");
    }
    if (n4 && !p4)
    {
        g_PcConfig.vrPositional = !g_PcConfig.vrPositional;
        VrSaveInt("vr_positional", g_PcConfig.vrPositional);
        printf("[VR] %s mode\n", g_PcConfig.vrPositional ? "6DOF" : "3DOF (rotation only)");
    }
    if (n7 && !p7)
    {
        g_PcConfig.vrScale = VrAdjustScale(g_PcConfig.vrScale, -1);
        VrSaveFloat("vr_scale", g_PcConfig.vrScale);
        printf("[VR] world scale %.2f\n", (double)g_PcConfig.vrScale);
    }
    if (n8 && !p8)
    {
        g_PcConfig.vrScale = VrAdjustScale(g_PcConfig.vrScale, +1);
        VrSaveFloat("vr_scale", g_PcConfig.vrScale);
        printf("[VR] world scale %.2f\n", (double)g_PcConfig.vrScale);
    }
    if (n5 && !p5)
    {
        g_PcConfig.vrRoomscaleScale = VrAdjustScale(g_PcConfig.vrRoomscaleScale, -1);
        VrSaveFloat("vr_roomscale_scale", g_PcConfig.vrRoomscaleScale);
        printf("[VR] roomscale scale %.2f\n", (double)g_PcConfig.vrRoomscaleScale);
    }
    if (n6 && !p6)
    {
        g_PcConfig.vrRoomscaleScale = VrAdjustScale(g_PcConfig.vrRoomscaleScale, +1);
        VrSaveFloat("vr_roomscale_scale", g_PcConfig.vrRoomscaleScale);
        printf("[VR] roomscale scale %.2f\n", (double)g_PcConfig.vrRoomscaleScale);
    }

    /* NUMPAD + / - : stereo depth (half-IPD) up/down */
    {
        static int pPlus = 0, pMinus = 0;
        int nPlus  = ks[SDL_SCANCODE_KP_PLUS]  != 0;
        int nMinus = ks[SDL_SCANCODE_KP_MINUS] != 0;
        if (nPlus && !pPlus)
        {
            g_PcConfig.vrStereoDepth += 0.005f;
            if (g_PcConfig.vrStereoDepth > 0.15f) g_PcConfig.vrStereoDepth = 0.15f;
            VrSaveFloat("vr_stereo_depth", g_PcConfig.vrStereoDepth);
            printf("[VR] stereo depth %.3fm\n", (double)g_PcConfig.vrStereoDepth);
        }
        if (nMinus && !pMinus)
        {
            g_PcConfig.vrStereoDepth -= 0.005f;
            if (g_PcConfig.vrStereoDepth < 0.005f) g_PcConfig.vrStereoDepth = 0.005f;
            VrSaveFloat("vr_stereo_depth", g_PcConfig.vrStereoDepth);
            printf("[VR] stereo depth %.3fm\n", (double)g_PcConfig.vrStereoDepth);
        }
        pPlus = nPlus; pMinus = nMinus;
    }

    /* NUMPAD . : cycle playspace mapping combos (calibration) */
    {
        static int pDot = 0;
        int nDot = ks[SDL_SCANCODE_KP_PERIOD] != 0;
        if (nDot && !pDot)
        {
            s_mappingCombo = (s_mappingCombo + 1) & 7;
            printf("[VR] mapping combo %d (rot%c x%c z%c) - lean and test!\n",
                   s_mappingCombo,
                   (s_mappingCombo & 1) ? '-' : '+',
                   (s_mappingCombo & 2) ? '-' : '+',
                   (s_mappingCombo & 4) ? '-' : '+');
        }
        pDot = nDot;
    }

    p1=n1; p2=n2; p3=n3; p4=n4; p5=n5; p6=n6; p7=n7; p8=n8;
}

/* Alternating-eye stereo (pc_port/src/vr_openvr.c): which eye this frame
 * renders for. -1 left, +1 right, 0 = stereo off. */
extern int VrOvr_StereoEyeSign(void);

/* Stereo depth (half-IPD) now lives in g_PcConfig.vrStereoDepth -
 * adjustable via the in-game VR menu and NUMPAD +/- hotkeys. */

/* Convergence for the SBS screen (queried by vr_openvr.c - kept here so
 * vr_openvr.c stays free of decomp/config headers). */
int VrTracking_GetConvergencePx(void)
{
    return (int)g_PcConfig.vrStereoConv;
}

/* In-process OpenVR poses (pc_port/src/vr_openvr.c). Returns 1 when the
 * native path is active and has a valid pose - then the file bridge is
 * skipped entirely (no FreePIE, no Python, no file, minimal latency). */
extern int VrOvr_GetHeadPose(float pos[3], float rot[3]);

static void VrReadBridge(void)
{
    static Uint32 lastRead = 0;
    Uint32 now = SDL_GetTicks();

    s_newSample = 0;

    /* The system recenter (Quest button held): the space changed under us.
     * A few frames later - the poses are then in the new space - take the
     * origin again and let the body height be measured again, or the head
     * height kept from the old space put the view near the floor (or in the
     * air) for the rest of the session. */
    {
        extern int  VrXr_TakeSpaceChanged(void);
        extern void VrIk_ResetBodyMeasure(void);
        extern void VrOvr_Log(const char* fmt, ...);
        static int  s_recenterIn = 0;
        if (VrXr_TakeSpaceChanged())
            s_recenterIn = 10;
        if (s_recenterIn > 0 && --s_recenterIn == 0)
        {
            s_haveOrigin = 0;          /* the next head sample recenters */
            VrIk_ResetBodyMeasure();
            VrOvr_Log("[VR] system recenter: origin and body height taken again\n");
        }
    }

    /* Try native poses first - every frame: the pose was polled for exactly
     * this frame (VrOvr_PollPoseForNextFrame). The read interval below used to
     * gate this too, on a whole-millisecond clock, so a frame starting under
     * 8 ms after the last one (normal jitter at 90 Hz) kept the previous pose
     * while the compositor got the new one: a jolt on head turns. */
    {
        float p[3], r[3];
        if (VrOvr_GetHeadPose(p, r))
        {
            s_prevPos[0] = s_headPos[0];
            s_prevPos[1] = s_headPos[1];
            s_prevPos[2] = s_headPos[2];
            if (s_haveOrigin) { s_havePrev = 1; }

            s_headPos[0] = p[0];
            s_headPos[1] = p[1];
            s_headPos[2] = p[2];
            s_headRot[0] = r[0];
            s_headRot[1] = r[1];
            s_headRot[2] = r[2];
            s_newSample  = 1;

            if (!s_haveOrigin)
            {
                VrRecenter();
            }
            return;
        }
    }

    /* the file bridge is throttled: no fopen every frame */
    if (now - lastRead < VR_READ_INTERVAL_MS)
    {
        return;
    }
    lastRead = now;

    FILE* f = fopen(VrFilePath(), "rb");
    if (f == NULL)
    {
        return;
    }

    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    float  x, y, z, pitch, yaw, roll;
    double stamp;
    if (sscanf(buf, "%f,%f,%f,%f,%f,%f,%lf",
               &x, &y, &z, &pitch, &yaw, &roll, &stamp) != 7)
    {
        return;
    }

    if (stamp == s_lastStamp)
    {
        return;
    }
    s_lastStamp = stamp;

    s_prevPos[0] = s_headPos[0];
    s_prevPos[1] = s_headPos[1];
    s_prevPos[2] = s_headPos[2];
    if (s_haveOrigin) { s_havePrev = 1; }

    s_headPos[0] = x;
    s_headPos[1] = y;
    s_headPos[2] = z;
    s_headRot[0] = pitch;
    s_headRot[1] = yaw;
    s_headRot[2] = roll;
    s_newSample  = 1;

    if (!s_haveOrigin)
    {
        VrRecenter();
    }
}

/* "An authored scene is on screen" - the game port's own predicate
 * (Pc_ScriptOwnsShot, game_main.c): a letterboxed cinematic, OR the script
 * driving both the camera and Harry, minus room-transition fades. Keying only
 * on the cutscene flag / letterbox missed the scripted scenes that have
 * neither - "Cheryl? Is that Cheryl?" ran as gameplay, so its dialogue was
 * taken for a message popup and dropped to the 2D screen whatever
 * Cutscene_View said. Debounced 100 ms so a one-frame blip of the camera/
 * control flags cannot flip view modes. */
static int VrCutsceneActive(void)
{
    extern int Pc_ScriptOwnsShot(void);
    static int    s_stable = 0, s_pending = 0;
    static Uint32 s_since  = 0;
    /* In-game only: on the boot logos and title screen the camera and
     * control flags read like a scripted shot, and the Full VR cutscene
     * view then blew those flat screens up to the whole field of view. */
    const int     raw = g_GameWork.gameState == GameState_InGame && Pc_ScriptOwnsShot();
    const Uint32  now = SDL_GetTicks();

    if (raw != s_pending)
    {
        s_pending = raw;
        s_since   = now;
    }
    if (s_pending != s_stable && now - s_since >= 100)
        s_stable = s_pending;
    return s_stable;
}

/* ---- UI context for auto view switching (queried by vr_openvr.c) -------
 * 0 = gameplay (3D world rendering - FULL immersion makes sense)
 * 1 = flat UI  (menus/inventory/map/documents/save/pause/game-over - a
 *     floating screen reads far better than a head-locked FULL window)
 *
 * FMV is deliberately NOT reported as UI: the FMV player bypasses the
 * main frame hook entirely and calls VrOvr_EndFrameFmv - that entry
 * point itself is the FMV signal, and FULL mode composes FMVs fine.
 *
 * Two state layers (both verified against include/game.h):
 *   g_GameWork.gameState  - top-level (MainMenu=7, InventoryScreen=14,
 *                           PaperMapScreen=15, SaveScreen=16, ...)
 *   g_SysWork.sysState    - sub-states of GameState_InGame (StatusMenu=2,
 *                           MapScreen=3, ReadMessage=7, SaveMenu, Paused) */
int VrTracking_GetUiContext(void)
{
    if (g_GameWork.gameState != GameState_InGame)
    {
        /* Everything outside InGame is a flat screenful: main menu,
         * load/save/option screens, inventory, paper map, logos.
         * (Movie GameStates route through the FMV player and never hit
         * the main-loop hook, so their value here is moot.) */
        return 1;
    }

    switch (g_SysWork.sysState)
    {
        case SysState_Gameplay:
        case SysState_EventCallback:
        case SysState_EventSetFlag:
        case SysState_EventPlaySound:
        case SysState_LoadOverlay:   /* brief area loads: don't thrash modes */
        case SysState_LoadRoom:
        case SysState_Fmv:           /* FMV path handles itself (see above) */
        case SysState_ReadMessage:   /* examining things: stays in the world */
        {
            /* Item pickups, examine text and other in-world popups stay
             * in the 3D world (Full VR): their text and pictures go to the
             * head-locked 2D interface panel over it (vr_full.c). Only the
             * screens that replace the world (inventory, map, pause,
             * save, options, game over) drop to the floating screen.
             * So does a full-screen 2D image outside plain gameplay - a
             * document, a close-up puzzle, a picture an event shows
             * (Screen_BackgroundImgDraw & co. raise g_Pc2dBackgroundActive):
             * treated as world it was blown up over the whole field of view. */
            extern s32 g_Pc2dBackgroundActive;
            extern int g_PsxPresentLastFrame;
            /* An event that swaps one full-screen image for another (the
             * piano: PIANO1 -> fade -> load PIANO2) draws no image while the
             * next one loads. Those few frames read as world: the frozen
             * world was re-captured from the black transition frame (piano
             * all black in Full VR), or the view fell to the plain 2D
             * screen, depending on the load time. Hold the screen context
             * through such a gap; a scripted 3D scene that follows starts
             * behind the event's own fade from black. */
            static Uint32 s_bg2dSeenAt = 0;
            if (g_SysWork.sysState != SysState_Gameplay && g_Pc2dBackgroundActive > 0)
            {
                s_bg2dSeenAt = SDL_GetTicks();
                return 1;
            }
            if (g_SysWork.sysState == SysState_Gameplay)
                s_bg2dSeenAt = 0;
            else if (s_bg2dSeenAt != 0 && SDL_GetTicks() - s_bg2dSeenAt < 1000)
                return 1;
            /* Scripted paper-map scenes (a new objective: the map zooms in,
             * arrow, "Cheryl" - map2_s00 / map1_s06 via Event_PaperMapCmd)
             * draw the map with the tile renderer (func_800692A4) instead of
             * Screen_BackgroundImgDraw, so the flag above stays 0 and the
             * map was taken for the world and blown up over the whole view.
             * Those events raise g_PcMapScreenActive while the map is up. */
            {
                extern int g_PcMapScreenActive;
                if (g_SysWork.sysState != SysState_Gameplay && g_PcMapScreenActive)
                    return 1;
            }
            /* An event that froze the world - the take screen of a pickup
             * (the item model turning, the paper map) - shows the game's own
             * screen on the 2D panel over the frozen stereo world. */
            {
                extern int Pc_VrPickupHeld(void);
                if (Pc_VrPickupHeld() ||
                    (g_SysWork.sysState == SysState_EventCallback && g_PsxPresentLastFrame))
                    return 1;
            }
            return 0;
        }
        default:
            /* OptionsMenu, StatusMenu, MapScreen, SaveMenu0/1, GameOver,
             * GamePaused */
            return 1;
    }
}

/* UI context 1 that belongs to a game in progress (inventory, paper map,
 * save, options, pause, game over...), as opposed to the boot logos, title
 * and main menu. In Full VR these stay in the world: a 2D screen in front of
 * the frozen last view (vr_openvr.c / vr_full.c). "In progress" = gameplay
 * was reached since the last title-side screen. */
int VrTracking_UiInWorld(void)
{
    static int s_gameLive = 0;

    switch (g_GameWork.gameState)
    {
        case GameState_InGame:
            s_gameLive = 1;
            return VrTracking_GetUiContext() != 0;

        case GameState_InventoryScreen:
        case GameState_PaperMapScreen:
        /* the scripted map scene (a new objective: zoom on the paper map,
         * arrow, "Cheryl") - a map screen like the one the player opens; it
         * was taken for a title-side screen and blown up */
        case GameState_MapEvent:
        case GameState_SaveScreen:
        case GameState_OptionScreen:
        case GameState_LoadStatusScreen:
        case GameState_LoadMapScreen:
            return s_gameLive;

        default:   /* logos, movies, title, main menu, loading a save */
            s_gameLive = 0;
            return 0;
    }
}

static int VrRoomscaleActive(void)
{
    /* Roomscale drives the PLAYER - correct where the eye follows Harry's
     * root without smoothing: Classic + FPS. TPS/OTS lerp their orbit and
     * use the camera-offset path instead. Always paused in cutscenes. */
    return g_PcConfig.vrRoomscale && g_PcConfig.vrPositional &&
           !VrCutsceneActive() &&
           (g_ControlStyle == ControlStyle_Classic ||
            g_ControlStyle == ControlStyle_Fps);
}

static float VrUnitsPerMeter(void)
{
    /* World_Scale (vr_scale): how big the world looks. Perceived size follows
     * real eye separation over the eye separation IN the world, so the world
     * looks k times bigger when a real meter is 1/k of a game meter - for the
     * eyes, head movement and roomscale alike. 1.0 = true scale. */
    return (float)VR_UNITS_PER_METER / (g_PcConfig.vrScale > 0.05f ? g_PcConfig.vrScale : 1.0f);
}

/* ---- PGXP stereo ("the summit") plumbing -------------------------------- */
/* When PsyX renders true per-vertex stereo on the GPU, the CPU-side
 * alternating-eye camera offset must be OFF (it would double the
 * separation AND re-introduce the alternating lag). vr_openvr.c sets
 * this every frame from the active stereo mode. */
static int s_gpuStereo = 0;
void VrTracking_SetGpuStereo(int on)
{
    s_gpuStereo = on;
}

/* Half-IPD in GTE WORLD units - queried by vr_openvr.c to feed
 * PsyX_Stereo_SetParams. Rides the same vrStereoDepth config key and
 * NUMPAD +/- hotkeys as before.
 * UNIT FIX (calibrated from SxS video: 4-12% width disparity = ~16x
 * oversized): the old CPU path computed ex at Q12 scale (the "264") but
 * applied it via Q12_TO_Q8 (>>4) to camMat->t - the EFFECTIVE offset in
 * the world/SZ3 space the shader divides by was ex/16 all along. Export
 * the effective value. */
float VrTracking_GetEyeSepUnits(void)
{
    return (g_PcConfig.vrStereoDepth * VrUnitsPerMeter()) / 16.0f;
}

/* ---- FROZEN WORLD view (PsyX_render.cpp "FROZEN WORLD") -------------------
 * The recorded world frame was seen from the head pose stored here at its
 * end; while a menu is up it is drawn from the CURRENT pose. The camera
 * yaws and pitches with the head (the roll is vr_full.c's, on the composed
 * image, as in live play), and moves with it at true scale (World_Scale).
 * Axes: tracking/head = OpenVR (x right, y up, z back); the game's view =
 * PSX camera (x right, y down, z ahead), i.e. view = F * head, F = diag(1,-1,-1). */
static float s_frzHeadPos[3], s_frzHeadRot[3];
static int   s_frzHeadOk;

void VrTracking_FrozenCapture(void)
{
    extern int VrOvr_GetHeadPose(float pos[3], float rot[3]);
    s_frzHeadOk = VrOvr_GetHeadPose(s_frzHeadPos, s_frzHeadRot);
}

/* tracking vector -> head yaw/pitch frame (vr_ik.c ToHead) and back */
static void FrzToHead(const float r[3], const float d[3], float h[3])
{
    const float cy = cosf(r[1]), sy = sinf(r[1]), cp = cosf(r[0]), sp = sinf(r[0]);
    const float x = cy * d[0] - sy * d[2];
    const float z = sy * d[0] + cy * d[2];
    h[0] = x;
    h[1] = cp * d[1] + sp * z;
    h[2] = -sp * d[1] + cp * z;
}

static void FrzFromHead(const float r[3], const float h[3], float d[3])
{
    const float cy = cosf(r[1]), sy = sinf(r[1]), cp = cosf(r[0]), sp = sinf(r[0]);
    const float z1 = sp * h[1] + cp * h[2];
    d[1] = cp * h[1] - sp * h[2];
    d[0] = cy * h[0] + sy * z1;
    d[2] = -sy * h[0] + cy * z1;
}

/* Rcm (column-major) and T take a captured view-space point v to the current
 * view: v' = R v + T. 0 = no pose (draw it as captured). */
int VrTracking_FrozenView(float Rcm[9], float T[3])
{
    extern int   VrOvr_GetHeadPose(float pos[3], float rot[3]);
    extern float VrIk_UnitsPerMeter(void);
    float hp[3], hr[3], d[3], h[3], w[3];
    int   i, k;

    if (!s_frzHeadOk || !VrOvr_GetHeadPose(hp, hr))
        return 0;
    for (i = 0; i < 3; i++)
    {
        /* column i: the captured view axis e_i, seen from the current view */
        const float e[3] = { (float)(i == 0), (float)(i == 1), (float)(i == 2) };
        const float he[3] = { e[0], -e[1], -e[2] };   /* F */
        FrzFromHead(s_frzHeadRot, he, w);
        FrzToHead(hr, w, h);
        Rcm[i * 3 + 0] = h[0];
        Rcm[i * 3 + 1] = -h[1];
        Rcm[i * 3 + 2] = -h[2];
    }
    /* the head moved by d (tracking, m): the world moves the other way */
    for (k = 0; k < 3; k++) d[k] = hp[k] - s_frzHeadPos[k];
    FrzToHead(hr, d, h);
    {
        const float s = VrIk_UnitsPerMeter();
        T[0] = -h[0] * s;
        T[1] =  h[1] * s;
        T[2] =  h[2] * s;
    }
    return 1;
}

/* ---- FULL mode projection: the world fills the eye ----------------------
 * The game fov decides how much world one frame holds. In FULL each eye
 * needs its whole frustum covered, or a black frame shows around the
 * image. This picks the GTE geom-screen distance H at which the world
 * image (the world pass's ortho rect around its projection centre, from
 * the last frame - neither depends on H) reaches every edge of both eyes.
 * vr_full.c shows the image 1:1 (PsyX_GetWorldTangents), so this widens
 * the view; it never magnifies anything.
 * Gameplay and in-engine cutscenes shown in FULL (3D): a cutscene's
 * authored zoom (the border ramp takes H from 224 towards 377) is a
 * narrower fov, which at 1:1 is a black frame around a smaller image - and
 * eyes cannot zoom anyway. Cutscenes on the cinema screen are not FULL, so
 * they keep the authored projection. Message popups and pickups (UI
 * context) always keep it too.
 *
 * Head roll: the game camera never rolls - vr_full.c counter-rotates the
 * image by the head roll instead - so the eye frustum turns inside the
 * image. Covering it at roll up to +-theta takes the frustum's corners
 * rotated by +-theta, i.e. x*cos -+ y*sin, which for a ~1.2 x 1.2 eye is
 * ~16% more than the flat need at 10 deg (a fixed 4% covered ~2 deg).
 * The roll budget is config vr_fov_roll_cover; beyond it the corners go
 * black rather than the whole image shrinking for rare head tilts.
 * Returns 0 = leave the projection alone. */
int VrTracking_VrGeomScreen(void)
{
    extern int   VrFull_GetEyeCoverage(float out[4]);
    extern float g_PcWorldOrthoRect[4];
    extern float g_PcWorldProj[3];
    float eye[4], need[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, units[4], h = 512.0f;
    float th, c, s;
    int   i, k, r;

    if (!s_gpuStereo || VrTracking_GetUiContext() != 0) return 0;
    if (g_PcWorldProj[0] <= 0.0f || !VrFull_GetEyeCoverage(eye)) return 0;

    /* extents (left, right, bottom, top; up +) the rolled eye corners reach */
    th = g_PcConfig.vrFovRollCover * (3.14159265f / 180.0f);
    for (r = -1; r <= 1; r += 2)
    {
        c = cosf(th);
        s = sinf(th) * (float)r;
        for (k = 0; k < 4; k++)
        {
            float x  = (k & 1) ? eye[1] : eye[0];
            float y  = (k & 2) ? eye[3] : eye[2];
            float rx = x * c - y * s;
            float ry = x * s + y * c;
            if (-rx > need[0]) need[0] = -rx;
            if ( rx > need[1]) need[1] =  rx;
            if (-ry > need[2]) need[2] = -ry;
            if ( ry > need[3]) need[3] =  ry;
        }
    }

    /* prim units from the projection centre to each image edge */
    units[0] = g_PcWorldProj[1] - g_PcWorldOrthoRect[0];   /* left   */
    units[1] = g_PcWorldOrthoRect[1] - g_PcWorldProj[1];   /* right  */
    units[2] = g_PcWorldOrthoRect[3] - g_PcWorldProj[2];   /* bottom */
    units[3] = g_PcWorldProj[2] - g_PcWorldOrthoRect[2];   /* top    */

    /* edge tangent = units / H, so H = units / need covers that edge */
    for (i = 0; i < 4; i++)
    {
        if (need[i] > 0.0f && units[i] > 0.0f && units[i] / need[i] < h)
            h = units[i] / need[i];
    }
    if (h < 16.0f) h = 16.0f;
    return (int)h;   /* round down: a hair wider, never short */
}

/* VR HEAD CAMERA: with a headset in use, gameplay runs the first-person
 * eye (control_style.c forces it, without saving) and that eye is held
 * still - no idle head sway, no animated head turn, no melee pull-back
 * (game_main.c). The classic chase camera moved, eased and shook on its
 * own; in a headset any view motion you didn't make with your head reads
 * as the world trembling. Only the headset (and stick turning) move the
 * view now, and the eye sits fixed in Harry's body frame - the anchor a
 * VR body will hang off. Cutscenes keep the game's cameras. */
int VrTracking_HeadCamActive(void)
{
    extern int VrOvr_IsReady(void);
    return g_PcConfig.vrEnabled && VrOvr_IsReady();
}

/* ---- BODY FOLLOWS THE HEAD (fixed deadzone) ----------------------------
 * The view yaw is the body's base yaw (g_TpsCamYaw, which the FPS body snaps
 * to in player_control.c) plus the head yaw relative to it:
 *     rel = wrap(SIGN_YAW * (head - origin) - s_bodyYawXfer)
 * Within +-deadzone nothing happens. Past it, only the EXCESS is handed over,
 * eased toward zero with a frame-rate independent exponential:
 *     excess = rel - clamp(rel, -dz, dz)
 *     turn   = excess * (1 - exp(-RATE * dt))
 * The turn is added to the body's yaw and to s_bodyYawXfer (taken back out
 * of the camera's head yaw), so the view does not move at all - only the
 * body catches up. The easing shrinks as the excess does, so the body slows
 * and stops on its own as the head comes back inside the deadzone or the
 * body reaches it; the excess is continuous across the edge, so there is
 * nothing to oscillate about. wrap() keeps every comparison the shortest way
 * round, so looking behind or spinning 360 deg never flips direction.
 * Stick turning moves body and view together - head-vs-body is untouched,
 * so manual turns are never undone. Cutscenes, menus and pickups (UI
 * context) never turn the body.
 * Returns the turn for this frame as a Q12 angle (4096 = 360 deg). */
#define VR_BODY_TURN_RATE 6.0f   /* 1/s: ~0.17 s time constant on the excess */
/* deg each side; was the Body_Turning_Deadzone option (removed 20/11/2026 at
 * Renan's request), fixed at the value his and the shipped Quest config used */
#define VR_BODY_TURN_DEADZONE_DEG 12.0f

int VrTracking_TakeBodyTurn(void)
{
    static Uint64 s_last = 0;
    const Uint64  now    = SDL_GetPerformanceCounter();
    float dt, rel, dz, excess, turn;
    int   q;

    dt = s_last ? (float)((double)(now - s_last) / (double)SDL_GetPerformanceFrequency()) : 0.0f;
    s_last = now;
    if (dt > 0.1f) dt = 0.1f;   /* a hitch must not swing the body */

    if (!VrTracking_HeadCamActive() || !s_haveOrigin || VrCutsceneActive() ||
        VrTracking_GetUiContext() != 0)
        return 0;

    rel = VR_SIGN_YAW * (s_headRot[1] - s_originRot[1]) - s_bodyYawXfer;
    while (rel >  3.14159265f) rel -= 6.28318531f;
    while (rel < -3.14159265f) rel += 6.28318531f;

    dz = VR_BODY_TURN_DEADZONE_DEG * (3.14159265f / 180.0f);
    if (rel > dz)       excess = rel - dz;
    else if (rel < -dz) excess = rel + dz;
    else                return 0;

    turn = excess * (1.0f - expf(-VR_BODY_TURN_RATE * dt));

    /* hand over whole Q12 steps only, so body and camera stay in lockstep;
     * at least one step while a step of excess remains, or the easing's
     * tail (under one step a frame) would park the body short of the edge */
    q = (int)(turn * (4096.0f / 6.28318531f));
    if (q == 0 && fabsf(excess) * (4096.0f / 6.28318531f) >= 1.0f && dt > 0.0f)
        q = (excess > 0.0f) ? 1 : -1;
    if (q == 0) return 0;
    s_bodyYawXfer += (float)q * (6.28318531f / 4096.0f);
    while (s_bodyYawXfer >  3.14159265f) s_bodyYawXfer -= 6.28318531f;
    while (s_bodyYawXfer < -3.14159265f) s_bodyYawXfer += 6.28318531f;
    return q;
}

/* Walk_Direction = Head (vr_move_dir 1): the stick walks along the headset's
 * yaw, not the body's. Returned as the head's yaw relative to the body (the
 * rel above, Q12 angle), which player_control.c adds to the movement heading
 * offset - the body still follows the head past the fixed deadzone.
 * 0 with Body, or when the head does not drive the view (cutscene, menus). */
int VrTracking_MoveYawOffset(void)
{
    float rel;

    if (!g_PcConfig.vrMoveDir || !VrTracking_HeadCamActive() || !s_haveOrigin || VrCutsceneActive() ||
        VrTracking_GetUiContext() != 0)
        return 0;
    rel = VR_SIGN_YAW * (s_headRot[1] - s_originRot[1]) - s_bodyYawXfer;
    while (rel >  3.14159265f) rel -= 6.28318531f;
    while (rel < -3.14159265f) rel += 6.28318531f;
    return (int)lrintf(rel * (4096.0f / 6.28318531f));
}

/* Snap turn step in degrees while a headset is in use (VR_Tuning menu,
 * config vr_snap_turn); 0 = smooth stick turning. */
int VrTracking_SnapTurnDeg(void)
{
    return VrTracking_HeadCamActive() ? g_PcConfig.vrSnapTurn : 0;
}

/* FULL (3D) cutscenes: the letterbox bars would sit as black bands across
 * the view, so cutscene_border.c skips drawing them. Cinema screen keeps them. */
int VrTracking_HideCutsceneBars(void)
{
    return s_gpuStereo;
}

/* Cinema / floating screen size and distance (meters), for vr_openvr.c. */
float VrTracking_GetScreenWidth(void)    { return g_PcConfig.vrScreenWidth; }
float VrTracking_GetScreenDistance(void) { return g_PcConfig.vrScreenDistance; }

void VrTracking_SetScreenWidth(float m)
{
    char b[16];
    g_PcConfig.vrScreenWidth = m;
    snprintf(b, sizeof(b), "%.2f", (double)m);
    PcConfig_SaveKeyValue("vr_screen_width", b);
}

/* ---- view-mode config plumbing (roadmap items 2-6) ----------------------
 * vr_openvr.c avoids decomp/config headers, so it reads these through
 * plain-int exports. Values match VR_STEREO_*: 0=2D screen, 1=3D screen,
 * 2=Full VR; cutscene mode adds 3 = same as game. */
int VrTracking_GetGameViewMode(void)
{
    int m = g_PcConfig.vrGameViewMode;
    return (m < 0 || m > 2) ? 2 : m;
}

/* Manual KP_ENTER cycles persist through here: the chosen gameplay mode
 * survives restarts (item 6: auto-start into the selected mode). */
void VrTracking_SetGameViewMode(int m)
{
    char b[8];
    if (m < 0 || m > 2) return;
    g_PcConfig.vrGameViewMode = m;
    snprintf(b, sizeof(b), "%d", m);
    PcConfig_SaveKeyValue("vr_game_view_mode", b);
}

int VrTracking_GetCutsceneViewMode(void)
{
    return (g_PcConfig.vrCutsceneViewMode == 0) ? 0 : 2;
}

int VrTracking_GetAutoScreen(void)
{
    return g_PcConfig.vrAutoScreen != 0;
}

/* In-engine cutscene signal for the cutscene-view auto switch. */
int VrTracking_CutsceneActive(void)
{
    return VrCutsceneActive();
}

/* ---- optional camera pitch flatten (VR comfort) -------------------------
 * The game camera pitches with mouse/controller vertical look - in FULL
 * that pitches the WHOLE WORLD while your head supplies its own pitch on
 * top (double-pitch: the scene shifts under you). When enabled, the view
 * rotation is rebuilt YAW-ONLY with the camera POSITION preserved
 * (translation re-derived), so vertical look comes exclusively from your
 * head. Gated: config key + FULL only + never in cutscenes (cinematic
 * pitch is intentional and separately tracked).
 * CONVENTION NOTE: forward is read from view-rotation ROW 2; if enabling
 * this skews the horizon or mirrors turning, the matrix convention is
 * transposed - report it and the reads flip to column 2. */
static void VrFlattenCameraPitch(MATRIX* m)
{
    if (!g_PcConfig.vrFlattenPitch) return;
    if (!s_gpuStereo)               return;
    if (VrCutsceneActive())         return;

    {
        /* CALIBRATED CONVENTION (v2, from the "world bobs with the stick"
         * bug): this pipeline's camera FORWARD lives in COLUMN 2 (matches
         * VrApplyRotation's layout: r[0][2]=sy*cp, r[1][2]=-sp), and the
         * translation t is WORLD-POSITION-ADDITIVE (the proven eye-offset
         * code adds world deltas straight to t) - so t must be left
         * COMPLETELY UNTOUCHED. v1 read row 2 and "re-derived" t through
         * the rotation, corrupting position as a function of the removed
         * pitch = vertical world bobbing. */
        float fx  = (float)m->m[0][2];
        float fz  = (float)m->m[2][2];
        float len = sqrtf(fx * fx + fz * fz);
        s16 c, s;

        if (len < 256.0f) return;  /* near straight up/down: leave it */

        c = (s16)(fz * 4096.0f / len);
        s = (s16)(fx * 4096.0f / len);

        /* yaw-only rotation, same layout as VrApplyRotation at pitch 0 */
        m->m[0][0] = c;         m->m[0][1] = 0;     m->m[0][2] = s;
        m->m[1][0] = 0;         m->m[1][1] = 4096;  m->m[1][2] = 0;
        m->m[2][0] = (s16)-s;   m->m[2][1] = 0;     m->m[2][2] = c;
        /* t untouched: it is not a function of the rotation here */
    }
}

/* ---- VR subtitle inset (text_draw.c hooks) --------------------------------
 * Full VR used to draw subtitles into the widened world image, so they were
 * shrunk (x0.5) and pulled toward the centre here. The UI pass now goes to
 * its own panel (PsyX_render.cpp UI layer, sized by 2D_Screen_Size in
 * vr_full.c), where the text keeps the game's own layout - so these stay
 * neutral; shrinking again would only halve text inside the panel. */
int VrTracking_SubtitleScaleQ8(void)
{
    return 256;
}

int VrTracking_SubtitleLiftY(void)
{
    return 0;
}

/* 2D interface panel (Full VR): width in degrees, from 2D_Screen_Size
 * (100% = 60 deg), and the virtual distance its stereo offset uses (the
 * cinema screen's, so both 2D surfaces sit at the same depth). */
/* Eye_Resolution (vr_eye_res): the eye images' cap over the window, 1..2 */
float VrTracking_GetEyeResMax(void)
{
    return g_PcConfig.vrEyeRes / 100.0f;
}

float VrTracking_GetUiPanelDeg(void)
{
    return 60.0f * g_PcConfig.vrUiScreenSize / 100.0f;
}

float VrTracking_GetUiPanelDistance(void)
{
    return g_PcConfig.vrScreenDistance;
}

/* Half the eye separation in meters (vr_stereo_depth). */
float VrTracking_GetHalfIpdMeters(void)
{
    return g_PcConfig.vrStereoDepth;
}

static void VrCamToWorld(const MATRIX* camMat, s32 lx, s32 ly, s32 lz,
                         s32* wx, s32* wy, s32* wz)
{
#if VR_MAT_TRANSPOSE
    *wx = (camMat->m[0][0] * lx + camMat->m[1][0] * ly + camMat->m[2][0] * lz) >> 12;
    *wy = (camMat->m[0][1] * lx + camMat->m[1][1] * ly + camMat->m[2][1] * lz) >> 12;
    *wz = (camMat->m[0][2] * lx + camMat->m[1][2] * ly + camMat->m[2][2] * lz) >> 12;
#else
    *wx = (camMat->m[0][0] * lx + camMat->m[0][1] * ly + camMat->m[0][2] * lz) >> 12;
    *wy = (camMat->m[1][0] * lx + camMat->m[1][1] * ly + camMat->m[1][2] * lz) >> 12;
    *wz = (camMat->m[2][0] * lx + camMat->m[2][1] * ly + camMat->m[2][2] * lz) >> 12;
#endif
}

static void VrBridgeToCamSpace(float dx, float dy, float dz, float unitsPerMeter,
                               s32* lx, s32* ly, s32* lz)
{
    float rotSign = (s_mappingCombo & 1) ? -1.0f : 1.0f;
    float sx      = (s_mappingCombo & 2) ? -1.0f : 1.0f;
    float sz      = (s_mappingCombo & 4) ? -1.0f : 1.0f;

    /* Pure rotation by the recenter-captured yaw: absorbs ANY playspace
     * calibration angle (the old SWAP was a reflection - wrong tool). */
    float a  = rotSign * s_originRot[1];
    float ca = cosf(a), sa = sinf(a);
    float rx = dx * ca - dz * sa;
    float rz = dx * sa + dz * ca;

    *lx = (s32)(sx * rx * unitsPerMeter);
    *ly = (s32)(VR_SIGN_Y * dy * unitsPerMeter);
    *lz = (s32)(sz * rz * unitsPerMeter);
}

/* ---- roomscale --------------------------------------------------------- */

static void VrApplyRoomscale(const MATRIX* camMat)
{
    if (!VrRoomscaleActive() || !s_newSample || !s_havePrev)
    {
        return;
    }

    float dx = (s_headPos[0] - s_prevPos[0]) * g_PcConfig.vrRoomscaleScale;
    float dz = (s_headPos[2] - s_prevPos[2]) * g_PcConfig.vrRoomscaleScale;

    if (dx >  VR_ROOMSCALE_MAX_STEP) dx =  VR_ROOMSCALE_MAX_STEP;
    if (dx < -VR_ROOMSCALE_MAX_STEP) dx = -VR_ROOMSCALE_MAX_STEP;
    if (dz >  VR_ROOMSCALE_MAX_STEP) dz =  VR_ROOMSCALE_MAX_STEP;
    if (dz < -VR_ROOMSCALE_MAX_STEP) dz = -VR_ROOMSCALE_MAX_STEP;

    s32 lx, ly, lz;
    VrBridgeToCamSpace(dx, 0.0f, dz, VrUnitsPerMeter(), &lx, &ly, &lz);

    s32 wx, wy, wz;
    VrCamToWorld(camMat, lx, 0, lz, &wx, &wy, &wz);

    s32 sgn = (g_ControlStyle == ControlStyle_Fps) ? VR_ROOMSCALE_SIGN_FPS
                                                   : VR_ROOMSCALE_SIGN;

    g_SysWork.playerWork.player.position.vx += sgn * wx;
    g_SysWork.playerWork.player.position.vz += sgn * wz;
}

/* ---- rotation ----------------------------------------------------------- */

static void VrApplyRotation(MATRIX* camMat)
{
    float pitch = VR_SIGN_PITCH * (s_headRot[0] - s_originRot[0]);
    /* minus the yaw already handed to the body (VrTracking_TakeBodyTurn):
     * the body's base yaw carries it now, so the view is unchanged */
    float yaw   = VR_SIGN_YAW   * (s_headRot[1] - s_originRot[1]) - s_bodyYawXfer;
    float roll  = VR_SIGN_ROLL  * (s_headRot[2] - s_originRot[2]);

#if VR_ROLL_TO_QUAD
    s_quadRollRad = roll; /* hand roll to the FULL-mode quad instead */
    roll = 0.0f;          /* the game camera stays roll-free */
#endif

    while (yaw >  3.14159265f) yaw -= 6.28318531f;
    while (yaw < -3.14159265f) yaw += 6.28318531f;

    /* record the angles actually applied (for vr_full.c reprojection) */
    s_appliedPitch = pitch;
    s_appliedYaw   = yaw;

    float cp = cosf(pitch), sp = sinf(pitch);
    float cy = cosf(yaw),   sy = sinf(yaw);
    float cr = cosf(roll),  sr = sinf(roll);

    float r[3][3];
    r[0][0] =  cy * cr + sy * sp * sr;
    r[0][1] = -cy * sr + sy * sp * cr;
    r[0][2] =  sy * cp;
    r[1][0] =  cp * sr;
    r[1][1] =  cp * cr;
    r[1][2] = -sp;
    r[2][0] = -sy * cr + cy * sp * sr;
    r[2][1] =  sy * sr + cy * sp * cr;
    r[2][2] =  cy * cp;

    float mf[3][3], nf[3][3];
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            mf[i][j] = (float)camMat->m[i][j] / 4096.0f;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            nf[i][j] = mf[i][0] * r[0][j] + mf[i][1] * r[1][j] + mf[i][2] * r[2][j];

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            camMat->m[i][j] = (short)(nf[i][j] * 4096.0f);
    {
        /* the head's rotation before the Q12 truncation above, for the
         * exact view matrices (vw_calc.c) */
        extern void Vw_PcCamRotSet(const MATRIX* rounded, const double m[9]);
        double e[9];
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
                e[i * 3 + j] = (double)nf[i][j];
        Vw_PcCamRotSet(camMat, e);
    }
}

/* ---- public entry point ------------------------------------------------- */

void VrApplyHeadOffset(MATRIX* camMat)
{
    static int bannerShown = 0;
    if (!bannerShown)
    {
        printf("[VR] head tracking v3.0-exp (alternating-eye stereo scaffold)\n");
        printf("[VR]   vr_enabled=%d vr_positional=%d vr_roomscale=%d\n",
               g_PcConfig.vrEnabled, g_PcConfig.vrPositional, g_PcConfig.vrRoomscale);
        printf("[VR]   vr_scale=%.2f vr_roomscale_scale=%.2f vr_track_cutscenes=%d\n",
               (double)g_PcConfig.vrScale, (double)g_PcConfig.vrRoomscaleScale,
               g_PcConfig.vrTrackCutscenes);
        bannerShown = 1;
    }

    VrPollHotkeys();
    VrReadBridge();

    /* VR always culls. disable_culling=1 (shipped default) draws every
     * object of the preloaded map, behind the player included - ~2500
     * batches a frame, each drawn three times in Full VR (display + two
     * eye passes): 50-77 ms frames. The game's own culling is safe here:
     * its screen-edge bounds follow the live projection (vw_calc.c and
     * bodyprog_80055028.c derive them from H, Hor+ width and hfov/vfov,
     * +64 units), so it tracks the widened VR fov, and those edges sit
     * outside the lenses anyway. Not saved - flat play keeps the setting. */
    if (VrTracking_HeadCamActive() && g_PcConfig.disableCulling)
    {
        g_PcConfig.disableCulling = 0;
        {
            extern void VrOvr_Log(const char* fmt, ...);
            VrOvr_Log("[VR] culling on (disable_culling ignored with a headset)\n");
        }
    }

    /* FULL: widen the projection to the eye frustum. The game camera set
     * its own H earlier this frame (vcExecCamera); the alt cameras re-apply
     * this in Pc_CameraFov_Update, which runs after us. */
    {
        int vh = VrTracking_VrGeomScreen();
        if (vh > 0) SetGeomScreen(vh);
    }

    if (!g_PcConfig.vrEnabled || !s_haveOrigin)
    {
        return;
    }

    /* (roomscale moved AFTER rotation - see below) */

    /* The head drives the game camera only in Full VR, where the image is
     * your view. On a floating screen (2D/3D screen, menus, examine and
     * pickup screens) the screen is a TV in the room: you move, the shot
     * does not - head motion there swung the examined item and the scene
     * around inside the screen. s_gpuStereo = Full VR this frame. */

    s_headOffsetQ8[0] = s_headOffsetQ8[1] = s_headOffsetQ8[2] = 0;

    /* camera positional offset. Head_Lock (IK body) keeps the eye inside
     * Harry's head during play: leaning or ducking for real no longer moves
     * the view away from the body. */
    if (s_gpuStereo && g_PcConfig.vrPositional &&
        !(g_PcConfig.vrHeadLock && VrTracking_HeadCamActive() && !VrCutsceneActive()) &&
        (g_PcConfig.vrTrackCutscenes || !VrCutsceneActive()))
    {
        float dx = s_headPos[0] - s_origin[0];
        float dy = s_headPos[1] - s_origin[1];
        float dz = s_headPos[2] - s_origin[2];

        if (dx >  VR_MAX_OFFSET_METERS) dx =  VR_MAX_OFFSET_METERS;
        if (dx < -VR_MAX_OFFSET_METERS) dx = -VR_MAX_OFFSET_METERS;
        if (dy >  VR_MAX_OFFSET_METERS) dy =  VR_MAX_OFFSET_METERS;
        if (dy < -VR_MAX_OFFSET_METERS) dy = -VR_MAX_OFFSET_METERS;
        if (dz >  VR_MAX_OFFSET_METERS) dz =  VR_MAX_OFFSET_METERS;
        if (dz < -VR_MAX_OFFSET_METERS) dz = -VR_MAX_OFFSET_METERS;

        /* with roomscale active, X/Z live in the player - camera keeps Y */
        if (VrRoomscaleActive())
        {
            dx = 0.0f;
            dz = 0.0f;
        }

        s32 lx, ly, lz;
        VrBridgeToCamSpace(dx, dy, dz, VrUnitsPerMeter(), &lx, &ly, &lz);

        s32 wx, wy, wz;
        VrCamToWorld(camMat, lx, ly, lz, &wx, &wy, &wz);

        camMat->t[0] += Q12_TO_Q8(wx);
        camMat->t[1] += Q12_TO_Q8(wy);
        camMat->t[2] += Q12_TO_Q8(wz);
        s_headOffsetQ8[0] = Q12_TO_Q8(wx);
        s_headOffsetQ8[1] = Q12_TO_Q8(wy);
        s_headOffsetQ8[2] = Q12_TO_Q8(wz);
    }

    /* rotational tracking (Full VR - see above) */
    if (s_gpuStereo)
    {
        VrFlattenCameraPitch(camMat);
        VrApplyRotation(camMat);
    }

    /* ROOMSCALE - applied AFTER head rotation so real-world steps map
     * through the matrix INCLUDING your head yaw. Applied before, steps
     * were correct only at the recenter facing and skewed as you turned
     * (the same class of bug positional tracking had). */
    if (s_gpuStereo)
        VrApplyRoomscale(camMat);

    /* STEREO EYE OFFSET - applied AFTER head rotation so the offset
     * rides the ROLLED camera-right (your true inter-eye axis). Applied
     * before rotation it caused vertical disparity between the eyes on
     * head tilt - the "warp on roll" artifact.
     * Also independent of positional tracking and
     * cutscene gates! Depth must work even in 3DOF mode; this was the
     * "flat stereo" bug (offset died with vr_positional=0). Pure
     * camera-right shift, no playspace mapping involved. */
    {
        int eye = VrOvr_StereoEyeSign();
        if (eye != 0 && !s_gpuStereo)  /* summit renders stereo on the GPU */
        {
            s32 ex = (s32)(eye * g_PcConfig.vrStereoDepth * VrUnitsPerMeter());

            /* stereo diagnostics: 1/sec */
            {
                static Uint32 lastD = 0;
                Uint32 dn = SDL_GetTicks();
                if (dn - lastD > 1000)
                {
                    lastD = dn;
                    printf("[VR][stereo] eye=%+d offsetUnits=%d depth=%.3f\n",
                           eye, (int)ex, (double)g_PcConfig.vrStereoDepth);
                }
            }
            s32 wx, wy, wz;
            VrCamToWorld(camMat, ex, 0, 0, &wx, &wy, &wz);
            camMat->t[0] += Q12_TO_Q8(wx);
            camMat->t[1] += Q12_TO_Q8(wy);
            camMat->t[2] += Q12_TO_Q8(wz);
        }
    }

    /* The view this frame is rendered from - where the headset IS in the
     * game world. The IK body (vr_ik.c) places the controllers from it. */
    s_lastCamMat   = *camMat;
    s_lastCamValid = 1;
    s_lastCamPlayerPos.vx = g_SysWork.playerWork.player.position.vx;
    s_lastCamPlayerPos.vy = g_SysWork.playerWork.player.position.vy;
    s_lastCamPlayerPos.vz = g_SysWork.playerWork.player.position.vz;
    s_lastCamPlayerYaw    = g_SysWork.playerWork.player.rotation.vy;
}

/* What the game was doing, for the slow-frame log (vr_openxr.c, which stays
 * free of the game headers). */
void VrTracking_PerfContext(char* buf, int n)
{
    snprintf(buf, (size_t)n, "state %d/%d room %d at %.1f,%.1f cutscene %d ui %d",
             (int)g_GameWork.gameState, (int)g_SysWork.sysState, (int)g_SavegamePtr->mapRoomIdx,
             (double)g_SysWork.playerWork.player.position.vx / 4096.0,
             (double)g_SysWork.playerWork.player.position.vz / 4096.0,
             VrCutsceneActive(), VrTracking_GetUiContext());
}

/* [VR-STATE] log: one line whenever the game state, the VR's reading of it
 * (UI context, in-world screen), a 2D background, the frozen frame or the
 * projection distance H changes - to find which state a scene runs in.
 * Capped so a long session cannot flood the log. Called once per frame. */
void VrTracking_StateLog(void)
{
    extern void VrOvr_Log(const char* fmt, ...);
    extern s32  g_Pc2dBackgroundActive;
    extern int  g_PsxPresentLastFrame;
    extern long ReadGeomScreen();
    static int  s_last[8] = { -1, -1, -1, -1, -1, -1, -1, -1 }, s_lines = 0;
    int cur[8];
    int i, changed = 0;

    cur[0] = (int)g_GameWork.gameState;
    cur[1] = (int)g_SysWork.sysState;
    cur[2] = VrTracking_GetUiContext();
    cur[3] = VrTracking_UiInWorld();
    cur[4] = g_Pc2dBackgroundActive > 0;
    cur[5] = g_PsxPresentLastFrame;
    cur[6] = VrCutsceneActive();
    cur[7] = (int)ReadGeomScreen();
    for (i = 0; i < 8; i++)
        if (cur[i] != s_last[i]) changed = 1;
    if (!changed || s_lines >= 600)
        return;
    s_lines++;
    memcpy(s_last, cur, sizeof(cur));
    VrOvr_Log("[VR-STATE] game %d sys %d | ui %d inworld %d bg2d %d frozen %d cutscene %d | H %d | room %d\n",
              cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], (int)g_SavegamePtr->mapRoomIdx);
}

/* Where Harry stood (world Q12) and faced (Q12 angle) when the last view
 * was built: the IK body runs before this frame's view exists and moves
 * that view along with him (vr_ik.c HeadFrameGet). */
/* Your physical head's displacement the view carries this frame, world Q8
 * (zero with Head_Lock, outside Full VR or with positional tracking off).
 * The IK torso follows it (vr_ik.c PlaceTorso). */
void VrTracking_GetHeadOffset(s32 out[3])
{
    out[0] = s_headOffsetQ8[0];
    out[1] = s_headOffsetQ8[1];
    out[2] = s_headOffsetQ8[2];
}

int VrTracking_GetCameraPlayer(VECTOR* pos, int* yaw)
{
    if (!s_lastCamValid) return 0;
    *pos = s_lastCamPlayerPos;
    *yaw = s_lastCamPlayerYaw;
    return 1;
}

/* Recenter on demand (vr_ik.c Rebuild Body): head yaw and position. */
void VrTracking_Recenter(void)
{
    VrRecenter();
}

/* Camera (eye) matrix of the last view built: rotation camera->world (Q12,
 * columns = camera right/down/forward in PSX axes), translation in world Q8
 * units. 0 until VR has produced one. */
int VrTracking_GetCameraMatrix(MATRIX* out)
{
    if (!s_lastCamValid) return 0;
    *out = s_lastCamMat;
    return 1;
}
