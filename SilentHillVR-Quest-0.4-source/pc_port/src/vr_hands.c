/*
=============================================================================
 SH1 PC Port - VR HANDS (the GTA SA VR / Bully VR hands)
=============================================================================
 Replaces the IK body: in first-person VR gameplay Harry is not drawn at all
 (bodyprog_bone_80044F14.c) and these two hands follow the controllers.
 Cutscenes and every other camera keep Harry's own body.

 Meshes: UltimateXR "BigHands" (MIT, see vrhands\SOURCE.md) in the UXRH
 format of GTA SA VR - every vertex carries 4 baked finger poses (open, grip,
 trigger, both), blended by the controller's grip and trigger values.
 Placement is GTA SA VR's: position = grip pose, forward = the pointing
 direction, up = the grip's +X (mirrored for the left hand); mesh axes are
 X forward (wrist 5.5 cm back), Z right, -Y up.

 Files: <game>\vrhands\BigHandLeft.uxrh, BigHandRight.uxrh, BigHandsAlbedo.rgba
 (1024x1024 RGBA8).

 Drawn by vr_full.c into each composed eye image, in the HEADSET's frame (the
 pose the eye image is stamped with), at real size - World_Scale does not
 apply: these are your hands. They have their own depth buffer (fingers over
 the palm) and are drawn over the world image, which has no depth to test
 against.

 The pointing direction: OpenXR has an aim pose for the right hand only; the
 aim's rotation relative to the grip is a fixed turn about the grip's X, so
 it is read from the right controller (when both are tracked) and applied to
 both hands. OpenVR's Touch pose already points ahead (identity).
=============================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include "vr_gl.h"

#include "openvr_capi.h"

extern void VrOvr_Log(const char* fmt, ...);
extern int  VrOvr_GetHandPose(int hand, float pos[3], float rot[3][3]);
extern int  VrOvr_GetAimPose(float pos[3], float rot[3][3]);
extern void VrInteract_RawHand(int hand, float* trigger, float* grip);
extern int  VrInteract_HandHolds(int hand);
extern int  VrIk_HandsActive(void);
extern float VrTracking_GetHalfIpdMeters(void);

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

typedef GLuint (*PH_CreateShader)(GLenum);
typedef void   (*PH_ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void   (*PH_CompileShader)(GLuint);
typedef void   (*PH_GetShaderiv)(GLuint, GLenum, GLint*);
typedef void   (*PH_GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
typedef GLuint (*PH_CreateProgram)(void);
typedef void   (*PH_AttachShader)(GLuint, GLuint);
typedef void   (*PH_LinkProgram)(GLuint);
typedef void   (*PH_GetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (*PH_UseProgram)(GLuint);
typedef void   (*PH_GenVertexArrays)(GLsizei, GLuint*);
typedef void   (*PH_BindVertexArray)(GLuint);
typedef void   (*PH_GenBuffers)(GLsizei, GLuint*);
typedef void   (*PH_BindBuffer)(GLenum, GLuint);
typedef void   (*PH_BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
typedef void   (*PH_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void   (*PH_EnableVertexAttribArray)(GLuint);
typedef GLint  (*PH_GetUniformLocation)(GLuint, const char*);
typedef void   (*PH_Uniform1i)(GLint, GLint);
typedef void   (*PH_Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (*PH_UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void   (*PH_ActiveTexture)(GLenum);
typedef void   (*PH_GenRenderbuffers)(GLsizei, GLuint*);
typedef void   (*PH_BindRenderbuffer)(GLenum, GLuint);
typedef void   (*PH_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (*PH_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef void   (*PH_BindFramebuffer)(GLenum, GLuint);
typedef void   (*PH_BindSampler)(GLuint, GLuint);

static PH_CreateShader            hCreateShader;
static PH_ShaderSource            hShaderSource;
static PH_CompileShader           hCompileShader;
static PH_GetShaderiv             hGetShaderiv;
static PH_GetShaderInfoLog        hGetShaderInfoLog;
static PH_CreateProgram           hCreateProgram;
static PH_AttachShader            hAttachShader;
static PH_LinkProgram             hLinkProgram;
static PH_GetProgramiv            hGetProgramiv;
static PH_UseProgram              hUseProgram;
static PH_GenVertexArrays         hGenVertexArrays;
static PH_BindVertexArray         hBindVertexArray;
static PH_GenBuffers              hGenBuffers;
static PH_BindBuffer              hBindBuffer;
static PH_BufferData              hBufferData;
static PH_VertexAttribPointer     hVertexAttribPointer;
static PH_EnableVertexAttribArray hEnableVertexAttribArray;
static PH_GetUniformLocation      hGetUniformLocation;
static PH_Uniform1i               hUniform1i;
static PH_Uniform3f               hUniform3f;
static PH_UniformMatrix4fv        hUniformMatrix4fv;
static PH_ActiveTexture           hActiveTexture;
static PH_GenRenderbuffers        hGenRenderbuffers;
static PH_BindRenderbuffer        hBindRenderbuffer;
static PH_RenderbufferStorage     hRenderbufferStorage;
static PH_FramebufferRenderbuffer hFramebufferRenderbuffer;
static PH_BindFramebuffer         hBindFramebuffer;
static PH_BindSampler             hBindSampler;

static int LoadGl(void)
{
    #define L(dst, name) dst = (void*)SDL_GL_GetProcAddress(name); if (!dst) { VrOvr_Log("[VR-HANDS] missing GL %s\n", name); return 0; }
    L(hCreateShader, "glCreateShader");
    L(hShaderSource, "glShaderSource");
    L(hCompileShader, "glCompileShader");
    L(hGetShaderiv, "glGetShaderiv");
    L(hGetShaderInfoLog, "glGetShaderInfoLog");
    L(hCreateProgram, "glCreateProgram");
    L(hAttachShader, "glAttachShader");
    L(hLinkProgram, "glLinkProgram");
    L(hGetProgramiv, "glGetProgramiv");
    L(hUseProgram, "glUseProgram");
    L(hGenVertexArrays, "glGenVertexArrays");
    L(hBindVertexArray, "glBindVertexArray");
    L(hGenBuffers, "glGenBuffers");
    L(hBindBuffer, "glBindBuffer");
    L(hBufferData, "glBufferData");
    L(hVertexAttribPointer, "glVertexAttribPointer");
    L(hEnableVertexAttribArray, "glEnableVertexAttribArray");
    L(hGetUniformLocation, "glGetUniformLocation");
    L(hUniform1i, "glUniform1i");
    L(hUniform3f, "glUniform3f");
    L(hUniformMatrix4fv, "glUniformMatrix4fv");
    L(hActiveTexture, "glActiveTexture");
    L(hGenRenderbuffers, "glGenRenderbuffers");
    L(hBindRenderbuffer, "glBindRenderbuffer");
    L(hRenderbufferStorage, "glRenderbufferStorage");
    L(hFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    L(hBindFramebuffer, "glBindFramebuffer");
    L(hBindSampler, "glBindSampler");
    #undef L
    return 1;
}

/* ---- assets ------------------------------------------------------------- */
typedef struct
{
    float pos[4][3];
    float nrm[4][3];
    float u, v;
} UxrhVertex;   /* 104 bytes */

