//**************************************************************
//* Internal glue for the 2ksbox build of OpenGLide: the host ops table
//* hw/3dfx hands us (glide_host.h), the configuration bits it sets
//* through setConfig, and a log that is off unless asked for.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifndef GLIDEPT_HOST_H
#define GLIDEPT_HOST_H

#include <stdio.h>

#include "glide_host.h"

/* NULL until hw/3dfx calls setHostOps (or if it passes a table we don't
 * understand): then the wrapper renders into whatever context is current. */
const GlideHostOps *GlideHost_Ops(void);

/*
 * The one log switch: GLIDE_HOST_LOG=<path> (or "-" for stderr) opens it,
 * nothing is written without it, and OpenGLide's own GlideMsg/Error go to
 * the same file (patches/openglide/02). Upstream instead writes
 * OpenGLid.log and OpenGLid.err into the working directory from a static
 * constructor, and exits the process when it cannot — inside QEMU that is
 * neither wanted nor survivable.
 */
FILE *GlideHost_LogFile(void);
void GlideHost_Log(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* The resolution glidewnd.c asked for through setConfigRes (0 = the one
 * the guest's grSstWinOpen names), and whether it wants a windowed frame.
 * Both are advisory here: the drawable is the host's. */
int GlideHost_ConfigWidth(void);

#endif /* GLIDEPT_HOST_H */
