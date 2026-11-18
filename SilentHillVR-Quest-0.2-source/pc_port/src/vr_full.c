/*
=============================================================================
 SH1 PC Port - FULL IMMERSION MODULE (quarantined)  v1.2
=============================================================================
 v1.2: DYNAMIC VIEWPORT CROP - the horizontal content crop is derived
 live from PsyX's actual display-viewport math each frame (ComputeCropU),
 so window/resolution/aspect changes no longer silently invalidate the
 angular calibration. HORPLUS_SCALE reverted to the crop-correct 1.4435.
=============================================================================
 Self-contained per-eye FOV composition for FULL stereo mode. Deliberately
 isolated from the working screen/overlay code in vr_openvr.c: own GL
 resources, own error handling, zero shared state beyond the parity int.

 v1.1: STALE-EYE ROTATION REPROJECTION (the alternating-eye ghosting fix).
 Both eyes are now RE-COMPOSED every frame from per-eye capture textures;
 the stale eye's quad is shifted in tangent space by the camera rotation
 delta since its capture (pitch/yaw from vr_tracking.c), re-aligning the
 eyes angularly. Same trick Model-B already plays with roll - roll was
 never ghosting BECAUSE it lives on the quad, not in the content.
 Calibration (in-HMD, like everything else):
   PAGE UP   - toggle reprojection on/off (A/B the ghosting live)
   PAGE DOWN - cycle the 4 tangent-shift sign combos; wrong signs DOUBLE
               the ghosting, right signs kill it - unmistakable. Bake the
               winner into VR_FULL_REPROJ_COMBO_DEFAULT.

 Install:
   1. This file -> pc_port/src/vr_full.c   (NEW file: run `build.bat configure`!)
   2. One block added to VrOvr_StereoPushFrame in vr_openvr.c (see chat).

 Behavior: VrFull_Push() returns 1 when it handled the frame (caller must
 return), 0 if init failed (caller falls through to the legacy direct
 submit - warped but functional).

 Image scale comes from the game's live projection (see below).
=============================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include "vr_gl.h"

#include "openvr_capi.h"

extern void VrOvr_Log(const char* fmt, ...);   /* SilentHillVR.log (vr_openvr.c) */
extern int  PsyX_StereoIsFrozen(void);         /* PsyX_render.cpp: eye images re-presented */
extern int  PsyX_StereoUiScreen(void);         /* PsyX_render.cpp: in-game menu as a 2D screen */
extern int  VrOvr_SceneReadBegin(int* outW, int* outH, int* outPrevRead);
extern void VrOvr_SceneReadEnd(int prevRead);

/* VR SNAPSHOTS: what the headset is shown, both eyes as composed here right
 * before submit, written to vrsnaps/ as TGA. Taken automatically a moment
 * after the view mode changes (UI context, cutscene, flat 2D screen, frozen
 * world) - the screens that go wrong are exactly those transitions - and on
 * demand while a file vrsnaps/snap.req exists (deleted once taken). Capped
 * per session so a long play does not fill the disk. */
#define VR_SNAP_MAX       80
#define VR_SNAP_DELAY_FR  25    /* frames after a change: the new screen has settled */
static int  s_snapCountdown = -1;
static int  s_snapTaken     = 0;
static int  s_snapEyesLeft  = 0;
static char s_snapTag[64];

static void SnapWriteTga(const char* path, const unsigned char* rgba, int w, int h)
{
    FILE*         fp = fopen(path, "wb");
    unsigned char hdr[18] = { 0 };
    int           i;
    if (!fp) return;
    hdr[2]  = 2;                       /* uncompressed true colour */
    hdr[12] = (unsigned char)(w & 0xFF); hdr[13] = (unsigned char)(w >> 8);
    hdr[14] = (unsigned char)(h & 0xFF); hdr[15] = (unsigned char)(h >> 8);
    hdr[16] = 32;                      /* bottom-up, like glReadPixels */
    hdr[17] = 8;
    fwrite(hdr, 1, sizeof(hdr), fp);
    for (i = 0; i < w * h; i++)
    {
        const unsigned char px[4] = { rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4 + 0], 255 };
        fwrite(px, 1, 4, fp);
    }
    fclose(fp);
}

/* Once a frame: arm a snapshot when the view mode changes, or on request. */
static void SnapTick(void)
{
    extern int VrTracking_GetUiContext(void);
    extern int VrTracking_CutsceneActive(void);
    extern int VrTracking_UiInWorld(void);
    static int s_lastKey = -1, s_reqPoll = 0;
    const int  ui  = VrTracking_GetUiContext(), cut = VrTracking_CutsceneActive();
    const int  inw = VrTracking_UiInWorld(), flat = PsyX_StereoUiScreen(), frz = PsyX_StereoIsFrozen();
    const int  key = ui | (cut << 2) | (inw << 3) | (flat << 4) | (frz << 5);

    /* automatic ones only for test runs (SH1VR_AUTOSNAP, set by the simulator
     * launcher): each pair is ~36 MB, and in normal play they had filled C:
     * (5.7 GB in vrsnaps\ on 28/10). Requests (snap.req) always work. */
    static int s_auto = -1;
    if (s_auto < 0)
        s_auto = (getenv("SH1VR_AUTOSNAP") != NULL);

    if (s_snapTaken >= VR_SNAP_MAX) return;
    if (key != s_lastKey)
    {
        if (s_lastKey >= 0 && s_auto)
        {
            snprintf(s_snapTag, sizeof(s_snapTag), "ui%d_cut%d_inworld%d_flat%d_frozen%d", ui, cut, inw, flat, frz);
            s_snapCountdown = VR_SNAP_DELAY_FR;
        }
        s_lastKey = key;
    }
    if (++s_reqPoll >= 45)
    {
        FILE* fp = fopen("vrsnaps/snap.req", "rb");
        s_reqPoll = 0;
        if (fp)
        {
            fclose(fp);
            remove("vrsnaps/snap.req");
            snprintf(s_snapTag, sizeof(s_snapTag), "request_ui%d_cut%d_flat%d", ui, cut, flat);
            s_snapCountdown = 0;
        }
    }
    if (s_snapCountdown >= 0 && s_snapCountdown-- == 0)
        s_snapEyesLeft = 2;
}

/* Inside the compose loop, with eye e's composed image bound. */
static void SnapEye(int e, int w, int h)
{
    extern int _mkdir(const char* dirname);
    unsigned char* buf;
    char           path[160];

    if (s_snapEyesLeft <= 0) return;
    buf = (unsigned char*)malloc((size_t)w * h * 4);
    if (!buf) return;
    _mkdir("vrsnaps");
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    snprintf(path, sizeof(path), "vrsnaps/snap%03d_%s_%s.tga", s_snapTaken, s_snapTag, e ? "right" : "left");
    SnapWriteTga(path, buf, w, h);
    free(buf);
    VrOvr_Log("[VR-SNAP] %s\n", path);
    if (--s_snapEyesLeft == 0)
        s_snapTaken++;
}

/* In-game menus (PsyX_StereoUiScreen): the whole flat frame, copied each
 * frame, shown as a 2D screen in front of you over the frozen world. */
static GLuint s_flatTex = 0;
static int    s_flatW = 0, s_flatH = 0;

static int FlatCapture(void)
{
    int w = 0, h = 0, prevRead = 0;
    if (!VrOvr_SceneReadBegin(&w, &h, &prevRead) || w <= 0 || h <= 0)
    {
        VrOvr_SceneReadEnd(prevRead);
        return 0;
    }
    if (s_flatTex == 0) glGenTextures(1, &s_flatTex);
    glBindTexture(GL_TEXTURE_2D, s_flatTex);
    if (w != s_flatW || h != s_flatH)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_flatW = w;
        s_flatH = h;
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    VrOvr_SceneReadEnd(prevRead);
    return 1;
}

/* ---- tunables ---------------------------------------------------------- */
/* ANGULAR SCALE: every frame the eye quad spans exactly the directions
 * the game image covers - PsyX_GetWorldTangents() derives them from the
 * world pass's GTE projection (H, centre) and ortho rect. Any fov, camera
 * style, Hor+ width or render resolution therefore lands 1:1: nothing is
 * enlarged or squeezed to fill the eye. The fixed calibration this
 * replaced (110 deg, gx x1.5879, gy x0.7079) only held for one config.
 * Filling the eye is the projection's job (VrTracking_VrGeomScreen widens
 * the game fov to the eye frustum), not the composition's. */

/* FMV in FULL mode (rare: video normally drops to the screen): flat 16:9
 * panel ~62 deg wide straight ahead. */
#define VR_FULL_FMV_TAN_X 0.60f
#define VR_FULL_FMV_TAN_Y 0.3375f

#define VR_FULL_CROP_V0 0.0f
#define VR_EYE_CROP_PAD  0.10f   /* tangent: eye crop margin on every side */
#define VR_EYE_CROP_ROLL 0.17f   /* rad: eye crop covers this much more head roll */
#define VR_FULL_CROP_V1 1.0f

/* ---- GL entry points (loaded at runtime) ------------------------------- */
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif
#ifndef GL_RASTERIZER_DISCARD
#define GL_RASTERIZER_DISCARD 0x8C89
#endif

