/*
=============================================================================
 SH1 PC Port - OPENXR BACKEND (behind the OpenVR function tables)
=============================================================================

 The VR code (vr_openvr.c / vr_full.c) talks to OpenVR through three C
 function tables and uses only 14 entries of them. This file implements
 exactly those entries on top of OpenXR and hands vr_openvr.c tables that
 look like OpenVR's - so every mode (FULL, 3D SCREEN, 2D SCREEN) runs
 unchanged on any OpenXR runtime (Virtual Desktop/VDXR, Oculus, WMR,
 SteamVR...).

 Mapping:
   GetRecommendedRenderTargetSize -> view configuration recommended size
   GetProjectionRaw               -> per-view fov tangents (OpenVR sign
                                     convention: top negative, y down)
   GetDeviceToAbsoluteTrackingPose / WaitGetPoses
                                  -> VIEW space located in the "standing"
                                     space (STAGE, or LOCAL dropped 1.6 m)
   WaitGetPoses                   -> also ends the open frame and runs
                                     xrWaitFrame + xrBeginFrame
   Submit                         -> copy into the eye swapchain; the pair
                                     becomes a projection layer at end frame
   Overlay*                       -> one quad layer (two with the
                                     side-by-side flag, one per eye)

 Frames: OpenXR needs a wait/begin/end every frame even when nothing calls
 WaitGetPoses (2D screen without backdrop), so vr_openvr.c calls
 VrXr_FrameTick() at the end of every hook.

 Loader: openxr_loader.dll (or MSYS2's libopenxr_loader.dll) next to the
 exe, loaded at runtime - the game still starts without it.
=============================================================================
*/

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <jni.h>
#include <EGL/egl.h>
#include <android/log.h>
#else
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include "vr_gl.h"

#include "openvr_capi.h"
#include "vr_input.h"
#include "pc_config.h"

#define XR_NO_PROTOTYPES
#if defined(__ANDROID__)
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#else
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/* Quest: the swapchain image struct and the graphics requirements call are
 * the GLES flavours of the desktop ones; the rest of the file is shared. */
#if defined(__ANDROID__)
typedef XrSwapchainImageOpenGLESKHR VrXrSwapchainImage;
#define VRXR_SWAPCHAIN_IMAGE_TYPE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR
#define VRXR_GFX_FUNCS(X) X(xrGetOpenGLESGraphicsRequirementsKHR)
#else
typedef XrSwapchainImageOpenGLKHR VrXrSwapchainImage;
#define VRXR_SWAPCHAIN_IMAGE_TYPE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR
#define VRXR_GFX_FUNCS(X) X(xrGetOpenGLGraphicsRequirementsKHR)
#endif

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

/* "Standing" fallback when the runtime has no STAGE space */
#define VRXR_STANDING_EYE_HEIGHT_M 1.6f

/* ---- OpenXR entry points (loaded at runtime) ----------------------------- */

#define VRXR_FUNCS(X) \
    X(xrDestroyInstance) X(xrDestroySession) \
    X(xrGetSystem) X(xrGetInstanceProperties) \
    X(xrEnumerateViewConfigurationViews) VRXR_GFX_FUNCS(X) \
    X(xrCreateSession) X(xrBeginSession) X(xrEndSession) \
    X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace) \
    X(xrPollEvent) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrLocateViews) X(xrLocateSpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) \
    X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) \
    X(xrGetActionStateVector2f) X(xrCreateActionSpace)

static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;
static PFN_xrCreateInstance      p_xrCreateInstance;
#define X(n) static PFN_##n p_##n;
VRXR_FUNCS(X)
#undef X

/* ---- GL entry points (GL 3.0, not exported by opengl32) ------------------ */

typedef void (APIENTRY *PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_BindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFN_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint,
                                             GLint, GLint, GLbitfield, GLenum);
static PFN_GenFramebuffers      gl_GenFramebuffers;
static PFN_BindFramebuffer      gl_BindFramebuffer;
static PFN_FramebufferTexture2D gl_FramebufferTexture2D;
static PFN_BlitFramebuffer      gl_BlitFramebuffer;

/* ---- state ---------------------------------------------------------------- */

typedef struct
{
    XrSwapchain sc;
    int         w, h;
    uint32_t    count;
    VrXrSwapchainImage img[8];
} VrXrChain;

#define VRXR_MAX_OVERLAYS 4

typedef struct
{
    int               used, visible, sbs;
    float             widthM, texelAspect;
    VRTextureBounds_t bounds;
    XrPosef           pose;
    GLuint            tex;
    VrXrChain         chain;
} VrXrOverlay;

#if defined(__ANDROID__)
static void*      s_loader;
#else
static HMODULE    s_loader;
#endif
static XrInstance s_inst    = XR_NULL_HANDLE;
static XrSystemId s_sysId   = XR_NULL_SYSTEM_ID;
static XrSession  s_session = XR_NULL_HANDLE;
static XrSpace    s_space   = XR_NULL_HANDLE;   /* "standing" */
static XrSpace    s_viewSpace = XR_NULL_HANDLE;
static int64_t    s_format;
static uint32_t   s_recW, s_recH;

static XrSessionState s_state = XR_SESSION_STATE_UNKNOWN;
static int        s_running = 0;   /* between xrBeginSession and xrEndSession */
static int        s_lost    = 0;   /* instance/session gone: everything no-ops */

static int        s_frameOpen = 0;
static XrFrameState s_fs;
static XrView     s_views[2];
static int        s_viewsValid = 0;
static XrFovf     s_fov[2];        /* cached for GetProjectionRaw */
static int        s_fovValid = 0;
static XrPosef    s_head;
static int        s_headValid = 0;

static VrXrChain  s_eyeChain[2];
static int        s_eyeSubmitted[2];
static int        s_waitedThisTick = 0;

static VrXrOverlay s_ovls[VRXR_MAX_OVERLAYS];

static GLuint     s_srcFbo, s_dstFbo;

/* Motion controllers (Oculus Touch profile) -> vr_input.c virtual pad */
enum
{
    VRXR_ACT_LSTICK, VRXR_ACT_RSTICK,                               /* vector2f */
    VRXR_ACT_LTRIG, VRXR_ACT_RTRIG, VRXR_ACT_LGRIP, VRXR_ACT_RGRIP, /* float */
    VRXR_ACT_A, VRXR_ACT_B, VRXR_ACT_X, VRXR_ACT_Y,                 /* boolean */
    VRXR_ACT_LCLICK, VRXR_ACT_RCLICK, VRXR_ACT_MENU,
    VRXR_ACT_LPOSE, VRXR_ACT_RPOSE, VRXR_ACT_RAIM,                  /* pose */
    VRXR_ACT_COUNT
};
extern void VrOvr_Log(const char* fmt, ...);   /* SilentHillVR.log (vr_openvr.c) */
#if defined(__ANDROID__)
#define printf VrOvr_Log
#endif

static XrActionSet s_actSet = XR_NULL_HANDLE;
static XrAction    s_act[VRXR_ACT_COUNT];
static int         s_haveTouchPlus = 0;   /* XR_META_touch_controller_plus enabled */
static int         s_actReady = 0;

/* Controller grip poses (IK body, vr_ik.c), in the standing space at the
 * frame's predicted display time - the same instant as the head pose. */
static XrSpace     s_handSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static XrPosef     s_handPose[2];
static int         s_handValid[2];

/* Right controller aim pose: the pointing ray (-Z), which the held weapon
 * and its shots follow (vr_ik.c / vr_interact.c). */
static XrSpace     s_aimSpace = XR_NULL_HANDLE;
static XrPosef     s_aimPose;
static int         s_aimValid;

static struct VR_IVRSystem_FnTable     s_sysTbl;
static struct VR_IVRCompositor_FnTable s_compTbl;
static struct VR_IVROverlay_FnTable    s_ovlTbl;

/* ---- math ------------------------------------------------------------------ */

static void PoseToMatrix(const XrPosef* p, HmdMatrix34_t* m)
{
    float x = p->orientation.x, y = p->orientation.y;
    float z = p->orientation.z, w = p->orientation.w;

    m->m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    m->m[0][1] = 2.0f * (x * y - z * w);
    m->m[0][2] = 2.0f * (x * z + y * w);
    m->m[1][0] = 2.0f * (x * y + z * w);
    m->m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    m->m[1][2] = 2.0f * (y * z - x * w);
    m->m[2][0] = 2.0f * (x * z - y * w);
    m->m[2][1] = 2.0f * (y * z + x * w);
    m->m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    m->m[0][3] = p->position.x;
    m->m[1][3] = p->position.y;
    m->m[2][3] = p->position.z;
}

