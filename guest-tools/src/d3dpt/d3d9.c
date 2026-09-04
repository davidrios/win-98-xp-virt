/*
 * d3d9.c — the guest side of the paravirtual Direct3D 9 device (doc 14,
 * ADR-006): a d3d9.dll for Windows 98/2000/XP that encodes the app's
 * calls into the device's shared window (d3dpt/d3dpt_enc.h) and rings the
 * doorbell; the host executes them on DXVK. Methods are forward (append a
 * record), shadow (answered from guest-side state) or sync (Present,
 * creates, readbacks). The device is found at fixed physical addresses
 * through FXPTL.SYS (\\.\MAPMEM, 2000/XP) or FXMEMMAP.VXD (9x), the
 * helper the qemu-3dfx GL wrapper already installs.
 *
 * P1 scope: IDirect3D9 + IDirect3DDevice9 with Clear / states / transforms
 * / lights / DrawPrimitiveUP / Present (the D3D9TEST triangle); resources
 * and shaders are P2 (the protocol and the executor already carry them).
 *
 * Build: guest-tools/build-wrappers.sh (msvcrt, -march=pentium3).
 * Log: d3dpt.log next to the DLL. Env D3DPT_LOG=0 disables it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "fxlib.h"
#include "../../../d3dpt/d3dpt_enc.h"

/* ------------------------------------------------------------- logging */
static FILE *log_file;
static int log_enabled = 1;
static void d3dpt_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    if (!log_enabled) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n++] = '\n'; buf[n] = 0;
    OutputDebugStringA(buf);
    if (log_file) { fputs(buf, log_file); fflush(log_file); }
}
#define D3DPT_STUB(name) do { static int once; if (!once) { once = 1; d3dpt_log("d3dpt: %s not implemented", name); } } while (0)

/* ----------------------------------------------------------- transport */
static DRVFUNC drv;
static volatile uint32_t *regs;
static uint8_t *shm;
static d3dpt_enc enc;
static int attached;

static void doorbell(d3dpt_enc *e)
{
    (void)e;
    regs[D3DPT_REG_DOORBELL / 4] = 1;       /* synchronous: the host runs the batch inside this store */
}

static int transport_init(void)
{
    OSVERSIONINFOA os = { sizeof os };
    unsigned long va, len;
    uint32_t magic, ver, status;

    GetVersionExA(&os);
    if (os.dwPlatformId == VER_PLATFORM_WIN32_NT) kmdDrvInit(&drv); else vxdDrvInit(&drv);
    if (!drv.Init()) { d3dpt_log("d3dpt: no FXPTL.SYS / FXMEMMAP.VXD (install the WIN2KXP / WIN9X step)"); return 0; }
    len = 0x1000;
    if (!drv.MapLinear(0, D3DPT_MM_BASE, &va, &len)) { d3dpt_log("d3dpt: cannot map the register page"); return 0; }
    regs = (volatile uint32_t *)va;
    magic = regs[D3DPT_REG_MAGIC / 4];
    ver = regs[D3DPT_REG_VERSION / 4];
    if (magic != D3DPT_MAGIC) { d3dpt_log("d3dpt: no device at %08x (read %08x)", D3DPT_MM_BASE, magic); return 0; }
    if (ver != D3DPT_PROTO_VERSION) { d3dpt_log("d3dpt: host protocol %u, this DLL speaks %u", ver, D3DPT_PROTO_VERSION); return 0; }
    status = regs[D3DPT_REG_STATUS / 4];
    if (status != D3DPT_STATUS_READY) { d3dpt_log("d3dpt: device present but no executor on the host (status %u)", status); return 0; }
    len = D3DPT_SHM_SIZE;
    if (!drv.MapLinear(0, D3DPT_SHM_BASE, &va, &len)) { d3dpt_log("d3dpt: cannot map the %u MiB window", D3DPT_SHM_SIZE >> 20); return 0; }
    shm = (uint8_t *)va;
    regs[D3DPT_REG_ATTACH / 4] = 1;
    attached = 1;
    d3dpt_enc_init(&enc, shm, doorbell);
    d3dpt_log("d3dpt: device v%u at %08x, window at %08x, attached (%u)", ver, D3DPT_MM_BASE, D3DPT_SHM_BASE, regs[D3DPT_REG_ATTACH / 4]);
    return 1;
}

static void transport_fini(void)
{
    if (attached) { d3dpt_enc_flush(&enc); regs[D3DPT_REG_ATTACH / 4] = 0; attached = 0; }
    if (drv.Fini) drv.Fini();
}

/* ------------------------------------------------------------- objects */
struct d3d9 {
    const IDirect3D9Vtbl *vt;
    LONG ref;
    int have_info;
    d3dpt_adapter_identifier ident;
    D3DCAPS9 caps;
    uint32_t mode_count;
};

#define MAX_RS 256
#define MAX_TSS_STAGES 8
#define MAX_SAMPLERS 16
struct device {
    const IDirect3DDevice9Vtbl *vt;
    LONG ref;
    struct d3d9 *d3d;
    uint32_t handle;
    HWND focus, window;
    DWORD behavior;
    D3DPRESENT_PARAMETERS pp;
    int mode_changed;
    /* shadow state */
    DWORD rs[MAX_RS];
    DWORD tss[MAX_TSS_STAGES][33];
    DWORD samp[MAX_SAMPLERS][14];
    D3DMATRIX world, view, proj, tex[8];
    D3DVIEWPORT9 vp;
    D3DMATERIAL9 material;
    D3DLIGHT9 lights[8];
    BOOL light_on[8];
    DWORD fvf;
    RECT scissor;
    BOOL swvp;
};

