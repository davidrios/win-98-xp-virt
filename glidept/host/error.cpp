//**************************************************************
//* OpenGLide's warning/error reporting for the 2ksbox build: into the
//* wrapper's log, never onto the process's stdout.
//*
//* Upstream's Unix platform prints to stdout, which here is the player's
//* (or QEMU's) own stdout — a "Warning: ..." line from a library loaded
//* three levels down is noise at best and confuses a script that parses
//* our output at worst.
//*
//* SPDX-License-Identifier: LGPL-2.1-or-later (matches OpenGLide)
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "platform/error.h"

#include "host.h"

void ReportWarning(const char *message)
{
    GlideHost_Log("warning: %s", message);
}

void ReportError(const char *message)
{
    GlideHost_Log("error: %s", message);
}