typedef struct
{
    UxrhVertex*     verts;
    unsigned short* idx;
    unsigned int    vc, ic;
} HandMesh;

static HandMesh s_mesh[2];
static int      s_state;   /* 0 not tried, 1 ready, -1 failed */
static GLuint   s_tex, s_prog, s_vao, s_vbo, s_ibo;
static GLint    s_uMvp = -1, s_uLight = -1;

/* this frame's vertices (pos, normal, uv) and indices, both hands */
static float*          s_vtx;
static unsigned short* s_ind;
static unsigned int    s_vtxN, s_indN;
static int             s_built;    /* the buffers hold this frame's hands */
static int             s_uploaded;
static unsigned int    s_handIndN, s_occIndN;   /* hands first, then the occluders */

/* what the hands hold, for their depth test (VrHands_OccluderSet) */
#define VR_OCC_SLOTS    2
#define VR_OCC_MAXV     1024      /* per slot, a triangle list */
#define VR_OCC_STALE_MS 150u
static float  s_occP[VR_OCC_SLOTS][VR_OCC_MAXV * 3];   /* tracking space, m */
static int    s_occN[VR_OCC_SLOTS];
static Uint32 s_occT[VR_OCC_SLOTS];

/* per eye depth renderbuffer, attached to vr_full's eye FBO */
static GLuint   s_depthRb[2], s_depthFbo[2];
static unsigned s_depthW[2], s_depthH[2];