typedef void   (*PF_GenFramebuffers)(GLsizei, GLuint*);
typedef void   (*PF_BindFramebuffer)(GLenum, GLuint);
typedef void   (*PF_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PF_CheckFramebufferStatus)(GLenum);
typedef GLuint (*PF_CreateShader)(GLenum);
typedef void   (*PF_ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void   (*PF_CompileShader)(GLuint);
typedef void   (*PF_GetShaderiv)(GLuint, GLenum, GLint*);
typedef GLuint (*PF_CreateProgram)(void);
typedef void   (*PF_AttachShader)(GLuint, GLuint);
typedef void   (*PF_LinkProgram)(GLuint);
typedef void   (*PF_GetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (*PF_UseProgram)(GLuint);
typedef void   (*PF_GenVertexArrays)(GLsizei, GLuint*);
typedef void   (*PF_BindVertexArray)(GLuint);
typedef void   (*PF_GenBuffers)(GLsizei, GLuint*);
typedef void   (*PF_BindBuffer)(GLenum, GLuint);
typedef void   (*PF_BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
typedef void   (*PF_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void   (*PF_EnableVertexAttribArray)(GLuint);
typedef GLint  (*PF_GetUniformLocation)(GLuint, const char*);
typedef void   (*PF_Uniform1i)(GLint, GLint);
typedef void   (*PF_ActiveTexture)(GLenum);
typedef void   (*PF_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (*PF_BlendEquationSeparate)(GLenum, GLenum);

static PF_GenFramebuffers         fGenFramebuffers;
static PF_BindFramebuffer         fBindFramebuffer;
static PF_FramebufferTexture2D    fFramebufferTexture2D;
static PF_CheckFramebufferStatus  fCheckFramebufferStatus;
static PF_CreateShader            fCreateShader;
static PF_ShaderSource            fShaderSource;
static PF_CompileShader           fCompileShader;
static PF_GetShaderiv             fGetShaderiv;
static PF_CreateProgram           fCreateProgram;
static PF_AttachShader            fAttachShader;
static PF_LinkProgram             fLinkProgram;
static PF_GetProgramiv            fGetProgramiv;
static PF_UseProgram              fUseProgram;
static PF_GenVertexArrays         fGenVertexArrays;
static PF_BindVertexArray         fBindVertexArray;
static PF_GenBuffers              fGenBuffers;
static PF_BindBuffer              fBindBuffer;
static PF_BufferData              fBufferData;
static PF_VertexAttribPointer     fVertexAttribPointer;
static PF_EnableVertexAttribArray fEnableVertexAttribArray;
static PF_GetUniformLocation      fGetUniformLocation;
static PF_Uniform1i               fUniform1i;
static PF_ActiveTexture           fActiveTexture;
static PF_BlendFuncSeparate       fBlendFuncSeparate;
static PF_BlendEquationSeparate   fBlendEquationSeparate;
typedef void   (*PF_BlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static PF_BlendColor              fBlendColor;

static int LoadGl(void)
{
    #define L(dst, name) dst = (void*)SDL_GL_GetProcAddress(name); if (!dst) return 0
    L(fGenFramebuffers, "glGenFramebuffers");
    L(fBindFramebuffer, "glBindFramebuffer");
    L(fFramebufferTexture2D, "glFramebufferTexture2D");
    L(fCheckFramebufferStatus, "glCheckFramebufferStatus");
    L(fCreateShader, "glCreateShader");
    L(fShaderSource, "glShaderSource");
    L(fCompileShader, "glCompileShader");
    L(fGetShaderiv, "glGetShaderiv");
    L(fCreateProgram, "glCreateProgram");
    L(fAttachShader, "glAttachShader");
    L(fLinkProgram, "glLinkProgram");
    L(fGetProgramiv, "glGetProgramiv");
    L(fUseProgram, "glUseProgram");
    L(fGenVertexArrays, "glGenVertexArrays");
    L(fBindVertexArray, "glBindVertexArray");
    L(fGenBuffers, "glGenBuffers");
    L(fBindBuffer, "glBindBuffer");
    L(fBufferData, "glBufferData");
    L(fVertexAttribPointer, "glVertexAttribPointer");
    L(fEnableVertexAttribArray, "glEnableVertexAttribArray");
    L(fGetUniformLocation, "glGetUniformLocation");
    L(fUniform1i, "glUniform1i");
    L(fActiveTexture, "glActiveTexture");
    L(fBlendFuncSeparate, "glBlendFuncSeparate");
    L(fBlendEquationSeparate, "glBlendEquationSeparate");
    L(fBlendColor, "glBlendColor");
    #undef L
    return 1;
}

/* ---- module state ------------------------------------------------------ */
static int    s_state = 0; /* 0 uninit, 1 ready, -1 failed */
static GLuint s_fbo[2], s_fboTex[2];
static GLuint s_submitTex[2]; /* plain (non-attachment) textures actually
                               * submitted - the compositor's GL interop
                               * propagates copied content but not FBO
                               * renders (the frozen-frame quirk) */
static unsigned int s_eyeW, s_eyeH;
static GLuint s_prog, s_vao[2], s_vbo[2];
static float  s_projExt[2][4]; /* per eye: xMin xMax yMin yMax (tangents, up +) */

/* Directions the composed image spans: left, right, bottom, top (tangents,
 * up +). Live world extents, or the FMV panel. Starts at the game's own
 * 71.1 deg 320x224 frame until the first world frame reports. */
static float  s_content[4] = { -1.4286f, 1.4286f, -1.0f, 1.0f };

/* PER-EYE capture textures (v1.1): each eye keeps ITS OWN copy of the
 * game frame it was captured from, so both eyes can be re-composed every
 * frame (the stale one with rotation compensation). One shared texture
 * meant the stale eye's FBO content was frozen at composition time. */
static GLuint s_gameTex[2] = {0, 0};
static int    s_gameW = 0, s_gameH = 0;

/* ---- stale-eye rotation reprojection (v1.1 ghosting fix) ---------------- */
/* Applied camera pitch/yaw at each eye's capture + this frame's reference.
 * delta = current - captured -> tangent-space quad shift for that eye.
 * Fresh eye's delta is 0 by construction. */
#define VR_FULL_REPROJ_DEFAULT       1  /* PAGE UP toggles live */
#define VR_FULL_REPROJ_COMBO_DEFAULT 1  /* CALIBRATED in-HMD: x- y+ (rotation ghosting far reduced; residual = translation/content motion, see notes) */
static int   s_reprojOn    = VR_FULL_REPROJ_DEFAULT;
static int   s_reprojCombo = VR_FULL_REPROJ_COMBO_DEFAULT; /* bit0=X bit1=Y sign */
static float s_capAng[2][2];            /* [eye][pitch,yaw] at capture */
static int   s_capValid[2] = {0, 0};
static float s_curAng[2]   = {0, 0};    /* this frame's applied pitch,yaw */

static float WrapPi(float a)
{
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

/* ---- dynamic content crop (v1.2) ---------------------------------------- */
/* Horizontal texture crop of the game frame, updated every push. */
static float s_cropU0 = 0.0f;
static float s_cropU1 = 1.0f;

/* Horizontal UV window the quad actually samples. Capture path: the
 * live crop as-is. GPU-stereo path (v2.0 summit): the crop remapped
 * into this eye's half of PsyX's side-by-side stereo texture. Set by
 * the caller before each BuildRolledQuad. */
static float s_quadU0 = 0.0f;
static float s_quadU1 = 1.0f;

/* EXACT replica of PsyX_render.cpp's "Display viewport" pillarbox block
 * (the vp=X+WxH in the [ASPECT] log line), reading the same globals the
 * renderer uses - so the sampled region always matches what the port
 * actually drew, at ANY window size/aspect/mode. Plain C ints defined in
 * PsyX_render.cpp; the port's own .c files extern them the same way. */
static void ComputeCropU(int winW, int winH)
{
    extern int g_PcHorPlusEnabled;   /* PsyX_render.cpp */
    extern int g_PcWidescreenMode;
    extern int g_PcMenuPillarbox;
    extern int g_PcNativeRender;

    int vpX = 0, vpW = winW;
    int wantPillarbox =
        !g_PcNativeRender &&
        ((g_PcHorPlusEnabled && g_PcWidescreenMode == 0) ||
         (!g_PcHorPlusEnabled && g_PcMenuPillarbox));

    if (wantPillarbox && winH > 0)
    {
        float psxAspect = 4.0f / 3.0f;
        float winAspect = (float)winW / (float)winH;
        if (winAspect > psxAspect)
        {
            vpW = (int)(winH * psxAspect + 0.5f);
            vpX = (winW - vpW) / 2;
        }
    }

    s_cropU0 = (winW > 0) ? (float)vpX / (float)winW : 0.0f;
    s_cropU1 = (winW > 0) ? (float)(vpX + vpW) / (float)winW : 1.0f;
}

static struct VR_IVRSystem_FnTable*     s_sys;
static struct VR_IVRCompositor_FnTable* s_comp;
static TrackedDevicePose_t s_poses[16];

/* forward declaration (defined below, used by VrFull_PushFmv above it) */
static void BuildRolledQuad(int eye, float rollRad, float dxT, float dyT,
                            float out[16]);

/* Pose stamping modes (VR_FULL_STAMP_MODE):
 *  0 = OFF        - plain submits (original: character-turn ghosting)
 *  1 = PER-EYE    - each eye stamped with its own render pose. Fixes
 *      character-turn ghosting but the differing per-eye reprojection
 *      of our HEAD-LOCKED window causes ghosting on head motion.
 *  2 = UNIFIED    - both eyes stamped with the SAME fresh pose each
 *      frame: reprojection is identical for both eyes (no inter-eye
 *      mismatch on head turns) while head-still behavior matches mode 1.
 *      The needle-threader - default. */
#define VR_FULL_STAMP_MODE 0 /* TESTED VERDICT: stamping conflicts with
                              * head-locked content (mode 1 splits eyes,
                              * mode 2 double-counts rotation). Unstamped
                              * is correct for this architecture. */

static HmdMatrix34_t s_stampPose[2];
static int           s_stampValid[2] = {0, 0};
static HmdMatrix34_t s_framePose;
static int           s_framePoseValid = 0;

static void PollCalibrationKeys(void)
{
    static int pPgUp = 0, pPgDn = 0;
    const Uint8* ks = SDL_GetKeyboardState(NULL);
    int nPgUp = ks[SDL_SCANCODE_PAGEUP] != 0;
    int nPgDn = ks[SDL_SCANCODE_PAGEDOWN] != 0;

    /* v1.1 reprojection A/B: toggle + sign-combo cycle. Turn your head
     * side to side while testing - wrong signs double the ghosting,
     * right signs collapse it. Bake the winner into the defines. */
    if (nPgUp && !pPgUp)
    {
        s_reprojOn = !s_reprojOn;
        printf("[VR-FULL] reprojection %s\n", s_reprojOn ? "ON" : "OFF");
    }
    if (nPgDn && !pPgDn)
    {
        s_reprojCombo = (s_reprojCombo + 1) & 3;
        printf("[VR-FULL] reproj combo %d (x%c y%c) - shake your head!\n",
               s_reprojCombo,
               (s_reprojCombo & 1) ? '-' : '+',
               (s_reprojCombo & 2) ? '-' : '+');
    }

    pPgUp = nPgUp; pPgDn = nPgDn;
}

static GLuint CompileProgram(void)
{
    static const char* vsrc =
        VR_GLSL_VERSION
        "layout(location=0) in vec2 aPos;\n"
        "layout(location=1) in vec2 aUv;\n"
        "out vec2 vUv;\n"
        "void main(){ vUv=aUv; gl_Position=vec4(aPos,0.0,1.0); }\n";
    static const char* fsrc =
        VR_GLSL_VERSION
        "in vec2 vUv; out vec4 oCol; uniform sampler2D uTex; uniform int uAlpha;\n"
        "void main(){ vec4 t=texture(uTex,vUv); oCol=vec4(t.rgb,uAlpha!=0?t.a:1.0); }\n";

    GLint ok = 0;
    GLuint vs = fCreateShader(GL_VERTEX_SHADER);
    fShaderSource(vs, 1, &vsrc, NULL);
    fCompileShader(vs);
    fGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) return 0;

    GLuint fs = fCreateShader(GL_FRAGMENT_SHADER);
    fShaderSource(fs, 1, &fsrc, NULL);
    fCompileShader(fs);
    fGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) return 0;

    GLuint p = fCreateProgram();
    fAttachShader(p, vs);
    fAttachShader(p, fs);
    fLinkProgram(p);
    fGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

static int FullInit(void)
{
    if (s_state != 0) return s_state == 1;
    s_state = -1;

    if (!LoadGl())
    {
        printf("[VR-FULL] GL loading failed - legacy submit fallback\n");
        return 0;
    }

    /* THE ROOT CAUSE OF THE ENTIRE BLACK-SCREEN SAGA lived here: init
     * creates FBOs/VAOs/textures and previously restored NOTHING.
     * GL 3.3 core requires a bound VAO to draw - PsyX binds its VAO once
     * and caches; leaving VAO=0 after init silently killed every PsyX
     * draw forever (black desktop, black captures, all modes). Same for
     * the texture bound on PsyX's cached active unit. Save it ALL. */
    GLint iFbo, iVao, iArr, iTex;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &iFbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &iVao);
    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &iArr);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &iTex);

    s_sys->GetRecommendedRenderTargetSize(&s_eyeW, &s_eyeH);
    if (s_eyeW == 0 || s_eyeH == 0) { s_eyeW = 1600; s_eyeH = 1800; }

    #define INIT_RESTORE() do { \
        fBindVertexArray((GLuint)iVao); \
        fBindBuffer(GL_ARRAY_BUFFER, (GLuint)iArr); \
        fBindFramebuffer(GL_FRAMEBUFFER, (GLuint)iFbo); \
        glBindTexture(GL_TEXTURE_2D, (GLuint)iTex); \
    } while (0)

    s_prog = CompileProgram();
    if (s_prog == 0)
    {
        printf("[VR-FULL] shader failed - legacy submit fallback\n");
        INIT_RESTORE();
        return 0;
    }

    int i;
    for (i = 0; i < 2; i++)
    {
        glGenTextures(1, &s_fboTex[i]);
        glBindTexture(GL_TEXTURE_2D, s_fboTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_eyeW, s_eyeH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        fGenFramebuffers(1, &s_fbo[i]);
        fBindFramebuffer(GL_FRAMEBUFFER, s_fbo[i]);
        fFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, s_fboTex[i], 0);
        GLenum st = fCheckFramebufferStatus(GL_FRAMEBUFFER);
        fBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE)
        {
            printf("[VR-FULL] FBO incomplete - legacy submit fallback\n");
            INIT_RESTORE();
            return 0;
        }

        glGenTextures(1, &s_submitTex[i]);
        glBindTexture(GL_TEXTURE_2D, s_submitTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_eyeW, s_eyeH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        float pl, pr, pt, pb;
        s_sys->GetProjectionRaw((EVREye)i, &pl, &pr, &pt, &pb);

        /* OpenVR's raw top/bottom are y-DOWN (top negative); the quad
         * maths is y-up, so top = -pt and bottom = -pb. (Reading them
         * straight mirrored any vertically asymmetric frustum.) */
        s_projExt[i][0] = (pl < pr) ? pl : pr;
        s_projExt[i][1] = (pl < pr) ? pr : pl;
        s_projExt[i][2] = (-pb < -pt) ? -pb : -pt;
        s_projExt[i][3] = (-pb < -pt) ? -pt : -pb;

        VrOvr_Log("[VR-FULL] eye %d tangents L%.3f R%.3f B%.3f T%.3f\n", i,
               (double)s_projExt[i][0], (double)s_projExt[i][1],
               (double)s_projExt[i][2], (double)s_projExt[i][3]);

        float quad[16] = {0}; /* filled per-frame (counter-rolled) */

        fGenVertexArrays(1, &s_vao[i]);
        fBindVertexArray(s_vao[i]);
        fGenBuffers(1, &s_vbo[i]);
        fBindBuffer(GL_ARRAY_BUFFER, s_vbo[i]);
        fBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, 0x88E8 /* GL_DYNAMIC_DRAW */);
        fEnableVertexAttribArray(0);
        fVertexAttribPointer(0, 2, GL_FLOAT, 0, 4 * sizeof(float), (void*)0);
        fEnableVertexAttribArray(1);
        fVertexAttribPointer(1, 2, GL_FLOAT, 0, 4 * sizeof(float),
                             (void*)(2 * sizeof(float)));
        fBindVertexArray(0);
    }

    INIT_RESTORE();
    #undef INIT_RESTORE

    s_state = 1;
    VrOvr_Log("[VR-FULL] ready: eye targets %ux%u\n", s_eyeW, s_eyeH);
    return 1;
}

/* Capture the finished game frame safely into THIS EYE's texture: pin the
 * read-FB to the scene first (OpenVR's internal GL can leave its own FBO
 * bound, invalidating naive reads; see VrOvr_SceneReadBegin). Both textures are (re)allocated together on
 * size change so the stale eye never samples a mis-sized texture. */
static void SafeCapture(int w, int h, int eye)
{
    /* record the caller's binding BEFORE any alloc work touches it */
    GLint prevTexBind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexBind);

    if (s_gameTex[0] == 0 || w != s_gameW || h != s_gameH)
    {
        int i;
        if (s_gameTex[0] == 0) glGenTextures(2, s_gameTex);
        for (i = 0; i < 2; i++)
        {
            glBindTexture(GL_TEXTURE_2D, s_gameTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        s_gameW = w;
        s_gameH = h;
        s_capValid[0] = 0; /* stale content gone - don't reproject it */
        s_capValid[1] = 0;
    }

    extern int  VrOvr_SceneReadBegin(int* outW, int* outH, int* outPrevRead);
    extern void VrOvr_SceneReadEnd(int prevRead);
    int rw, rh, prevRead;

    while (glGetError() != 0) {}

    VrOvr_SceneReadBegin(&rw, &rh, &prevRead);
    glBindTexture(GL_TEXTURE_2D, s_gameTex[eye]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexBind);

    {
        GLenum e = glGetError();
        if (e != 0)
        {
            static Uint32 t = 0;
            Uint32 n = SDL_GetTicks();
            if (n - t > 1000)
            {
                t = n;
                printf("[VR-FULL] capture glErr=0x%X\n", (unsigned)e);
            }
        }
    }

    VrOvr_SceneReadEnd(prevRead);
}

/* ---- public entry ------------------------------------------------------ */

/* OpenVR reads the submitted GL textures on its own, so the composed and
 * copied content must be finished first (glFinish). OpenXR copies into its
 * swapchain and syncs on xrReleaseSwapchainImage itself - a glFinish there
 * only stalled the CPU on the GPU every frame, so neither ran in parallel. */
static void VrFull_SyncForCompositor(void)
{
    extern int VrOvr_RuntimeIsOpenXR(void);
    if (!VrOvr_RuntimeIsOpenXR())
        glFinish();
}

/* Directions both eyes can see together - left, right, bottom, top
 * tangents (up +). vr_tracking.c sizes the game projection to cover them,
 * which is what makes FULL borderless. 0 until FULL has initialised. */
int VrFull_GetEyeCoverage(float out[4])
{
    if (s_state != 1) return 0;
    out[0] = (s_projExt[0][0] < s_projExt[1][0]) ? s_projExt[0][0] : s_projExt[1][0];
    out[1] = (s_projExt[0][1] > s_projExt[1][1]) ? s_projExt[0][1] : s_projExt[1][1];
    out[2] = (s_projExt[0][2] < s_projExt[1][2]) ? s_projExt[0][2] : s_projExt[1][2];
    out[3] = (s_projExt[0][3] > s_projExt[1][3]) ? s_projExt[0][3] : s_projExt[1][3];
    return 1;
}

/* Ensure the session-immutable submit texture pair exists (created ONCE,
 * never re-allocated - the compositor latches the first handles per eye
 * and ignores later handle changes; only CONTENT updates propagate). */
static int s_submitReady = 0;
static int s_lastWasFull = 0;

static int EnsureSubmitTex(void)
{
    if (s_submitReady) return 1;

    if (s_eyeW == 0 || s_eyeH == 0)
    {
        s_sys->GetRecommendedRenderTargetSize(&s_eyeW, &s_eyeH);
        if (s_eyeW == 0 || s_eyeH == 0) { s_eyeW = 1600; s_eyeH = 1800; }
    }

    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    int i;
    for (i = 0; i < 2; i++)
    {
        glGenTextures(1, &s_submitTex[i]);
        glBindTexture(GL_TEXTURE_2D, s_submitTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_eyeW, s_eyeH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    s_submitReady = 1;
    return 1;
}

static void BlackenSubmitTex(void)
{
    unsigned char* zero = (unsigned char*)calloc((size_t)s_eyeW * s_eyeH, 4);
    if (!zero) return;

    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    int i;
    for (i = 0; i < 2; i++)
    {
        glBindTexture(GL_TEXTURE_2D, s_submitTex[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_eyeW, s_eyeH,
                        GL_RGBA, GL_UNSIGNED_BYTE, zero);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    free(zero);
}

static void SubmitPair(void)
{
    VRTextureBounds_t b;
    b.uMin = 0.0f; b.vMin = 0.0f; b.uMax = 1.0f; b.vMax = 1.0f;

    int i;
    for (i = 0; i < 2; i++)
    {
        EVREye e = (i == 0) ? EVREye_Eye_Left : EVREye_Eye_Right;
        if (s_stampValid[i])
        {
            struct VRTextureWithPose_t tp;
            tp.handle      = (void*)(uintptr_t)s_submitTex[i];
            tp.eType       = ETextureType_TextureType_OpenGL;
            tp.eColorSpace = EColorSpace_ColorSpace_Gamma;
            tp.mDeviceToAbsoluteTracking = s_stampPose[i];
            s_comp->Submit(e, (struct Texture_t*)&tp, &b,
                           EVRSubmitFlags_Submit_TextureWithPose);
        }
        else
        {
            struct Texture_t tex;
            tex.handle      = (void*)(uintptr_t)s_submitTex[i];
            tex.eType       = ETextureType_TextureType_OpenGL;
            tex.eColorSpace = EColorSpace_ColorSpace_Gamma;
            s_comp->Submit(e, &tex, &b, EVRSubmitFlags_Submit_Default);
        }
    }
}

/* Called by vr_openvr.c's backdrop instead of submitting its own black
 * texture - keeps the session's submitted handles UNIFIED (the latch). */
void VrFull_SubmitBlack(void* sysTbl, void* compTbl)
{
    s_sys  = (struct VR_IVRSystem_FnTable*)sysTbl;
    s_comp = (struct VR_IVRCompositor_FnTable*)compTbl;
    if (!s_sys || !s_comp) return;
    if (!EnsureSubmitTex()) return;

    if (s_lastWasFull)
    {
        BlackenSubmitTex();
        s_lastWasFull = 0;
        s_stampValid[0] = 0; /* static content: no pose stamping */
        s_stampValid[1] = 0;
    }

    SubmitPair();
}

/* FMV frames in FULL mode: flat content composed into BOTH eyes (no
 * alternation - avoids the per-eye strobe), keeps the compositor paced. */
void VrFull_PushFmv(void* sysTbl, void* compTbl)
{
    s_sys  = (struct VR_IVRSystem_FnTable*)sysTbl;
    s_comp = (struct VR_IVRCompositor_FnTable*)compTbl;
    if (!s_sys || !s_comp) return;
    if (!EnsureSubmitTex() || !FullInit()) return;

    extern int VrOvr_SceneSize(int* outW, int* outH);
    int w = 0, h = 0;
    if (!VrOvr_SceneSize(&w, &h)) return;

    ComputeCropU(w, h);  /* live viewport crop (v1.2) */
    s_content[0] = -VR_FULL_FMV_TAN_X; s_content[1] = VR_FULL_FMV_TAN_X;
    s_content[2] = -VR_FULL_FMV_TAN_Y; s_content[3] = VR_FULL_FMV_TAN_Y;
    s_quadU0 = s_cropU0;  /* FMV always samples the mono capture */
    s_quadU1 = s_cropU1;
    SafeCapture(w, h, 0);

    /* flat FMV content: both eyes sample capture 0, no reprojection -
     * and invalidate the gameplay captures so the first frames back in
     * gameplay don't reproject leftover video */
    s_capValid[0] = 0;
    s_capValid[1] = 0;

    /* minimal state save (FMV player's own GL state is in flight) */
    GLint pFbo, pProg, pVao, pTex, pTex0, pActive, pVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &pFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &pProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &pVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &pActive);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex);
    glGetIntegerv(GL_VIEWPORT, pVp);
    fActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex0);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);

    int i;
    for (i = 0; i < 2; i++)
    {
        fBindFramebuffer(GL_FRAMEBUFFER, s_fbo[i]);
        glViewport(0, 0, (GLsizei)s_eyeW, (GLsizei)s_eyeH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        fUseProgram(s_prog);
        fActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_gameTex[0]);
        fUniform1i(fGetUniformLocation(s_prog, "uTex"), 0);
        {
            extern float VrTracking_GetHeadRollRad(void);
            float q[16];
            GLint pArr = 0;
            glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &pArr);
            BuildRolledQuad(i, VrTracking_GetHeadRollRad(), 0.0f, 0.0f, q);
            fBindVertexArray(s_vao[i]);
            fBindBuffer(GL_ARRAY_BUFFER, s_vbo[i]);
            fBufferData(GL_ARRAY_BUFFER, sizeof(q), q, 0x88E8);
            fBindBuffer(GL_ARRAY_BUFFER, (GLuint)pArr);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindTexture(GL_TEXTURE_2D, s_submitTex[i]);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                            (GLsizei)s_eyeW, (GLsizei)s_eyeH);
    }

    glBindTexture(GL_TEXTURE_2D, (GLuint)pTex0);
    fActiveTexture((GLenum)pActive);
    glBindTexture(GL_TEXTURE_2D, (GLuint)pTex);
    fBindVertexArray((GLuint)pVao);
    fUseProgram((GLuint)pProg);
    fBindFramebuffer(GL_FRAMEBUFFER, (GLuint)pFbo);
    glViewport(pVp[0], pVp[1], pVp[2], pVp[3]);

    if (s_poses[0].bPoseIsValid)
    {
        s_stampPose[0] = s_poses[0].mDeviceToAbsoluteTracking;
        s_stampPose[1] = s_stampPose[0];
        s_stampValid[0] = 1;
        s_stampValid[1] = 1;
    }

    VrFull_SyncForCompositor();
    SubmitPair();
    s_lastWasFull = 1;

    s_comp->WaitGetPoses(s_poses, 16, NULL, 0);

    {
        static Uint32 t = 0;
        Uint32 n = SDL_GetTicks();
        if (n - t > 1000)
        {
            t = n;
            printf("[VR-FULL] fmv frame pushed\n");
        }
    }
}


/* Build eye quad counter-rotated by head roll (Model-B): corners rotate
 * in TANGENT space (angularly correct), then map into this eye's NDC.
 * dxT/dyT (v1.1): reprojection shift in tangent space - moves the whole
 * quad so stale content re-aligns with the current camera rotation. The
 * shift rides the SAME counter-roll rotation as the corners (content
 * axes and quad axes stay locked together under roll). */
static void BuildRolledQuad(int eye, float rollRad, float dxT, float dyT,
                            float out[16])
{
    float cxT = 0.0f, cyT = 0.0f;
    float cr = cosf(-rollRad), sr = sinf(-rollRad);

    /* reprojection shift, counter-rolled like the corners */
    cxT += dxT * cr - dyT * sr;
    cyT += dxT * sr + dyT * cr;

    float xMin = s_projExt[eye][0], xMax = s_projExt[eye][1];
    float yMin = s_projExt[eye][2], yMax = s_projExt[eye][3];

    /* corner order matches the triangle strip: BL BR TL TR
     * (UVs from the LIVE crop - see ComputeCropU) */
    static const float cs[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };
    const float uv[4][2] = {
        { s_quadU0, VR_FULL_CROP_V0 },
        { s_quadU1, VR_FULL_CROP_V0 },
        { s_quadU0, VR_FULL_CROP_V1 },
        { s_quadU1, VR_FULL_CROP_V1 },
    };

    int k;
    for (k = 0; k < 4; k++)
    {
        float tx = (cs[k][0] < 0.0f) ? s_content[0] : s_content[1];
        float ty = (cs[k][1] < 0.0f) ? s_content[2] : s_content[3];
        float rx = tx * cr - ty * sr + cxT;
        float ry = tx * sr + ty * cr + cyT;

        float nx = (2.0f * rx - (xMax + xMin)) / (xMax - xMin);
        float ny = (2.0f * ry - (yMax + yMin)) / (yMax - yMin);
        out[k * 4 + 0] = nx;
        out[k * 4 + 1] = ny;
        out[k * 4 + 2] = uv[k][0];
        out[k * 4 + 3] = uv[k][1];
    }
}

/* Tangent-space reprojection shift for this eye: rotation delta between
 * NOW (this frame's applied camera angles) and the eye's capture. Fresh
 * eye = 0 by construction; stale eye = one frame of rotation. Clamped -
 * past ~20 deg the flat-shift approximation is garbage anyway (recenter
 * jumps, teleports), and SteamVR's own reprojection has the same limit. */
static void ComputeReprojShift(int eye, float* dxT, float* dyT)
{
    *dxT = 0.0f;
    *dyT = 0.0f;
    if (!s_reprojOn || !s_capValid[eye]) return;

    float dPitch = WrapPi(s_curAng[0] - s_capAng[eye][0]);
    float dYaw   = WrapPi(s_curAng[1] - s_capAng[eye][1]);
    float sx = (s_reprojCombo & 1) ? -1.0f : 1.0f;
    float sy = (s_reprojCombo & 2) ? -1.0f : 1.0f;

    float x = sx * tanf(dYaw);
    float y = sy * tanf(dPitch);
    if (x >  0.4f) x =  0.4f;
    if (x < -0.4f) x = -0.4f;
    if (y >  0.4f) y =  0.4f;
    if (y < -0.4f) y = -0.4f;
    *dxT = x;
    *dyT = y;
}

/* 2D INTERFACE PANEL. PsyX draws the UI pass (subtitles, message text, HUD)
 * into its own 4:3 texture in Full VR (PsyX_Stereo_GetUiTexture); here it is
 * laid over each eye as a flat panel straight ahead, head-locked like a HUD:
 *   width  = 2D_Screen_Size (100% = 60 deg), height = width * 3/4 - the
 *            UI frame's 4:3 picture, so it is never stretched;
 *   depth  = the virtual distance d (vr_screen_distance): each eye sees the
 *            panel offset by half the eye separation / d, the disparity of a
 *            real panel at d - consistent between the eyes, independent of
 *            the panel's size and of the world fov.
 * Composited with the "screen" blend (1-(1-a)(1-b)): the layer is cleared to
 * black and black stays see-through, without trusting the game shaders'
 * alpha (it is not coverage). The world image is not touched otherwise -
 * except while the quick options menu is open: a screen blend cannot make a
 * dark panel opaque, so the world is first darkened by the layer's alpha,
 * which the menu writes as its coverage (pc_quick_options.c, VR layer). */
static void DrawUiPanel(int eye)
{
    extern int Pc_QuickOptions_IsOpen(void);
    extern unsigned int PsyX_Stereo_GetUiTexture(int* w, int* h);
    extern float VrTracking_GetUiPanelDeg(void);
    extern float VrTracking_GetUiPanelDistance(void);
    extern float VrTracking_GetHalfIpdMeters(void);
    GLuint uiTex = PsyX_Stereo_GetUiTexture(NULL, NULL);
    float  hw, hh, off, xMin, xMax, yMin, yMax, q[16];
    GLint  bSrc, bDst, bSrcA, bDstA, bEq, bEqA, pArr = 0;
    int    k;
    static const float cs[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };

    if (uiTex == 0) return;

    hw  = tanf(VrTracking_GetUiPanelDeg() * 0.5f * 3.14159265f / 180.0f);
    hh  = hw * 0.75f;
    off = VrTracking_GetHalfIpdMeters() / VrTracking_GetUiPanelDistance();
    if (eye == 1) off = -off;   /* left eye sees a near panel to the right */

    xMin = s_projExt[eye][0]; xMax = s_projExt[eye][1];
    yMin = s_projExt[eye][2]; yMax = s_projExt[eye][3];
    for (k = 0; k < 4; k++)
    {
        float tx = cs[k][0] * hw + off, ty = cs[k][1] * hh;
        q[k * 4 + 0] = (2.0f * tx - (xMax + xMin)) / (xMax - xMin);
        q[k * 4 + 1] = (2.0f * ty - (yMax + yMin)) / (yMax - yMin);
        q[k * 4 + 2] = (cs[k][0] + 1.0f) * 0.5f;
        q[k * 4 + 3] = (cs[k][1] + 1.0f) * 0.5f;
    }

    glGetIntegerv(0x80C9 /* GL_BLEND_SRC_RGB */,   &bSrc);
    glGetIntegerv(0x80C8 /* GL_BLEND_DST_RGB */,   &bDst);
    glGetIntegerv(0x80CB /* GL_BLEND_SRC_ALPHA */, &bSrcA);
    glGetIntegerv(0x80CA /* GL_BLEND_DST_ALPHA */, &bDstA);
    glGetIntegerv(0x8009 /* GL_BLEND_EQUATION_RGB */,   &bEq);
    glGetIntegerv(0x883D /* GL_BLEND_EQUATION_ALPHA */, &bEqA);

    glEnable(GL_BLEND);
    fBlendEquationSeparate(0x8006 /* GL_FUNC_ADD */, 0x8006);

    glBindTexture(GL_TEXTURE_2D, uiTex);
    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &pArr);
    fBindVertexArray(s_vao[eye]);
    fBindBuffer(GL_ARRAY_BUFFER, s_vbo[eye]);
    fBufferData(GL_ARRAY_BUFFER, sizeof(q), q, 0x88E8);
    fBindBuffer(GL_ARRAY_BUFFER, (GLuint)pArr);

    if (Pc_QuickOptions_IsOpen())
    {
        /* the menu's backdrop: world * (1 - layer alpha). Only this pass
         * reads the texture's alpha (uAlpha); every other draw keeps 1. */
        const GLint aLoc = fGetUniformLocation(s_prog, "uAlpha");
        fUniform1i(aLoc, 1);
        fBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        fUniform1i(aLoc, 0);
    }
    fBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* the caller restores the GL_BLEND enable; the funcs are ours to undo */
    fBlendFuncSeparate((GLenum)bSrc, (GLenum)bDst, (GLenum)bSrcA, (GLenum)bDstA);
    fBlendEquationSeparate((GLenum)bEq, (GLenum)bEqA);
    glDisable(GL_BLEND);
}