static HRESULT get_adapter_info(struct d3d9 *d)
{
    uint32_t off;
    d3dpt_get_adapter *a;
    d3dpt_ret *r;
    const d3dpt_adapter_info *info;
    if (d->have_info) return D3D_OK;
    off = d3dpt_enc_ret(&enc, sizeof(d3dpt_adapter_info) + 64 * sizeof(d3dpt_mode));
    a = d3dpt_enc_cmd(&enc, D3DPT_OP_GET_ADAPTER, sizeof *a, 0);
    if (!a) return E_FAIL;
    a->adapter = 0; a->ret_off = off;
    d3dpt_enc_flush(&enc);
    if (enc.last_status) return E_FAIL;
    r = d3dpt_enc_result(&enc, off);
    if (FAILED(r->hr)) return r->hr;
    info = (const d3dpt_adapter_info *)(r + 1);
    d->ident = info->identifier;
    memcpy(&d->caps, info->caps, sizeof d->caps);
    d->mode_count = info->mode_count;
    d->have_info = 1;
    d3dpt_log("d3dpt: host adapter \"%s\" vs %u.%u ps %u.%u", d->ident.description,
              (unsigned)D3DSHADER_VERSION_MAJOR(d->caps.VertexShaderVersion), (unsigned)D3DSHADER_VERSION_MINOR(d->caps.VertexShaderVersion),
              (unsigned)D3DSHADER_VERSION_MAJOR(d->caps.PixelShaderVersion), (unsigned)D3DSHADER_VERSION_MINOR(d->caps.PixelShaderVersion));
    return D3D_OK;
}

/* the guest's own display modes: what the emulated VGA driver offers */
static D3DFORMAT bpp_format(DWORD bpp) { return bpp == 32 ? D3DFMT_X8R8G8B8 : bpp == 16 ? D3DFMT_R5G6B5 : D3DFMT_UNKNOWN; }
static int enum_mode(UINT idx, D3DFORMAT want, D3DDISPLAYMODE *out)
{
    DEVMODEA dm;
    UINT i, n = 0;
    memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        D3DFORMAT f = bpp_format(dm.dmBitsPerPel);
        if (f == D3DFMT_UNKNOWN || (want != D3DFMT_UNKNOWN && f != want)) continue;
        if (dm.dmPelsWidth < 640) continue;
        if (n == idx) {
            if (out) { out->Width = dm.dmPelsWidth; out->Height = dm.dmPelsHeight; out->RefreshRate = dm.dmDisplayFrequency; out->Format = f; }
            return 1;
        }
        n++;
    }
    return 0;
}
static UINT count_modes(D3DFORMAT want)
{
    UINT n = 0;
    while (enum_mode(n, want, NULL)) n++;
    return n;
}
static void current_mode(D3DDISPLAYMODE *m)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        m->Width = dm.dmPelsWidth; m->Height = dm.dmPelsHeight; m->RefreshRate = dm.dmDisplayFrequency;
        m->Format = bpp_format(dm.dmBitsPerPel);
        if (m->Format == D3DFMT_UNKNOWN) m->Format = D3DFMT_X8R8G8B8;   /* 24-bit desktops present as 8888 */
    } else { m->Width = 640; m->Height = 480; m->RefreshRate = 0; m->Format = D3DFMT_X8R8G8B8; }
}

#include "d3d9_vtbl.h"

