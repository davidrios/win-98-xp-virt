/*
 * ddrawint.h stand-in. mingw-w64 (14.0) ships winddi.h but not the
 * DirectDraw DDI header it includes; winddi.h only names these types in
 * prototypes we do not implement (DrvGetDirectDrawInfo, DrvEnableDirectDraw,
 * DrvDeriveSurface, ...), so opaque declarations are enough for the GDI-only
 * driver of M7a. M7b (the DirectDraw DDI) replaces this with real
 * definitions.
 */
#ifndef D3DPT_DDRAWINT_STUB_H
#define D3DPT_DDRAWINT_STUB_H
typedef struct _DD_SURFACE_LOCAL DD_SURFACE_LOCAL, *PDD_SURFACE_LOCAL;
typedef struct _DD_DIRECTDRAW_GLOBAL DD_DIRECTDRAW_GLOBAL, *PDD_DIRECTDRAW_GLOBAL;
typedef struct _DD_HALINFO DD_HALINFO, *PDD_HALINFO;
typedef struct _DD_CALLBACKS DD_CALLBACKS, *PDD_CALLBACKS;
typedef struct _DD_SURFACECALLBACKS DD_SURFACECALLBACKS, *PDD_SURFACECALLBACKS;
typedef struct _DD_PALETTECALLBACKS DD_PALETTECALLBACKS, *PDD_PALETTECALLBACKS;
typedef struct _VIDEOMEMORY VIDEOMEMORY, *PVIDEOMEMORY;
#endif
