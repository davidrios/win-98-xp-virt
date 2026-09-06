//**************************************************************
//* <GL/gl.h> for a host that has OpenGL.framework and no GL/ dir
//* (2ksbox, doc 12 §5).
//*
//* OpenGLide's platform/window.h reaches for <OpenGL/gl.h> only under
//* __MACOSX__, an SDL-era define nothing sets, and everything else in the
//* tree says <GL/gl.h> outright. macOS has no such header: the framework
//* keeps its own under OpenGL/, and the only GL/ on the box belongs to
//* XQuartz's Mesa, which is the one implementation this build must not
//* bind to (docs/build-macos.md). So the include path gets this directory
//* on Darwin only — scripts/build-glide.sh adds it, no other host sees it.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifndef GLIDEPT_MACOS_GL_H
#define GLIDEPT_MACOS_GL_H

#include <OpenGL/gl.h>
#include <GL/glext.h>

#endif
