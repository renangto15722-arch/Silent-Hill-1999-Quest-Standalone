/*
=============================================================================
 SH1 PC Port - IN-PROCESS OPENVR  (Stage 1: poses, Stage 2: floating screen)
=============================================================================

 EXPERIMENTAL - first build of the native OpenVR path.

 INSTALL:
 1. OpenVR SDK (github.com/ValveSoftware/openvr):
      headers/openvr_capi.h   -> pc_port/include/openvr_capi.h
      bin/win64/openvr_api.dll -> next to SilentHillPC.exe
 2. This file -> pc_port/src/vr_openvr.c
 3. Hook in pc_port/src/main_pc.c, immediately BEFORE the PsyX_EndScene call:
        { extern void VrOvr_EndFrame(void); VrOvr_EndFrame(); }

 BEHAVIOR:
 - openvr_api.dll missing or no HMD -> everything silently no-ops;
   the game runs exactly as before. Zero risk install.
 - NUMPAD 9 toggles the floating screen (game view inside the headset).
 - NUMPAD * / NUMPAD / grow/shrink the screen.
 - AUTO SCREEN (item 7): menus/inventory/map/documents auto-drop
   FULL -> 3D SCREEN, restored on gameplay return (vr_tracking.c detects).
 - FMV (item 8): mode hotkeys stay live during video playback.
 - Poses: set environment variable SH1VR_OPENVR_POSES=1 to feed head
   tracking from here instead of the Python bridge file (OFF by default
   until axis conventions are verified against the tuned bridge).

 This file deliberately includes NO decomp headers (windows.h lives here;
 the 'byte' typedef clash stays impossible).
=============================================================================
*/

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include <SDL.h>
#include <SDL_opengl.h>

/* Official Valve C API header - correct ABI guaranteed. */
#include "openvr_capi.h"
#include "vr_input.h"

/* ---- diagnostics log ---------------------------------------------------- */

/* SilentHillVR.log next to the exe: the values each VR stage actually used
 * (runtime, per-eye sizes and fov, image angles, projection, screen).
 * Separate from SilentHill.log, which is off unless enable_debug_log=1;
 * printf output goes to a hidden console. Echoed to stdout too. */