/* The flat frame as a 2D screen: Screen_Width at Screen_Distance, like the
 * floating screen, in front of the frozen world (drawn into the frozen
 * image, so it stays where it opened). Menus with their own screen
 * (inventory, options, save) are opaque. Screens the game lays over its
 * frozen view (pause, the take screens of pickups: g_PsxPresentLastFrame)
 * get a black backdrop instead of that flat view (PsyX_render.cpp
 * GR_PresentLastFrame) - the stereo world around is the view - so here the
 * world behind the panel is only dimmed and the panel is added with the
 * screen blend: black stays see-through, the text and the item show. */
#define VR_FLAT_OVERLAY_DIM 0.55f
/* Over the frozen WORLD the eye images carry the current head pose, so the
 * screen is placed in the room itself: the head pose when it opened
 * (s_panelRef), the screen in front of it, seen from the current pose. Over
 * the old frozen IMAGE (no recorded world) the image keeps its old pose and
 * the screen rides in it as before. */
static HmdMatrix34_t s_panelRef;
static int           s_panelRefOn;

static void DrawFlatPanel(int eye)
{
    extern int g_PsxPresentLastFrame;
    const int  overlay = g_PsxPresentLastFrame != 0;
    extern float VrTracking_GetScreenWidth(void);
    extern float VrTracking_GetScreenDistance(void);
    extern float VrTracking_GetHalfIpdMeters(void);
    float  dist = VrTracking_GetScreenDistance(), hw, hh, off, xMin, xMax, yMin, yMax, q[16];
    GLint  pArr = 0;
    int    k;
    static const float cs[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };

    if (s_flatTex == 0 || s_flatW <= 0 || dist < 0.1f) return;

    hw  = VrTracking_GetScreenWidth() * 0.5f / dist;
    hh  = hw * (float)s_flatH / (float)s_flatW;
    off = VrTracking_GetHalfIpdMeters() / dist;
    if (eye == 1) off = -off;

    xMin = s_projExt[eye][0]; xMax = s_projExt[eye][1];
    yMin = s_projExt[eye][2]; yMax = s_projExt[eye][3];
    for (k = 0; k < 4; k++)
    {
        float tx = cs[k][0] * hw + off, ty = cs[k][1] * hh;
        if (s_panelRefOn && s_framePoseValid && !PsyX_StereoIsFrozen())
        {
            /* the corner in the opening head frame -> tracking -> current eye */
            const float pc[3] = { cs[k][0] * hw * dist, cs[k][1] * hh * dist, -dist };
            float       w[3], l[3];
            int         i, j;
            for (i = 0; i < 3; i++)
            {
                w[i] = s_panelRef.m[i][3];
                for (j = 0; j < 3; j++) w[i] += s_panelRef.m[i][j] * pc[j];
            }
            for (i = 0; i < 3; i++)
            {
                l[i] = 0.0f;
                for (j = 0; j < 3; j++) l[i] += s_framePose.m[j][i] * (w[j] - s_framePose.m[j][3]);
            }
            l[0] += (eye == 0) ? VrTracking_GetHalfIpdMeters() : -VrTracking_GetHalfIpdMeters();
            if (l[2] > -0.05f)
                return;   /* the screen is behind you */
            tx = l[0] / -l[2];
            ty = l[1] / -l[2];
        }
        q[k * 4 + 0] = (2.0f * tx - (xMax + xMin)) / (xMax - xMin);
        q[k * 4 + 1] = (2.0f * ty - (yMax + yMin)) / (yMax - yMin);
        q[k * 4 + 2] = (cs[k][0] + 1.0f) * 0.5f;
        q[k * 4 + 3] = (cs[k][1] + 1.0f) * 0.5f;
    }

    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, s_flatTex);
    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &pArr);
    fBindVertexArray(s_vao[eye]);
    fBindBuffer(GL_ARRAY_BUFFER, s_vbo[eye]);
    fBufferData(GL_ARRAY_BUFFER, sizeof(q), q, 0x88E8);
    fBindBuffer(GL_ARRAY_BUFFER, (GLuint)pArr);
    if (!overlay)
    {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        return;
    }
    glEnable(GL_BLEND);
    fBlendEquationSeparate(0x8006 /* GL_FUNC_ADD */, 0x8006);
    /* world * (1 - dim) under the panel */
    fBlendColor(0.0f, 0.0f, 0.0f, VR_FLAT_OVERLAY_DIM);
    fBlendFuncSeparate(GL_ZERO, 0x8004 /* GL_ONE_MINUS_CONSTANT_ALPHA */, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    /* + the panel, screen blend */
    fBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    fBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
}

