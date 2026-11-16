/* GL headers for the VR modules (vr_openvr.c, vr_openxr.c, vr_full.c,
 * vr_hands.c). Desktop: SDL_opengl.h (opengl32 1.1 + GetProcAddress for the
 * rest). Quest: the platform GLES 3.0 headers, plus the desktop tokens and the
 * shadowed-state redirect PsyCross uses (gles_desktop_tokens.h), so glViewport
 * & co. keep PsyCross's state copy exact here too. */
#ifndef VR_GL_H
#define VR_GL_H

#if defined(__ANDROID__)
#   include <GLES3/gl32.h>
#   include <GLES2/gl2ext.h>
#   include "PsyX/common/gles_desktop_tokens.h"
#   ifndef GL_FRAMEBUFFER_SRGB
#       define GL_FRAMEBUFFER_SRGB 0x8DB9   /* EXT_sRGB_write_control */
#   endif
    /* Shader preamble: GLSL ES 3.00 for the 330-core sources. */
#   define VR_GLSL_VERSION "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
#   include <SDL_opengl.h>
#   define VR_GLSL_VERSION "#version 330 core\n"
#endif

#endif
