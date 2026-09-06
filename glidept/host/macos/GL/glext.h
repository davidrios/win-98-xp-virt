//**************************************************************
//* <GL/glext.h> for OpenGL.framework (2ksbox, doc 12 §5).
//*
//* Apple's <OpenGL/glext.h> stopped at GL_GLEXT_VERSION 8 (2003) and was
//* written before the PFNGL…PROC convention: it declares the extension
//* entry points as functions, not as pointer typedefs, and defines no
//* APIENTRY. OpenGLide's Glextensions.h wants the typedefs, because it
//* resolves every extension through the platform's own GetProcAddress.
//* The seventeen it names are all we add, plus the four EXT_paletted_texture
//* / EXT_packed_pixels enums PGTexture.cpp uses — those two extensions do
//* not exist on macOS at all, and OpenGLide already asks for them at
//* runtime before it uses either, so this only lets the file compile.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifndef GLIDEPT_MACOS_GLEXT_H
#define GLIDEPT_MACOS_GLEXT_H

#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#ifndef GLAPI
#define GLAPI extern
#endif

/* GL_EXT_paletted_texture / GL_EXT_packed_pixels: absent from the
 * framework's headers and from the driver. PGTexture.cpp names them
 * behind InternalConfig.PaletteEXTEnable, which the extension string
 * never turns on here. */
#ifndef GL_COLOR_INDEX8_EXT
#define GL_COLOR_INDEX8_EXT             0x80E5
#endif
#ifndef GL_UNSIGNED_BYTE_3_3_2_EXT
#define GL_UNSIGNED_BYTE_3_3_2_EXT      0x8032
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_EXT
#define GL_UNSIGNED_SHORT_4_4_4_4_EXT   0x8033
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1_EXT
#define GL_UNSIGNED_SHORT_5_5_5_1_EXT   0x8034
#endif

typedef void (*PFNGLACTIVETEXTUREARBPROC)(GLenum texture);
typedef void (*PFNGLCLIENTACTIVETEXTUREARBPROC)(GLenum texture);
typedef void (*PFNGLMULTITEXCOORD4FARBPROC)(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
typedef void (*PFNGLMULTITEXCOORD4FVARBPROC)(GLenum target, const GLfloat *v);
typedef void (*PFNGLSECONDARYCOLOR3FEXTPROC)(GLfloat red, GLfloat green, GLfloat blue);
typedef void (*PFNGLSECONDARYCOLOR3FVEXTPROC)(const GLfloat *v);
typedef void (*PFNGLSECONDARYCOLOR3UBEXTPROC)(GLubyte red, GLubyte green, GLubyte blue);
typedef void (*PFNGLSECONDARYCOLOR3UBVEXTPROC)(const GLubyte *v);
typedef void (*PFNGLSECONDARYCOLORPOINTEREXTPROC)(GLint size, GLenum type, GLsizei stride, const void *pointer);
typedef void (*PFNGLFOGCOORDFEXTPROC)(GLfloat coord);
typedef void (*PFNGLFOGCOORDPOINTEREXTPROC)(GLenum type, GLsizei stride, const void *pointer);
typedef void (*PFNGLBLENDFUNCSEPARATEEXTPROC)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
typedef void (*PFNGLCOLORTABLEEXTPROC)(GLenum target, GLenum internalFormat, GLsizei width, GLenum format, GLenum type, const void *table);
typedef void (*PFNGLCOLORSUBTABLEEXTPROC)(GLenum target, GLsizei start, GLsizei count, GLenum format, GLenum type, const void *data);
typedef void (*PFNGLGETCOLORTABLEEXTPROC)(GLenum target, GLenum format, GLenum type, void *table);
typedef void (*PFNGLGETCOLORTABLEPARAMETERIVEXTPROC)(GLenum target, GLenum pname, GLint *params);
typedef void (*PFNGLGETCOLORTABLEPARAMETERFVEXTPROC)(GLenum target, GLenum pname, GLfloat *params);

#endif