/* VR POINTER: the right controller as the mouse on whichever menu panel
 * is open - the quick menu on the interface panel (DrawUiPanel), or a game
 * screen on the 2D screen (DrawFlatPanel). The aim ray is taken into the
 * head's frame (both panels are head-locked, centred straight ahead) and
 * cut with the panel's plane; the hit, as a fraction of the picture, is
 * handed to pc_mouse_cursor.c, so everything that already takes the mouse
 * takes it. Trigger = left click, grip = right click (vr_interact.c keeps
 * them from the game while the ray is on the panel). */
static int s_ptrOnPanel;

int VrFull_PointerOnPanel(void)
{
    return s_ptrOnPanel;
}

static void PointerUpdate(void)
{
    extern int   Pc_QuickOptions_IsOpen(void);
    extern unsigned int PsyX_Stereo_GetUiTexture(int* w, int* h);
    extern void  Pc_MouseCursor_SetVr(int on, int inView, float vx, float vy, int left, int right);
    extern float VrTracking_GetUiPanelDeg(void);
    extern float VrTracking_GetUiPanelDistance(void);
    extern float VrTracking_GetScreenWidth(void);
    extern float VrTracking_GetScreenDistance(void);
    extern int   VrOvr_GetAimPose(float pos[3], float rot[3][3]);
    extern void  VrInteract_RawRight(float* trigger, float* grip);
    extern void  PsyX_GetDisplayViewport(int* outX, int* outY, int* outW, int* outH);
    static int   s_click, s_rclick;
    const int    qo   = Pc_QuickOptions_IsOpen() && PsyX_Stereo_GetUiTexture(NULL, NULL) != 0;
    const int    flat = !qo && PsyX_StereoUiScreen() && s_flatTex != 0 && s_flatW > 0;
    float ap[3], ar[3][3], o[3], d[3], dist, hw, hh, t, u, v, trig = 0.0f, grip = 0.0f;
    int   i, in;

    s_ptrOnPanel = 0;
    if ((!qo && !flat) || !s_framePoseValid || !VrOvr_GetAimPose(ap, ar))
    {
        Pc_MouseCursor_SetVr(0, 0, 0.0f, 0.0f, 0, 0);
        s_click = s_rclick = 0;
        return;
    }

    /* tracking space -> the panel's head frame: R^T (p - t); the ray is the
     * aim's -Z. A 2D screen over the frozen world stays where it opened
     * (s_panelRef, DrawFlatPanel); the others follow the head. */
    {
        const HmdMatrix34_t* fp = (flat && s_panelRefOn && !PsyX_StereoIsFrozen()) ? &s_panelRef : &s_framePose;
        for (i = 0; i < 3; i++)
        {
            o[i] = fp->m[0][i] * (ap[0] - fp->m[0][3]) + fp->m[1][i] * (ap[1] - fp->m[1][3]) +
                   fp->m[2][i] * (ap[2] - fp->m[2][3]);
            d[i] = -(fp->m[0][i] * ar[0][2] + fp->m[1][i] * ar[1][2] + fp->m[2][i] * ar[2][2]);
        }
    }

    if (qo)
    {
        dist = VrTracking_GetUiPanelDistance();
        hw   = tanf(VrTracking_GetUiPanelDeg() * 0.5f * 3.14159265f / 180.0f) * dist;
        hh   = hw * 0.75f;
    }
    else
    {
        dist = VrTracking_GetScreenDistance();
        hw   = VrTracking_GetScreenWidth() * 0.5f;
        hh   = hw * (float)s_flatH / (float)s_flatW;
    }
    if (dist < 0.1f || d[2] > -1e-3f)
    {
        Pc_MouseCursor_SetVr(1, 0, 0.0f, 0.0f, 0, 0);   /* pointing away */
        return;
    }
    t = (-dist - o[2]) / d[2];
    u = ((o[0] + d[0] * t) / hw + 1.0f) * 0.5f;          /* 0 = left    */
    v = (1.0f - (o[1] + d[1] * t) / hh) * 0.5f;          /* 0 = top     */
    in = t > 0.0f && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;

    if (flat)
    {
        /* the 2D screen is the whole frame: the picture is its viewport,
         * centred in it (pillarbox / letterbox) */
        int vx, vy, vw, vh;
        PsyX_GetDisplayViewport(&vx, &vy, &vw, &vh);
        if (vw > 0 && vh > 0)
        {
            const float fl = (float)vx / (float)(2 * vx + vw), fw = (float)vw / (float)(2 * vx + vw);
            const float ft = (float)vy / (float)(2 * vy + vh), fh = (float)vh / (float)(2 * vy + vh);
            u = (u - fl) / fw;
            v = (v - ft) / fh;
            in = in && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
        }
    }

    VrInteract_RawRight(&trig, &grip);
    s_click  = s_click  ? trig > 0.45f : trig > 0.65f;
    s_rclick = s_rclick ? grip > 0.45f : grip > 0.65f;
    s_ptrOnPanel = in;
    Pc_MouseCursor_SetVr(1, in, u, v, in && s_click, in && s_rclick);
}

