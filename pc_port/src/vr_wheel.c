/*
 * vr_wheel.c - the VR radial wheel (vr_interact.c holds its state).
 *
 * Drawn into the Full VR 2D interface layer (PsyX_UiLayerBeginOverlay), so
 * it floats head-locked in the centre of the view like the other 2D panels,
 * and only exists in Full VR. That layer is composited with a screen blend:
 * dark is see-through and brightness is what shows, so Wheel_Opacity scales
 * how bright the wheel is drawn.
 *
 * Same GL rules as pc_confirm_dialog.c: its own program / VAO / textures,
 * LEGACY GLSL, and every piece of GL state it touches restored. Text is
 * stb_truetype (the toast owns the implementation).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "stb_truetype.h"

#include "lang_quick.h"
#include "pc_config.h"
#include "sh_log.h"
#include "game.h"
#include "bodyprog/items.h"
#include "vr_interact.h"

#define VW_VBO_FLOATS 4096
#define VW_GARBAGE    (VR_WHEEL_MAX * 2 + 4)

static GLuint s_prog, s_vao, s_vbo, s_texWhite;
static GLint  s_locColor;
static int    s_glReady;
static GLuint s_garbage[VW_GARBAGE];
static int    s_garbageCount;

static stbtt_fontinfo s_font;
static int            s_fontOk, s_fontsTried, s_fontsScript = -1;

/* Baked labels for the wheel as it is now (re-baked when it changes). */
static GLuint   s_texSlot[VR_WHEEL_MAX], s_texCentre[VR_WHEEL_MAX];
static int      s_slotW[VR_WHEEL_MAX], s_slotH[VR_WHEEL_MAX];
static int      s_centreW[VR_WHEEL_MAX], s_centreH[VR_WHEEL_MAX];
static unsigned s_bakedKey;

static const char* ItemName(unsigned char id)
{
    switch (id)
    {
        case InvItemId_HealthDrink:  return "Health Drink";
        case InvItemId_FirstAidKit:  return "First Aid Kit";
        case InvItemId_Ampoule:      return "Ampoule";
        case VR_WHEEL_ID_MAP:        return "Map";
    }
    return "?";
}

static int IsHealItem(unsigned char id)
{
    return id == InvItemId_HealthDrink || id == InvItemId_FirstAidKit || id == InvItemId_Ampoule;
}

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint vw_make_shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };
    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[VR-WHEEL] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void vw_gl_init(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";

    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0, prevProg = 0;

    s_glReady = -1;
    vs = vw_make_shader(GL_VERTEX_SHADER, vs_src);
    fs = vw_make_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    /* Restore the bound program: PsyCross caches it (see pc_confirm_dialog.c). */
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
    s_locColor = glGetUniformLocation(s_prog, "u_color");
    glUseProgram((GLuint)prevProg);

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, VW_VBO_FLOATS * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    s_glReady = 1;
}

static GLuint vw_upload_rgba(const unsigned char* rgba, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

static void vw_retire(GLuint tex)
{
    if (tex && s_garbageCount < VW_GARBAGE)
        s_garbage[s_garbageCount++] = tex;
}

static void vw_gl_pump(void)
{
    int i;
    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;
}

/* Triangles in NDC, 4 floats per vertex (x, y, u, v). */
static void vw_tris(GLuint tex, const float* v, int verts, float r, float g, float b, float a)
{
    if (!tex || verts <= 0 || verts * 4 > VW_VBO_FLOATS)
        return;
    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(verts * 4 * sizeof(float)), v);
    glDrawArrays(GL_TRIANGLES, 0, verts);
}