void VrOvr_Log(const char* fmt, ...)
{
    static FILE* f = NULL;
    static int   tried = 0;
    va_list ap;

    if (!tried)
    {
        tried = 1;
        /* the last session's log survives one more start (a relaunch used to
         * wipe the log of the session that had the problem) */
        remove("SilentHillVR.prev.log");
        rename("SilentHillVR.log", "SilentHillVR.prev.log");
        f = fopen("SilentHillVR.log", "w");
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (f)
    {
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fflush(f);
    }
}

/* ---- reading the finished game frame ----------------------------------- */

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

static void (APIENTRY *s_pBindFramebuffer)(GLenum, GLuint) = NULL;

extern int          g_windowWidth, g_windowHeight; /* PsyX: scene-target size */
extern int          g_PcNativeRender;
extern unsigned int g_internalFBO;
extern unsigned int GR_ScreenReadFBO(void);         /* resolves MSAA */

/* Since beta-2026.10 the scene renders into an internal FBO and reaches the
 * window only at swap, so the backbuffer is stale while these hooks run.
 * Native-resolution mode runs the hooks after the present, on the window,
 * so it keeps reading the backbuffer. */
static int VrOvr_SceneInFbo(void)
{
    return g_internalFBO != 0 && !g_PcNativeRender;
}

/* Size of the finished game frame. Returns 0 when there is none. */
int VrOvr_SceneSize(int* outW, int* outH)
{
    *outW = *outH = 0;
    if (VrOvr_SceneInFbo())
    {
        *outW = g_windowWidth;
        *outH = g_windowHeight;
    }
    else
    {
        SDL_Window* win = SDL_GL_GetCurrentWindow();
        if (win) SDL_GL_GetDrawableSize(win, outW, outH);
    }
    return (*outW > 0 && *outH > 0);
}

/* Bind the finished game frame as the GL read source and return its size
 * and the previous read binding (hand it to VrOvr_SceneReadEnd).
 * Returns 0 when nothing is readable. */
int VrOvr_SceneReadBegin(int* outW, int* outH, int* outPrevRead)
{
    extern int PsyX_GlShadowGet(unsigned int* drawFbo, unsigned int* readFbo, int* scissorOn);
    unsigned int shDraw, shRead;
    int          shScissor;
    GLint prev = 0;

    /* shadowed binding when available: glGet* syncs the driver */
    if (PsyX_GlShadowGet(&shDraw, &shRead, &shScissor))
        prev = (GLint)shRead;
    else
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
    *outPrevRead = (int)prev;

    if (!s_pBindFramebuffer)
    {
        s_pBindFramebuffer = (void (APIENTRY *)(GLenum, GLuint))
            SDL_GL_GetProcAddress("glBindFramebuffer");
        if (!s_pBindFramebuffer) return 0;
    }

    if (VrOvr_SceneInFbo())
    {
        s_pBindFramebuffer(GL_READ_FRAMEBUFFER, GR_ScreenReadFBO());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    else
    {
        s_pBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
    }
    return VrOvr_SceneSize(outW, outH);
}

void VrOvr_SceneReadEnd(int prevRead)
{
    if (s_pBindFramebuffer) s_pBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
}

/* ---- dynamic OpenVR loading -------------------------------------------- */

typedef intptr_t (*PFN_VR_InitInternal)(EVRInitError* err, EVRApplicationType type);
typedef void     (*PFN_VR_ShutdownInternal)(void);
typedef intptr_t (*PFN_VR_GetGenericInterface)(const char* name, EVRInitError* err);
typedef int      (*PFN_VR_IsHmdPresent)(void);

static HMODULE s_dll = NULL;
static PFN_VR_InitInternal        p_Init = NULL;
static PFN_VR_ShutdownInternal    p_Shutdown = NULL;
static PFN_VR_GetGenericInterface p_GetIface = NULL;
static PFN_VR_IsHmdPresent        p_HmdPresent = NULL;

static struct VR_IVRSystem_FnTable*     s_sys  = NULL;
static struct VR_IVROverlay_FnTable*    s_ovl  = NULL;
static struct VR_IVRCompositor_FnTable* s_comp = NULL;

static int s_vrReady   = 0;
static int s_triedInit = 0;

/* VR runtime picked at startup (VrOvr_ChooseRuntime). OpenXR runs behind
 * OpenVR-shaped function tables (vr_openxr.c), so nothing below changes. */
#define VR_RUNTIME_OPENVR 1
#define VR_RUNTIME_OPENXR 2
static int s_runtime = VR_RUNTIME_OPENVR;

/* Game exit (atexit; registered after PsyX's, so it runs BEFORE the window
 * and GL context go): end the VR session, then arm a watchdog - a runtime
 * that still holds a thread in its DLL detach would otherwise leave the
 * process running invisibly after F4 / closing the window. */
static DWORD WINAPI VrOvr_ExitWatchdog(LPVOID unused)
{
    (void)unused;
    Sleep(3000);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void VrOvr_Shutdown(void)
{
    HANDLE t = CreateThread(NULL, 0, VrOvr_ExitWatchdog, NULL, 0, NULL);
    if (t) CloseHandle(t);

    if (s_runtime == VR_RUNTIME_OPENXR)
    {
        extern void VrXr_Shutdown(void);
        VrXr_Shutdown();
    }
    else if (s_dll && p_Shutdown && s_vrReady)
    {
        p_Shutdown();
    }
    s_vrReady = 0;
}

/* Startup menu, shown before the game window exists. */
void VrOvr_ChooseRuntime(void)
{
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, VR_RUNTIME_OPENXR, "OpenXR" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, VR_RUNTIME_OPENVR, "OpenVR (SteamVR)" },
    };
    SDL_MessageBoxData box;
    int choice = -1;

    /* SH1VR_RUNTIME=openxr|openvr skips the question - the OpenXR simulator
     * (K:\sh1vr\xrsim64) launches the game unattended. */
    {
        const char* env = getenv("SH1VR_RUNTIME");
        if (env && env[0])
        {
            s_runtime = (_stricmp(env, "openvr") == 0) ? VR_RUNTIME_OPENVR : VR_RUNTIME_OPENXR;
            VrOvr_Log("[VR-OVR] runtime: %s (SH1VR_RUNTIME)\n",
                      s_runtime == VR_RUNTIME_OPENXR ? "OpenXR" : "OpenVR");
            return;
        }
    }

    memset(&box, 0, sizeof(box));
    box.flags      = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    box.title      = "Silent Hill VR";
    box.message    = "Which VR runtime should the game use?\n\n"
                     "OpenXR: your active OpenXR runtime (Virtual Desktop, Oculus, SteamVR...)\n"
                     "OpenVR: SteamVR";
    box.numbuttons = 2;
    box.buttons    = buttons;

    if (SDL_ShowMessageBox(&box, &choice) == 0 && choice == VR_RUNTIME_OPENXR)
        s_runtime = VR_RUNTIME_OPENXR;
    else
        s_runtime = VR_RUNTIME_OPENVR;

    VrOvr_Log("[VR-OVR] runtime: %s\n", s_runtime == VR_RUNTIME_OPENXR ? "OpenXR" : "OpenVR");
}

/* ---- floating screen state ---------------------------------------------- */

static VROverlayHandle_t s_screen = 0;
static int    s_screenVisible = 0;
static float  s_screenWidth   = 1.6f;   /* meters; config vr_screen_width */

/* ---- alternating-eye stereo state (EXPERIMENTAL Stage 3a) ---------------- */
/* Even frames render+submit LEFT eye, odd frames RIGHT; the other eye
 * re-shows its previous texture. Half-rate per eye; SteamVR reprojection
 * smooths the gap. Toggle: NUMPAD ENTER. */

/* Stereo modes (NUMPAD ENTER cycles):
 *  0 = OFF
 *  1 = 3D SCREEN  - the floating screen becomes stereoscopic (side-by-side
 *      overlay). Undistorted, comfortable - the recommended mode.
 *  2 = FULL       - raw compositor submission (experimental; image warps
 *      until per-eye FOV composition is built). */
#define VR_STEREO_OFF    0
#define VR_STEREO_SCREEN 1
#define VR_STEREO_FULL   2
static int    s_stereoMode  = 0;
static int    s_frameParity = 0;      /* 0 = left eye this frame */
static GLuint s_sbsTex      = 0;      /* side-by-side texture (2w x h) */
static int    s_sbsW = 0, s_sbsH = 0;
static GLuint s_blackTex    = 0;      /* theater backdrop for SCREEN mode */
static int    s_sbsNeedsClear = 0;    /* wipe SBS texture on mode entry */
static int    s_prevParity    = 0;    /* parity of the PREVIOUS frame's build */

/* PS1-style pipelining: the image finished at frame end was built with
 * LAST frame's camera (double-buffered ordering tables) - so captures
 * must go into the PREVIOUS parity's half or the eyes self-cancel
 * (the "flat stereo at max depth" bug). Set 0 to disable if the port
 * turns out to draw same-frame. */
#define VR_SBS_PIPELINE_DELAY 0

/* SBS displayed-aspect compensation. 1.0 = trust SteamVR's side-by-side
 * handling (mono path proves plain aspect works). If the 3D screen is
 * stretched WIDE try 0.5; stretched TALL try 2.0. */
#define VR_SBS_TEXEL_ASPECT 1.0f
static GLuint s_eyeTex[2]   = {0, 0};
static int    s_eyeTexW = 0, s_eyeTexH = 0;
static HmdMatrix34_t s_eyePose[2];    /* HMD pose at each eye's capture */
static int    s_eyePoseValid[2] = {0, 0};
static TrackedDevicePose_t s_waitPoses[16]; /* from WaitGetPoses */
static int    s_waitPosesValid = 0;

/* forward declarations (helpers defined lower, used by ScreenToggle etc.) */
static void VrOvr_ApplyOverlaySampling(void);
static int  VrOvr_EnsureSharedTex(int* outW, int* outH);
static void VrOvr_SetSharedOverlayTexture(void);

/* ---- pose state ---------------------------------------------------------- */

static int   s_usePoses = 0;   /* SH1VR_OPENVR_POSES=1 */
static int   s_havePose = 0;
static float s_pos[3]   = {0, 0, 0};
static float s_rot[3]   = {0, 0, 0}; /* pitch, yaw, roll */

/* ------------------------------------------------------------------------- */

static void VrOvr_TryInit(void)
{
    if (s_triedInit) return;
    s_triedInit = 1;

    /* Runs on the first frame, after PsyX_Initialise registered its own
     * teardown - so VR shuts down first, with the GL context alive. */
    atexit(VrOvr_Shutdown);

    /* The headset paces the frames. Desktop vsync would lock the loop to the
     * monitor (60 Hz) on top of that - two clocks, uneven frames, judder. */
    {
        extern void PsyX_ApplyVsync(int vsync);
        PsyX_ApplyVsync(0);
    }

    /* Shadowed GL state (PsyX_render.cpp) from now on, so the per-frame VR
     * copies can restore bindings without querying the driver. */
    {
        extern void PsyX_StereoShadowInstall(void);
        PsyX_StereoShadowInstall();
    }

    /* Native poses are the DEFAULT now (FreePIE/file-bridge era over).
     * Set SH1VR_OPENVR_POSES=0 to force the legacy file bridge. */
    const char* env = getenv("SH1VR_OPENVR_POSES");
    s_usePoses = !(env != NULL && env[0] == '0');

    if (s_runtime == VR_RUNTIME_OPENXR)
    {
        extern int VrXr_Init(void** sysTbl, void** compTbl, void** ovlTbl);
        if (!VrXr_Init((void**)&s_sys, (void**)&s_comp, (void**)&s_ovl))
        {
            printf("[VR-OVR] OpenXR unavailable - HMD screen disabled\n");
            s_sys = NULL; s_comp = NULL; s_ovl = NULL;
            return;
        }
        s_vrReady = 1;
        printf("[VR-OVR] OpenXR ready (poses via %s)\n",
               s_usePoses ? "OpenXR in-process" : "file bridge");
        return;
    }

    s_dll = LoadLibraryA("openvr_api.dll");
    if (!s_dll)
    {
        printf("[VR-OVR] openvr_api.dll not found - HMD screen disabled\n");
        return;
    }

    p_Init       = (PFN_VR_InitInternal)GetProcAddress(s_dll, "VR_InitInternal");
    p_Shutdown   = (PFN_VR_ShutdownInternal)GetProcAddress(s_dll, "VR_ShutdownInternal");
    p_GetIface   = (PFN_VR_GetGenericInterface)GetProcAddress(s_dll, "VR_GetGenericInterface");
    p_HmdPresent = (PFN_VR_IsHmdPresent)GetProcAddress(s_dll, "VR_IsHmdPresent");

    if (!p_Init || !p_Shutdown || !p_GetIface)
    {
        printf("[VR-OVR] openvr_api.dll exports missing\n");
        return;
    }

    if (p_HmdPresent && !p_HmdPresent())
    {
        printf("[VR-OVR] no HMD present - HMD screen disabled\n");
        return;
    }

    EVRInitError err = EVRInitError_VRInitError_None;
    /* Scene app type: required for compositor eye submission (stereo);
     * overlays (the floating screen) still work from scene apps. */
    p_Init(&err, EVRApplicationType_VRApplication_Scene);
    if (err != EVRInitError_VRInitError_None)
    {
        printf("[VR-OVR] VR_InitInternal failed (%d)\n", (int)err);
        return;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "FnTable:%s", IVRSystem_Version);
    s_sys = (struct VR_IVRSystem_FnTable*)p_GetIface(buf, &err);
    snprintf(buf, sizeof(buf), "FnTable:%s", IVROverlay_Version);
    s_ovl = (struct VR_IVROverlay_FnTable*)p_GetIface(buf, &err);
    snprintf(buf, sizeof(buf), "FnTable:%s", IVRCompositor_Version);
    s_comp = (struct VR_IVRCompositor_FnTable*)p_GetIface(buf, &err);

    if (!s_sys || !s_ovl)
    {
        printf("[VR-OVR] interface tables unavailable\n");
        p_Shutdown();
        return;
    }

    s_vrReady = 1;
    printf("[VR-OVR] OpenVR ready (poses via %s)\n",
           s_usePoses ? "OpenVR in-process" : "file bridge");
}

/* ---- floating screen ------------------------------------------------------ */

/* Placement in the standing universe. Height lowered 1.4 -> 1.1m per
 * in-HMD feedback ("a little high") - 1.1m centers it for seated play.
 * Width and distance come from config (vr_screen_width 1.6 m and
 * vr_screen_distance 2 m = ~44 deg wide; the old fixed 2.5 m was ~64 deg).
 * Height follows the image aspect, so nothing is stretched. */
#define VR_SCREEN_HEIGHT_M 1.1f

static void VrOvr_ScreenPlace(void)
{
    extern float VrTracking_GetScreenWidth(void);
    extern float VrTracking_GetScreenDistance(void);

    if (!s_vrReady || s_screen == 0) return;
    s_screenWidth = VrTracking_GetScreenWidth();

    /* 16:9-ish plane in front of the standing origin */
    HmdMatrix34_t m;
    memset(&m, 0, sizeof(m));
    m.m[0][0] = 1.0f;
    m.m[1][1] = 1.0f;
    m.m[2][2] = 1.0f;
    m.m[0][3] = 0.0f;
    m.m[1][3] = VR_SCREEN_HEIGHT_M;
    m.m[2][3] = -VrTracking_GetScreenDistance();

    VrOvr_Log("[VR-SCREEN] width %.2f m at %.2f m = %.1f deg wide, centre %.2f m high\n",
              (double)s_screenWidth, (double)-m.m[2][3],
              2.0 * atan((double)s_screenWidth * 0.5 / (double)-m.m[2][3]) * 57.29578,
              (double)VR_SCREEN_HEIGHT_M);

    s_ovl->SetOverlayTransformAbsolute(
        s_screen, ETrackingUniverseOrigin_TrackingUniverseStanding, &m);
    s_ovl->SetOverlayWidthInMeters(s_screen, s_screenWidth);
}

static void VrOvr_ScreenEnsure(void)
{
    if (!s_vrReady || s_screen != 0) return;

    EVROverlayError e = s_ovl->CreateOverlay("sh1.screen", "Silent Hill", &s_screen);
    if (e != EVROverlayError_VROverlayError_None)
    {
        printf("[VR-OVR] CreateOverlay failed (%d)\n", (int)e);
        s_screen = 0;
        return;
    }
    VrOvr_ScreenPlace();
}

static void VrOvr_ScreenToggle(void)
{
    VrOvr_TryInit();
    if (!s_vrReady) return;
    VrOvr_ScreenEnsure();
    if (s_screen == 0) return;

    s_screenVisible = !s_screenVisible;
    if (s_screenVisible)
    {
        VrOvr_ScreenPlace();
        VrOvr_ApplyOverlaySampling();
        EVROverlayError es = s_ovl->ShowOverlay(s_screen);
        printf("[VR-OVR] floating screen ON (showErr=%d handle=%llu)\n",
               (int)es, (unsigned long long)s_screen);
    }
    else
    {
        s_ovl->HideOverlay(s_screen);
        printf("[VR-OVR] floating screen OFF\n");
    }
}

static void VrOvr_ScreenSize(float delta)
{
    if (!s_vrReady || s_screen == 0) return;
    s_screenWidth += delta;
    if (s_screenWidth < 0.5f) s_screenWidth = 0.5f;
    if (s_screenWidth > 8.0f) s_screenWidth = 8.0f;
    s_ovl->SetOverlayWidthInMeters(s_screen, s_screenWidth);
    {
        extern void VrTracking_SetScreenWidth(float m);
        VrTracking_SetScreenWidth(s_screenWidth);   /* KP* / KP/ persist */
    }
    printf("[VR-OVR] screen width %.2fm\n", (double)s_screenWidth);
}

/* One-shot at startup: as soon as VR is ready and no stereo mode is
 * active, show the floating screen automatically so splash screens,
 * intro movies, and the main menu are visible in-HMD from launch -
 * no KP9 required (KP9 still toggles freely afterwards). Called from
 * both frame entry points since boot movies use the FMV path. */
/* Show the floating screen if it isn't up (used by boot, and by every
 * auto path that lands on 2D Screen mode - OFF renders nothing in-HMD
 * without it). */
static void VrOvr_EnsureScreenVisible(void)
{
    if (!s_vrReady || s_screenVisible) return;
    VrOvr_ScreenEnsure();
    if (s_screen != 0)
    {
        VrOvr_ScreenPlace();
        VrOvr_ApplyOverlaySampling();
        s_ovl->ShowOverlay(s_screen);
        s_screenVisible = 1;
    }
}

static int s_bootScreenDone = 0;
static void VrOvr_BootScreenAutoShow(void)
{
    if (s_bootScreenDone || !s_vrReady) return;
    s_bootScreenDone = 1;

    if (s_stereoMode == VR_STEREO_OFF && !s_screenVisible)
    {
        VrOvr_EnsureScreenVisible();
        if (s_screenVisible)
            printf("[VR-OVR] boot: floating screen auto-shown\n");
    }
}


/* UNIFIED OVERLAY TEXTURE ARCHITECTURE:
 * OpenVR's GL path latches the first texture handle an overlay receives -
 * switching handles later silently fails (the mode-switch freeze bug).
 * Cure: ONE texture (2w x h) forever. Mono writes+samples the left half
 * (bounds 0..0.5); SBS writes both halves and samples 0..1 with the
 * side-by-side flag. Only bounds/flags change - never the handle. */
static void VrOvr_ApplyOverlaySampling(void)
{
    if (s_screen == 0) return;

    VRTextureBounds_t b;
    if (s_stereoMode == VR_STEREO_SCREEN)
    {
        b.uMin = 0.0f; b.vMin = 0.0f; b.uMax = 1.0f; b.vMax = 1.0f;
        s_ovl->SetOverlayFlag(s_screen, VROverlayFlags_SideBySide_Parallel, 1);
        s_ovl->SetOverlayTexelAspect(s_screen, VR_SBS_TEXEL_ASPECT);
    }
    else
    {
        b.uMin = 0.0f; b.vMin = 0.0f; b.uMax = 0.5f; b.vMax = 1.0f;
        s_ovl->SetOverlayFlag(s_screen, VROverlayFlags_SideBySide_Parallel, 0);
        s_ovl->SetOverlayTexelAspect(s_screen, 1.0f);
    }
    s_ovl->SetOverlayTextureBounds(s_screen, &b);
}

/* Ensure the shared 2w x h texture exists at the right size. Returns the
 * game frame size via out params; 0 on failure. */
static int VrOvr_EnsureSharedTex(int* outW, int* outH)
{
    int w = 0, h = 0;
    if (!VrOvr_SceneSize(&w, &h)) return 0;

    if (s_sbsTex == 0 || w != s_sbsW || h != s_sbsH)
    {
        if (s_sbsTex == 0) glGenTextures(1, &s_sbsTex);
        glBindTexture(GL_TEXTURE_2D, s_sbsTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * 2, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        s_sbsW = w;
        s_sbsH = h;
        s_sbsNeedsClear = 1;
    }

    if (s_sbsNeedsClear)
    {
        unsigned char* zero = (unsigned char*)calloc((size_t)w * 2 * h, 4);
        if (zero)
        {
            glBindTexture(GL_TEXTURE_2D, s_sbsTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w * 2, h,
                            GL_RGBA, GL_UNSIGNED_BYTE, zero);
            glBindTexture(GL_TEXTURE_2D, 0);
            free(zero);
        }
        s_sbsNeedsClear = 0;
    }

    *outW = w;
    *outH = h;
    return 1;
}

static void VrOvr_SetSharedOverlayTexture(void)
{
    struct Texture_t tex;
    tex.handle      = (void*)(uintptr_t)s_sbsTex;
    tex.eType       = ETextureType_TextureType_OpenGL;
    tex.eColorSpace = EColorSpace_ColorSpace_Gamma;
    s_ovl->SetOverlayTexture(s_screen, &tex);
}

/* ---- 3D SCREEN mode: side-by-side stereo on the floating screen ---------- */

static void VrOvr_SbsPushFrame(void)
{
    {
        static int wasRunning = -1;
        int running = (s_vrReady && s_stereoMode == VR_STEREO_SCREEN &&
                       s_screen != 0 && s_screenVisible);
        if (running != wasRunning)
        {
            printf("[VR-OVR][sbs] %s (ready=%d mode=%d screen=%d vis=%d)\n",
                   running ? "RUNNING" : "GATED",
                   s_vrReady, s_stereoMode, (int)(s_screen != 0), s_screenVisible);
            wasRunning = running;
        }
    }
    if (!s_vrReady || s_stereoMode != VR_STEREO_SCREEN || s_screen == 0 ||
        !s_screenVisible) return;

    int w, h;
    if (!VrOvr_EnsureSharedTex(&w, &h)) return;

#if VR_SBS_PIPELINE_DELAY
    int half = s_prevParity;   /* image was built LAST frame */
#else
    int half = s_frameParity;
#endif

    /* Copy this frame's backbuffer into the half for the CURRENT eye.
     * Convergence: shift the copy horizontally, opposite per eye - moves
     * the whole scene toward/away from the screen plane. */
    {
        extern int VrTracking_GetConvergencePx(void);
        int conv = VrTracking_GetConvergencePx();
        int shift = half ? -conv : conv;
        int dstX = (half ? w : 0) + shift;
        int srcX = 0, cw = w;
        if (dstX < (half ? w : 0))
        {
            srcX = (half ? w : 0) - dstX;
            dstX = (half ? w : 0);
            cw   = w - srcX;
        }
        else if (dstX + cw > (half ? 2 * w : w))
        {
            cw = (half ? 2 * w : w) - dstX;
        }
        if (cw > 0)
        {
            int rw, rh, prevRead;
            VrOvr_SceneReadBegin(&rw, &rh, &prevRead);
            glBindTexture(GL_TEXTURE_2D, s_sbsTex);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dstX, 0, srcX, 0, cw, h);
            VrOvr_SceneReadEnd(prevRead);

            /* CONVERGENCE STRIP FIX: the shifted copy leaves an uncovered
             * strip at one edge of this half - blank it, or it keeps stale
             * pixels forever (the "garbled" edges on mode switches). */
            if (cw < w)
            {
                static unsigned char* stripZero = NULL;
                static int stripCap = 0;
                int stripW  = w - cw;
                int halfX0  = half ? w : 0;
                int stripX  = (dstX > halfX0) ? halfX0 : dstX + cw;
                int need    = stripW * h * 4;

                if (stripZero == NULL || stripCap < need)
                {
                    free(stripZero);
                    stripZero = (unsigned char*)calloc(need, 1);
                    stripCap  = stripZero ? need : 0;
                }
                if (stripZero)
                {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, stripX, 0, stripW, h,
                                    GL_RGBA, GL_UNSIGNED_BYTE, stripZero);
                }
            }

            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    VrOvr_ApplyOverlaySampling();
    VrOvr_SetSharedOverlayTexture();

    /* SBS diagnostics: 1/sec */
    {
        static Uint32 lastD = 0;
        Uint32 dn = SDL_GetTicks();
        if (dn - lastD > 1000)
        {
            lastD = dn;
            printf("[VR-OVR][sbs] buildParity=%d capturedHalf=%d delay=%d\n",
                   s_frameParity, half, VR_SBS_PIPELINE_DELAY);
        }
    }

    s_prevParity = s_frameParity;
    s_frameParity ^= 1;
}

static void VrOvr_SetStereoMode(int mode)
{
    s_stereoMode = mode;

    if (mode == VR_STEREO_SCREEN)
    {
        s_sbsNeedsClear = 1; /* wipe stale halves from previous modes */

        VrOvr_ScreenEnsure();
        if (s_screen != 0 && !s_screenVisible)
        {
            VrOvr_ScreenPlace();
            s_ovl->ShowOverlay(s_screen);
            s_screenVisible = 1;
        }
    }
    else if (mode == VR_STEREO_FULL && s_screen != 0 && s_screenVisible)
    {
        /* full immersion: the screen just occludes - hide it */
        s_ovl->HideOverlay(s_screen);
        s_screenVisible = 0;
    }
    else if (mode == VR_STEREO_OFF)
    {
        /* OFF with no screen visible = scene app goes quiet AND nothing
         * renders -> SteamVR fades to its loading grid (the FULL->OFF
         * cutscene-restore "hang"). Landing on OFF through ANY path
         * (manual, auto restore, gameplay entry) brings the screen up;
         * KP9 can still hide it afterwards if the void is wanted. */
        VrOvr_EnsureScreenVisible();
    }

    VrOvr_ApplyOverlaySampling();

    printf("[VR-OVR] stereo mode: %s\n",
           mode == VR_STEREO_SCREEN ? "3D SCREEN (side-by-side overlay)" :
           mode == VR_STEREO_FULL   ? "FULL (experimental, warped)" : "OFF");
}

/* ---- AUTO SCREEN (roadmap item 7) ----------------------------------------
 * Flat UI contexts (menus/inventory/map/documents/save/pause) read terribly
 * inside the head-locked FULL window - auto-drop to the 2D SCREEN while UI
 * is up, restore FULL when gameplay resumes. Detection lives in
 * vr_tracking.c (decomp headers stay out of this file).
 * A manual KP_ENTER cycle while UI is up CANCELS the pending restore -
 * the user's explicit choice wins.
 * Config wiring (vr_auto_screen key + menu row) lands with roadmap 2-6. */
#define VR_AUTO_SCREEN_DEFAULT 1
static int s_autoScreen  = VR_AUTO_SCREEN_DEFAULT;
static int s_uiSavedMode = -1;   /* stereo mode to restore at gameplay return */

/* FMV auto-screen (item 8): FULL drops to the floating SCREEN for video
 * playback, restored when the FMV ends. s_fmvLive is set every FMV frame
 * and cleared by the main hook - its set->clear transition IS the
 * "FMV ended" signal (the two entry points never run in the same loop). */
static int s_fmvSavedMode = -1;
static int s_fmvLive      = 0;

/* Gameplay target mode (item 6): the stereo mode gameplay should run in.
 * SOURCE OF TRUTH is now the vr_game_view_mode config key (menu row
 * "Game_View_Mode"); synced from config every frame, and manual
 * KP_ENTER cycles write BACK through VrTracking_SetGameViewMode - the
 * chosen mode survives restarts. */
static int s_gameplayMode = VR_STEREO_FULL;

/* In-engine cutscene view (Cutscene_View menu row - FMVs are NOT
 * governed by this; they always use the 3D screen):
 * 0 = 2D screen (cinema), 2 = Full VR (stays 3D). */
static int s_cutSavedMode = -1;
static int s_cutActive    = 0;
static int s_uiCtxStable = 0;    /* debounced UI context (0 gameplay, 1 UI) */
static int s_uiCtxLast   = 0;
static int s_uiCtxFrames = 0;

/* In-game menus in Full VR (vr_tracking.c VrTracking_UiInWorld): the mode
 * stays FULL, the world freezes and the menu's flat frame is a 2D screen in
 * front of it (PsyX_StereoSetUiScreen, vr_full.c DrawFlatPanel). */
static int s_uiInWorld = 0;

static void VrOvr_UiInWorldSet(int on)
{
    extern void PsyX_StereoSetUiScreen(int on);
    if (on != s_uiInWorld)
        printf("[VR-OVR][auto] in-game menu as a 2D screen in the world: %s\n", on ? "on" : "off");
    s_uiInWorld = on;
    PsyX_StereoSetUiScreen(on);
}

static void VrOvr_AutoScreenUpdate(void)
{
    extern int VrTracking_GetUiContext(void); /* vr_tracking.c */
    extern int VrTracking_UiInWorld(void);
    extern int PsyX_StereoCanFreeze(void);

    /* anything else took the mode away from FULL (cutscene, FMV, keys) */
    if (s_uiInWorld && (s_stereoMode != VR_STEREO_FULL || !s_autoScreen || !s_vrReady))
        VrOvr_UiInWorldSet(0);

    if (!s_autoScreen || !s_vrReady) return;

    /* the game ended to the title side: the plain floating screen */
    if (s_uiInWorld && s_uiCtxStable == 1 && !VrTracking_UiInWorld())
    {
        VrOvr_UiInWorldSet(0);
        s_uiSavedMode = VR_STEREO_FULL;
        VrOvr_SetStereoMode(VR_STEREO_OFF);
        VrOvr_EnsureScreenVisible();
    }

    int ctx = VrTracking_GetUiContext();
    if (ctx == s_uiCtxLast)
    {
        if (s_uiCtxFrames < 1000) s_uiCtxFrames++;
    }
    else
    {
        s_uiCtxLast   = ctx;
        s_uiCtxFrames = 0;
    }

    /* 3-frame debounce rides out single-frame state flickers during
     * screen transitions (mode switches are cheap but not free). */
    if (s_uiCtxFrames < 3 || ctx == s_uiCtxStable) return;
    s_uiCtxStable = ctx;

    if (ctx == 1)   /* gameplay -> UI */
    {
        if (s_stereoMode == VR_STEREO_FULL && VrTracking_UiInWorld() && PsyX_StereoCanFreeze())
        {
            VrOvr_UiInWorldSet(1);
            s_uiSavedMode = -1;   /* still FULL */
        }
        else if (s_stereoMode == VR_STEREO_FULL)
        {
            /* the 2D (mono) screen: menus, examine and pickup screens are
             * flat content, and the 3D screen's alternating-eye capture
             * ghosted and shook on them (one eye per frame, each half from
             * a different frame of a still or changing image) */
            s_uiSavedMode = s_stereoMode;
            printf("[VR-OVR][auto] UI up: FULL -> 2D SCREEN\n");
            VrOvr_SetStereoMode(VR_STEREO_OFF);
            VrOvr_EnsureScreenVisible();
        }
        else
        {
            s_uiSavedMode = -1;  /* OFF / SCREEN already read fine as-is */
        }
    }
    else            /* UI -> gameplay */
    {
        if (s_uiInWorld)
            VrOvr_UiInWorldSet(0);
        if (s_uiSavedMode >= 0)
        {
            printf("[VR-OVR][auto] gameplay resumed: restoring mode %d\n", s_uiSavedMode);
            VrOvr_SetStereoMode(s_uiSavedMode);
            s_uiSavedMode = -1;
        }
        else if (s_stereoMode != s_gameplayMode)
        {
            /* Nothing to restore (e.g. first entry from the boot screen):
             * enter the gameplay target mode (item 6). s_gameplayMode
             * tracks manual KP_ENTER choices, so a deliberate OFF/SCREEN
             * pick is respected, not fought. */
            printf("[VR-OVR][auto] entering gameplay: mode %d\n", s_gameplayMode);
            VrOvr_SetStereoMode(s_gameplayMode);
            if (s_gameplayMode == VR_STEREO_OFF) VrOvr_EnsureScreenVisible();
        }
    }
}

/* A held-button menu was just opened (vr_interact.c Pc_VrMenuRequestTake):
 * freeze the world from the next frame, without waiting for the debounced
 * UI context above. Waiting froze one of the menu's transition frames -
 * drawn without the VR projection (the game's own ~98 x 66 deg, the rest
 * black) and already fading - so the menu stood on a dark strip of world.
 * The context above then finds it on and keeps it. */
void VrOvr_MenuFreezeNow(void)
{
    extern int  PsyX_StereoCanFreeze(void);
    extern void PsyX_StereoSetUiScreenNextFrame(void);
    if (s_stereoMode != VR_STEREO_FULL || !s_autoScreen || !s_vrReady || s_uiInWorld || !PsyX_StereoCanFreeze())
        return;
    printf("[VR-OVR][auto] in-game menu as a 2D screen in the world: on (menu opened)\n");
    s_uiInWorld   = 1;
    s_uiSavedMode = -1;
    PsyX_StereoSetUiScreenNextFrame();
}

/* In-engine cutscene view switch (item 5): when SysFlag_CutsceneActive
 * (+ border state) turns on, switch to the Cutscene_View mode, restore
 * on cutscene end.
 * Manual KP_ENTER during the cutscene cancels the restore (user wins). */
static void VrOvr_CutsceneViewUpdate(void)
{
    extern int VrTracking_CutsceneActive(void);
    extern int VrTracking_GetCutsceneViewMode(void);
    int active;

    if (!s_vrReady) return;
    active = VrTracking_CutsceneActive();

    if (active && !s_cutActive)
    {
        s_cutActive = 1;
        {
            int target = VrTracking_GetCutsceneViewMode();
            if (target != s_stereoMode)
            {
                s_cutSavedMode = s_stereoMode;
                printf("[VR-OVR][auto] cutscene: mode %d\n", target);
                VrOvr_SetStereoMode(target);
                if (target == VR_STEREO_OFF) VrOvr_EnsureScreenVisible();
            }
        }
    }
    else if (!active && s_cutActive)
    {
        s_cutActive = 0;
        if (s_cutSavedMode >= 0)
        {
            /* Ending INTO GAMEPLAY: the right mode is the gameplay
             * target, not whatever was live before the cutscene - the
             * intro cutscene starts BEFORE the UI->gameplay transition,
             * so the saved mode can be the stale menu-era OFF/SCREEN
             * (the "3D screen after the opening cutscene" bug). Ending
             * anywhere else (straight into a menu) keeps the save. */
            int restore = (s_uiCtxStable == 0) ? s_gameplayMode : s_cutSavedMode;
            printf("[VR-OVR][auto] cutscene end: restoring mode %d\n", restore);
            VrOvr_SetStereoMode(restore);
            s_cutSavedMode = -1;
        }
    }
}

/* SCREEN mode: keep the compositor fed with black so SteamVR presents a
 * clean dark theater void behind the 3D screen (a Scene app that stops
 * submitting gets stale frames / fades - the "full VR ghost" bug). */
static void VrOvr_SubmitBlackBackdrop(void)
{
    if (!s_comp) return;

    if (s_blackTex == 0)
    {
        unsigned char px[4] = {0, 0, 0, 255};
        glGenTextures(1, &s_blackTex);
        glBindTexture(GL_TEXTURE_2D, s_blackTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    VRTextureBounds_t b;
    struct Texture_t tex;
    b.uMin = 0.0f; b.vMin = 0.0f; b.uMax = 1.0f; b.vMax = 1.0f;
    /* UNIFIED SUBMIT HANDLES: the compositor latches the first texture
     * handle per eye for the whole session - so the backdrop must submit
     * the SAME textures FULL mode uses (blackened), via vr_full.c. */
    (void)tex;
    {
        extern void VrFull_SubmitBlack(void* sysTbl, void* compTbl);
        VrFull_SubmitBlack((void*)s_sys, (void*)s_comp);
    }

    s_comp->WaitGetPoses(s_waitPoses, 16, NULL, 0);
    s_waitPosesValid = 1;
}

/* ---- stereo: capture into the current eye texture and submit both ------- */

static void VrOvr_StereoPushFrame(void)
{
    if (!s_vrReady || s_stereoMode != VR_STEREO_FULL || !s_comp) return;

    /* Quarantined FULL composition (pc_port/src/vr_full.c): handles the
     * frame when healthy; on init failure falls through to the legacy
     * direct submit below (warped but functional). */
    {
        extern int VrFull_Push(void* sysTbl, void* compTbl, int* parity,
                               void* outPoses16, int* outPosesValid);
        if (VrFull_Push((void*)s_sys, (void*)s_comp, &s_frameParity,
                        (void*)s_waitPoses, &s_waitPosesValid))
        {
            return;
        }
    }

    int w = 0, h = 0;
    if (!VrOvr_SceneSize(&w, &h)) return;

    if (s_eyeTex[0] == 0 || w != s_eyeTexW || h != s_eyeTexH)
    {
        int i;
        if (s_eyeTex[0] == 0) glGenTextures(2, s_eyeTex);
        for (i = 0; i < 2; i++)
        {
            glBindTexture(GL_TEXTURE_2D, s_eyeTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        s_eyeTexW = w;
        s_eyeTexH = h;
    }

    /* Capture this frame into the CURRENT eye's texture, and stamp it
     * with the head pose it was rendered from. */
    int eye = s_frameParity; /* 0 = left, 1 = right */
    {
        int rw, rh, prevRead;
        VrOvr_SceneReadBegin(&rw, &rh, &prevRead);
        glBindTexture(GL_TEXTURE_2D, s_eyeTex[eye]);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        glBindTexture(GL_TEXTURE_2D, 0);
        VrOvr_SceneReadEnd(prevRead);
    }

    if (s_waitPosesValid && s_waitPoses[0].bPoseIsValid)
    {
        s_eyePose[eye]      = s_waitPoses[0].mDeviceToAbsoluteTracking;
        s_eyePoseValid[eye] = 1;
    }

    /* Submit BOTH eyes, each stamped with ITS OWN capture-time pose -
     * the compositor reprojects the stale eye to the current pose,
     * killing the rotation ghosting. */
    {
        VRTextureBounds_t b;
        b.uMin = 0.0f; b.vMin = 0.0f; b.uMax = 1.0f; b.vMax = 1.0f;

        int i;
        for (i = 0; i < 2; i++)
        {
            if (s_eyePoseValid[i])
            {
                struct VRTextureWithPose_t texp;
                texp.handle      = (void*)(uintptr_t)s_eyeTex[i];
                texp.eType       = ETextureType_TextureType_OpenGL;
                texp.eColorSpace = EColorSpace_ColorSpace_Gamma;
                texp.mDeviceToAbsoluteTracking = s_eyePose[i];
                s_comp->Submit(i == 0 ? EVREye_Eye_Left : EVREye_Eye_Right,
                               (struct Texture_t*)&texp, &b,
                               EVRSubmitFlags_Submit_TextureWithPose);
            }
            else
            {
                struct Texture_t tex;
                tex.handle      = (void*)(uintptr_t)s_eyeTex[i];
                tex.eType       = ETextureType_TextureType_OpenGL;
                tex.eColorSpace = EColorSpace_ColorSpace_Gamma;
                s_comp->Submit(i == 0 ? EVREye_Eye_Left : EVREye_Eye_Right,
                               &tex, &b, EVRSubmitFlags_Submit_Default);
            }
        }
    }

    s_frameParity ^= 1;

    /* CORRECT PACING: wait for the compositor AFTER submitting, so the
     * NEXT frame's game build starts with the freshest predicted pose
     * (standard wait -> render -> submit loop). */
    {
        s_comp->WaitGetPoses(s_waitPoses, 16, NULL, 0);
        s_waitPosesValid = 1;
    }
}

/* 1 once a VR runtime (OpenXR or OpenVR) is up - for code that changes
 * behaviour only when a headset is actually in use. */
int VrOvr_IsReady(void)
{
    return s_vrReady;
}

/* 1 when the frames go through OpenXR (vr_openxr.c), 0 for OpenVR. */
int VrOvr_RuntimeIsOpenXR(void)
{
    return s_runtime == VR_RUNTIME_OPENXR;
}

/* Eye sign for the camera offset this frame (queried by vr_tracking.c
 * during the game's build phase, BEFORE this frame's capture/submit):
 * -1 = left eye, +1 = right eye, 0 = stereo off. */
int VrOvr_StereoEyeSign(void)
{
    if (s_stereoMode == VR_STEREO_OFF) return 0;
    return s_frameParity ? +1 : -1;
}

/* Mono screen: game frame -> LEFT half of the shared texture */
static void VrOvr_PushFrame(void)
{
    {
        static int wasRunning = -1;
        int running = (s_vrReady && s_screenVisible && s_screen != 0);
        if (running != wasRunning)
        {
            printf("[VR-OVR][mono] %s (ready=%d vis=%d screen=%d mode=%d)\n",
                   running ? "RUNNING" : "GATED",
                   s_vrReady, s_screenVisible, (int)(s_screen != 0), s_stereoMode);
            wasRunning = running;
        }
    }
    if (!s_vrReady || !s_screenVisible || s_screen == 0) return;

    int w, h;
    if (!VrOvr_EnsureSharedTex(&w, &h)) return;

    {
        int rw, rh, prevRead;
        VrOvr_SceneReadBegin(&rw, &rh, &prevRead);
        glBindTexture(GL_TEXTURE_2D, s_sbsTex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        glBindTexture(GL_TEXTURE_2D, 0);
        VrOvr_SceneReadEnd(prevRead);
    }

    VrOvr_ApplyOverlaySampling();
    VrOvr_SetSharedOverlayTexture();
}

/* ---- poses ----------------------------------------------------------------- */

static void VrOvr_PollPose(void)
{
    if (!s_vrReady || !s_usePoses) return;

    TrackedDevicePose_t poses[1];

    /* In stereo mode, WaitGetPoses' predicted pose is fresher than a
     * cold GetDeviceToAbsoluteTrackingPose - track from it directly. */
    if (s_stereoMode && s_waitPosesValid && s_waitPoses[0].bPoseIsValid)
    {
        poses[0] = s_waitPoses[0];
    }
    else
    {
        s_sys->GetDeviceToAbsoluteTrackingPose(
            ETrackingUniverseOrigin_TrackingUniverseStanding, 0.0f, poses, 1);
    }

    if (!poses[0].bPoseIsValid) { s_havePose = 0; return; }

    const HmdMatrix34_t* m = &poses[0].mDeviceToAbsoluteTracking;

    s_pos[0] = m->m[0][3];
    s_pos[1] = m->m[1][3];
    s_pos[2] = m->m[2][3];

    /* Euler extraction matching the common triad_openvr convention the
     * Python bridge family uses. If axes misbehave vs the file bridge,
     * this is the place to adjust. */
    s_rot[1] = atan2f(m->m[0][2], m->m[2][2]);                    /* yaw */
    s_rot[0] = asinf(-m->m[1][2]);                                /* pitch */
    s_rot[2] = atan2f(m->m[1][0], m->m[1][1]);                    /* roll */

    s_havePose = 1;
}

/* ---- public API ------------------------------------------------------------ */

/* Called by vr_tracking.c: returns 1 and fills the pose when in-process
 * poses are active and valid; 0 = caller should use the file bridge. */
int VrOvr_GetHeadPose(float pos[3], float rot[3])
{
    if (!s_havePose || !s_usePoses) return 0;
    pos[0] = s_pos[0]; pos[1] = s_pos[1]; pos[2] = s_pos[2];
    rot[0] = s_rot[0]; rot[1] = s_rot[1]; rot[2] = s_rot[2];
    return 1;
}

/* Screen/mode hotkeys - shared by the main frame hook AND the FMV path
 * (roadmap item 8: live mode cycling during playback). Statics inside =
 * one debounce state no matter who calls. */
static void VrOvr_PollModeHotkeys(void)
{
    static int prev9 = 0, prevMul = 0, prevDiv = 0, prevEnt = 0;

    const Uint8* ks = SDL_GetKeyboardState(NULL);
    int n9   = ks[SDL_SCANCODE_KP_9] != 0;
    int nMul = ks[SDL_SCANCODE_KP_MULTIPLY] != 0;
    int nDiv = ks[SDL_SCANCODE_KP_DIVIDE] != 0;
    int nEnt = ks[SDL_SCANCODE_KP_ENTER] != 0;

    if (n9 && !prev9)     { VrOvr_ScreenToggle(); }
    if (nEnt && !prevEnt)
    {
        VrOvr_TryInit();
        if (s_vrReady)
        {
            /* Manual cycle = explicit user choice: cancel any pending
             * auto-screen restore (UI, FMV, or cutscene). */
            s_uiSavedMode  = -1;
            s_fmvSavedMode = -1;
            s_cutSavedMode = -1;

            /* BISECT TEST ORDER: OFF -> FULL -> SCREEN so FULL is
             * reachable with zero prior SCREEN-mode activity.
             * Restore to (s_stereoMode + 1) % 3 after testing. */
            {
                int next = (s_stereoMode == VR_STEREO_OFF)  ? VR_STEREO_FULL
                         : (s_stereoMode == VR_STEREO_FULL) ? VR_STEREO_SCREEN
                                                            : VR_STEREO_OFF;
                {   /* manual pick becomes the PERSISTED target (item 6) */
                    extern void VrTracking_SetGameViewMode(int m);
                    VrTracking_SetGameViewMode(next);
                }
                s_gameplayMode = next;
                VrOvr_SetStereoMode(next);
                if (next == VR_STEREO_OFF) VrOvr_EnsureScreenVisible();
            }
        }
    }
    if (nMul && !prevMul) { VrOvr_ScreenSize(+0.25f); }
    if (nDiv && !prevDiv) { VrOvr_ScreenSize(-0.25f); }

    prev9 = n9; prevMul = nMul; prevDiv = nDiv; prevEnt = nEnt;
}

/* ---- PGXP stereo ("the summit") params ------------------------------------
 * Pushed every frame: FULL mode = true per-vertex GPU stereo in PsyX
 * (both eyes same frame - no alternating, no ghosting). The CPU-side
 * alternating camera offset in vr_tracking.c is gated off in lockstep.
 * Convergence: world-unit distance of the zero-disparity (screen) plane,
 * now driven LIVE by the existing vr_stereo_convergence slider/hotkeys
 * (same control as the SBS screen). Mapping: slider value * 25 GTE units,
 * so the default 80 -> 2000 units (~close-NPC range at vr_scale 2.5);
 * slider 0 -> converge at infinity (everything pops toward you). */
#define VR_SUMMIT_CONV_UNITS_PER_STEP 25.0f

static void VrOvr_PushSummitParams(void)
{
    extern void  PsyX_Stereo_SetParams(int active, float eyeSepUnits, float convDistUnits);
    extern float VrTracking_GetEyeSepUnits(void);
    extern int   VrTracking_GetConvergencePx(void);
    extern void  VrTracking_SetGpuStereo(int on);

    int summit = (s_vrReady && s_stereoMode == VR_STEREO_FULL) ? 1 : 0;

    /* The stereo shift is per vertex, from its view depth (a_pgxp.z) when
     * PGXP records it; without PGXP world polygons take their OT-bucket depth
     * in the shader (u_stereoWorld), so use_pgxp is the player's choice here
     * as anywhere. It used to be forced on in Full VR. PENDING: the VR
     * jitter Renan saw (geometry and textures, 26/10) - PGXP off is his test. */
    if (summit)
    {
        extern int g_PsxUsePgxp;
        static int s_logged = -1;
        if (s_logged != g_PsxUsePgxp)
        {
            s_logged = g_PsxUsePgxp;
            VrOvr_Log("[VR-OVR] Full VR with PGXP %s (use_pgxp)\n", g_PsxUsePgxp ? "on" : "off");
        }
    }

    VrTracking_SetGpuStereo(summit);
    PsyX_Stereo_SetParams(summit, VrTracking_GetEyeSepUnits(),
                          (float)VrTracking_GetConvergencePx()
                              * VR_SUMMIT_CONV_UNITS_PER_STEP);
}

/* Controller grip poses for the IK body (vr_ik.c), refreshed with the
 * buttons each frame, in the same standing space as the head pose. */
static HmdMatrix34_t s_handM[2];
static int           s_handOk[2];

/* Right controller pointing ray (OpenXR aim pose; OpenVR's legacy device
 * pose already points like one), for the held weapon and its shots. */
static HmdMatrix34_t s_aimM;
static int           s_aimOk;

/* Hand pose (0 = left, 1 = right): position (meters) and rotation rows, in
 * the standing tracking space (OpenVR axes). 0 = not tracked. */
int VrOvr_GetHandPose(int hand, float pos[3], float rot[3][3])
{
    int i, j;
    if (hand < 0 || hand > 1 || !s_handOk[hand]) return 0;
    for (i = 0; i < 3; i++)
    {
        pos[i] = s_handM[hand].m[i][3];
        for (j = 0; j < 3; j++) rot[i][j] = s_handM[hand].m[i][j];
    }
    return 1;
}

/* Same as VrOvr_GetHandPose, for the right controller's pointing ray. */
int VrOvr_GetAimPose(float pos[3], float rot[3][3])
{
    int i, j;
    if (!s_aimOk) return 0;
    for (i = 0; i < 3; i++)
    {
        pos[i] = s_aimM.m[i][3];
        for (j = 0; j < 3; j++) rot[i][j] = s_aimM.m[i][j];
    }
    return 1;
}

static void VrOvr_PollHandPoses(void)
{
    s_handOk[0] = s_handOk[1] = 0;
    s_aimOk = 0;
    if (!s_vrReady) return;

    if (s_runtime == VR_RUNTIME_OPENXR)
    {
        extern int VrXr_GetHandPose(int hand, HmdMatrix34_t* out);
        extern int VrXr_GetAimPose(HmdMatrix34_t* out);
        s_handOk[0] = VrXr_GetHandPose(0, &s_handM[0]);
        s_handOk[1] = VrXr_GetHandPose(1, &s_handM[1]);
        s_aimOk     = VrXr_GetAimPose(&s_aimM);
    }
    else if (s_sys && s_sys->GetDeviceToAbsoluteTrackingPose && s_sys->GetTrackedDeviceIndexForControllerRole)
    {
        TrackedDevicePose_t poses[16];
        int h;
        s_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding,
                                               0.0f, poses, 16);
        for (h = 0; h < 2; h++)
        {
            TrackedDeviceIndex_t idx = s_sys->GetTrackedDeviceIndexForControllerRole(
                h == 0 ? ETrackedControllerRole_TrackedControllerRole_LeftHand
                       : ETrackedControllerRole_TrackedControllerRole_RightHand);
            if (idx < 16 && poses[idx].bPoseIsValid)
            {
                s_handM[h]  = poses[idx].mDeviceToAbsoluteTracking;
                s_handOk[h] = 1;
            }
        }
        s_aimM  = s_handM[1];
        s_aimOk = s_handOk[1];
    }

    /* A controller drops out of tracking for a frame or a few (out of the
     * headset cameras' view, close to the body): the IK body used to let go
     * of that arm at once - it fell back to the animation and snapped back
     * when tracking returned ("o IK às vezes desativa"). Hold its last good
     * pose for up to VR_HAND_HOLD_MS instead; past that it really is lost. */
    {
#define VR_HAND_HOLD_MS 1000u
        static HmdMatrix34_t s_lastHand[2], s_lastAim;
        static Uint32        s_lastHandAt[2], s_lastAimAt;
        const Uint32         now = SDL_GetTicks();
        int                  h;
        for (h = 0; h < 2; h++)
        {
            if (s_handOk[h])
            {
                s_lastHand[h]   = s_handM[h];
                s_lastHandAt[h] = now ? now : 1;
            }
            else if (s_lastHandAt[h] && now - s_lastHandAt[h] <= VR_HAND_HOLD_MS)
            {
                s_handM[h]  = s_lastHand[h];
                s_handOk[h] = 1;
            }
        }
        if (s_aimOk)
        {
            s_lastAim   = s_aimM;
            s_lastAimAt = now ? now : 1;
        }
        else if (s_lastAimAt && now - s_lastAimAt <= VR_HAND_HOLD_MS)
        {
            s_aimM  = s_lastAim;
            s_aimOk = 1;
        }
    }
}

/* Motion controllers -> virtual gamepad (vr_input.c), every frame. OpenVR
 * uses the legacy controller state (SteamVR's Touch legacy binding has no
 * menu button, so the left stick click doubles as Start there). */
static void VrOvr_PollControllers(void)
{
    VrPadState st;
    memset(&st, 0, sizeof(st));
    if (!s_vrReady) return;

    if (s_runtime == VR_RUNTIME_OPENXR)
    {
        extern void VrXr_PollInput(VrPadState* out);
        VrXr_PollInput(&st);
    }
    else if (s_sys && s_sys->GetControllerState && s_sys->GetTrackedDeviceIndexForControllerRole)
    {
        #define VR_BTN(cs, id) (((cs).ulButtonPressed & (1ull << (id))) != 0)
        VRControllerState_t cs;
        TrackedDeviceIndex_t li = s_sys->GetTrackedDeviceIndexForControllerRole(
            ETrackedControllerRole_TrackedControllerRole_LeftHand);
        TrackedDeviceIndex_t ri = s_sys->GetTrackedDeviceIndexForControllerRole(
            ETrackedControllerRole_TrackedControllerRole_RightHand);

        if (li != k_unTrackedDeviceIndexInvalid && s_sys->GetControllerState(li, &cs, sizeof(cs)))
        {
            st.valid = 1;
            st.lx    = cs.rAxis[0].x;
            st.ly    = cs.rAxis[0].y;
            st.lt    = cs.rAxis[1].x;
            st.lgrip = VR_BTN(cs, EVRButtonId_k_EButton_Grip) ? 1.0f : cs.rAxis[2].x;
            st.x     = VR_BTN(cs, EVRButtonId_k_EButton_A);
            st.y     = VR_BTN(cs, EVRButtonId_k_EButton_ApplicationMenu);
            st.menu  = VR_BTN(cs, EVRButtonId_k_EButton_SteamVR_Touchpad);
        }
        if (ri != k_unTrackedDeviceIndexInvalid && s_sys->GetControllerState(ri, &cs, sizeof(cs)))
        {
            st.valid  = 1;
            st.rx     = cs.rAxis[0].x;
            st.ry     = cs.rAxis[0].y;
            st.rt     = cs.rAxis[1].x;
            st.rgrip  = VR_BTN(cs, EVRButtonId_k_EButton_Grip) ? 1.0f : cs.rAxis[2].x;
            st.a      = VR_BTN(cs, EVRButtonId_k_EButton_A);
            st.b      = VR_BTN(cs, EVRButtonId_k_EButton_ApplicationMenu);
            st.rstick = VR_BTN(cs, EVRButtonId_k_EButton_SteamVR_Touchpad);
        }
        #undef VR_BTN
    }

    /* Y: hold ~0.6 s = quick settings (PC Options on the page last shown,
     * vr_interact.c VrInteract_RequestPcOptions; the VR quick menu panel is
     * off for now), a tap = Y for the game as before (Triangle / map),
     * delivered on release for a few frames since a tap cannot be told from a
     * hold until then. */
    {
        #define VR_MENU_HOLD_MS 600u
        static Uint32 s_yDown     = 0;
        static int    s_yConsumed = 0;
        static int    s_yPulse    = 0;
        const Uint32  now = SDL_GetTicks();

        if (st.y)
        {
            if (s_yDown == 0) s_yDown = now;
            if (!s_yConsumed && now - s_yDown >= VR_MENU_HOLD_MS)
            {
                extern void VrInteract_RequestPcOptions(void);
                VrInteract_RequestPcOptions();
                s_yConsumed = 1;
            }
            st.y = 0;
        }
        else
        {
            if (s_yDown != 0 && !s_yConsumed) s_yPulse = 4;
            s_yDown     = 0;
            s_yConsumed = 0;
        }
        if (s_yPulse > 0)
        {
            st.y = 1;
            s_yPulse--;
        }
        #undef VR_MENU_HOLD_MS
    }

    /* Grip / wheel / weapon priorities (vr_interact.c) decide what the
     * game sees of the right grip, A, X and the triggers. */
    {
        extern void VrInteract_Process(VrPadState* st);
        VrInteract_Process(&st);
    }

    VrInput_Push(&st);
}

/* Head pose for the NEXT game frame, read at the end of this hook - right
 * after WaitGetPoses opened the next compositor frame. The camera the game
 * builds next is then exactly the pose that frame is submitted with. It used
 * to be read at the START of the hook, from the previous wait: every image
 * went out one frame older than the pose it claimed, so the compositor's
 * rotation correction was off by a frame - the micro-stutter on head turns. */
static void VrOvr_PollPoseForNextFrame(void)
{
    VrOvr_PollPose();
}

/* OpenXR needs a frame every game frame, submitted or not (vr_openxr.c). */
static void VrOvr_RuntimeFrameTick(void)
{
    extern void VrXr_FrameTick(void);
    if (s_vrReady && s_runtime == VR_RUNTIME_OPENXR) VrXr_FrameTick();
}

/* Hook: call once per frame from main_pc.c, right BEFORE PsyX_EndScene. */
void VrOvr_EndFrame(void)
{
    VrOvr_TryInit();

    VrOvr_BootScreenAutoShow();  /* splash/menus visible in-HMD from launch */

    /* FMV just ended (this hook resumed after the FMV path stopped):
     * restore the pre-FMV mode. Runs BEFORE AutoScreenUpdate so a
     * movie-into-menu transition restores FULL first, then the UI
     * auto-screen re-drops it - net result is always correct. */
    if (s_fmvLive)
    {
        s_fmvLive = 0;
        if (s_fmvSavedMode >= 0)
        {
            printf("[VR-OVR][auto] FMV ended: restoring mode %d\n", s_fmvSavedMode);
            VrOvr_SetStereoMode(s_fmvSavedMode);
            s_fmvSavedMode = -1;
        }
    }

    /* Sync targets from config every frame: the in-game VR menu is the
     * source of truth (items 2-6); KP_ENTER writes back through it. */
    {
        extern int VrTracking_GetGameViewMode(void);
        extern int VrTracking_GetAutoScreen(void);
        s_gameplayMode = VrTracking_GetGameViewMode();
        s_autoScreen   = VrTracking_GetAutoScreen();
    }

    VrOvr_PollModeHotkeys();
    VrOvr_AutoScreenUpdate();   /* item 7: menus auto-drop FULL -> SCREEN */
    VrOvr_CutsceneViewUpdate(); /* item 5: in-engine cutscene view mode */

    VrOvr_PushSummitParams();   /* PGXP stereo on/off follows the mode */

    /* Screen_Width / Screen_Distance (VR Tuning menu) apply live */
    {
        extern float VrTracking_GetScreenWidth(void);
        extern float VrTracking_GetScreenDistance(void);
        static float appliedW = -1.0f, appliedD = -1.0f;
        float w = VrTracking_GetScreenWidth(), d = VrTracking_GetScreenDistance();
        if (s_screen != 0 && (w != appliedW || d != appliedD))
        {
            appliedW = w;
            appliedD = d;
            VrOvr_ScreenPlace();
        }
    }

    if (s_stereoMode == VR_STEREO_SCREEN)
    {
        VrOvr_SbsPushFrame();          /* 3D floating screen (SBS overlay) */
        VrOvr_SubmitBlackBackdrop();   /* clean theater void behind it */
    }
    else
    {
        VrOvr_PushFrame();        /* mono floating screen, if visible */
        VrOvr_StereoPushFrame();  /* FULL stereo, if enabled */

        /* A Scene app that never submits gets faded out by SteamVR -
         * which took the mono screen down with it (black-void bug).
         * Keep the theater backdrop up whenever the screen is visible. */
        if (s_stereoMode != VR_STEREO_FULL && s_screenVisible)
        {
            VrOvr_SubmitBlackBackdrop();
        }
    }

    VrOvr_RuntimeFrameTick();
    VrOvr_PollPoseForNextFrame();
    VrOvr_PollControllers();
    VrOvr_PollHandPoses();
}

/* FMV frame push: videos are flat, low-fps content - alternating the
 * eye halves at FMV framerate strobes each eye at 7-15Hz (the judder).
 * Instead: identical frame into BOTH halves = smooth, flat, still on
 * the 3D screen. Called from fmv_player.cpp instead of VrOvr_EndFrame. */
static void VrOvr_EndFrameFmvBody(void);

void VrOvr_EndFrameFmv(void)
{
    VrOvr_EndFrameFmvBody();
    VrOvr_RuntimeFrameTick();
    VrOvr_PollPoseForNextFrame();
    VrOvr_PollControllers();
    VrOvr_PollHandPoses();
}

static void VrOvr_EndFrameFmvBody(void)
{
    VrOvr_TryInit();
    if (!s_vrReady) return;

    VrOvr_BootScreenAutoShow();  /* boot intro movies visible in-HMD */

    /* FMV defaults to the floating screen: drop FULL -> SCREEN for the
     * duration of playback (restored by the main hook when video ends).
     * Manual KP_ENTER during playback cancels the restore. */
    s_fmvLive = 1;
    /* FMV ALWAYS plays on the 3D screen (user decision): pre-rendered
     * video is flat source material - a theater screen is the only view
     * that makes sense for it. The Cutscene_View setting governs
     * IN-ENGINE cutscenes only (VrOvr_CutsceneViewUpdate). */
    if (s_fmvSavedMode < 0 && s_stereoMode != VR_STEREO_SCREEN)
    {
        s_fmvSavedMode = s_stereoMode;
        printf("[VR-OVR][auto] FMV start: 3D SCREEN\n");
        VrOvr_SetStereoMode(VR_STEREO_SCREEN);
    }

    VrOvr_PushSummitParams();   /* keep PsyX stereo state in sync during FMV */

    /* Item 8: LIVE MODE CYCLING DURING PLAYBACK. The FMV player runs its
     * own present loop; if it doesn't pump SDL events, keyboard state
     * goes stale - pump here so KP_ENTER/KP9/size keys stay live.
     * (SDL_PumpEvents only moves OS events into SDL's queue - nothing is
     * consumed, the game's own polling still sees everything.) */
    SDL_PumpEvents();
    VrOvr_PollModeHotkeys();

    if (s_stereoMode == VR_STEREO_SCREEN)
    {
        if (s_screen == 0 || !s_screenVisible) return;

        int w, h;
        if (!VrOvr_EnsureSharedTex(&w, &h)) return;

        int rw, rh, prevRead;
        VrOvr_SceneReadBegin(&rw, &rh, &prevRead);
        glBindTexture(GL_TEXTURE_2D, s_sbsTex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, w, 0, 0, 0, w, h);
        glBindTexture(GL_TEXTURE_2D, 0);
        VrOvr_SceneReadEnd(prevRead);

        VrOvr_ApplyOverlaySampling();
        VrOvr_SetSharedOverlayTexture();
        VrOvr_SubmitBlackBackdrop();
    }
    else if (s_stereoMode == VR_STEREO_FULL)
    {
        /* FULL immersion: compose the video into both eyes (vr_full.c) */
        extern void VrFull_PushFmv(void* sysTbl, void* compTbl);
        VrFull_PushFmv((void*)s_sys, (void*)s_comp);
    }
    else
    {
        VrOvr_PushFrame();
        if (s_screenVisible)
        {
            VrOvr_SubmitBlackBackdrop();
        }
    }
}

/* Chained PsyX post-capture hook: preserves the port's dev-console
 * rendering, then pushes the finished frame to the HMD screen.
 * Installed by main_pc.c as g_PsyX_PostCaptureHook - runs every frame
 * inside PsyX_EndScene, pre-swap (backbuffer complete and current). */
/* Time spent in the VR hooks (composition, copies, submit - and the wait
 * for the headset, which happens inside them), for the frame log. */
static double s_hookMs = 0.0;
static int    s_hookN  = 0;

static void VrOvr_HookTimed(void (*fn)(void))
{
    Uint64 t0 = SDL_GetPerformanceCounter();
    fn();
    s_hookMs += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                (double)SDL_GetPerformanceFrequency();
    s_hookN++;
}

/* Average hook time per frame since the last call (vr_openxr.c log). */
double VrOvr_TakeHookMs(void)
{
    double r = s_hookN ? s_hookMs / s_hookN : 0.0;
    s_hookMs = 0.0;
    s_hookN  = 0;
    return r;
}

void VrOvr_PostCaptureHook(void)
{
    extern void DbgOverlay_Render(void);
    DbgOverlay_Render();   /* the hook's original occupant - keep it working */
    VrOvr_HookTimed(VrOvr_EndFrame);   /* hotkeys + pose poll + HMD frame push */
}