static int LoadUxrh(const char* name, HandMesh* m)
{
    char  path[512];
    char  magic[4];
    unsigned int version = 0;
    FILE* f;
    int   ok;

    snprintf(path, sizeof(path), "vrhands/%s", name);
    f = fopen(path, "rb");
    if (!f)
    {
        VrOvr_Log("[VR-HANDS] cannot open %s\n", path);
        return 0;
    }
    ok = fread(magic, 1, 4, f) == 4 && fread(&version, 4, 1, f) == 1 && fread(&m->vc, 4, 1, f) == 1 &&
         fread(&m->ic, 4, 1, f) == 1 && memcmp(magic, "UXRH", 4) == 0 && m->vc > 0 && m->vc < 65536 &&
         m->ic > 0 && m->ic < 1000000;
    if (ok)
    {
        m->verts = (UxrhVertex*)malloc(sizeof(UxrhVertex) * m->vc);
        m->idx   = (unsigned short*)malloc(sizeof(unsigned short) * m->ic);
        ok = m->verts && m->idx && fread(m->verts, sizeof(UxrhVertex), m->vc, f) == m->vc &&
             fread(m->idx, sizeof(unsigned short), m->ic, f) == m->ic;
    }
    fclose(f);
    VrOvr_Log("[VR-HANDS] %s: %u verts, %u indices, %s\n", name, m->vc, m->ic, ok ? "ok" : "BAD");
    return ok;
}

static GLuint Compile(GLenum type, const char* src)
{
    GLint  ok = 0;
    GLuint s  = hCreateShader(type);
    hShaderSource(s, 1, &src, NULL);
    hCompileShader(s);
    hGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = { 0 };
        hGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        VrOvr_Log("[VR-HANDS] shader: %s\n", log);
        return 0;
    }
    return s;
}

static const char* kVs =
    VR_GLSL_VERSION
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec3 aNrm;\n"
    "layout(location=2) in vec2 aUv;\n"
    "uniform mat4 uMvp;\n"
    "out vec3 vNrm; out vec2 vUv;\n"
    "void main(){ gl_Position = uMvp * vec4(aPos, 1.0); vNrm = aNrm; vUv = aUv; }\n";
/* Silent Hill is dark: the hands are lit a little lower than in GTA SA */
static const char* kFs =
    VR_GLSL_VERSION
    "uniform sampler2D uTex; uniform vec3 uLight;\n"
    "in vec3 vNrm; in vec2 vUv; out vec4 oCol;\n"
    "void main(){\n"
    "  float l = 0.35 + 0.45 * max(dot(normalize(vNrm), uLight), 0.0);\n"
    "  oCol = vec4(texture(uTex, vUv).rgb * l, 1.0);\n"
    "}\n";