static void vw_quad(GLuint tex, float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    const float v[6][4] = {
        { x0, yTop, 0, 0 }, { x0, yBot, 0, 1 }, { x1, yTop, 1, 0 },
        { x1, yTop, 1, 0 }, { x0, yBot, 0, 1 }, { x1, yBot, 1, 1 },
    };
    vw_tris(tex, &v[0][0], 6, r, g, b, a);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static void vw_fonts_init(void)
{
    static const char* paths[16];
    int count = Pc_LangQuickFontPaths(paths, 16);
    int i;

    s_fontsTried  = 1;
    s_fontsScript = Pc_LangQuickScript();
    s_fontOk      = 0;
    s_bakedKey    = 0;   /* anything baked came from the old face */
    for (i = 0; i < count; i++)
    {
        FILE*          f = fopen(paths[i], "rb");
        long           n;
        unsigned char* data;
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = (n > 0) ? (unsigned char*)malloc((size_t)n) : NULL;
        if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
        fclose(f);
        if (!data)
            continue;
        if (stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            s_fontOk = 1;   /* stbtt keeps pointers into data - never free */
            return;
        }
        free(data);
    }
    SH_DBG("[VR-WHEEL] no usable font - labels disabled");
}

/* One line of text to a white-coverage RGBA texture, trimmed to the ink. */
static GLuint vw_bake(const char* text, float px, int* outW, int* outH)
{
    const char* p;
    float scale, penX, baseY;
    int   asc, desc, gap, W, H, i, pad = 1, prev = 0;
    unsigned char *cov, *rgba, *scratch;
    GLuint tex;

    if (!s_fontOk || !text || !text[0] || px < 4.0f)
        return 0;

    scale = stbtt_ScaleForPixelHeight(&s_font, px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);

    W = (int)(px * 0.62f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf(px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 4096 || H > 512)
        return 0;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch) { free(cov); free(rgba); free(scratch); return 0; }

    penX  = (float)pad;
    baseY = (float)pad + asc * scale;
    p     = text;
    while (*p)
    {
        int cp = (int)Pc_LangUtf8Next(&p);
        int gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;
        if (prev)
            penX += stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font, scratch, gw, gh, gw, scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H) continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int dx = (int)penX + gx0 + sx;
                    unsigned char v;
                    if (dx < 0 || dx >= W) continue;
                    v = scratch[sy * gw + sx];
                    if (v > cov[dy * W + dx]) cov[dy * W + dx] = v;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        penX += adv * scale;
        prev = cp;
        if (penX > (float)(W - pad)) break;
    }

    {
        int stride = W;
        int inkW   = (int)ceilf(penX) + pad;
        if (inkW > 0 && inkW < W) W = inkW;
        for (i = 0; i < W * H; i++)
        {
            rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = cov[(i / W) * stride + (i % W)];
        }
    }
    tex = vw_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

static void vw_free_text(void)
{
    int i;
    for (i = 0; i < VR_WHEEL_MAX; i++)
    {
        vw_retire(s_texSlot[i]);   s_texSlot[i]   = 0;
        vw_retire(s_texCentre[i]); s_texCentre[i] = 0;
    }
    s_bakedKey = 0;
}

static unsigned vw_key(const VrWheelView* w, int px)
{
    unsigned h = 2166136261u ^ (unsigned)px;
    int      i;
    for (i = 0; i < w->n; i++)
    {
        h = (h ^ w->ids[i]) * 16777619u;
        h = (h ^ (unsigned)w->counts[i]) * 16777619u;
    }
    return (h ^ (unsigned)w->n) | 1u;
}

/* Slot label: short, on the ring. Centre label: the full line for the
 * selected entry. */
static void vw_bake_all(const VrWheelView* w, int px)
{
    int i;
    vw_free_text();
    for (i = 0; i < w->n; i++)
    {
        char        line[48];
        const char* name = ItemName(w->ids[i]);

        if (IsHealItem(w->ids[i]))
            snprintf(line, sizeof(line), "%s x%d", name, w->counts[i]);
        else
            snprintf(line, sizeof(line), "%s", name);
        s_texSlot[i] = vw_bake(line, (float)px, &s_slotW[i], &s_slotH[i]);

        if (IsHealItem(w->ids[i]))
            snprintf(line, sizeof(line), "Use %s", name);
        else
            snprintf(line, sizeof(line), "Open the %s", name);
        s_texCentre[i] = vw_bake(line, (float)px * 1.25f, &s_centreW[i], &s_centreH[i]);
    }
    s_bakedKey = vw_key(w, px);
}

static int vw_text_lost(const VrWheelView* w)
{
    int i;
    for (i = 0; i < w->n; i++)
        if ((s_texSlot[i] && !glIsTexture(s_texSlot[i])) ||
            (s_texCentre[i] && !glIsTexture(s_texCentre[i])))
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static void vw_draw(const VrWheelView* w, float vpW, float vpH)
{
    const float o   = g_PcConfig.vrWheelOpacity / 100.0f;
    const float cx  = vpW * 0.5f, cy = vpH * 0.5f;
    float       R   = 300.0f * (vpH / 960.0f) * g_PcConfig.vrWheelSize / 100.0f;
    float       r0, seg;
    int         i, k;

    if (R > vpH * 0.48f) R = vpH * 0.48f;
    r0 = R * 0.45f;

    #define NX(x) ((x) / vpW * 2.0f - 1.0f)
    #define NY(y) ((y) / vpH * 2.0f - 1.0f)

    if (w->n <= 0)
        return;
    seg = 6.28318531f / (float)w->n;

    /* the ring: one wedge per entry, slot 0 at the top, clockwise */
    for (i = 0; i < w->n; i++)
    {
        static float v[VW_VBO_FLOATS];
        const int    steps = (w->n < 4) ? 24 : 96 / w->n;
        const float  gap   = 0.025f;
        const float  a0    = (float)i * seg - seg * 0.5f + gap;
        const float  a1    = (float)i * seg + seg * 0.5f - gap;
        int          nv    = 0;
        float        cr, cg, cb;

        for (k = 0; k < steps; k++)
        {
            const float aa = a0 + (a1 - a0) * (float)k / (float)steps;
            const float ab = a0 + (a1 - a0) * (float)(k + 1) / (float)steps;
            const float p[4][2] = {
                { cx + r0 * sinf(aa), cy + r0 * cosf(aa) }, { cx + R * sinf(aa), cy + R * cosf(aa) },
                { cx + r0 * sinf(ab), cy + r0 * cosf(ab) }, { cx + R * sinf(ab), cy + R * cosf(ab) },
            };
            static const int tri[6] = { 0, 1, 2, 2, 1, 3 };
            int t;
            for (t = 0; t < 6; t++)
            {
                v[nv * 4 + 0] = NX(p[tri[t]][0]);
                v[nv * 4 + 1] = NY(p[tri[t]][1]);
                v[nv * 4 + 2] = 0.0f;
                v[nv * 4 + 3] = 0.0f;
                nv++;
            }
        }

        if (i == w->sel) { cr = 0.70f; cg = 0.50f; cb = 0.18f; }
        else             { cr = 0.22f; cg = 0.22f; cb = 0.24f; }
        vw_tris(s_texWhite, v, nv, cr, cg, cb, o);
    }

    /* labels on the ring, centred in their wedge */
    for (i = 0; i < w->n; i++)
    {
        const float a  = (float)i * seg;
        const float rm = (r0 + R) * 0.5f;
        const float lx = cx + rm * sinf(a), ly = cy + rm * cosf(a);
        float       sw = (float)s_slotW[i], sh = (float)s_slotH[i];
        const float maxW = 2.0f * rm * sinf(seg * 0.5f) * 0.95f;   /* the wedge's chord */

        if (!s_texSlot[i]) continue;
        if (w->n > 1 && sw > maxW && maxW > 8.0f) { sh *= maxW / sw; sw = maxW; }
        vw_quad(s_texSlot[i], NX(lx - sw * 0.5f), NY(ly + sh * 0.5f),
                NX(lx + sw * 0.5f), NY(ly - sh * 0.5f), 1.0f, 1.0f, 1.0f, o);
    }

    /* centre: what releasing A will do */
    if (w->sel >= 0 && w->sel < w->n && s_texCentre[w->sel])
    {
        float sw = (float)s_centreW[w->sel], sh = (float)s_centreH[w->sel];
        const float maxW = r0 * 1.8f;
        if (sw > maxW) { sh *= maxW / sw; sw = maxW; }
        vw_quad(s_texCentre[w->sel], NX(cx - sw * 0.5f), NY(cy + sh * 0.5f),
                NX(cx + sw * 0.5f), NY(cy - sh * 0.5f), 1.0f, 0.9f, 0.7f, o);
    }

    #undef NX
    #undef NY
}

/* dbg_overlay.c, every frame: the wheel into the Full VR interface layer. */
void VrWheel_DrawOverlay(void)
{
    extern int  PsyX_UiLayerBeginOverlay(void);
    extern void PsyX_UiLayerEndOverlay(void);
    VrWheelView w;
    GLint vp[4];
    GLint prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;
    int px;

    if (!VrInteract_GetWheel(&w))
        return;
    if (!PsyX_UiLayerBeginOverlay())
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) { PsyX_UiLayerEndOverlay(); return; }

    if (!s_glReady) vw_gl_init();
    if (s_glReady != 1) { PsyX_UiLayerEndOverlay(); return; }

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    vw_gl_pump();
    if (s_texWhite && !glIsTexture(s_texWhite))
        s_texWhite = 0;   /* freed by something else: make a new one */
    if (!s_texWhite)
    {
        const unsigned char one[4] = { 255, 255, 255, 255 };
        s_texWhite = vw_upload_rgba(one, 1, 1);
    }
    if (!s_fontsTried || s_fontsScript != Pc_LangQuickScript())
        vw_fonts_init();

    {
        float R = 300.0f * ((float)vp[3] / 960.0f) * g_PcConfig.vrWheelSize / 100.0f;
        if (R > (float)vp[3] * 0.48f) R = (float)vp[3] * 0.48f;
        px = (int)(R * 0.085f);
        if (px < 8) px = 8;
    }
    if (s_bakedKey != vw_key(&w, px) || vw_text_lost(&w))
        vw_bake_all(&w, px);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    vw_draw(&w, (float)vp[2], (float)vp[3]);

    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);

    PsyX_UiLayerEndOverlay();
}
