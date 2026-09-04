#!/usr/bin/env python3
"""Generate d3d9_vtbl.h for the paravirtual d3d9.dll from mingw's d3d9.h:
prototypes for every IDirect3D9 / IDirect3DDevice9 method (so the
implementations are signature-checked), E_NOTIMPL stubs (log once) for
the ones not implemented, a call-trace wrapper per method (the vtable
entry; logs entry/exit to d3dpt_trace.log when tracing is on, see
d3dpt_trace in d3d9.c) and the vtables in header order.
Usage: gen_vtbl.py /usr/i686-w64-mingw32/include/d3d9.h > d3d9_vtbl.h
Implemented method names are read from the D3DPT_IMPL_* lists below."""
import re, sys

IMPL_D3D = set("""QueryInterface AddRef Release RegisterSoftwareDevice GetAdapterCount
GetAdapterIdentifier GetAdapterModeCount EnumAdapterModes GetAdapterDisplayMode CheckDeviceType
CheckDeviceFormat CheckDeviceMultiSampleType CheckDepthStencilMatch CheckDeviceFormatConversion
GetDeviceCaps GetAdapterMonitor CreateDevice""".split())
IMPL_RES = set("""QueryInterface AddRef Release GetDevice SetPrivateData GetPrivateData FreePrivateData
SetPriority GetPriority PreLoad GetType""".split())
IMPL_VB = IMPL_RES | set("Lock Unlock GetDesc".split())
IMPL_IB = IMPL_VB
IMPL_TEX = IMPL_RES | set("""SetLOD GetLOD GetLevelCount SetAutoGenFilterType GetAutoGenFilterType
GenerateMipSubLevels GetLevelDesc GetSurfaceLevel LockRect UnlockRect AddDirtyRect""".split())
IMPL_SURF = IMPL_RES | set("GetContainer GetDesc LockRect UnlockRect".split())
IMPL_SH = set("QueryInterface AddRef Release GetDevice GetFunction".split())
IMPL_CUBE = IMPL_RES | set("""SetLOD GetLOD GetLevelCount SetAutoGenFilterType GetAutoGenFilterType
GenerateMipSubLevels GetLevelDesc GetCubeMapSurface LockRect UnlockRect AddDirtyRect""".split())
IMPL_DECL = set("QueryInterface AddRef Release GetDevice GetDeclaration".split())
IMPL_QUERY = set("QueryInterface AddRef Release GetDevice GetType GetDataSize Issue GetData".split())
IMPL_SB = set("QueryInterface AddRef Release GetDevice Capture Apply".split())
IMPL_DEV = set("""QueryInterface AddRef Release TestCooperativeLevel GetAvailableTextureMem
EvictManagedResources GetDirect3D GetDeviceCaps GetDisplayMode GetCreationParameters
SetCursorProperties SetCursorPosition ShowCursor GetNumberOfSwapChains Reset Present
BeginScene EndScene Clear SetTransform GetTransform MultiplyTransform SetViewport GetViewport
SetMaterial GetMaterial SetLight GetLight LightEnable GetLightEnable SetRenderState GetRenderState
SetTexture SetTextureStageState GetTextureStageState SetSamplerState GetSamplerState ValidateDevice
SetScissorRect GetScissorRect SetSoftwareVertexProcessing GetSoftwareVertexProcessing
DrawPrimitiveUP SetFVF GetFVF SetVertexShader SetPixelShader SetStreamSource SetIndices
SetVertexShaderConstantF SetPixelShaderConstantF GetVertexShader GetPixelShader GetStreamSource GetIndices
GetTexture CreateTexture CreateVertexBuffer CreateIndexBuffer CreateRenderTarget CreateDepthStencilSurface
CreateOffscreenPlainSurface GetRenderTargetData StretchRect SetRenderTarget GetRenderTarget
SetDepthStencilSurface GetDepthStencilSurface GetBackBuffer DrawPrimitive DrawIndexedPrimitive
DrawIndexedPrimitiveUP CreateVertexShader CreatePixelShader GetSwapChain SetClipPlane GetClipPlane
CreateCubeTexture CreateVertexDeclaration SetVertexDeclaration GetVertexDeclaration CreateQuery
CreateStateBlock BeginStateBlock EndStateBlock UpdateSurface UpdateTexture ColorFill
GetVertexShaderConstantF GetPixelShaderConstantF SetVertexShaderConstantI GetVertexShaderConstantI
SetVertexShaderConstantB GetVertexShaderConstantB SetPixelShaderConstantI GetPixelShaderConstantI
SetPixelShaderConstantB GetPixelShaderConstantB GetRasterStatus SetGammaRamp GetGammaRamp""".split())


# traced methods: creation, locks, uploads, presents and the like; the
# per-frame state/draw calls stay direct (tracing them would be most of the
# frame's time and megabytes per second)
TRACE_RX = re.compile(r'^(Create|Lock|Unlock|Copy|Update|Present|Reset|Delete|TestCooperativeLevel|EvictManagedResources|ResourceManagerDiscardBytes|GetAvailableTextureMem|StretchRect|ColorFill|GetRenderTargetData|GetFrontBuffer|AddDirty|GetSurfaceLevel|GetCubeMapSurface|GetLevelDesc|GetVolumeLevel|QueryInterface|SetGammaRamp|ProcessVertices|GetDeviceCaps|CheckDevice|CheckDepthStencilMatch|EnumAdapterModes|GetAdapter|GetVertexShaderFunction|GetPixelShaderFunction|GetVertexShaderDeclaration|GetFunction|GetDeclaration|GenerateMipSubLevels|SetAutoGenFilterType|Capture|Apply|SetPriority|PreLoad|SetLOD)')
def traced(name):
    return TRACE_RX.match(name) is not None