int VrFull_Push(void* sysTbl, void* compTbl, int* parity,
                void* outPoses16, int* outPosesValid)
{
    s_sys  = (struct VR_IVRSystem_FnTable*)sysTbl;
    s_comp = (struct VR_IVRCompositor_FnTable*)compTbl;
    if (!s_sys || !s_comp) return 0;

    if (!EnsureSubmitTex()) return 0;
    if (!FullInit()) return 0;

    extern int VrOvr_SceneSize(int* outW, int* outH);
    int w = 0, h = 0;
    if (!VrOvr_SceneSize(&w, &h)) return 1;

    PollCalibrationKeys();
    SnapTick();

    /* the pose this frame was RENDERED with = the caller's buffer content
     * from the PREVIOUS WaitGetPoses (we submit before refreshing it) */
    s_framePoseValid = 0;
    {
        TrackedDevicePose_t* pp = (TrackedDevicePose_t*)outPoses16;
        if (pp == NULL) pp = s_poses;
        if (pp[0].bPoseIsValid)
        {
            s_framePose = pp[0].mDeviceToAbsoluteTracking;
            s_framePoseValid = 1;
        }
    }
    PointerUpdate();

    int eye = (*parity) & 1;

    /* The image's geometry - the viewport crop and the angles it spans - is
     * the geometry it was RENDERED with. A frozen world (inspect, read,
     * pickups, the pause/inventory/map screens behind their 2D screen) is
     * the last rendered eye image re-presented, while the game meanwhile
     * switches to its own projection for those screens (H 224..376 instead
     * of the wide Full VR one, a 4:3 crop): taking that live geometry blew
     * the frozen picture up several times over every such screen. So it is
     * only read from frames whose world was really drawn; the frozen image
     * keeps it, like it keeps its pose (s_stampPose below). */
    /* ...and not from a menu's transition frame (UI context already up,
     * world drawn with the game's own H ~187): it set the 98 x 66 deg of the
     * game's view, and the frozen VR world was then squeezed into that. */
    extern int VrTracking_GetUiContext(void);
    if (!PsyX_StereoIsFrozen() && !PsyX_StereoUiScreen() && VrTracking_GetUiContext() == 0)
    {
        extern int PsyX_GetWorldTangents(float out[4]);
        float t[4];
        ComputeCropU(w, h);  /* live viewport crop (v1.2) */
        if (PsyX_GetWorldTangents(t))
            memcpy(s_content, t, sizeof(s_content));

        /* EYE CROP (PsyX_render.cpp PsyX_Stereo_SetEyeCrop, Quest): the part
         * of the eye image each eye shows - its frustum (s_projExt) in the
         * image's tangents (s_content), turned by the head roll the compose
         * counter-rotates by (BuildRolledQuad), +-VR_EYE_CROP_ROLL of roll
         * and VR_EYE_CROP_PAD of tangent on every side for what changes
         * until the image is shown. */
        {
            extern void  PsyX_Stereo_SetEyeCrop(int eye, float u0, float u1, float v0, float v1);
            extern float VrTracking_GetHeadRollRad(void);
            const float roll = VrTracking_GetHeadRollRad();
            const float cw = s_content[1] - s_content[0], ch = s_content[3] - s_content[2];
            int e;
            for (e = 0; e < 2 && cw > 0.0f && ch > 0.0f; e++)
            {
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                int   a, k;
                for (a = -1; a <= 1; a++)
                {
                    const float rr = roll + (float)a * VR_EYE_CROP_ROLL;
                    const float c = cosf(rr), sn = sinf(rr);
                    for (k = 0; k < 4; k++)
                    {
                        const float rx = (k & 1) ? s_projExt[e][1] : s_projExt[e][0];
                        const float ry = (k & 2) ? s_projExt[e][3] : s_projExt[e][2];
                        const float tx = rx * c - ry * sn, ty = rx * sn + ry * c;
                        if (tx < x0) x0 = tx;
                        if (tx > x1) x1 = tx;
                        if (ty < y0) y0 = ty;
                        if (ty > y1) y1 = ty;
                    }
                }
                x0 -= VR_EYE_CROP_PAD; x1 += VR_EYE_CROP_PAD;
                y0 -= VR_EYE_CROP_PAD; y1 += VR_EYE_CROP_PAD;
                PsyX_Stereo_SetEyeCrop(e,
                    s_cropU0 + (x0 - s_content[0]) / cw * (s_cropU1 - s_cropU0),
                    s_cropU0 + (x1 - s_content[0]) / cw * (s_cropU1 - s_cropU0),
                    VR_FULL_CROP_V0 + (y0 - s_content[2]) / ch * (VR_FULL_CROP_V1 - VR_FULL_CROP_V0),
                    VR_FULL_CROP_V0 + (y1 - s_content[2]) / ch * (VR_FULL_CROP_V1 - VR_FULL_CROP_V0));
                {
                    static int s_cropLog;
                    if (s_cropLog < 4)
                    {
                        s_cropLog++;
                        VrOvr_Log("[VR-FULL] eye %d crop u %.3f..%.3f v %.3f..%.3f of its image\n", e,
                                  (double)(s_cropU0 + (x0 - s_content[0]) / cw * (s_cropU1 - s_cropU0)),
                                  (double)(s_cropU0 + (x1 - s_content[0]) / cw * (s_cropU1 - s_cropU0)),
                                  (double)((y0 - s_content[2]) / ch), (double)((y1 - s_content[2]) / ch));
                    }
                }
            }
        }

        /* EYE RESOLUTION (PsyX_render.cpp PsyX_Stereo_SetScale): the factor
         * over the window that gives the eye image the headset's own pixel
         * density - pixels the eye needs across the image's angles over the
         * pixels the window's viewport has for them, the larger axis, 1..2.
         * Changed only past 5%, so the targets are not rebuilt for jitter. */
        {
            extern void PsyX_Stereo_SetScale(float k);
            static float s_kSet = 0.0f;
            const float eyeTx = s_projExt[0][1] - s_projExt[0][0];
            const float eyeTy = s_projExt[0][3] - s_projExt[0][2];
            const float haveX = (float)w * (s_cropU1 - s_cropU0);
            const float haveY = (float)h * (VR_FULL_CROP_V1 - VR_FULL_CROP_V0);
            if (eyeTx > 0.0f && eyeTy > 0.0f && haveX > 1.0f && haveY > 1.0f)
            {
                const float kx = (float)s_eyeW * ((s_content[1] - s_content[0]) / eyeTx) / haveX;
                const float ky = (float)s_eyeH * ((s_content[3] - s_content[2]) / eyeTy) / haveY;
                extern float VrTracking_GetEyeResMax(void);
                const float kmax = VrTracking_GetEyeResMax();   /* Eye_Resolution: GPU time is the player's call */
                float       k  = kx > ky ? kx : ky;
                if (k < 1.0f) k = 1.0f;
                if (k > kmax) k = kmax;
                if (fabsf(k - s_kSet) > 0.05f)
                {
                    s_kSet = k;
                    PsyX_Stereo_SetScale(k);
                    VrOvr_Log("[VR-FULL] eye images at %.2fx the window (needed %.2f across, %.2f up)\n",
                              (double)k, (double)kx, (double)ky);
                }
            }
        }
    }

    /* Log the image geometry whenever it changes (fov switch, camera,
     * resolution): render size, sampled crop, the GTE H the world used and
     * the angles the image spans - what to compare against the eye fov. */
    {
        extern float g_PcWorldProj[3];
        static float last[6];
        float cur[6] = { (float)w, (float)h, g_PcWorldProj[0],
                         s_content[1] - s_content[0], s_content[3] - s_content[2], s_cropU1 - s_cropU0 };
        int k, changed = 0;
        for (k = 0; k < 6; k++)
            if (fabsf(cur[k] - last[k]) > 0.01f * (fabsf(last[k]) + 1e-3f)) changed = 1;
        if (changed)
        {
            memcpy(last, cur, sizeof(last));
            VrOvr_Log("[VR-FULL] render %dx%d crop u %.3f..%.3f  H %.0f  image tan L%.3f R%.3f B%.3f T%.3f = %.1f x %.1f deg\n",
                      w, h, (double)s_cropU0, (double)s_cropU1, (double)g_PcWorldProj[0],
                      (double)s_content[0], (double)s_content[1], (double)s_content[2], (double)s_content[3],
                      (atan((double)s_content[1]) - atan((double)s_content[0])) * 57.29578,
                      (atan((double)s_content[3]) - atan((double)s_content[2])) * 57.29578);
        }
    }

    /* v2.0 SUMMIT: if PsyX rendered true per-vertex stereo this frame,
     * both eyes live in its side-by-side texture - SAME frame, real
     * parallax. Backbuffer capture, angle stamping, and stale-eye
     * reprojection are all bypassed (nothing is stale anymore). The
     * capture path below remains as the automatic fallback. */
    unsigned int stTex = 0;
    int stW = 0, stH = 0;
    {
        extern unsigned int PsyX_Stereo_GetTexture(int* perEyeW, int* perEyeH);
        stTex = PsyX_Stereo_GetTexture(&stW, &stH);
    }
    const int flatOk = stTex != 0 && PsyX_StereoUiScreen() && FlatCapture();
    /* the 2D screen over the frozen world opens where you look (DrawFlatPanel) */
    if (flatOk && !s_panelRefOn && s_framePoseValid)
    {
        s_panelRef   = s_framePose;
        s_panelRefOn = 1;
    }
    else if (!flatOk)
        s_panelRefOn = 0;

    /* VR hands (vr_hands.c): live world frames only - a frozen image keeps an
     * older pose, and the 2D screens are menus */
    extern int  VrHands_BeginFrame(const HmdMatrix34_t* head, int poseValid);
    extern void VrHands_DrawEye(int eye, GLuint fbo, unsigned w, unsigned h, const float ext[4]);
    const int handsOn = stTex != 0 && !PsyX_StereoIsFrozen() && !PsyX_StereoUiScreen() &&
                        VrHands_BeginFrame(&s_framePose, s_framePoseValid);

    if (stTex == 0)
    {
        SafeCapture(w, h, eye);

        /* stamp this eye's capture with the camera angles it was built with */
        {
            extern void VrTracking_GetAppliedAngles(float* pitch, float* yaw);
            VrTracking_GetAppliedAngles(&s_curAng[0], &s_curAng[1]);
            s_capAng[eye][0] = s_curAng[0];
            s_capAng[eye][1] = s_curAng[1];
            s_capValid[eye]  = 1;
        }
    }

    /* ---- compose BOTH eyes (v1.1): fresh eye as-is, stale eye with its
     * rotation-compensating quad shift - the alternating-eye ghosting
     * fix. Each eye samples ITS OWN capture texture. ---- */
    GLint pFbo, pProg, pVao, pTex, pTex0, pActive, pVp[4];
    float pClearF[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &pFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &pProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &pVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &pActive);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, pClearF);
    glGetIntegerv(GL_VIEWPORT, pVp);

    fActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex0);

    GLboolean hBlend = glIsEnabled(GL_BLEND);
    GLboolean hDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean hScis  = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean hCull  = glIsEnabled(GL_CULL_FACE);
    GLboolean hSten  = glIsEnabled(GL_STENCIL_TEST);
    GLboolean hDisc  = glIsEnabled(GL_RASTERIZER_DISCARD);
    GLboolean hClip[8];
    {
        int ci;
        for (ci = 0; ci < 8; ci++)
        {
            hClip[ci] = glIsEnabled(GL_CLIP_DISTANCE0 + ci);
            if (hClip[ci]) glDisable(GL_CLIP_DISTANCE0 + ci);
        }
    }
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_RASTERIZER_DISCARD);

    {
        extern float VrTracking_GetHeadRollRad(void);
        float roll = VrTracking_GetHeadRollRad();
        int e;
        for (e = 0; e < 2; e++)
        {
            float dxT = 0.0f, dyT = 0.0f;

            if (stTex != 0)
            {
                /* summit: this eye's half of the SxS stereo texture,
                 * crop remapped into the half; zero reprojection (both
                 * eyes are this frame - nothing to compensate) */
                s_quadU0 = 0.5f * (float)e + 0.5f * s_cropU0;
                s_quadU1 = 0.5f * (float)e + 0.5f * s_cropU1;
            }
            else
            {
                if (!s_capValid[e]) continue; /* nothing captured yet */
                s_quadU0 = s_cropU0;
                s_quadU1 = s_cropU1;
                ComputeReprojShift(e, &dxT, &dyT);
            }

            fBindFramebuffer(GL_FRAMEBUFFER, s_fbo[e]);
            glViewport(0, 0, (GLsizei)s_eyeW, (GLsizei)s_eyeH);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            fUseProgram(s_prog);
            fActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, (stTex != 0) ? (GLuint)stTex : s_gameTex[e]);
            fUniform1i(fGetUniformLocation(s_prog, "uTex"), 0);

            {
                float q[16];
                GLint pArr = 0;
                glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &pArr);
                BuildRolledQuad(e, roll, dxT, dyT, q);
                fBindVertexArray(s_vao[e]);
                fBindBuffer(GL_ARRAY_BUFFER, s_vbo[e]);
                fBufferData(GL_ARRAY_BUFFER, sizeof(q), q, 0x88E8);
                fBindBuffer(GL_ARRAY_BUFFER, (GLuint)pArr); /* PsyX's binding back */
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            /* the hands over the world, under the interface panel */
            if (handsOn)
                VrHands_DrawEye(e, s_fbo[e], s_eyeW, s_eyeH, s_projExt[e]);

            /* the 2D interface panel (subtitles, messages) over the world,
             * or an in-game menu's whole frame as a 2D screen */
            if (stTex != 0 && flatOk)
                DrawFlatPanel(e);
            else if (stTex != 0)
                DrawUiPanel(e);
            SnapEye(e, (int)s_eyeW, (int)s_eyeH);

            /* composed image -> THE immutable submit texture (content
             * update on the latched handle: the only live path) */
#if defined(__ANDROID__)
            {
                /* [VR-DIAG] composed eye content, once a second for a while */
                static Uint32 tD = 0;
                static int nD = 0;
                const Uint32 now = SDL_GetTicks();
                if (e == 0 && nD < 15 && now - tD >= 1000)
                {
                    unsigned char cp[4] = { 0 }, sp[4] = { 0 };
                    GLenum err = glGetError();
                    tD = now;
                    nD++;
                    glReadPixels((GLint)s_eyeW / 2, (GLint)s_eyeH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, cp);
                    if (stTex != 0)
                    {
                        static GLuint probeFbo = 0;
                        if (!probeFbo) fGenFramebuffers(1, &probeFbo);
                        fBindFramebuffer(GL_READ_FRAMEBUFFER, probeFbo);
                        fFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)stTex, 0);
                        glReadPixels(stW / 2, stH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sp);
                        fBindFramebuffer(GL_FRAMEBUFFER, s_fbo[e]);
                    }
                    VrOvr_Log("[VR-DIAG] full: stTex=%u %dx%d centre %u,%u,%u | eye0 composed %u,%u,%u | prog=%u frozen=%d ui=%d flat=%d hands=%d | err before=0x%X after=0x%X\n",
                              (unsigned)stTex, stW, stH, sp[0], sp[1], sp[2], cp[0], cp[1], cp[2],
                              (unsigned)s_prog, PsyX_StereoIsFrozen(), PsyX_StereoUiScreen(), flatOk, handsOn,
                              (unsigned)err, (unsigned)glGetError());
                }
            }
