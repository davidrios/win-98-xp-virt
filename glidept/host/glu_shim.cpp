//**************************************************************
//* The two GLU calls OpenGLide makes, without GLU (2ksbox).
//*
//* Upstream links libGLU for gluErrorString (one log line) and
//* gluBuild2DMipmaps (behind the BuildMipMaps option). GLU is a
//* deprecated library that is not in every runtime we ship into — the
//* Flatpak's org.freedesktop.Sdk among them — and both uses have a
//* one-line replacement in core GL, so patches/openglide/01 renames the
//* call sites to these.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "GlOgl.h"

#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191 /* GL 1.4 */
#endif

const char *ogl_error_string(GLenum error)
{
    switch (error) {
    case GL_NO_ERROR:          return "no error";
    case GL_INVALID_ENUM:      return "invalid enum";
    case GL_INVALID_VALUE:     return "invalid value";
    case GL_INVALID_OPERATION: return "invalid operation";
    case GL_STACK_OVERFLOW:    return "stack overflow";
    case GL_STACK_UNDERFLOW:   return "stack underflow";
    case GL_OUT_OF_MEMORY:     return "out of memory";
    default:                   return "unknown error";
    }
}

/*
 * gluBuild2DMipmaps scaled the image to a power of two and built every
 * level on the CPU. Glide textures are already power-of-two and at most
 * 256x256, so GL_GENERATE_MIPMAP over the same upload is the whole of it —
 * and it is the driver's own downsample rather than GLU's box filter.
 */
GLint ogl_build_2d_mipmaps(GLenum target, GLint components,
                           GLsizei width, GLsizei height,
                           GLenum format, GLenum type, const void *data)
{
    glTexParameteri(target, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(target, 0, components, width, height, 0, format, type, data);
    glTexParameteri(target, GL_GENERATE_MIPMAP, GL_FALSE);
    return 0;
}