def named_args(args):
    """(declaration list, names); mingw leaves some parameters unnamed"""
    decls, names = [], []
    for i, a in enumerate(args.split(',')):
        a = a.strip()
        if not a:
            continue
        words = [w for w in re.findall(r'\w+', a) if w not in ('const', 'CONST', 'struct')]
        if len(words) >= 2 and not a.endswith('*') and not a.endswith('&'):
            names.append(words[-1]); decls.append(a)
        else:
            names.append('a%d' % i); decls.append('%s a%d' % (a, i))
    return ', '.join(decls), names

def wrapper(iface, prefix, ret, name, args):
    """the vtable entry: a call-trace wrapper around prefix_name (D3DPT_TRACE, d3dpt_trace.log)"""
    label = '%s::%s' % (iface, name)
    decls, names = named_args(args)
    call = '%s_%s(This%s)' % (prefix, name, ''.join(', ' + n for n in names))
    proto = 'static %s WINAPI t_%s_%s(%s *This%s)' % (ret, prefix, name, iface, (', ' + decls) if decls else '')
    shown = names[:6]      # the first arguments as hex: sizes, formats, pools, pointers
    enter = 'if (d3dpt_trace_on) d3dpt_trace("> %s %%p%s"%s);' % (
        label, ''.join(' %s=%%08lx' % n for n in shown),
        ', (void *)This' + ''.join(', (unsigned long)(uintptr_t)%s' % n for n in shown))
    if ret == 'void':
        body = '%s %s; if (d3dpt_trace_on) d3dpt_trace("< %s");' % (enter, call, label)
    elif ret == 'float':
        body = '%s r_; %s r_ = %s; if (d3dpt_trace_on) d3dpt_trace("< %s = %%g", (double)r_); return r_;' % (ret, enter, call, label)
    else:
        body = '%s r_; %s r_ = %s; if (d3dpt_trace_on) d3dpt_trace("< %s = 0x%%08lx", (unsigned long)r_); return r_;' % (ret, enter, call, label)
    return '%s { %s }' % (proto, body)

src = open(sys.argv[1]).read()
rx = re.compile(r'STDMETHOD(?:_\(\s*([^,]+?)\s*,\s*(\w+)\s*\)|\((\w+)\))\s*\(\s*THIS(?:_\s*(.*?))?\s*\)\s*PURE;', re.S)

def methods(iface):
    m = re.search(r'DECLARE_INTERFACE_IID_\(%s,.*?\n\{(.*?)\n\};' % iface, src, re.S)
    out = []
    for r in rx.finditer(m.group(1)):
        ret = (r.group(1) or 'HRESULT').strip()
        name = r.group(2) or r.group(3)
        args = ' '.join((r.group(4) or '').split())
        out.append((ret, name, args))
    expect = {'IDirect3D9': 17, 'IDirect3DDevice9': 119, 'IDirect3DVertexBuffer9': 14, 'IDirect3DIndexBuffer9': 14,
              'IDirect3DTexture9': 22, 'IDirect3DSurface9': 17, 'IDirect3DVertexShader9': 5, 'IDirect3DPixelShader9': 5,
              'IDirect3DCubeTexture9': 22, 'IDirect3DVertexDeclaration9': 5, 'IDirect3DQuery9': 8, 'IDirect3DStateBlock9': 6}[iface]
    assert len(out) == expect, (iface, len(out))
    return out

def emit(iface, prefix, impl):
    ms = methods(iface)
    print('/* --- %s: %d methods --- */' % (iface, len(ms)))
    for ret, name, args in ms:
        proto = '%s WINAPI %s_%s(%s *This%s)' % (ret, prefix, name, iface, (', ' + args) if args else '')
        if name in impl:
            print('%s;' % proto)
        else:
            if ret == 'HRESULT': rv = 'E_NOTIMPL'
            elif ret == 'void': rv = None
            else: rv = '0'
            print('static %s { D3DPT_STUB("%s::%s"); %s }' % (proto, iface, name, ('return %s;' % rv) if rv else ''))
    for ret, name, args in ms:
        if traced(name): print(wrapper(iface, prefix, ret, name, args))
    print('static const %sVtbl %s_vtbl = {' % (iface, prefix))
    for ret, name, args in ms:
        print('    %s%s_%s,' % ('t_' if traced(name) else '', prefix, name))
    print('};')
    print()

print('/* generated by gen_vtbl.py from mingw d3d9.h; do not edit */')
print('#ifndef D3D9_VTBL_H\n#define D3D9_VTBL_H')
emit('IDirect3D9', 'd3d', IMPL_D3D)
emit('IDirect3DDevice9', 'dev', IMPL_DEV)
emit('IDirect3DVertexBuffer9', 'vb', IMPL_VB)
emit('IDirect3DIndexBuffer9', 'ib', IMPL_IB)
emit('IDirect3DTexture9', 'tex', IMPL_TEX)
emit('IDirect3DSurface9', 'surf', IMPL_SURF)
emit('IDirect3DVertexShader9', 'vs', IMPL_SH)
emit('IDirect3DPixelShader9', 'ps', IMPL_SH)
emit('IDirect3DCubeTexture9', 'cube', IMPL_CUBE)
emit('IDirect3DVertexDeclaration9', 'decl', IMPL_DECL)
emit('IDirect3DQuery9', 'query', IMPL_QUERY)
emit('IDirect3DStateBlock9', 'sb', IMPL_SB)
print('#endif')