static void MatrixToPose(const HmdMatrix34_t* m, XrPosef* p)
{
    float tr = m->m[0][0] + m->m[1][1] + m->m[2][2];
    float x, y, z, w;

    if (tr > 0.0f)
    {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (m->m[2][1] - m->m[1][2]) / s;
        y = (m->m[0][2] - m->m[2][0]) / s;
        z = (m->m[1][0] - m->m[0][1]) / s;
    }
    else if (m->m[0][0] > m->m[1][1] && m->m[0][0] > m->m[2][2])
    {
        float s = sqrtf(1.0f + m->m[0][0] - m->m[1][1] - m->m[2][2]) * 2.0f;
        w = (m->m[2][1] - m->m[1][2]) / s;
        x = 0.25f * s;
        y = (m->m[0][1] + m->m[1][0]) / s;
        z = (m->m[0][2] + m->m[2][0]) / s;
    }
    else if (m->m[1][1] > m->m[2][2])
    {
        float s = sqrtf(1.0f + m->m[1][1] - m->m[0][0] - m->m[2][2]) * 2.0f;
        w = (m->m[0][2] - m->m[2][0]) / s;
        x = (m->m[0][1] + m->m[1][0]) / s;
        y = 0.25f * s;
        z = (m->m[1][2] + m->m[2][1]) / s;
    }
    else
    {
        float s = sqrtf(1.0f + m->m[2][2] - m->m[0][0] - m->m[1][1]) * 2.0f;
        w = (m->m[1][0] - m->m[0][1]) / s;
        x = (m->m[0][2] + m->m[2][0]) / s;
        y = (m->m[1][2] + m->m[2][1]) / s;
        z = 0.25f * s;
    }

    p->orientation.x = x; p->orientation.y = y;
    p->orientation.z = z; p->orientation.w = w;
    p->position.x = m->m[0][3];
    p->position.y = m->m[1][3];
    p->position.z = m->m[2][3];
}

/* ---- GL helpers ------------------------------------------------------------ */

/* GL state around the swapchain copies. Every glGet* / glIsEnabled makes
 * NVIDIA's threaded driver drain its queue, and these ran several times per
 * copy, three copies a frame - so the bindings come from PsyX's shadow copy
 * (PsyX_GlShadowGet) when it is installed. The copy only touches framebuffer
 * bindings and the scissor enable; nothing in the game enables sRGB writes. */
typedef struct
{
    GLuint readFb, drawFb;
    int    scissor;
} VrXrGlState;

static void GlSave(VrXrGlState* s)
{
    extern int PsyX_GlShadowGet(unsigned int* drawFbo, unsigned int* readFbo, int* scissorOn);
    GLint r = 0, d = 0;

    if (PsyX_GlShadowGet(&s->drawFb, &s->readFb, &s->scissor))
        return;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &r);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &d);
    s->readFb  = (GLuint)r;
    s->drawFb  = (GLuint)d;
    s->scissor = glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0;
}

static void GlRestore(const VrXrGlState* s)
{
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, s->readFb);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER, s->drawFb);
    if (s->scissor) glEnable(GL_SCISSOR_TEST);
}

/* Texture sizes, cached per handle and re-read every ~2 s: the textures
 * handed in (eye images, the screen texture) keep their handle across a
 * resize, so the cache must expire, but not every frame (glGetTexLevel*
 * queries sync the driver like any other). */
#define VRXR_TEXSIZE_SLOTS   8
#define VRXR_TEXSIZE_REFRESH 120

static void TexSize(GLuint tex, int* w, int* h)
{
    static struct { GLuint tex; int w, h, age; } cache[VRXR_TEXSIZE_SLOTS];
    static int next = 0;
    GLint prev = 0, tw = 0, th = 0;
    int i;

    for (i = 0; i < VRXR_TEXSIZE_SLOTS; i++)
    {
        if (cache[i].tex == tex && cache[i].tex != 0 && ++cache[i].age < VRXR_TEXSIZE_REFRESH)
        {
            *w = cache[i].w;
            *h = cache[i].h;
            return;
        }
    }

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    *w = tw;
    *h = th;

    for (i = 0; i < VRXR_TEXSIZE_SLOTS; i++)
        if (cache[i].tex == tex) break;
    if (i == VRXR_TEXSIZE_SLOTS)
    {
        i = next;
        next = (next + 1) % VRXR_TEXSIZE_SLOTS;
    }
    cache[i].tex = tex;
    cache[i].w   = tw;
    cache[i].h   = th;
    cache[i].age = 0;
}

/* ---- swapchains ------------------------------------------------------------ */

static int ChainEnsure(VrXrChain* c, int w, int h)
{
    if (c->sc != XR_NULL_HANDLE && c->w == w && c->h == h) return 1;
    if (c->sc != XR_NULL_HANDLE)
    {
        p_xrDestroySwapchain(c->sc);
        c->sc = XR_NULL_HANDLE;
    }

    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format      = s_format;
    ci.sampleCount = 1;
    ci.width       = (uint32_t)w;
    ci.height      = (uint32_t)h;
    ci.faceCount   = 1;
    ci.arraySize   = 1;
    ci.mipCount    = 1;
    if (XR_FAILED(p_xrCreateSwapchain(s_session, &ci, &c->sc)))
    {
        printf("[VR-XR] swapchain %dx%d failed\n", w, h);
        c->sc = XR_NULL_HANDLE;
        return 0;
    }

    uint32_t i;
    for (i = 0; i < 8; i++)
    {
        c->img[i].type = VRXR_SWAPCHAIN_IMAGE_TYPE;
        c->img[i].next = NULL;
    }
    c->count = 0;
    p_xrEnumerateSwapchainImages(c->sc, 8, &c->count, (XrSwapchainImageBaseHeader*)c->img);
    c->w = w;
    c->h = h;
    return 1;
}

/* Copy a GL texture into the next image of a swapchain (raw bytes: sRGB
 * swapchains get the gamma-encoded game image unconverted, which is what
 * OpenVR's "gamma" textures meant). */
static int ChainCopy(VrXrChain* c, GLuint src, int srcW, int srcH)
{
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo    wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    VrXrGlState st;

    if (c->sc == XR_NULL_HANDLE) return 0;
    if (XR_FAILED(p_xrAcquireSwapchainImage(c->sc, &ai, &idx)) || idx >= c->count) return 0;
    wi.timeout = XR_INFINITE_DURATION;
    p_xrWaitSwapchainImage(c->sc, &wi);

    GlSave(&st);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, s_srcFbo);
    gl_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER, s_dstFbo);
    gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            c->img[idx].image, 0);
    {
#if defined(__ANDROID__)
        static int diagFrame = 0, diagCount = 0;
        int diag = (diagCount < 60 && (++diagFrame % 360) == 1);   /* Quest bring-up: reads pixels back */
#else
        static int diagCount = 0;
        const int  diag = 0;
#endif
        unsigned char sp[4] = { 0, 0, 0, 0 }, dp[4] = { 0, 0, 0, 0 };
        GLenum rs = 0, ds = 0, e1 = 0, e2 = 0;
        if (diag)
        {
            while (glGetError() != GL_NO_ERROR) {}
            rs = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            ds = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            glReadPixels(srcW / 2, srcH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sp);
            e1 = glGetError();
        }
        gl_BlitFramebuffer(0, 0, srcW, srcH, 0, 0, c->w, c->h, GL_COLOR_BUFFER_BIT,
                           (srcW == c->w && srcH == c->h) ? GL_NEAREST : GL_LINEAR);
        if (diag)
        {
            e2 = glGetError();
            gl_BindFramebuffer(GL_READ_FRAMEBUFFER, s_dstFbo);
            glReadPixels(c->w / 2, c->h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, dp);
            printf("[VR-DIAG] chain %p src tex %u %dx%d -> %dx%d img %u fmt 0x%llX | status r=0x%X d=0x%X | src %u,%u,%u,%u dst %u,%u,%u,%u | err read=0x%X blit=0x%X after=0x%X\n",
                   (void*)c, (unsigned)src, srcW, srcH, c->w, c->h, (unsigned)c->img[idx].image,
                   (unsigned long long)s_format, (unsigned)rs, (unsigned)ds,
                   sp[0], sp[1], sp[2], sp[3], dp[0], dp[1], dp[2], dp[3],
                   (unsigned)e1, (unsigned)e2, (unsigned)glGetError());
            diagCount++;
        }
    }
    GlRestore(&st);

    p_xrReleaseSwapchainImage(c->sc, &ri);
    return 1;
}

