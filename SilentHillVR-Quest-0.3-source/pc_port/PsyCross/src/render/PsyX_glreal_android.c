/* SH_QUEST: the real GLES state setters behind the shadowed-state redirect
 * (see gles_desktop_tokens.h). Built on every platform, empty off Android. */
#if defined(__ANDROID__)
#define PSYX_SH_NO_REDIRECT
#include "PsyX/common/gles_desktop_tokens.h"

void PsyX_GlReal_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { glViewport(x, y, w, h); }
void PsyX_GlReal_Scissor(GLint x, GLint y, GLsizei w, GLsizei h)  { glScissor(x, y, w, h); }
void PsyX_GlReal_Enable(GLenum cap)                               { glEnable(cap); }
void PsyX_GlReal_Disable(GLenum cap)                              { glDisable(cap); }
void PsyX_GlReal_BindFramebuffer(GLenum target, GLuint fb)        { glBindFramebuffer(target, fb); }
void PsyX_GlReal_DrawArrays(GLenum mode, GLint first, GLsizei count) { glDrawArrays(mode, first, count); }
void PsyX_GlReal_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei n) { glDrawArraysInstanced(mode, first, count, n); }
#endif