/* ============================================================ IDirect3D9 */
HRESULT WINAPI d3d_QueryInterface(IDirect3D9 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3D9)) { *ppv = This; IDirect3D9_AddRef(This); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
ULONG WINAPI d3d_AddRef(IDirect3D9 *This) { return InterlockedIncrement(&((struct d3d9 *)This)->ref); }
ULONG WINAPI d3d_Release(IDirect3D9 *This)
{
    struct d3d9 *d = (struct d3d9 *)This;
    LONG r = InterlockedDecrement(&d->ref);
    if (r == 0) { d3dpt_log("d3dpt: IDirect3D9 released"); HeapFree(GetProcessHeap(), 0, d); }
    return r;
}
HRESULT WINAPI d3d_RegisterSoftwareDevice(IDirect3D9 *This, void *pInitializeFunction) { return D3D_OK; }
UINT WINAPI d3d_GetAdapterCount(IDirect3D9 *This) { return 1; }
HRESULT WINAPI d3d_GetAdapterIdentifier(IDirect3D9 *This, UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *id)
{
    struct d3d9 *d = (struct d3d9 *)This;
    HRESULT hr;
    if (Adapter != 0 || !id) return D3DERR_INVALIDCALL;
    hr = get_adapter_info(d);
    if (FAILED(hr)) return hr;
    memset(id, 0, sizeof *id);
    memcpy(id->Description, d->ident.description, sizeof id->Description);
    memcpy(id->Driver, d->ident.driver, sizeof id->Driver);
    memcpy(id->DeviceName, d->ident.device_name, sizeof id->DeviceName);
    id->DriverVersion.LowPart = d->ident.driver_version_lo;
    id->DriverVersion.HighPart = (LONG)d->ident.driver_version_hi;
    id->VendorId = d->ident.vendor_id; id->DeviceId = d->ident.device_id;
    id->SubSysId = d->ident.subsys_id; id->Revision = d->ident.revision;
    memcpy(&id->DeviceIdentifier, d->ident.guid, 16);
    id->WHQLLevel = d->ident.whql_level;
    return D3D_OK;
}
UINT WINAPI d3d_GetAdapterModeCount(IDirect3D9 *This, UINT Adapter, D3DFORMAT Format) { return Adapter ? 0 : count_modes(Format); }
HRESULT WINAPI d3d_EnumAdapterModes(IDirect3D9 *This, UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode)
{
    if (Adapter || !pMode) return D3DERR_INVALIDCALL;
    return enum_mode(Mode, Format, pMode) ? D3D_OK : D3DERR_INVALIDCALL;
}
HRESULT WINAPI d3d_GetAdapterDisplayMode(IDirect3D9 *This, UINT Adapter, D3DDISPLAYMODE *pMode)
{
    if (Adapter || !pMode) return D3DERR_INVALIDCALL;
    current_mode(pMode);
    return D3D_OK;
}
HRESULT WINAPI d3d_CheckDeviceType(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, WINBOOL bWindowed)
{
    if (Adapter || DevType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}
static int format_ok(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_A8L8: case D3DFMT_R8G8B8:
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case D3DFMT_D16: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: case D3DFMT_D16_LOCKABLE:
    case D3DFMT_INDEX16: case D3DFMT_INDEX32: case D3DFMT_VERTEXDATA:
    case D3DFMT_V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
        return 1;
    default:
        return 0;
    }
}
HRESULT WINAPI d3d_CheckDeviceFormat(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{
    if (Adapter || DeviceType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return format_ok(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
HRESULT WINAPI d3d_CheckDeviceMultiSampleType(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, WINBOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels)
{
    if (pQualityLevels) *pQualityLevels = 1;
    return MultiSampleType == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
}
HRESULT WINAPI d3d_CheckDepthStencilMatch(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
{
    return format_ok(DepthStencilFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
HRESULT WINAPI d3d_CheckDeviceFormatConversion(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) { return D3D_OK; }
HRESULT WINAPI d3d_GetDeviceCaps(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps)
{
    struct d3d9 *d = (struct d3d9 *)This;
    HRESULT hr;
    if (Adapter || !pCaps) return D3DERR_INVALIDCALL;
    if (DeviceType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    hr = get_adapter_info(d);
    if (FAILED(hr)) return hr;
    *pCaps = d->caps;
    pCaps->DeviceType = D3DDEVTYPE_HAL;
    pCaps->AdapterOrdinal = 0;
    return D3D_OK;
}
HMONITOR WINAPI d3d_GetAdapterMonitor(IDirect3D9 *This, UINT Adapter)
{
    POINT p = { 0, 0 };
    return Adapter ? NULL : MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
}

/* ------------------------------------------------------------ device */
static void set_fpu_pc24(void)
{
    unsigned short cw;
    __asm__ volatile ("fnstcw %0" : "=m" (cw));
    cw = (unsigned short)((cw & ~0x0300) | 0x0000);   /* PC = 24-bit, like the native runtime */
    __asm__ volatile ("fldcw %0" : : "m" (cw));
}

static void shadow_defaults(struct device *dev)
{
    int i;
    static const D3DMATRIX ident = {{{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }}};
    memset(dev->rs, 0, sizeof dev->rs);
    dev->rs[D3DRS_ZENABLE] = dev->pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    dev->rs[D3DRS_FILLMODE] = D3DFILL_SOLID; dev->rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    dev->rs[D3DRS_ZWRITEENABLE] = TRUE; dev->rs[D3DRS_LASTPIXEL] = TRUE;
    dev->rs[D3DRS_SRCBLEND] = D3DBLEND_ONE; dev->rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    dev->rs[D3DRS_CULLMODE] = D3DCULL_CCW; dev->rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    dev->rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS; dev->rs[D3DRS_DITHERENABLE] = FALSE;
    dev->rs[D3DRS_SPECULARENABLE] = FALSE; dev->rs[D3DRS_FOGCOLOR] = 0;
    dev->rs[D3DRS_LIGHTING] = TRUE; dev->rs[D3DRS_AMBIENT] = 0;
    dev->rs[D3DRS_COLORVERTEX] = TRUE; dev->rs[D3DRS_LOCALVIEWER] = TRUE;
    dev->rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1; dev->rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    dev->rs[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL; dev->rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    dev->rs[D3DRS_COLORWRITEENABLE] = 0xf; dev->rs[D3DRS_STENCILMASK] = 0xffffffff; dev->rs[D3DRS_STENCILWRITEMASK] = 0xffffffff;
    dev->rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS; dev->rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    dev->rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP; dev->rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    dev->rs[D3DRS_POINTSIZE] = 0x3f800000; dev->rs[D3DRS_POINTSIZE_MAX] = 0x42800000; dev->rs[D3DRS_POINTSCALE_A] = 0x3f800000;
    dev->rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD; dev->rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE; dev->rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    dev->rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD; dev->rs[D3DRS_COLORWRITEENABLE1] = dev->rs[D3DRS_COLORWRITEENABLE2] = dev->rs[D3DRS_COLORWRITEENABLE3] = 0xf;
    dev->rs[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE; dev->rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    dev->rs[D3DRS_CCW_STENCILFAIL] = dev->rs[D3DRS_CCW_STENCILZFAIL] = dev->rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    memset(dev->tss, 0, sizeof dev->tss);
    for (i = 0; i < MAX_TSS_STAGES; i++) {
        dev->tss[i][D3DTSS_COLOROP] = i ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        dev->tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE; dev->tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        dev->tss[i][D3DTSS_ALPHAOP] = i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        dev->tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; dev->tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        dev->tss[i][D3DTSS_TEXCOORDINDEX] = i; dev->tss[i][D3DTSS_COLORARG0] = dev->tss[i][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        dev->tss[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    }
    memset(dev->samp, 0, sizeof dev->samp);
    for (i = 0; i < MAX_SAMPLERS; i++) {
        dev->samp[i][D3DSAMP_ADDRESSU] = dev->samp[i][D3DSAMP_ADDRESSV] = dev->samp[i][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        dev->samp[i][D3DSAMP_MAGFILTER] = dev->samp[i][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        dev->samp[i][D3DSAMP_MIPFILTER] = D3DTEXF_NONE; dev->samp[i][D3DSAMP_MAXANISOTROPY] = 1;
    }
    dev->world = dev->view = dev->proj = ident;
    for (i = 0; i < 8; i++) dev->tex[i] = ident;
    dev->vp.X = dev->vp.Y = 0; dev->vp.Width = dev->pp.BackBufferWidth; dev->vp.Height = dev->pp.BackBufferHeight;
    dev->vp.MinZ = 0.0f; dev->vp.MaxZ = 1.0f;
    memset(&dev->material, 0, sizeof dev->material);
    memset(dev->lights, 0, sizeof dev->lights); memset(dev->light_on, 0, sizeof dev->light_on);
    dev->fvf = 0;
    dev->scissor.left = dev->scissor.top = 0; dev->scissor.right = dev->pp.BackBufferWidth; dev->scissor.bottom = dev->pp.BackBufferHeight;
}

static void normalize_pp(struct device *dev, D3DPRESENT_PARAMETERS *pp)
{
    D3DDISPLAYMODE cur;
    current_mode(&cur);
    if (!pp->hDeviceWindow) pp->hDeviceWindow = dev->focus;
    if (pp->Windowed) {
        if (!pp->BackBufferWidth || !pp->BackBufferHeight) {
            RECT rc = { 0, 0, 640, 480 };
            if (pp->hDeviceWindow) GetClientRect(pp->hDeviceWindow, &rc);
            if (!pp->BackBufferWidth) pp->BackBufferWidth = rc.right - rc.left ? rc.right - rc.left : 640;
            if (!pp->BackBufferHeight) pp->BackBufferHeight = rc.bottom - rc.top ? rc.bottom - rc.top : 480;
        }
        if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = cur.Format;
    } else {
        if (!pp->BackBufferWidth) pp->BackBufferWidth = cur.Width;
        if (!pp->BackBufferHeight) pp->BackBufferHeight = cur.Height;
        if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = cur.Format;
    }
    if (!pp->BackBufferCount) pp->BackBufferCount = 1;
}

/* fullscreen: switch the guest display so the desktop matches; the host
 * renders at the backbuffer size regardless and shows it instead of VGA */
static void apply_display_mode(struct device *dev, const D3DPRESENT_PARAMETERS *pp)
{
    if (!pp->Windowed) {
        DEVMODEA dm;
        LONG r;
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        dm.dmPelsWidth = pp->BackBufferWidth; dm.dmPelsHeight = pp->BackBufferHeight;
        dm.dmBitsPerPel = (pp->BackBufferFormat == D3DFMT_R5G6B5 || pp->BackBufferFormat == D3DFMT_X1R5G5B5 || pp->BackBufferFormat == D3DFMT_A1R5G5B5) ? 16 : 32;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        if (pp->FullScreen_RefreshRateInHz) { dm.dmDisplayFrequency = pp->FullScreen_RefreshRateInHz; dm.dmFields |= DM_DISPLAYFREQUENCY; }
        r = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
        if (r != DISP_CHANGE_SUCCESSFUL && dm.dmBitsPerPel == 32) {   /* 24-bit desktops */
            dm.dmBitsPerPel = 24;
            r = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
        }
        d3dpt_log("d3dpt: fullscreen %ux%ux%u -> ChangeDisplaySettings %ld", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, r);
        dev->mode_changed = (r == DISP_CHANGE_SUCCESSFUL);
        if (pp->hDeviceWindow) {
            SetWindowPos(pp->hDeviceWindow, HWND_TOPMOST, 0, 0, pp->BackBufferWidth, pp->BackBufferHeight, SWP_SHOWWINDOW);
        }
    } else if (dev->mode_changed) {
        ChangeDisplaySettingsA(NULL, 0);
        dev->mode_changed = 0;
    }
}

static HRESULT send_create(struct device *dev, uint32_t op)
{
    uint32_t off = d3dpt_enc_ret(&enc, 0);
    d3dpt_create_device *c = d3dpt_enc_cmd(&enc, op, sizeof *c, 0);
    HRESULT hr;
    if (!c) return E_FAIL;
    memset(c, 0, sizeof *c);
    c->handle = dev->handle; c->ret_off = off;
    c->adapter = 0; c->devtype = D3DDEVTYPE_HAL; c->behavior = dev->behavior;
    c->pp.width = dev->pp.BackBufferWidth; c->pp.height = dev->pp.BackBufferHeight;
    c->pp.format = dev->pp.BackBufferFormat; c->pp.backbuffer_count = dev->pp.BackBufferCount;
    c->pp.multisample = dev->pp.MultiSampleType; c->pp.multisample_quality = dev->pp.MultiSampleQuality;
    c->pp.swap_effect = dev->pp.SwapEffect; c->pp.windowed = dev->pp.Windowed;
    c->pp.auto_depth = dev->pp.EnableAutoDepthStencil; c->pp.depth_format = dev->pp.AutoDepthStencilFormat;
    c->pp.flags = dev->pp.Flags; c->pp.refresh = dev->pp.FullScreen_RefreshRateInHz; c->pp.interval = dev->pp.PresentationInterval;
    d3dpt_enc_flush(&enc);
    if (enc.last_status) { d3dpt_log("d3dpt: create/reset batch failed (%u)", enc.last_status); return E_FAIL; }
    hr = d3dpt_enc_result(&enc, off)->hr;
    d3dpt_log("d3dpt: %s %ux%u fmt %u %s depth %u/%u behavior 0x%lx -> 0x%08lx", op == D3DPT_OP_CREATE_DEVICE ? "CreateDevice" : "Reset",
              dev->pp.BackBufferWidth, dev->pp.BackBufferHeight, dev->pp.BackBufferFormat, dev->pp.Windowed ? "windowed" : "fullscreen",
              dev->pp.EnableAutoDepthStencil, dev->pp.AutoDepthStencilFormat, (unsigned long)dev->behavior, (unsigned long)hr);
    return hr;
}

HRESULT WINAPI d3d_CreateDevice(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    struct d3d9 *d = (struct d3d9 *)This;
    struct device *dev;
    HRESULT hr;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (Adapter || !pp) return D3DERR_INVALIDCALL;
    if (DeviceType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    hr = get_adapter_info(d);
    if (FAILED(hr)) return hr;
    dev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *dev);
    if (!dev) return E_OUTOFMEMORY;
    dev->vt = &dev_vtbl; dev->ref = 1; dev->d3d = d; IDirect3D9_AddRef(This);
    dev->handle = d3dpt_enc_handle(&enc);
    dev->focus = hFocusWindow; dev->behavior = BehaviorFlags;
    dev->pp = *pp;
    normalize_pp(dev, &dev->pp);
    dev->window = dev->pp.hDeviceWindow;
    *pp = dev->pp;
    hr = send_create(dev, D3DPT_OP_CREATE_DEVICE);
    if (FAILED(hr)) { IDirect3D9_Release(This); HeapFree(GetProcessHeap(), 0, dev); return hr; }
    apply_display_mode(dev, &dev->pp);
    shadow_defaults(dev);
    if (!(BehaviorFlags & D3DCREATE_FPU_PRESERVE)) set_fpu_pc24();
    *out = (IDirect3DDevice9 *)dev;
    return D3D_OK;
}

/* ====================================================== IDirect3DDevice9 */
#define DEV(This) ((struct device *)(This))

HRESULT WINAPI dev_QueryInterface(IDirect3DDevice9 *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DDevice9)) { *ppv = This; IDirect3DDevice9_AddRef(This); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
ULONG WINAPI dev_AddRef(IDirect3DDevice9 *This) { return InterlockedIncrement(&DEV(This)->ref); }
ULONG WINAPI dev_Release(IDirect3DDevice9 *This)
{
    struct device *dev = DEV(This);
    LONG r = InterlockedDecrement(&dev->ref);
    if (r == 0) {
        d3dpt_handle *h = d3dpt_enc_cmd(&enc, D3DPT_OP_RELEASE, sizeof *h, 0);
        if (h) { h->handle = dev->handle; h->pad = 0; }
        d3dpt_enc_flush(&enc);
        if (dev->mode_changed) ChangeDisplaySettingsA(NULL, 0);
        d3dpt_log("d3dpt: device released");
        IDirect3D9_Release((IDirect3D9 *)dev->d3d);
        HeapFree(GetProcessHeap(), 0, dev);
    }
    return r;
}
HRESULT WINAPI dev_TestCooperativeLevel(IDirect3DDevice9 *This) { return D3D_OK; }
UINT WINAPI dev_GetAvailableTextureMem(IDirect3DDevice9 *This) { return 256u << 20; }
HRESULT WINAPI dev_EvictManagedResources(IDirect3DDevice9 *This) { return D3D_OK; }
HRESULT WINAPI dev_GetDirect3D(IDirect3DDevice9 *This, IDirect3D9 **pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = (IDirect3D9 *)DEV(This)->d3d; IDirect3D9_AddRef(*pp);
    return D3D_OK;
}
HRESULT WINAPI dev_GetDeviceCaps(IDirect3DDevice9 *This, D3DCAPS9 *pCaps) { return d3d_GetDeviceCaps((IDirect3D9 *)DEV(This)->d3d, 0, D3DDEVTYPE_HAL, pCaps); }
HRESULT WINAPI dev_GetDisplayMode(IDirect3DDevice9 *This, UINT iSwapChain, D3DDISPLAYMODE *pMode)
{
    struct device *dev = DEV(This);
    if (iSwapChain || !pMode) return D3DERR_INVALIDCALL;
    if (dev->pp.Windowed) current_mode(pMode);
    else { pMode->Width = dev->pp.BackBufferWidth; pMode->Height = dev->pp.BackBufferHeight; pMode->RefreshRate = dev->pp.FullScreen_RefreshRateInHz; pMode->Format = dev->pp.BackBufferFormat; }
    return D3D_OK;
}
HRESULT WINAPI dev_GetCreationParameters(IDirect3DDevice9 *This, D3DDEVICE_CREATION_PARAMETERS *p)
{
    if (!p) return D3DERR_INVALIDCALL;
    p->AdapterOrdinal = 0; p->DeviceType = D3DDEVTYPE_HAL; p->hFocusWindow = DEV(This)->focus; p->BehaviorFlags = DEV(This)->behavior;
    return D3D_OK;
}
HRESULT WINAPI dev_SetCursorProperties(IDirect3DDevice9 *This, UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) { return D3D_OK; }
void WINAPI dev_SetCursorPosition(IDirect3DDevice9 *This, int X, int Y, DWORD Flags) { SetCursorPos(X, Y); }
WINBOOL WINAPI dev_ShowCursor(IDirect3DDevice9 *This, WINBOOL bShow) { return FALSE; }
UINT WINAPI dev_GetNumberOfSwapChains(IDirect3DDevice9 *This) { return 1; }
HRESULT WINAPI dev_Reset(IDirect3DDevice9 *This, D3DPRESENT_PARAMETERS *pp)
{
    struct device *dev = DEV(This);
    HRESULT hr;
    if (!pp) return D3DERR_INVALIDCALL;
    dev->pp = *pp;
    normalize_pp(dev, &dev->pp);
    *pp = dev->pp;
    hr = send_create(dev, D3DPT_OP_RESET_DEVICE);
    if (FAILED(hr)) return hr;
    apply_display_mode(dev, &dev->pp);
    shadow_defaults(dev);
    return D3D_OK;
}
HRESULT WINAPI dev_Present(IDirect3DDevice9 *This, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
{
    uint32_t hr = d3dpt_enc_sync(&enc, D3DPT_OP_PRESENT, DEV(This)->handle);
    if (enc.last_status) { d3dpt_log("d3dpt: Present: batch error %u at record %u", enc.last_status, d3dpt_enc_hdr(&enc)->ret_index); return D3DERR_DRIVERINTERNALERROR; }
    return (HRESULT)hr;
}
HRESULT WINAPI dev_BeginScene(IDirect3DDevice9 *This) { d3dpt_enc_nobody(&enc, D3DPT_OP_BEGIN_SCENE); return D3D_OK; }
HRESULT WINAPI dev_EndScene(IDirect3DDevice9 *This) { d3dpt_enc_nobody(&enc, D3DPT_OP_END_SCENE); return D3D_OK; }
HRESULT WINAPI dev_Clear(IDirect3DDevice9 *This, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    d3dpt_clear *c;
    if (Count > 64 || (Count && !pRects)) return D3DERR_INVALIDCALL;
    c = d3dpt_enc_cmd(&enc, D3DPT_OP_CLEAR, sizeof *c, Count * sizeof(D3DRECT));
    if (!c) return E_FAIL;
    c->count = Count; c->flags = Flags; c->color = Color; c->z = Z; c->stencil = Stencil; c->pad = 0;
    if (Count) memcpy(c + 1, pRects, Count * sizeof(D3DRECT));
    return D3D_OK;
}
static D3DMATRIX *xform_slot(struct device *dev, D3DTRANSFORMSTATETYPE t)
{
    if (t == D3DTS_VIEW) return &dev->view;
    if (t == D3DTS_PROJECTION) return &dev->proj;
    if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7) return &dev->tex[t - D3DTS_TEXTURE0];
    if (t == D3DTS_WORLD) return &dev->world;
    return NULL;
}
HRESULT WINAPI dev_SetTransform(IDirect3DDevice9 *This, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *m)
{
    D3DMATRIX *slot = xform_slot(DEV(This), State);
    d3dpt_transform *t;
    if (!m) return D3DERR_INVALIDCALL;
    if (slot) *slot = *m;
    t = d3dpt_enc_cmd(&enc, D3DPT_OP_SET_TRANSFORM, sizeof *t, 0);
    if (!t) return E_FAIL;
    t->state = State; t->pad = 0; memcpy(t->m, m, sizeof t->m);
    return D3D_OK;
}
HRESULT WINAPI dev_GetTransform(IDirect3DDevice9 *This, D3DTRANSFORMSTATETYPE State, D3DMATRIX *m)
{
    D3DMATRIX *slot = xform_slot(DEV(This), State);
    if (!m || !slot) return D3DERR_INVALIDCALL;
    *m = *slot;
    return D3D_OK;
}
HRESULT WINAPI dev_MultiplyTransform(IDirect3DDevice9 *This, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *m)
{
    D3DMATRIX *slot = xform_slot(DEV(This), State), r;
    int i, j, k;
    if (!m || !slot) return D3DERR_INVALIDCALL;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) {
        float s = 0.0f;
        for (k = 0; k < 4; k++) s += m->m[i][k] * slot->m[k][j];
        r.m[i][j] = s;
    }
    return dev_SetTransform(This, State, &r);
}
HRESULT WINAPI dev_SetViewport(IDirect3DDevice9 *This, const D3DVIEWPORT9 *vp)
{
    d3dpt_viewport *v;
    if (!vp) return D3DERR_INVALIDCALL;
    DEV(This)->vp = *vp;
    v = d3dpt_enc_cmd(&enc, D3DPT_OP_SET_VIEWPORT, sizeof *v, 0);
    if (!v) return E_FAIL;
    memcpy(v, vp, sizeof *v);
    return D3D_OK;
}
HRESULT WINAPI dev_GetViewport(IDirect3DDevice9 *This, D3DVIEWPORT9 *vp) { if (!vp) return D3DERR_INVALIDCALL; *vp = DEV(This)->vp; return D3D_OK; }
HRESULT WINAPI dev_SetMaterial(IDirect3DDevice9 *This, const D3DMATERIAL9 *m)
{
    d3dpt_material *p;
    if (!m) return D3DERR_INVALIDCALL;
    DEV(This)->material = *m;
    p = d3dpt_enc_cmd(&enc, D3DPT_OP_SET_MATERIAL, sizeof *p, 0);
    if (!p) return E_FAIL;
    memcpy(p->material, m, sizeof p->material); p->pad = 0;
    return D3D_OK;
}
HRESULT WINAPI dev_GetMaterial(IDirect3DDevice9 *This, D3DMATERIAL9 *m) { if (!m) return D3DERR_INVALIDCALL; *m = DEV(This)->material; return D3D_OK; }
HRESULT WINAPI dev_SetLight(IDirect3DDevice9 *This, DWORD Index, const D3DLIGHT9 *l)
{
    d3dpt_light *p;
    if (!l) return D3DERR_INVALIDCALL;
    if (Index < 8) DEV(This)->lights[Index] = *l;
    p = d3dpt_enc_cmd(&enc, D3DPT_OP_SET_LIGHT, sizeof *p, 0);
    if (!p) return E_FAIL;
    p->index = Index; p->pad = 0; memcpy(p->light, l, sizeof p->light);
    return D3D_OK;
}
HRESULT WINAPI dev_GetLight(IDirect3DDevice9 *This, DWORD Index, D3DLIGHT9 *l)
{
    if (!l || Index >= 8) return D3DERR_INVALIDCALL;
    *l = DEV(This)->lights[Index];
    return D3D_OK;
}
HRESULT WINAPI dev_LightEnable(IDirect3DDevice9 *This, DWORD Index, WINBOOL Enable)
{
    if (Index < 8) DEV(This)->light_on[Index] = Enable;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_LIGHT_ENABLE, Index, Enable ? 1 : 0);
    return D3D_OK;
}
HRESULT WINAPI dev_GetLightEnable(IDirect3DDevice9 *This, DWORD Index, WINBOOL *pEnable)
{
    if (!pEnable || Index >= 8) return D3DERR_INVALIDCALL;
    *pEnable = DEV(This)->light_on[Index];
    return D3D_OK;
}
HRESULT WINAPI dev_SetRenderState(IDirect3DDevice9 *This, D3DRENDERSTATETYPE State, DWORD Value)
{
    if ((DWORD)State < MAX_RS) DEV(This)->rs[State] = Value;
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_RENDER_STATE, State, Value);
    return D3D_OK;
}
HRESULT WINAPI dev_GetRenderState(IDirect3DDevice9 *This, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    if (!pValue || (DWORD)State >= MAX_RS) return D3DERR_INVALIDCALL;
    *pValue = DEV(This)->rs[State];
    return D3D_OK;
}
HRESULT WINAPI dev_SetTexture(IDirect3DDevice9 *This, DWORD Stage, IDirect3DBaseTexture9 *pTexture)
{
    if (pTexture) { D3DPT_STUB("IDirect3DDevice9::SetTexture(non-NULL) [P2]"); return D3DERR_INVALIDCALL; }
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_TEXTURE, Stage, 0);
    return D3D_OK;
}
HRESULT WINAPI dev_SetTextureStageState(IDirect3DDevice9 *This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    if (Stage < MAX_TSS_STAGES && (DWORD)Type < 33) DEV(This)->tss[Stage][Type] = Value;
    d3dpt_enc_u32x3(&enc, D3DPT_OP_SET_TEXTURE_STAGE_STATE, Stage, Type, Value);
    return D3D_OK;
}
HRESULT WINAPI dev_GetTextureStageState(IDirect3DDevice9 *This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    if (!pValue || Stage >= MAX_TSS_STAGES || (DWORD)Type >= 33) return D3DERR_INVALIDCALL;
    *pValue = DEV(This)->tss[Stage][Type];
    return D3D_OK;
}
HRESULT WINAPI dev_SetSamplerState(IDirect3DDevice9 *This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    if (Sampler < MAX_SAMPLERS && (DWORD)Type < 14) DEV(This)->samp[Sampler][Type] = Value;
    d3dpt_enc_u32x3(&enc, D3DPT_OP_SET_SAMPLER_STATE, Sampler, Type, Value);
    return D3D_OK;
}
HRESULT WINAPI dev_GetSamplerState(IDirect3DDevice9 *This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue)
{
    if (!pValue || Sampler >= MAX_SAMPLERS || (DWORD)Type >= 14) return D3DERR_INVALIDCALL;
    *pValue = DEV(This)->samp[Sampler][Type];
    return D3D_OK;
}
HRESULT WINAPI dev_ValidateDevice(IDirect3DDevice9 *This, DWORD *pNumPasses) { if (pNumPasses) *pNumPasses = 1; return D3D_OK; }
HRESULT WINAPI dev_SetScissorRect(IDirect3DDevice9 *This, const RECT *pRect)
{
    if (!pRect) return D3DERR_INVALIDCALL;
    DEV(This)->scissor = *pRect;
    D3DPT_STUB("IDirect3DDevice9::SetScissorRect [P2: not forwarded yet]");
    return D3D_OK;
}
HRESULT WINAPI dev_GetScissorRect(IDirect3DDevice9 *This, RECT *pRect) { if (!pRect) return D3DERR_INVALIDCALL; *pRect = DEV(This)->scissor; return D3D_OK; }
HRESULT WINAPI dev_SetSoftwareVertexProcessing(IDirect3DDevice9 *This, WINBOOL bSoftware) { DEV(This)->swvp = bSoftware; return D3D_OK; }
WINBOOL WINAPI dev_GetSoftwareVertexProcessing(IDirect3DDevice9 *This) { return DEV(This)->swvp; }
static UINT prim_vertex_count(D3DPRIMITIVETYPE t, UINT n)
{
    switch (t) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n * 2;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return n * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
    default: return 0;
    }
}
HRESULT WINAPI dev_DrawPrimitiveUP(IDirect3DDevice9 *This, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    UINT nv = prim_vertex_count(PrimitiveType, PrimitiveCount), bytes;
    d3dpt_draw_up *d;
    if (!nv || !pVertexStreamZeroData || !VertexStreamZeroStride) return D3DERR_INVALIDCALL;
    bytes = nv * VertexStreamZeroStride;
    d = d3dpt_enc_cmd(&enc, D3DPT_OP_DRAW_PRIMITIVE_UP, sizeof *d, bytes);
    if (!d) return D3DERR_INVALIDCALL;
    d->type = PrimitiveType; d->prim_count = PrimitiveCount; d->stride = VertexStreamZeroStride; d->bytes = bytes;
    memcpy(d + 1, pVertexStreamZeroData, bytes);
    return D3D_OK;
}
HRESULT WINAPI dev_SetFVF(IDirect3DDevice9 *This, DWORD FVF) { DEV(This)->fvf = FVF; d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_FVF, FVF, 0); return D3D_OK; }
HRESULT WINAPI dev_GetFVF(IDirect3DDevice9 *This, DWORD *pFVF) { if (!pFVF) return D3DERR_INVALIDCALL; *pFVF = DEV(This)->fvf; return D3D_OK; }
HRESULT WINAPI dev_SetVertexShader(IDirect3DDevice9 *This, IDirect3DVertexShader9 *pShader)
{
    if (pShader) { D3DPT_STUB("IDirect3DDevice9::SetVertexShader(non-NULL) [P2]"); return D3DERR_INVALIDCALL; }
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_VERTEX_SHADER, 0, 0);
    return D3D_OK;
}
HRESULT WINAPI dev_SetPixelShader(IDirect3DDevice9 *This, IDirect3DPixelShader9 *pShader)
{
    if (pShader) { D3DPT_STUB("IDirect3DDevice9::SetPixelShader(non-NULL) [P2]"); return D3DERR_INVALIDCALL; }
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_PIXEL_SHADER, 0, 0);
    return D3D_OK;
}
HRESULT WINAPI dev_SetStreamSource(IDirect3DDevice9 *This, UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride)
{
    if (pStreamData) { D3DPT_STUB("IDirect3DDevice9::SetStreamSource(non-NULL) [P2]"); return D3DERR_INVALIDCALL; }
    d3dpt_enc_u32x4(&enc, D3DPT_OP_SET_STREAM_SOURCE, StreamNumber, 0, OffsetInBytes, Stride);
    return D3D_OK;
}
HRESULT WINAPI dev_SetIndices(IDirect3DDevice9 *This, IDirect3DIndexBuffer9 *pIndexData)
{
    if (pIndexData) { D3DPT_STUB("IDirect3DDevice9::SetIndices(non-NULL) [P2]"); return D3DERR_INVALIDCALL; }
    d3dpt_enc_u32x2(&enc, D3DPT_OP_SET_INDICES, 0, 0);
    return D3D_OK;
}
static HRESULT set_const_f(uint32_t op, UINT start, const float *data, UINT count)
{
    d3dpt_u32x2 *p;
    if (!data || count > 256 || start + count > 256) return D3DERR_INVALIDCALL;
    if (!count) return D3D_OK;
    p = d3dpt_enc_cmd(&enc, op, sizeof *p, count * 16);
    if (!p) return E_FAIL;
    p->a = start; p->b = count;
    memcpy(p + 1, data, count * 16);
    return D3D_OK;
}
HRESULT WINAPI dev_SetVertexShaderConstantF(IDirect3DDevice9 *This, UINT StartRegister, const float *pConstantData, UINT Vector4fCount) { return set_const_f(D3DPT_OP_SET_VS_CONST_F, StartRegister, pConstantData, Vector4fCount); }
HRESULT WINAPI dev_SetPixelShaderConstantF(IDirect3DDevice9 *This, UINT StartRegister, const float *pConstantData, UINT Vector4fCount) { return set_const_f(D3DPT_OP_SET_PS_CONST_F, StartRegister, pConstantData, Vector4fCount); }

/* ============================================================== exports */
__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion)
{
    struct d3d9 *d;
    if (!attached) { d3dpt_log("d3dpt: Direct3DCreate9: no device"); return NULL; }
    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) return NULL;
    d->vt = &d3d_vtbl; d->ref = 1;
    d3dpt_log("d3dpt: Direct3DCreate9(sdk %u)", SDKVersion);
    return (IDirect3D9 *)d;
}
__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **out) { if (out) *out = NULL; return D3DERR_NOTAVAILABLE; }
__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) { return 0; }
__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void) { return 0; }
__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) { }
__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) { }
__declspec(dllexport) WINBOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD dwOptions) { }
__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
__declspec(dllexport) HRESULT WINAPI DebugSetMute(void) { return D3D_OK; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *p;
        const char *env = getenv("D3DPT_LOG");
        if (env && env[0] == '0') log_enabled = 0;
        if (log_enabled && GetModuleFileNameA(inst, path, sizeof path)) {
            p = strrchr(path, '\\');
            if (p) { strcpy(p + 1, "d3dpt.log"); log_file = fopen(path, "a"); }
            if (!log_file) log_file = fopen("C:\\d3dpt.log", "a");   /* read-only media (the ISO) */
        }
        d3dpt_log("d3dpt: d3d9.dll (paravirtual Direct3D, protocol %u) attached to process %lu", D3DPT_PROTO_VERSION, (unsigned long)GetCurrentProcessId());
        DisableThreadLibraryCalls(inst);
        if (!transport_init()) {
            d3dpt_log("d3dpt: refusing to load without the device");
            if (log_file) { fclose(log_file); log_file = NULL; }
            return FALSE;
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        transport_fini();
        d3dpt_log("d3dpt: detached");
        if (log_file) fclose(log_file);
    }
    return TRUE;
}