/* ---- session / frames -------------------------------------------------------- */
/* XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING seen (taken once by
 * vr_tracking.c VrReadBridge) */
static int s_spaceChanged;
int VrXr_TakeSpaceChanged(void)
{
    const int c = s_spaceChanged;
    s_spaceChanged = 0;
    return c;
}


static void PollEvents(void)
{
    XrEventDataBuffer ev;

    for (;;)
    {
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
        if (p_xrPollEvent(s_inst, &ev) != XR_SUCCESS) break;

        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
            s_state = e->state;
            printf("[VR-XR] session state %d\n", (int)s_state);

            if (s_state == XR_SESSION_STATE_READY && !s_running)
            {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(p_xrBeginSession(s_session, &bi)))
                    s_running = 1;
            }
            else if (s_state == XR_SESSION_STATE_STOPPING && s_running)
            {
                p_xrEndSession(s_session);
                s_running   = 0;
                s_frameOpen = 0;
            }
            else if (s_state == XR_SESSION_STATE_EXITING ||
                     s_state == XR_SESSION_STATE_LOSS_PENDING)
            {
                s_lost      = 1;
                s_running   = 0;
                s_frameOpen = 0;
            }
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
        {
            static PFN_xrGetCurrentInteractionProfile getProf = NULL;
            static PFN_xrPathToString                 pathStr = NULL;
            int h;
            if (!getProf)
                p_xrGetInstanceProcAddr(s_inst, "xrGetCurrentInteractionProfile", (PFN_xrVoidFunction*)&getProf);
            if (!pathStr)
                p_xrGetInstanceProcAddr(s_inst, "xrPathToString", (PFN_xrVoidFunction*)&pathStr);
            for (h = 0; h < 2; h++)
            {
                XrPath hp = XR_NULL_PATH;
                XrInteractionProfileState ps = { XR_TYPE_INTERACTION_PROFILE_STATE };
                char buf[256] = "(none)";
                uint32_t len = 0;
                p_xrStringToPath(s_inst, h ? "/user/hand/right" : "/user/hand/left", &hp);
                if (getProf && XR_SUCCEEDED(getProf(s_session, hp, &ps)) &&
                    ps.interactionProfile != XR_NULL_PATH && pathStr)
                    pathStr(s_inst, ps.interactionProfile, sizeof(buf), &len, buf);
                printf("[VR-INPUT] interaction profile changed: %s hand = %s\n", h ? "right" : "left", buf);
            }
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            /* the system recenter (holding the Quest button) redefines the
             * tracking space: the game's origin and the measured body height
             * must be taken again in the new one (vr_tracking.c) */
            const XrEventDataReferenceSpaceChangePending* e = (const XrEventDataReferenceSpaceChangePending*)&ev;
            s_spaceChanged = 1;
            VrOvr_Log("[VR-XR] reference space %d changed (system recenter)\n", (int)e->referenceSpaceType);
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            s_lost      = 1;
            s_running   = 0;
            s_frameOpen = 0;
        }
    }
}

static void EndFrameIfOpen(void)
{
    XrCompositionLayerProjection     proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerQuad           quads[VRXR_MAX_OVERLAYS * 2];
    const XrCompositionLayerBaseHeader* layers[1 + VRXR_MAX_OVERLAYS * 2];
    uint32_t nLayers = 0, nQuads = 0;
    int i;

    if (!s_frameOpen) return;
    s_frameOpen = 0;

    if (s_fs.shouldRender && s_viewsValid && s_eyeSubmitted[0] && s_eyeSubmitted[1])
    {
        for (i = 0; i < 2; i++)
        {
            memset(&pv[i], 0, sizeof(pv[i]));
            pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[i].pose = s_views[i].pose;
            pv[i].fov  = s_views[i].fov;
            pv[i].subImage.swapchain = s_eyeChain[i].sc;
            pv[i].subImage.imageRect.extent.width  = s_eyeChain[i].w;
            pv[i].subImage.imageRect.extent.height = s_eyeChain[i].h;
        }
        proj.space     = s_space;
        proj.viewCount = 2;
        proj.views     = pv;
        layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&proj;
    }

    if (s_fs.shouldRender)
    {
        for (i = 0; i < VRXR_MAX_OVERLAYS; i++)
        {
            VrXrOverlay* o = &s_ovls[i];
            int tw, th, x0, x1, y0, y1, halves, k;

            if (!o->used || !o->visible || o->tex == 0) continue;
            TexSize(o->tex, &tw, &th);
            if (tw <= 0 || th <= 0) continue;
            if (!ChainEnsure(&o->chain, tw, th)) continue;
            if (!ChainCopy(&o->chain, o->tex, tw, th)) continue;

            x0 = (int)(o->bounds.uMin * tw + 0.5f);
            x1 = (int)(o->bounds.uMax * tw + 0.5f);
            y0 = (int)(o->bounds.vMin * th + 0.5f);
            y1 = (int)(o->bounds.vMax * th + 0.5f);
            if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
            if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
            halves = o->sbs ? 2 : 1;
            if ((x1 - x0) / halves <= 0 || y1 <= y0) continue;

            for (k = 0; k < halves; k++)
            {
                XrCompositionLayerQuad* q = &quads[nQuads++];
                int rw = (x1 - x0) / halves;

                memset(q, 0, sizeof(*q));
                q->type  = XR_TYPE_COMPOSITION_LAYER_QUAD;
                q->space = s_space;
                q->eyeVisibility = (halves == 1) ? XR_EYE_VISIBILITY_BOTH
                                 : (k == 0) ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                q->subImage.swapchain = o->chain.sc;
                q->subImage.imageRect.offset.x = x0 + k * rw;
                q->subImage.imageRect.offset.y = y0;
                q->subImage.imageRect.extent.width  = rw;
                q->subImage.imageRect.extent.height = y1 - y0;
                q->pose = o->pose;
                q->size.width  = o->widthM;
                q->size.height = o->widthM * (float)(y1 - y0) /
                                 ((float)rw * (o->texelAspect > 0.0f ? o->texelAspect : 1.0f));
                layers[nLayers++] = (const XrCompositionLayerBaseHeader*)q;
            }
        }
    }

    {
        XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
        ei.displayTime          = s_fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount           = nLayers;
        ei.layers               = layers;
        {
            static int diagFrame = 0, diagCount = 0;
            XrResult er = p_xrEndFrame(s_session, &ei);
            if (diagCount < 40 && ((++diagFrame % 360) == 1 || XR_FAILED(er)))
            {
                int nOvl = 0;
                for (i = 0; i < VRXR_MAX_OVERLAYS; i++)
                    if (s_ovls[i].used) nOvl++;
                printf("[VR-DIAG] endFrame r=%d layers=%u (proj=%d quads=%u) shouldRender=%d overlays used=%d",
                       (int)er, nLayers, (int)(nLayers > nQuads), nQuads, (int)s_fs.shouldRender, nOvl);
                for (i = 0; i < VRXR_MAX_OVERLAYS; i++)
                {
                    int tw = 0, th = 0;
                    if (!s_ovls[i].used) continue;
                    if (s_ovls[i].tex) TexSize(s_ovls[i].tex, &tw, &th);
                    printf(" [%d vis=%d tex=%u %dx%d w=%.2f pos %.2f,%.2f,%.2f]", i, s_ovls[i].visible,
                           (unsigned)s_ovls[i].tex, tw, th, s_ovls[i].widthM,
                           s_ovls[i].pose.position.x, s_ovls[i].pose.position.y, s_ovls[i].pose.position.z);
                }
                printf("\n");
                diagCount++;
            }
        }
    }

    s_eyeSubmitted[0] = s_eyeSubmitted[1] = 0;
}

#if defined(__ANDROID__)
/* GPU TIME (Quest perf log): a GL_EXT_disjoint_timer_query time-elapsed
 * query around each frame's GL work (frame begin -> next frame's wait).
 * Results are collected a few frames later, never waited for. */
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
#define GPU_Q_COUNT 6
static GLuint s_gpuQ[GPU_Q_COUNT];
static int    s_gpuQBusy[GPU_Q_COUNT];
static int    s_gpuQOpen = -1, s_gpuQOk = -1, s_gpuN = 0;
static double s_gpuMs = 0.0;