#endif
            glBindTexture(GL_TEXTURE_2D, s_submitTex[e]);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                (GLsizei)s_eyeW, (GLsizei)s_eyeH);
        }
    }

    /* restore everything */
    glBindTexture(GL_TEXTURE_2D, (GLuint)pTex0);
    fActiveTexture((GLenum)pActive);
    glBindTexture(GL_TEXTURE_2D, (GLuint)pTex);
    fBindVertexArray((GLuint)pVao);
    fUseProgram((GLuint)pProg);
    fBindFramebuffer(GL_FRAMEBUFFER, (GLuint)pFbo);
    glClearColor(pClearF[0], pClearF[1], pClearF[2], pClearF[3]);
    glViewport(pVp[0], pVp[1], pVp[2], pVp[3]);
    {
        int ci;
        for (ci = 0; ci < 8; ci++)
        {
            if (hClip[ci]) glEnable(GL_CLIP_DISTANCE0 + ci);
        }
    }
    if (hDisc)  glEnable(GL_RASTERIZER_DISCARD);
    if (hSten)  glEnable(GL_STENCIL_TEST);
    if (hBlend) glEnable(GL_BLEND);
    if (hDepth) glEnable(GL_DEPTH_TEST);
    if (hScis)  glEnable(GL_SCISSOR_TEST);
    if (hCull)  glEnable(GL_CULL_FACE);

    /* a frozen frame (PsyX_StereoIsFrozen) is the image of an earlier pose:
     * keep that pose on it, so it stays put in the room */
    if (s_framePoseValid && !PsyX_StereoIsFrozen())
    {
#if VR_FULL_STAMP_MODE == 2
        /* unified: BOTH eyes carry this frame's fresh pose */
        s_stampPose[0] = s_framePose;
        s_stampPose[1] = s_framePose;
        s_stampValid[0] = 1;
        s_stampValid[1] = 1;
#elif VR_FULL_STAMP_MODE == 1
        s_stampPose[eye]  = s_framePose;
        s_stampValid[eye] = 1;
#else
        s_stampValid[0] = 0;
        s_stampValid[1] = 0;
#endif
    }

    VrFull_SyncForCompositor();

    SubmitPair();
    s_lastWasFull = 1;

    *parity ^= 1;

    /* deliver poses to the CALLER's buffer so vr_openvr's pose polling
     * (and therefore head tracking) stays alive in FULL mode - the
     * module doing a private WaitGetPoses starved tracking. */
    if (outPoses16 != NULL)
    {
        s_comp->WaitGetPoses((TrackedDevicePose_t*)outPoses16, 16, NULL, 0);
        if (outPosesValid != NULL)
        {
            *outPosesValid = 1;
        }
    }
    else
    {
        s_comp->WaitGetPoses(s_poses, 16, NULL, 0);
    }

    {
        static Uint32 t = 0;
        Uint32 n = SDL_GetTicks();
        if (n - t > 1000)
        {
            t = n;
            int stale = eye ^ 1;
            float dP = s_capValid[stale] ? WrapPi(s_curAng[0] - s_capAng[stale][0]) : 0.0f;
            float dY = s_capValid[stale] ? WrapPi(s_curAng[1] - s_capAng[stale][1]) : 0.0f;
            printf("[VR-FULL] live eye=%d src=%s reproj=%s combo=%d staleDelta(%.2f, %.2f)deg\n",
                   eye, (stTex != 0) ? "GPU-STEREO" : "capture",
                   s_reprojOn ? "ON" : "off", s_reprojCombo,
                   (double)(dP * 57.2958f), (double)(dY * 57.2958f));
        }
    }

    return 1;
}