static int Init(void)
{
    GLint  pVao = 0, pArr = 0, pTex = 0, pProg = 0, ok = 0;
    GLuint vs, fs;
    unsigned char* pixels;
    FILE*  f;
    int    read;

    if (s_state != 0) return s_state == 1;
    s_state = -1;

    if (!LoadGl()) return 0;
    if (!LoadUxrh("BigHandLeft.uxrh", &s_mesh[0]) || !LoadUxrh("BigHandRight.uxrh", &s_mesh[1]))
        return 0;

    pixels = (unsigned char*)malloc(1024 * 1024 * 4);
    f      = fopen("vrhands/BigHandsAlbedo.rgba", "rb");
    read   = pixels && f && fread(pixels, 1, 1024 * 1024 * 4, f) == 1024 * 1024 * 4;
    if (f) fclose(f);
    if (!read)
    {
        VrOvr_Log("[VR-HANDS] cannot read vrhands/BigHandsAlbedo.rgba\n");
        free(pixels);
        return 0;
    }

    glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &pVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &pArr);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex);
    glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &pProg);

    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(pixels);

    vs = Compile(GL_VERTEX_SHADER, kVs);
    fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (vs && fs)
    {
        s_prog = hCreateProgram();
        hAttachShader(s_prog, vs);
        hAttachShader(s_prog, fs);
        hLinkProgram(s_prog);
        hGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    }
    if (ok)
    {
        hUseProgram(s_prog);
        hUniform1i(hGetUniformLocation(s_prog, "uTex"), 0);
        s_uMvp   = hGetUniformLocation(s_prog, "uMvp");
        s_uLight = hGetUniformLocation(s_prog, "uLight");

        hGenVertexArrays(1, &s_vao);
        hGenBuffers(1, &s_vbo);
        hGenBuffers(1, &s_ibo);
        hBindVertexArray(s_vao);
        hBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        hBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
        hEnableVertexAttribArray(0);
        hEnableVertexAttribArray(1);
        hEnableVertexAttribArray(2);
        hVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
        hVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12);
        hVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
    }

    hBindVertexArray((GLuint)pVao);
    hBindBuffer(GL_ARRAY_BUFFER, (GLuint)pArr);
    glBindTexture(GL_TEXTURE_2D, (GLuint)pTex);
    hUseProgram((GLuint)pProg);
    if (!ok)
    {
        VrOvr_Log("[VR-HANDS] program failed - hands off\n");
        return 0;
    }

    s_vtx = (float*)malloc(sizeof(float) * 8 * (s_mesh[0].vc + s_mesh[1].vc + VR_OCC_SLOTS * VR_OCC_MAXV));
    s_ind = (unsigned short*)malloc(sizeof(unsigned short) * (s_mesh[0].ic + s_mesh[1].ic + VR_OCC_SLOTS * VR_OCC_MAXV));
    if (!s_vtx || !s_ind) return 0;

    s_state = 1;
    VrOvr_Log("[VR-HANDS] ready\n");
    return 1;
}