static void GpuTimerEnd(void)
{
    int i;
    if (s_gpuQOk != 1) return;
    if (s_gpuQOpen >= 0)
    {
        glEndQuery(GL_TIME_ELAPSED_EXT);
        s_gpuQBusy[s_gpuQOpen] = 1;
        s_gpuQOpen = -1;
    }
    for (i = 0; i < GPU_Q_COUNT; i++)
    {
        GLuint avail = 0, ns = 0;
        GLint  disjoint = 0;
        if (!s_gpuQBusy[i]) continue;
        glGetQueryObjectuiv(s_gpuQ[i], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) continue;
        glGetQueryObjectuiv(s_gpuQ[i], GL_QUERY_RESULT, &ns);
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        if (!disjoint) { s_gpuMs += (double)ns / 1e6; s_gpuN++; }
        s_gpuQBusy[i] = 0;
    }
}

static void GpuTimerBegin(void)
{
    int i;
    if (s_gpuQOk < 0)
    {
        GLint n = 0;
        s_gpuQOk = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (i = 0; i < n; i++)
        {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && strcmp(e, "GL_EXT_disjoint_timer_query") == 0) { s_gpuQOk = 1; break; }
        }
        if (s_gpuQOk) glGenQueries(GPU_Q_COUNT, s_gpuQ);
        VrOvr_Log("[VR-XR] GPU timer queries: %s\n", s_gpuQOk ? "on" : "not supported");
    }
    if (s_gpuQOk != 1 || s_gpuQOpen >= 0) return;
    for (i = 0; i < GPU_Q_COUNT; i++)
    {
        if (!s_gpuQBusy[i])
        {
            glBeginQuery(GL_TIME_ELAPSED_EXT, s_gpuQ[i]);
            s_gpuQOpen = i;
            return;
        }
    }
}
#endif

static void BeginNextFrame(void)
{
    PollEvents();
    if (!s_running || s_lost) return;
#if defined(__ANDROID__)
    GpuTimerEnd();
#endif

    {
        static Uint64 s_workStart = 0;   /* when the previous frame began */
        static double s_workMs = 0.0, s_waitMs = 0.0;
        XrFrameWaitInfo  wi = { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
        Uint64 freq = SDL_GetPerformanceFrequency();
        Uint64 t1   = SDL_GetPerformanceCounter(), t2;

        s_fs.type = XR_TYPE_FRAME_STATE;
        s_fs.next = NULL;
        if (XR_FAILED(p_xrWaitFrame(s_session, &wi, &s_fs))) return;
        t2 = SDL_GetPerformanceCounter();
        if (XR_FAILED(p_xrBeginFrame(s_session, &bi))) return;
        s_frameOpen = 1;
#if defined(__ANDROID__)
        GpuTimerBegin();
#endif
        {
            extern void VrTracking_StateLog(void);
            VrTracking_StateLog();
        }

        /* frame rate vs the headset's every 5 s, split into the game's own
         * work (frame begin -> next wait) and time blocked in xrWaitFrame.
         * work > headset period = the game is too slow; small work with
         * uneven fps = pacing. */
        if (s_workStart) s_workMs += (double)(t1 - s_workStart) * 1000.0 / (double)freq;
        s_waitMs   += (double)(t2 - t1) * 1000.0 / (double)freq;

        /* SLOW FRAMES. The 5 s averages below hide a drop: one 60 ms frame in
         * 450 barely moves them. Every frame that misses the headset's period
         * is counted; the worst one in each 5 s goes on the [VR-PERF] line,
         * and a frame that takes over twice the period is logged on its own
         * (at most once a second) with what the game was doing then: game
         * work (begin -> next wait), time blocked in xrWaitFrame, stereo
         * draws and their CPU, state, room, position, cutscene, menu. */
        {
            extern void PsyX_Stereo_Totals(unsigned* draws, double* cpuMs);
            extern void VrTracking_PerfContext(char* buf, int n);
            static unsigned s_drawsPrev;
            static double   s_cpuPrev, s_worstMs, s_worstWork, s_worstWait;
            static unsigned s_worstDraws;
            static int      s_late, s_frames;
            static Uint32   s_lastSpike, s_t0;
            const double    workMs  = s_workStart ? (double)(t1 - s_workStart) * 1000.0 / (double)freq : 0.0;
            const double    waitMs  = (double)(t2 - t1) * 1000.0 / (double)freq;
            const double    frameMs = workMs + waitMs;
            const double    period  = s_fs.predictedDisplayPeriod > 0 ? (double)s_fs.predictedDisplayPeriod / 1e6 : 11.1;
            unsigned        draws;
            double          cpu;
            const Uint32    now = SDL_GetTicks();

            PsyX_Stereo_Totals(&draws, &cpu);
            s_frames++;
            if (frameMs > period * 1.5) s_late++;
            if (frameMs > s_worstMs)
            {
                s_worstMs = frameMs; s_worstWork = workMs; s_worstWait = waitMs;
                s_worstDraws = draws - s_drawsPrev;
            }
            if (s_workStart && frameMs > period * 2.0 && now - s_lastSpike >= 1000)
            {
                char ctx[160];
                VrTracking_PerfContext(ctx, sizeof(ctx));
                VrOvr_Log("[VR-PERF] slow frame %.1f ms (game %.1f, wait %.1f; headset period %.1f) stereo draws %u (%.1f ms CPU) | %s\n",
                          frameMs, workMs, waitMs, period, draws - s_drawsPrev, cpu - s_cpuPrev, ctx);
                s_lastSpike = now;
            }
            if (s_t0 == 0) s_t0 = now;
            if (now - s_t0 >= 5000)
            {
                VrOvr_Log("[VR-PERF] %d of %d frames late (>1.5x period); worst %.1f ms (game %.1f, wait %.1f, stereo draws %u)\n",
                          s_late, s_frames, s_worstMs, s_worstWork, s_worstWait, s_worstDraws);
                s_t0 = now; s_late = s_frames = 0; s_worstMs = 0.0;
            }
            s_drawsPrev = draws;
            s_cpuPrev   = cpu;
        }
        s_workStart = t2;
        {
            static Uint32 t0 = 0;
            static int    n  = 0;
            Uint32 now = SDL_GetTicks();
            if (t0 == 0) t0 = now;
            if (++n, now - t0 >= 5000)
            {
                extern int    PsyX_Stereo_TakeDrawCount(void);
                extern double PsyX_Stereo_TakeCpuMs(void);
                extern double VrOvr_TakeHookMs(void);
                double hookMs = VrOvr_TakeHookMs() - s_waitMs / n;   /* the wait runs inside the hook */
                VrOvr_Log("[VR-XR] %.1f fps, headset %.1f Hz | per frame: game %.1f ms (of which VR hook %.1f ms), waiting for headset %.1f ms, stereo draws %d (%.1f ms CPU)\n",
                          n * 1000.0 / (double)(now - t0),
                          s_fs.predictedDisplayPeriod > 0 ? 1e9 / (double)s_fs.predictedDisplayPeriod : 0.0,
                          s_workMs / n, hookMs > 0.0 ? hookMs : 0.0, s_waitMs / n, PsyX_Stereo_TakeDrawCount() / n, PsyX_Stereo_TakeCpuMs() / n);
#if defined(__ANDROID__)
                {
                    /* where the game's time goes: the draw lists (CPU parse +
                     * GL submit), the VR hook, the rest (game logic etc.), and
                     * the GPU's own time for the frame */
                    extern double PsyX_Perf_TakeOtMs(void);
                    const double ot   = PsyX_Perf_TakeOtMs() / n;
                    const double hk   = hookMs > 0.0 ? hookMs : 0.0;
                    const double rest = s_workMs / n - ot - hk;
                    VrOvr_Log("[VR-SPLIT] CPU: draw lists %.1f ms, VR hook %.1f ms, game logic + rest %.1f ms | GPU %.1f ms per frame (%d measured)\n",
                              ot, hk, rest > 0.0 ? rest : 0.0, s_gpuN ? s_gpuMs / s_gpuN : 0.0, s_gpuN);
                    s_gpuMs = 0.0;
                    s_gpuN  = 0;
                }
#endif
                t0 = now;
                n  = 0;
                s_workMs = s_waitMs = 0.0;
            }
        }
    }

    {
        XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
        XrViewState      vs = { XR_TYPE_VIEW_STATE };
        uint32_t n = 0;

        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = s_fs.predictedDisplayTime;
        li.space       = s_space;
        s_views[0].type = s_views[1].type = XR_TYPE_VIEW;
        s_views[0].next = s_views[1].next = NULL;
        s_viewsValid = XR_SUCCEEDED(p_xrLocateViews(s_session, &li, &vs, 2, &n, s_views)) &&
                       n == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (s_viewsValid && !s_fovValid)
        {
            int e;
            s_fov[0]  = s_views[0].fov;
            s_fov[1]  = s_views[1].fov;
            s_fovValid = 1;
            for (e = 0; e < 2; e++)
                VrOvr_Log("[VR-XR] eye %d fov deg: left %.1f right %.1f up %.1f down %.1f (%.1f x %.1f), eye pos x %.4f m\n",
                          e, s_fov[e].angleLeft * 57.29578f, s_fov[e].angleRight * 57.29578f,
                          s_fov[e].angleUp * 57.29578f, s_fov[e].angleDown * 57.29578f,
                          (s_fov[e].angleRight - s_fov[e].angleLeft) * 57.29578f,
                          (s_fov[e].angleUp - s_fov[e].angleDown) * 57.29578f,
                          s_views[e].pose.position.x);
        }
    }

    {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        s_headValid = XR_SUCCEEDED(p_xrLocateSpace(s_viewSpace, s_space,
                                                   s_fs.predictedDisplayTime, &loc)) &&
                      (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (s_headValid) s_head = loc.pose;
    }

    {
        int h;
        for (h = 0; h < 2; h++)
        {
            XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                              XR_SPACE_LOCATION_POSITION_VALID_BIT;
            s_handValid[h] = s_handSpace[h] != XR_NULL_HANDLE &&
                             XR_SUCCEEDED(p_xrLocateSpace(s_handSpace[h], s_space,
                                                          s_fs.predictedDisplayTime, &loc)) &&
                             (loc.locationFlags & need) == need;
            if (s_handValid[h]) s_handPose[h] = loc.pose;
        }
    }

    {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                          XR_SPACE_LOCATION_POSITION_VALID_BIT;
        s_aimValid = s_aimSpace != XR_NULL_HANDLE &&
                     XR_SUCCEEDED(p_xrLocateSpace(s_aimSpace, s_space,
                                                  s_fs.predictedDisplayTime, &loc)) &&
                     (loc.locationFlags & need) == need;
        if (s_aimValid) s_aimPose = loc.pose;
    }
}

static void FillHeadPose(TrackedDevicePose_t* p)
{
    memset(p, 0, sizeof(*p));
    p->bDeviceIsConnected = 1;
    if (!s_headValid)
    {
        p->eTrackingResult = ETrackingResult_TrackingResult_Running_OutOfRange;
        return;
    }
    PoseToMatrix(&s_head, &p->mDeviceToAbsoluteTracking);
    p->eTrackingResult = ETrackingResult_TrackingResult_Running_OK;
    p->bPoseIsValid    = 1;
}

/* ---- IVRSystem ---------------------------------------------------------------- */

static void OPENVR_FNTABLE_CALLTYPE Sys_GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h)
{
    *w = s_recW;
    *h = s_recH;
}

static void OPENVR_FNTABLE_CALLTYPE Sys_GetProjectionRaw(EVREye eye, float* l, float* r,
                                                        float* t, float* b)
{
    XrFovf f;
    if (s_fovValid)
        f = s_fov[eye == EVREye_Eye_Right ? 1 : 0];
    else
    {
        f.angleLeft = -0.785f; f.angleRight = 0.785f;
        f.angleUp   =  0.785f; f.angleDown  = -0.785f;
    }
    *l = tanf(f.angleLeft);
    *r = tanf(f.angleRight);
    *t = -tanf(f.angleUp);     /* OpenVR: y down, top negative */
    *b = -tanf(f.angleDown);
}

static void OPENVR_FNTABLE_CALLTYPE Sys_GetDeviceToAbsoluteTrackingPose(
    ETrackingUniverseOrigin origin, float secs, struct TrackedDevicePose_t* poses, uint32_t count)
{
    uint32_t i;
    (void)origin; (void)secs;
    for (i = 0; i < count; i++)
    {
        if (i == 0) FillHeadPose(&poses[0]);
        else memset(&poses[i], 0, sizeof(poses[i]));
    }
}

/* ---- IVRCompositor ------------------------------------------------------------ */

static EVRCompositorError OPENVR_FNTABLE_CALLTYPE Comp_WaitGetPoses(
    struct TrackedDevicePose_t* render, uint32_t nRender,
    struct TrackedDevicePose_t* game, uint32_t nGame)
{
    if (s_lost) return EVRCompositorError_VRCompositorError_DoNotHaveFocus;

    EndFrameIfOpen();
    BeginNextFrame();
    s_waitedThisTick = 1;

    Sys_GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding,
                                        0.0f, render, nRender);
    if (game) Sys_GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding,
                                                  0.0f, game, nGame);
    return EVRCompositorError_VRCompositorError_None;
}

