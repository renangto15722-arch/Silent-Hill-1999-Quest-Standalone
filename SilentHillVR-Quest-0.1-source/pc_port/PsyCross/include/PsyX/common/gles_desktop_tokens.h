/* SH_QUEST: desktop-GL names on Android's ES 3.0 headers.
 *
 * The renderer picks desktop vs GLES behaviour at runtime (g_grIsGLES), so the
 * desktop half of each branch must still compile against <GLES3/gl3.h>.
 * Tokens are the GL registry values, only there so the branches parse; every
 * use sits behind a check that is false on a GLES context. The few functions
 * are mapped onto their ES 3.0 equivalent where one exists. */
#ifndef PSYX_GLES_DESKTOP_TOKENS_H
#define PSYX_GLES_DESKTOP_TOKENS_H

#if defined(__ANDROID__)
#include <GLES3/gl3.h>

#ifndef GL_TEXTURE_WIDTH
#   define GL_TEXTURE_WIDTH             0x1000
#endif
#ifndef GL_TEXTURE_HEIGHT
#   define GL_TEXTURE_HEIGHT            0x1001
#endif
#ifndef GL_TEXTURE_INTERNAL_FORMAT
#   define GL_TEXTURE_INTERNAL_FORMAT   0x1003
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#   define GL_TEXTURE_BORDER_COLOR      0x1004
#endif
#ifndef GL_CLAMP_TO_BORDER
#   define GL_CLAMP_TO_BORDER           0x812D
#endif
#ifndef GL_MULTISAMPLE
#   define GL_MULTISAMPLE               0x809D
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#   define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#   define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_BGRA
#   define GL_BGRA                      0x80E1
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#   define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#   define GL_UNSIGNED_INT_8_8_8_8_REV  0x8367
#endif
#ifndef GL_FILL
#   define GL_FILL                      0x1B02
#endif
#ifndef GL_LINE
#   define GL_LINE                      0x1B01
#endif
#ifndef GL_FRONT_AND_BACK
#   define GL_FRONT_AND_BACK            0x0408
#endif

/* glDrawBuffer(single) == glDrawBuffers(1, &buf) on ES 3.0 (GL_BACK for the
 * default framebuffer, GL_COLOR_ATTACHMENTi for an FBO). */
static inline void PsyX_GlesDrawBuffer(GLenum buf)
{
    glDrawBuffers(1, &buf);
}
#define glDrawBuffer PsyX_GlesDrawBuffer

/* No polygon modes on ES (wireframe debug only). */
static inline void PsyX_GlesPolygonMode(GLenum face, GLenum mode)
{
    (void)face; (void)mode;
}
#define glPolygonMode PsyX_GlesPolygonMode

/* glClearDepth(double) is desktop-only; ES has the float spelling. */
#define glClearDepth(d) glClearDepthf((GLfloat)(d))


#ifndef APIENTRY
#   define APIENTRY
#endif

/* Shadowed GL state (PsyX_render.cpp, "SHADOWED GL STATE"): on desktop the
 * glad pointers are swapped; on Android the five state setters are redirected
 * here by name, in every TU that sees this header, so the copy stays exact.
 * PsyX_glreal_android.c defines PSYX_SH_NO_REDIRECT and makes the real calls. */
#ifdef __cplusplus
extern "C" {
#endif
void PsyX_ShViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void PsyX_ShScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void PsyX_ShEnable(GLenum cap);
void PsyX_ShDisable(GLenum cap);
void PsyX_ShBindFramebuffer(GLenum target, GLuint fb);
void PsyX_GlReal_Viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void PsyX_GlReal_Scissor(GLint x, GLint y, GLsizei w, GLsizei h);
void PsyX_GlReal_Enable(GLenum cap);
void PsyX_GlReal_Disable(GLenum cap);
void PsyX_GlReal_BindFramebuffer(GLenum target, GLuint fb);
#ifdef __cplusplus
}
#endif
#ifndef PSYX_SH_NO_REDIRECT
#   define glViewport        PsyX_ShViewport
#   define glScissor         PsyX_ShScissor
#   define glEnable          PsyX_ShEnable
#   define glDisable         PsyX_ShDisable
#   define glBindFramebuffer PsyX_ShBindFramebuffer
#endif

#endif /* __ANDROID__ */
#endif