/* ---- math --------------------------------------------------------------- */
static void V3Norm(float v[3])
{
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

/* aim rotation relative to the grip (Rg^T Ra), learned from the right hand */
static float s_aimRel[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
static int   s_aimRelKnown;

static void LearnAimRel(void)
{
    float gp[3], gr[3][3], ap[3], ar[3][3];
    int   i, j;
    if (s_aimRelKnown || !VrOvr_GetHandPose(1, gp, gr) || !VrOvr_GetAimPose(ap, ar))
        return;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            s_aimRel[i][j] = gr[0][i] * ar[0][j] + gr[1][i] * ar[1][j] + gr[2][i] * ar[2][j];
    s_aimRelKnown = 1;
    VrOvr_Log("[VR-HANDS] aim vs grip: fwd in grip frame %.2f %.2f %.2f\n",
              (double)-s_aimRel[0][2], (double)-s_aimRel[1][2], (double)-s_aimRel[2][2]);
}

/* Builds both hands in the HEAD frame of `head` (tracking -> head: R^T (p - t)),
 * once per frame, before the first eye. */
static void Build(const HmdMatrix34_t* head)
{
    int hand;

    s_vtxN = s_indN = 0;
    LearnAimRel();

    for (hand = 0; hand < 2; hand++)
    {
        const HandMesh* m = &s_mesh[hand];
        float gp[3], gr[3][3], o[3], R[3][3], fwd[3], up[3], right[3], gx[3], gfwd[3], az[3];
        float trig = 0.0f, grip = 0.0f, w[4], d;
        const float sign = hand == 1 ? 1.0f : -1.0f;
        const unsigned short base = (unsigned short)(s_vtxN / 8);
        unsigned int k;
        int i, j;

        if (!VrOvr_GetHandPose(hand, gp, gr))
            continue;

        /* the grip rotation in the head frame: R = Rh^T Rg, origin = Rh^T (p - t) */
        for (i = 0; i < 3; i++)
        {
            o[i] = head->m[0][i] * (gp[0] - head->m[0][3]) + head->m[1][i] * (gp[1] - head->m[1][3]) +
                   head->m[2][i] * (gp[2] - head->m[2][3]);
            for (j = 0; j < 3; j++)
                R[i][j] = head->m[0][i] * gr[0][j] + head->m[1][i] * gr[1][j] + head->m[2][i] * gr[2][j];
        }
        for (i = 0; i < 3; i++)
        {
            gx[i]   = R[i][0];
            gfwd[i] = -R[i][2];
            /* aim -Z = grip * aimRel, column 2 */
            az[i]  = R[i][0] * s_aimRel[0][2] + R[i][1] * s_aimRel[1][2] + R[i][2] * s_aimRel[2][2];
            fwd[i] = -az[i];
        }
        V3Norm(fwd);
        for (i = 0; i < 3; i++) up[i] = gx[i] * sign;
        d = up[0] * fwd[0] + up[1] * fwd[1] + up[2] * fwd[2];
        for (i = 0; i < 3; i++) up[i] -= fwd[i] * d;
        V3Norm(up);
        right[0] = up[1] * fwd[2] - up[2] * fwd[1];
        right[1] = up[2] * fwd[0] - up[0] * fwd[2];
        right[2] = up[0] * fwd[1] - up[1] * fwd[0];
        V3Norm(right);
        if (right[0] * gfwd[0] + right[1] * gfwd[1] + right[2] * gfwd[2] < 0.0f)
            for (i = 0; i < 3; i++) right[i] = -right[i];

        /* fingers: grip and trigger; holding something closes the hand */
        VrInteract_RawHand(hand, &trig, &grip);
        if (VrInteract_HandHolds(hand)) grip = 1.0f;
        if (grip < 0.0f) grip = 0.0f; else if (grip > 1.0f) grip = 1.0f;
        if (trig < 0.0f) trig = 0.0f; else if (trig > 1.0f) trig = 1.0f;
        w[0] = (1.0f - grip) * (1.0f - trig);
        w[1] = grip * (1.0f - trig);
        w[2] = (1.0f - grip) * trig;
        w[3] = grip * trig;

        for (k = 0; k < m->vc; k++)
        {
            const UxrhVertex* v = &m->verts[k];
            float l[3], n[3];
            float* out = s_vtx + s_vtxN;
            for (i = 0; i < 3; i++)
            {
                l[i] = v->pos[0][i] * w[0] + v->pos[1][i] * w[1] + v->pos[2][i] * w[2] + v->pos[3][i] * w[3];
                n[i] = v->nrm[0][i] * w[0] + v->nrm[1][i] * w[1] + v->nrm[2][i] * w[2] + v->nrm[3][i] * w[3];
            }
            /* UXR local axes: X forward (wrist 5.5 cm back), Z right, -Y up */
            for (i = 0; i < 3; i++)
            {
                out[i]     = o[i] + fwd[i] * (l[0] - 0.055f) + right[i] * l[2] - up[i] * l[1];
                out[3 + i] = fwd[i] * n[0] + right[i] * n[2] - up[i] * n[1];
            }
            out[6] = v->u;
            out[7] = v->v;
            s_vtxN += 8;
        }
        for (k = 0; k < m->ic; k++)
            s_ind[s_indN++] = (unsigned short)(base + m->idx[k]);
    }
    s_handIndN = s_indN;
    s_occIndN  = 0;

    /* the held weapon's / map's shapes after the hands (depth only, see
     * VrHands_DrawEye), in the same head frame */
    if (s_handIndN > 0)
    {
        const Uint32 now = SDL_GetTicks();
        int          slot;
        for (slot = 0; slot < VR_OCC_SLOTS; slot++)
        {
            int v;
            if (s_occN[slot] <= 0 || now - s_occT[slot] > VR_OCC_STALE_MS)
                continue;
            for (v = 0; v < s_occN[slot]; v++)
            {
                const float* gp  = &s_occP[slot][v * 3];
                float*       out = s_vtx + s_vtxN;
                int          i;
                for (i = 0; i < 3; i++)
                {
                    out[i] = head->m[0][i] * (gp[0] - head->m[0][3]) + head->m[1][i] * (gp[1] - head->m[1][3]) +
                             head->m[2][i] * (gp[2] - head->m[2][3]);
                    out[3 + i] = 0.0f;
                }
                out[6] = out[7] = 0.0f;
                s_ind[s_indN++] = (unsigned short)(s_vtxN / 8);
                s_vtxN += 8;
            }
        }
        s_occIndN = s_indN - s_handIndN;
    }
    s_uploaded = 0;
}

/* OCCLUDERS. The hands are drawn over the composed eye image, which has no
 * depth, so a hand holding a weapon covered all of it. The game hands over
 * the shapes of what is in the hands - slot 0 the held weapon's close-up
 * model (vr_weapon_hd.c), slot 1 the held map's sheet (pc_minimap.c) - as
 * triangles in world Q8, each game frame it draws them. They go into the
 * hands' depth buffer only (no colour), so a finger behind the grip is
 * hidden and the weapon's image shows there, a finger in front of it is
 * drawn. A slot not refreshed for VR_OCC_STALE_MS (put away) stops counting. */
void VrHands_OccluderSet(int slot, const float* worldPts, int nVerts)
{
    extern int VrIk_WorldToTracking(const float* w, float* p, int n);
    if (slot < 0 || slot >= VR_OCC_SLOTS)
        return;
    if (nVerts > VR_OCC_MAXV) nVerts = VR_OCC_MAXV;
    nVerts -= nVerts % 3;
    if (worldPts == NULL || nVerts <= 0 || !VrIk_WorldToTracking(worldPts, s_occP[slot], nVerts))
    {
        s_occN[slot] = 0;
        return;
    }
    s_occN[slot] = nVerts;
    s_occT[slot] = SDL_GetTicks();
}

/* Depth renderbuffer of eye e's FBO, (re)made at its size. */
static void EnsureDepth(int e, GLuint fbo, unsigned w, unsigned h)
{
    if (s_depthRb[e] != 0 && s_depthFbo[e] == fbo && s_depthW[e] == w && s_depthH[e] == h)
        return;
    if (s_depthRb[e] == 0) hGenRenderbuffers(1, &s_depthRb[e]);
    hBindRenderbuffer(GL_RENDERBUFFER, s_depthRb[e]);
    hRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
    hBindRenderbuffer(GL_RENDERBUFFER, 0);
    hFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_depthRb[e]);
    s_depthFbo[e] = fbo;
    s_depthW[e]   = w;
    s_depthH[e]   = h;
}

/* vr_full.c, once a frame before the eyes: is there anything to draw? */
int VrHands_BeginFrame(const HmdMatrix34_t* head, int poseValid)
{
    s_built = 0;
    if (!poseValid || !VrIk_HandsActive() || !Init())
        return 0;
    Build(head);
    s_built = s_indN > 0;
    return s_built;
}

/* vr_full.c, per eye with that eye's FBO bound (w x h): the hands over the
 * composed image. ext = the eye's tangents left, right, bottom, top (up +).
 * Leaves blend/depth/cull disabled, as the compose loop runs. */
void VrHands_DrawEye(int eye, GLuint fbo, unsigned w, unsigned h, const float ext[4])
{
    GLint  pVao = 0, pArr = 0, pProg = 0, pTex = 0, pDepthFunc = GL_LESS;
    GLboolean pDepthMask = GL_TRUE;
    GLfloat pClearDepth = 1.0f;
    float  mvp[16];
    const float ex = (eye == 0) ? -VrTracking_GetHalfIpdMeters() : VrTracking_GetHalfIpdMeters();
    const float zn = 0.02f, zf = 10.0f;
    const float l = ext[0], r = ext[1], b = ext[2], t = ext[3];

    if (!s_built || s_state != 1)
        return;

    EnsureDepth(eye, fbo, w, h);

    /* off-axis projection from the tangents (view: x right, y up, -z ahead),
     * times the eye's offset from the head centre; column-major */
    memset(mvp, 0, sizeof(mvp));
    mvp[0]  = 2.0f / (r - l);
    mvp[8]  = (r + l) / (r - l);
    mvp[5]  = 2.0f / (t - b);
    mvp[9]  = (t + b) / (t - b);
    mvp[10] = -(zf + zn) / (zf - zn);
    mvp[11] = -1.0f;
    mvp[14] = -2.0f * zf * zn / (zf - zn);
    /* translate by -ex along x: column 3 += column 0 * -ex (and col 2 terms none) */
    mvp[12] = mvp[0] * -ex;
    mvp[13] = 0.0f;
    mvp[15] = 0.0f;

    glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &pVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &pArr);
    glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &pProg);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex);

    hBindVertexArray(s_vao);
    if (!s_uploaded)
    {
        hBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        hBufferData(GL_ARRAY_BUFFER, (ptrdiff_t)(s_vtxN * sizeof(float)), s_vtx, GL_STREAM_DRAW);
        hBufferData(GL_ELEMENT_ARRAY_BUFFER, (ptrdiff_t)(s_indN * sizeof(unsigned short)), s_ind, GL_STREAM_DRAW);
        s_uploaded = 1;
    }

    glGetIntegerv(GL_DEPTH_FUNC, &pDepthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &pDepthMask);
    glGetFloatv(0x0B73 /* GL_DEPTH_CLEAR_VALUE */, &pClearDepth);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    hUseProgram(s_prog);
    hUniformMatrix4fv(s_uMvp, 1, GL_FALSE, mvp);
    /* light from above and a little ahead, in the head frame */
    hUniform3f(s_uLight, 0.25f, 0.85f, 0.46f);
    hActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    hBindSampler(0, 0);
    if (s_occIndN > 0)
    {
        /* what the hands hold: depth only, so the hand passes behind it */
        GLboolean cm[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDrawElements(GL_TRIANGLES, (GLsizei)s_occIndN, GL_UNSIGNED_SHORT,
                       (void*)(size_t)(s_handIndN * sizeof(unsigned short)));
        glColorMask(cm[0], cm[1], cm[2], cm[3]);
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)s_handIndN, GL_UNSIGNED_SHORT, (void*)0);

    glDisable(GL_DEPTH_TEST);
    glDepthFunc((GLenum)pDepthFunc);
    glDepthMask(pDepthMask);
    glClearDepth((double)pClearDepth);
    hBindVertexArray((GLuint)pVao);
    hBindBuffer(GL_ARRAY_BUFFER, (GLuint)pArr);
    hUseProgram((GLuint)pProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)pTex);
}