static EVRCompositorError OPENVR_FNTABLE_CALLTYPE Comp_Submit(
    EVREye eye, struct Texture_t* tex, struct VRTextureBounds_t* bounds, EVRSubmitFlags flags)
{
    int i = (eye == EVREye_Eye_Right) ? 1 : 0, w, h;
    (void)bounds; (void)flags;

    if (s_lost || !s_frameOpen || !tex || !tex->handle)
        return EVRCompositorError_VRCompositorError_DoNotHaveFocus;

    TexSize((GLuint)(uintptr_t)tex->handle, &w, &h);
    if (w <= 0 || h <= 0) return EVRCompositorError_VRCompositorError_InvalidTexture;
    if (!ChainCopy(&s_eyeChain[i], (GLuint)(uintptr_t)tex->handle, w, h))
        return EVRCompositorError_VRCompositorError_InvalidTexture;

    s_eyeSubmitted[i] = 1;
    return EVRCompositorError_VRCompositorError_None;
}

/* ---- IVROverlay --------------------------------------------------------------- */

static VrXrOverlay* Ovl(VROverlayHandle_t h)
{
    if (h == 0 || h > VRXR_MAX_OVERLAYS || !s_ovls[h - 1].used) return NULL;
    return &s_ovls[h - 1];
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_CreateOverlay(char* key, char* name,
                                                                VROverlayHandle_t* out)
{
    int i;
    (void)key; (void)name;
    for (i = 0; i < VRXR_MAX_OVERLAYS; i++)
    {
        if (s_ovls[i].used) continue;
        memset(&s_ovls[i], 0, sizeof(s_ovls[i]));
        s_ovls[i].used        = 1;
        s_ovls[i].widthM      = 1.0f;
        s_ovls[i].texelAspect = 1.0f;
        s_ovls[i].bounds.uMax = 1.0f;
        s_ovls[i].bounds.vMax = 1.0f;
        s_ovls[i].pose.orientation.w = 1.0f;
        *out = (VROverlayHandle_t)(i + 1);
        return EVROverlayError_VROverlayError_None;
    }
    return EVROverlayError_VROverlayError_OverlayLimitExceeded;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_ShowOverlay(VROverlayHandle_t h)
{
    VrXrOverlay* o = Ovl(h);
    if (!o) return EVROverlayError_VROverlayError_InvalidHandle;
    o->visible = 1;
    return EVROverlayError_VROverlayError_None;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_HideOverlay(VROverlayHandle_t h)
{
    VrXrOverlay* o = Ovl(h);
    if (!o) return EVROverlayError_VROverlayError_InvalidHandle;
    o->visible = 0;
    return EVROverlayError_VROverlayError_None;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_SetOverlayFlag(VROverlayHandle_t h,
                                                                 VROverlayFlags flag, bool on)
{
    VrXrOverlay* o = Ovl(h);
    if (!o) return EVROverlayError_VROverlayError_InvalidHandle;
    if (flag == VROverlayFlags_SideBySide_Parallel) o->sbs = on ? 1 : 0;
    return EVROverlayError_VROverlayError_None;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_SetOverlayTexelAspect(VROverlayHandle_t h,
                                                                        float aspect)
{
    VrXrOverlay* o = Ovl(h);
    if (!o) return EVROverlayError_VROverlayError_InvalidHandle;
    o->texelAspect = aspect;
    return EVROverlayError_VROverlayError_None;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_SetOverlayWidthInMeters(VROverlayHandle_t h,
                                                                          float m)
{
    VrXrOverlay* o = Ovl(h);
    if (!o) return EVROverlayError_VROverlayError_InvalidHandle;
    o->widthM = m;
    return EVROverlayError_VROverlayError_None;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_SetOverlayTextureBounds(
    VROverlayHandle_t h, struct VRTextureBounds_t* b)
{
    VrXrOverlay* o = Ovl(h);
    if (!o || !b) return EVROverlayError_VROverlayError_InvalidHandle;
    o->bounds = *b;
    return EVROverlayError_VROverlayError_None;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_SetOverlayTransformAbsolute(
    VROverlayHandle_t h, ETrackingUniverseOrigin origin, struct HmdMatrix34_t* m)
{
    VrXrOverlay* o = Ovl(h);
    (void)origin;
    if (!o || !m) return EVROverlayError_VROverlayError_InvalidHandle;
    MatrixToPose(m, &o->pose);
    return EVROverlayError_VROverlayError_None;
}

static EVROverlayError OPENVR_FNTABLE_CALLTYPE Ovl_SetOverlayTexture(VROverlayHandle_t h,
                                                                    struct Texture_t* tex)
{
    VrXrOverlay* o = Ovl(h);
    if (!o || !tex) return EVROverlayError_VROverlayError_InvalidHandle;
    o->tex = (GLuint)(uintptr_t)tex->handle;
    return EVROverlayError_VROverlayError_None;
}

/* ---- init ----------------------------------------------------------------------- */

#if defined(__ANDROID__)
/* Quest: the Khronos Android loader ships in the APK (lib/arm64-v8a) and must
 * be told the JavaVM and the activity before anything else. */
static int LoadXr(void)
{
    PFN_xrInitializeLoaderKHR initLoader = NULL;
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    JavaVM* vm  = NULL;

    s_loader = dlopen("libopenxr_loader.so", RTLD_NOW);
    if (!s_loader)
    {
        printf("[VR-XR] libopenxr_loader.so not found (%s)\n", dlerror());
        return 0;
    }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(s_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr || !env) return 0;
    (*env)->GetJavaVM(env, &vm);

    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                            (PFN_xrVoidFunction*)&initLoader);
    if (initLoader)
    {
        XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        li.applicationVM      = vm;
        li.applicationContext = SDL_AndroidGetActivity();
        if (XR_FAILED(initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&li)))
        {
            printf("[VR-XR] xrInitializeLoaderKHR failed\n");
            return 0;
        }
    }
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance",
                            (PFN_xrVoidFunction*)&p_xrCreateInstance);
    return p_xrCreateInstance != NULL;
}
#else
static int LoadXr(void)
{
    s_loader = LoadLibraryA("openxr_loader.dll");
    if (!s_loader) s_loader = LoadLibraryA("libopenxr_loader.dll");
    if (!s_loader)
    {
        printf("[VR-XR] openxr_loader.dll not found\n");
        return 0;
    }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)
        GetProcAddress(s_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) return 0;
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance",
                            (PFN_xrVoidFunction*)&p_xrCreateInstance);
    return p_xrCreateInstance != NULL;
}
#endif

static int LoadInstanceFuncs(void)
{
#define X(n) \
    if (XR_FAILED(p_xrGetInstanceProcAddr(s_inst, #n, (PFN_xrVoidFunction*)&p_##n)) || !p_##n) \
    { printf("[VR-XR] missing %s\n", #n); return 0; }
    VRXR_FUNCS(X)
#undef X
    return 1;
}

static int LoadGl(void)
{
    gl_GenFramebuffers      = (PFN_GenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
    gl_BindFramebuffer      = (PFN_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
    gl_FramebufferTexture2D = (PFN_FramebufferTexture2D)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    gl_BlitFramebuffer      = (PFN_BlitFramebuffer)SDL_GL_GetProcAddress("glBlitFramebuffer");
    if (!gl_GenFramebuffers || !gl_BindFramebuffer || !gl_FramebufferTexture2D || !gl_BlitFramebuffer)
        return 0;
    gl_GenFramebuffers(1, &s_srcFbo);
    gl_GenFramebuffers(1, &s_dstFbo);
    return 1;
}

static int CreateSpaces(void)
{
    XrReferenceSpaceType types[8];
    uint32_t n = 0, i;
    int haveStage = 0;
    XrReferenceSpaceCreateInfo ci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };

    if (XR_SUCCEEDED(p_xrEnumerateReferenceSpaces(s_session, 8, &n, types)))
        for (i = 0; i < n; i++)
            if (types[i] == XR_REFERENCE_SPACE_TYPE_STAGE) haveStage = 1;

    ci.poseInReferenceSpace.orientation.w = 1.0f;
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(p_xrCreateReferenceSpace(s_session, &ci, &s_viewSpace))) return 0;

    if (haveStage)
    {
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    else
    {
        /* LOCAL sits at the head: drop the origin to a floor guess so
         * content placed for OpenVR's standing universe lands right */
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        ci.poseInReferenceSpace.position.y = -VRXR_STANDING_EYE_HEIGHT_M;
    }
    if (XR_FAILED(p_xrCreateReferenceSpace(s_session, &ci, &s_space))) return 0;

    printf("[VR-XR] standing space: %s\n", haveStage ? "STAGE" : "LOCAL (floor guess)");
    return 1;
}

static int PickFormat(void)
{
    int64_t fmts[256];
    uint32_t n = 0, i;
    XrResult r;

    /* two-call idiom: the Quest runtime lists more formats than a fixed
     * small buffer holds (XR_ERROR_SIZE_INSUFFICIENT) */
    r = p_xrEnumerateSwapchainFormats(s_session, 0, &n, NULL);
    if (XR_FAILED(r) || n == 0)
    {
        VrOvr_Log("[VR-XR] xrEnumerateSwapchainFormats(count) failed (%d, n=%u)\n", (int)r, n);
        return 0;
    }
    if (n > 256) n = 256;
    r = p_xrEnumerateSwapchainFormats(s_session, n, &n, fmts);
    if (XR_FAILED(r) || n == 0)
    {
        VrOvr_Log("[VR-XR] xrEnumerateSwapchainFormats failed (%d, n=%u)\n", (int)r, n);
        return 0;
    }
    s_format = fmts[0];
    for (i = 0; i < n; i++) if (fmts[i] == GL_RGBA8)        s_format = GL_RGBA8;
    for (i = 0; i < n; i++) if (fmts[i] == GL_SRGB8_ALPHA8) s_format = GL_SRGB8_ALPHA8;
    VrOvr_Log("[VR-XR] swapchain format 0x%llX\n", (unsigned long long)s_format);
    return 1;
}

/* Actions + Touch bindings. Must run before the first frame: action sets
 * can be attached to a session only once. A failure only costs input. */
static void CreateActions(void)
{
    static const struct { const char* name; XrActionType type; const char* path; } defs[VRXR_ACT_COUNT] = {
        { "left_stick",   XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/left/input/thumbstick" },
        { "right_stick",  XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/right/input/thumbstick" },
        { "left_trigger", XR_ACTION_TYPE_FLOAT_INPUT,    "/user/hand/left/input/trigger/value" },
        { "right_trigger",XR_ACTION_TYPE_FLOAT_INPUT,    "/user/hand/right/input/trigger/value" },
        { "left_grip",    XR_ACTION_TYPE_FLOAT_INPUT,    "/user/hand/left/input/squeeze/value" },
        { "right_grip",   XR_ACTION_TYPE_FLOAT_INPUT,    "/user/hand/right/input/squeeze/value" },
        { "button_a",     XR_ACTION_TYPE_BOOLEAN_INPUT,  "/user/hand/right/input/a/click" },
        { "button_b",     XR_ACTION_TYPE_BOOLEAN_INPUT,  "/user/hand/right/input/b/click" },
        { "button_x",     XR_ACTION_TYPE_BOOLEAN_INPUT,  "/user/hand/left/input/x/click" },
        { "button_y",     XR_ACTION_TYPE_BOOLEAN_INPUT,  "/user/hand/left/input/y/click" },
        { "left_click",   XR_ACTION_TYPE_BOOLEAN_INPUT,  "/user/hand/left/input/thumbstick/click" },
        { "right_click",  XR_ACTION_TYPE_BOOLEAN_INPUT,  "/user/hand/right/input/thumbstick/click" },
        { "menu",         XR_ACTION_TYPE_BOOLEAN_INPUT,  "/user/hand/left/input/menu/click" },
        { "left_pose",    XR_ACTION_TYPE_POSE_INPUT,     "/user/hand/left/input/grip/pose" },
        { "right_pose",   XR_ACTION_TYPE_POSE_INPUT,     "/user/hand/right/input/grip/pose" },
        { "right_aim",    XR_ACTION_TYPE_POSE_INPUT,     "/user/hand/right/input/aim/pose" },
    };
    XrActionSuggestedBinding sb[VRXR_ACT_COUNT];
    XrPath profile;
    int i;

    {
        XrActionSetCreateInfo ci = { XR_TYPE_ACTION_SET_CREATE_INFO };
        strcpy(ci.actionSetName, "gameplay");
        strcpy(ci.localizedActionSetName, "Gameplay");
        if (XR_FAILED(p_xrCreateActionSet(s_inst, &ci, &s_actSet))) return;
    }

    for (i = 0; i < VRXR_ACT_COUNT; i++)
    {
        XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
        strcpy(ci.actionName, defs[i].name);
        strcpy(ci.localizedActionName, defs[i].name);
        ci.actionType = defs[i].type;
        if (XR_FAILED(p_xrCreateAction(s_actSet, &ci, &s_act[i]))) return;
        sb[i].action = s_act[i];
        p_xrStringToPath(s_inst, defs[i].path, &sb[i].binding);
    }

    p_xrStringToPath(s_inst, "/interaction_profiles/oculus/touch_controller", &profile);
    {
        XrInteractionProfileSuggestedBinding pb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        pb.interactionProfile     = profile;
        pb.countSuggestedBindings = VRXR_ACT_COUNT;
        pb.suggestedBindings      = sb;
        XrResult r = p_xrSuggestInteractionProfileBindings(s_inst, &pb);
        if (XR_FAILED(r))
        {
            printf("[VR-XR] Touch bindings rejected (%d)\n", (int)r);
            return;
        }
    }
    /* Same paths exist on Touch Plus (Quest 3 / 3S) */
    if (s_haveTouchPlus)
    {
        XrInteractionProfileSuggestedBinding pb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        XrPath plus;
        p_xrStringToPath(s_inst, "/interaction_profiles/meta/touch_controller_plus", &plus);
        pb.interactionProfile     = plus;
        pb.countSuggestedBindings = VRXR_ACT_COUNT;
        pb.suggestedBindings      = sb;
        XrResult r = p_xrSuggestInteractionProfileBindings(s_inst, &pb);
        printf("[VR-XR] Touch Plus bindings %s (%d)\n", XR_FAILED(r) ? "rejected" : "ok", (int)r);
    }

    {
        XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        ai.countActionSets = 1;
        ai.actionSets      = &s_actSet;
        if (XR_FAILED(p_xrAttachSessionActionSets(s_session, &ai))) return;
    }

    for (i = 0; i < 2; i++)
    {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.action = s_act[VRXR_ACT_LPOSE + i];
        sci.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(p_xrCreateActionSpace(s_session, &sci, &s_handSpace[i])))
            s_handSpace[i] = XR_NULL_HANDLE;
    }
    {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.action = s_act[VRXR_ACT_RAIM];
        sci.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(p_xrCreateActionSpace(s_session, &sci, &s_aimSpace)))
            s_aimSpace = XR_NULL_HANDLE;
    }

    s_actReady = 1;
    printf("[VR-XR] motion controller actions ready\n");
}

static int ActBool(int i)
{
    XrActionStateGetInfo   gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean   st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    gi.action = s_act[i];
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(s_session, &gi, &st)) && st.isActive &&
           st.currentState;
}

static float ActFloat(int i)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat   st = { XR_TYPE_ACTION_STATE_FLOAT };
    gi.action = s_act[i];
    if (XR_FAILED(p_xrGetActionStateFloat(s_session, &gi, &st)) || !st.isActive) return 0.0f;
    return st.currentState;
}

static int ActVec2(int i, float* x, float* y)
{
    XrActionStateGetInfo  gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    gi.action = s_act[i];
    *x = *y = 0.0f;
    if (XR_FAILED(p_xrGetActionStateVector2f(s_session, &gi, &st)) || !st.isActive) return 0;
    *x = st.currentState.x;
    *y = st.currentState.y;
    return 1;
}

/* Read the controllers into *out (valid = 0 when unfocused or absent). */
void VrXr_PollInput(VrPadState* out)
{
    static int diagN = 0, lastSync = 12345, lastValid = -1;
    XrResult sr;

    memset(out, 0, sizeof(*out));
    if (!s_actReady || !s_running || s_lost)
    {
        if (diagN < 4 && (++diagN, 1))
            printf("[VR-INPUT] skipped: actReady=%d running=%d lost=%d\n", s_actReady, s_running, s_lost);
        return;
    }

    {
        XrActiveActionSet as = { s_actSet, XR_NULL_PATH };
        XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
        si.countActiveActionSets = 1;
        si.activeActionSets      = &as;
        sr = p_xrSyncActions(s_session, &si);
        if ((int)sr != lastSync)
        {
            printf("[VR-INPUT] xrSyncActions -> %d (state %d)\n", (int)sr, (int)s_state);
            lastSync = (int)sr;
        }
        if (sr != XR_SUCCESS) return;  /* not focused */
    }

    out->valid  = ActVec2(VRXR_ACT_LSTICK, &out->lx, &out->ly);
    out->valid |= ActVec2(VRXR_ACT_RSTICK, &out->rx, &out->ry);
    if (out->valid != lastValid)
    {
        XrActionStateGetInfo  gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        XrActionStateBoolean  bs = { XR_TYPE_ACTION_STATE_BOOLEAN };
        XrResult br;
        gi.action = s_act[VRXR_ACT_A];
        br = p_xrGetActionStateBoolean(s_session, &gi, &bs);
        printf("[VR-INPUT] sticks valid=%d | button A get=%d active=%d\n",
               out->valid, (int)br, (int)bs.isActive);
        lastValid = out->valid;
    }
    out->lt     = ActFloat(VRXR_ACT_LTRIG);
    out->rt     = ActFloat(VRXR_ACT_RTRIG);
    out->lgrip  = ActFloat(VRXR_ACT_LGRIP);
    out->rgrip  = ActFloat(VRXR_ACT_RGRIP);
    out->a      = ActBool(VRXR_ACT_A);
    out->b      = ActBool(VRXR_ACT_B);
    out->x      = ActBool(VRXR_ACT_X);
    out->y      = ActBool(VRXR_ACT_Y);
    out->lstick = ActBool(VRXR_ACT_LCLICK);
    out->rstick = ActBool(VRXR_ACT_RCLICK);
    out->menu   = ActBool(VRXR_ACT_MENU);
}

static void BuildTables(void)
{
    memset(&s_sysTbl, 0, sizeof(s_sysTbl));
    memset(&s_compTbl, 0, sizeof(s_compTbl));
    memset(&s_ovlTbl, 0, sizeof(s_ovlTbl));

    s_sysTbl.GetRecommendedRenderTargetSize  = Sys_GetRecommendedRenderTargetSize;
    s_sysTbl.GetProjectionRaw                = Sys_GetProjectionRaw;
    s_sysTbl.GetDeviceToAbsoluteTrackingPose = Sys_GetDeviceToAbsoluteTrackingPose;

    s_compTbl.WaitGetPoses = Comp_WaitGetPoses;
    s_compTbl.Submit       = Comp_Submit;

    s_ovlTbl.CreateOverlay               = Ovl_CreateOverlay;
    s_ovlTbl.ShowOverlay                 = Ovl_ShowOverlay;
    s_ovlTbl.HideOverlay                 = Ovl_HideOverlay;
    s_ovlTbl.SetOverlayFlag              = Ovl_SetOverlayFlag;
    s_ovlTbl.SetOverlayTexelAspect       = Ovl_SetOverlayTexelAspect;
    s_ovlTbl.SetOverlayWidthInMeters     = Ovl_SetOverlayWidthInMeters;
    s_ovlTbl.SetOverlayTextureBounds     = Ovl_SetOverlayTextureBounds;
    s_ovlTbl.SetOverlayTransformAbsolute = Ovl_SetOverlayTransformAbsolute;
    s_ovlTbl.SetOverlayTexture           = Ovl_SetOverlayTexture;
}

/* Must run on the render thread with the game's GL context current.
 * Returns 1 with OpenVR-shaped tables, 0 = no OpenXR VR (game runs flat). */
int VrXr_Init(void** sysTbl, void** compTbl, void** ovlTbl)
{
#if defined(__ANDROID__)
    const char* exts[3] = { XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, NULL };
    uint32_t nExts = 2;
    XrInstanceCreateInfoAndroidKHR aci = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
#else
    const char* exts[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    uint32_t nExts = 1;
#endif

    if (!LoadXr()) return 0;

#if defined(__ANDROID__)
    /* Quest 3 / 3S controllers are "Touch Plus": offer their own profile
     * when the runtime has it, next to the classic Touch bindings. */
    {
        PFN_xrEnumerateInstanceExtensionProperties enumExt = NULL;
        XrExtensionProperties props[128];
        uint32_t n = 0, i;
        p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                (PFN_xrVoidFunction*)&enumExt);
        for (i = 0; i < 128; i++) { props[i].type = XR_TYPE_EXTENSION_PROPERTIES; props[i].next = NULL; }
        if (enumExt && XR_SUCCEEDED(enumExt(NULL, 128, &n, props)))
            for (i = 0; i < n; i++)
                if (strcmp(props[i].extensionName, XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME) == 0)
                {
                    exts[nExts++] = XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME;
                    s_haveTouchPlus = 1;
                }
        VrOvr_Log("[VR-XR] Touch Plus profile: %s\n", s_haveTouchPlus ? "yes" : "no");
    }
#endif

    {
        XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
        strncpy(ci.applicationInfo.applicationName, "Silent Hill VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        strncpy(ci.applicationInfo.engineName, "PsyCross", XR_MAX_ENGINE_NAME_SIZE - 1);
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = nExts;
        ci.enabledExtensionNames = exts;
#if defined(__ANDROID__)
        {
            JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
            JavaVM* vm  = NULL;
            (*env)->GetJavaVM(env, &vm);
            aci.applicationVM       = vm;
            aci.applicationActivity = SDL_AndroidGetActivity();
            ci.next = &aci;
        }
#endif
        XrResult r = p_xrCreateInstance(&ci, &s_inst);
        if (XR_FAILED(r))
        {
            printf("[VR-XR] xrCreateInstance failed (%d) - no OpenXR runtime with OpenGL?\n", (int)r);
            return 0;
        }
    }
    if (!LoadInstanceFuncs()) return 0;

    {
        XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(p_xrGetInstanceProperties(s_inst, &ip)))
            VrOvr_Log("[VR-XR] runtime: %s\n", ip.runtimeName);
    }

    {
        XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
        gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (XR_FAILED(p_xrGetSystem(s_inst, &gi, &s_sysId)))
        {
            printf("[VR-XR] no headset found\n");
            return 0;
        }
    }

    {
        XrViewConfigurationView v[2];
        uint32_t n = 0;
        v[0].type = v[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        v[0].next = v[1].next = NULL;
        if (XR_FAILED(p_xrEnumerateViewConfigurationViews(s_inst, s_sysId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)) || n != 2)
            return 0;
        s_recW = v[0].recommendedImageRectWidth;
        s_recH = v[0].recommendedImageRectHeight;
        {
            /* Headset_Res (vr_headset_res): render above the runtime's
             * recommendation (Quest 3 recommends ~81% of its panel). Kept
             * inside the runtime's max, even sizes. */
            const float    k  = g_PcConfig.vrHeadsetRes / 100.0f;
            const uint32_t rw = s_recW, rh = s_recH;
            if (k > 1.0f)
            {
                uint32_t w = (uint32_t)(s_recW * k + 0.5f) & ~1u;
                uint32_t h = (uint32_t)(s_recH * k + 0.5f) & ~1u;
                if (v[0].maxImageRectWidth  > 0 && w > v[0].maxImageRectWidth)  w = v[0].maxImageRectWidth;
                if (v[0].maxImageRectHeight > 0 && h > v[0].maxImageRectHeight) h = v[0].maxImageRectHeight;
                s_recW = w;
                s_recH = h;
            }
            VrOvr_Log("[VR-XR] runtime recommends %ux%u, max %ux%u; Headset_Res %.0f%% -> %ux%u\n",
                      rw, rh, v[0].maxImageRectWidth, v[0].maxImageRectHeight,
                      (double)g_PcConfig.vrHeadsetRes, s_recW, s_recH);
        }
    }

#if defined(__ANDROID__)
    {
        /* SDL's EGL context, current on this (the game's render) thread. */
        XrGraphicsRequirementsOpenGLESKHR   req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
        XrGraphicsBindingOpenGLESAndroidKHR gb  = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
        XrSessionCreateInfo                 sci = { XR_TYPE_SESSION_CREATE_INFO };
        EGLint  cfgId = 0, nCfg = 0;
        EGLint  attrs[] = { EGL_CONFIG_ID, 0, EGL_NONE };

        p_xrGetOpenGLESGraphicsRequirementsKHR(s_inst, s_sysId, &req);
        gb.display = eglGetCurrentDisplay();
        gb.context = eglGetCurrentContext();
        if (gb.display == EGL_NO_DISPLAY || gb.context == EGL_NO_CONTEXT)
        {
            printf("[VR-XR] no current EGL context\n");
            return 0;
        }
        eglQueryContext(gb.display, gb.context, EGL_CONFIG_ID, &cfgId);
        attrs[1] = cfgId;
        eglChooseConfig(gb.display, attrs, &gb.config, 1, &nCfg);
        sci.next     = &gb;
        sci.systemId = s_sysId;
        XrResult r = p_xrCreateSession(s_inst, &sci, &s_session);
        if (XR_FAILED(r))
        {
            printf("[VR-XR] xrCreateSession failed (%d)\n", (int)r);
            return 0;
        }
    }
#else
    {
        XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
        XrGraphicsBindingOpenGLWin32KHR gb  = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
        XrSessionCreateInfo             sci = { XR_TYPE_SESSION_CREATE_INFO };

        p_xrGetOpenGLGraphicsRequirementsKHR(s_inst, s_sysId, &req);
        gb.hDC   = wglGetCurrentDC();
        gb.hGLRC = wglGetCurrentContext();
        if (!gb.hDC || !gb.hGLRC)
        {
            printf("[VR-XR] no current WGL context (renderer must be native OpenGL)\n");
            return 0;
        }
        sci.next     = &gb;
        sci.systemId = s_sysId;
        XrResult r = p_xrCreateSession(s_inst, &sci, &s_session);
        if (XR_FAILED(r))
        {
            printf("[VR-XR] xrCreateSession failed (%d)\n", (int)r);
            return 0;
        }
    }
#endif

    if (!LoadGl())       { VrOvr_Log("[VR-XR] GL framebuffer functions missing\n"); return 0; }
    if (!CreateSpaces()) { VrOvr_Log("[VR-XR] reference space creation failed\n"); return 0; }
    if (!PickFormat())   return 0;
    CreateActions();
    if (!ChainEnsure(&s_eyeChain[0], (int)s_recW, (int)s_recH) ||
        !ChainEnsure(&s_eyeChain[1], (int)s_recW, (int)s_recH))
    {
        VrOvr_Log("[VR-XR] eye swapchain %ux%u creation failed\n", s_recW, s_recH);
        return 0;
    }

    /* Wait (bounded) for READY so the first frame can locate the views -
     * vr_full.c reads the fov once, at its own init. */
    {
        Uint32 t0 = SDL_GetTicks();
        while (!s_running && !s_lost && SDL_GetTicks() - t0 < 5000)
        {
            PollEvents();
            if (!s_running) SDL_Delay(10);
        }
        if (!s_running)
        {
            printf("[VR-XR] session never became ready\n");
            return 0;
        }
    }

    BeginNextFrame();   /* leaves a frame open, like OpenVR after init */

    BuildTables();
    *sysTbl  = &s_sysTbl;
    *compTbl = &s_compTbl;
    *ovlTbl  = &s_ovlTbl;

    VrOvr_Log("[VR-XR] ready: render target %ux%u per eye\n", s_recW, s_recH);
    return 1;
}

/* Controller grip pose (0 = left, 1 = right) as an OpenVR-style matrix in
 * the standing space, for vr_openvr.c. 0 = not tracked this frame. */
int VrXr_GetHandPose(int hand, HmdMatrix34_t* out)
{
    if (hand < 0 || hand > 1 || !s_handValid[hand]) return 0;
    PoseToMatrix(&s_handPose[hand], out);
    return 1;
}

/* Right controller aim pose (the pointing ray), same space as the grips. */
int VrXr_GetAimPose(HmdMatrix34_t* out)
{
    if (!s_aimValid) return 0;
    PoseToMatrix(&s_aimPose, out);
    return 1;
}

/* Game exit: release the session and instance while the GL context still
 * exists. Left alive, the runtime's threads kept the process from exiting. */
void VrXr_Shutdown(void)
{
    if (s_session != XR_NULL_HANDLE)
    {
        if (s_running) p_xrEndSession(s_session);
        p_xrDestroySession(s_session);   /* also frees spaces, swapchains, actions */
        s_session = XR_NULL_HANDLE;
    }
    if (s_inst != XR_NULL_HANDLE)
    {
        p_xrDestroyInstance(s_inst);
        s_inst = XR_NULL_HANDLE;
    }
    s_running = 0;
    s_frameOpen = 0;
    s_lost = 1;
}

/* End of every VR hook: keep the OpenXR frame loop alive when this game
 * frame never reached WaitGetPoses (2D screen with no backdrop). */
void VrXr_FrameTick(void)
{
    if (s_session == XR_NULL_HANDLE || s_lost) return;

    if (!s_waitedThisTick)
    {
        EndFrameIfOpen();
        BeginNextFrame();
    }
    s_waitedThisTick = 0;
}
