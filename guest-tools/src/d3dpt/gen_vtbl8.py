#!/usr/bin/env python3
"""Generate d3d8_vtbl.h for the paravirtual d3d8.dll from mingw's d3d8.h.
d3d8.h and d3d9.h cannot be included together (both define D3DFORMAT and
friends), so this emits the D3D8 interface/vtable structs itself, using the
D3D9 types where the layouts are identical and renamed D3D8-only types
(D3DPRESENT_PARAMETERS8, D3DSURFACE_DESC8, D3DVOLUME_DESC8, defined in
d3d8.c) elsewhere. Also: prototypes for implemented methods, E_NOTIMPL
stubs (log once) for the rest, a call-trace wrapper per method (the
vtable entry, see d3dpt_trace in d3d9.c) and the vtable instances.
Usage: gen_vtbl8.py /usr/i686-w64-mingw32/include/d3d8.h > d3d8_vtbl.h"""
import re, sys

RENAME = {'D3DPRESENT_PARAMETERS': 'D3DPRESENT_PARAMETERS8', 'D3DSURFACE_DESC': 'D3DSURFACE_DESC8',
          'D3DVOLUME_DESC': 'D3DVOLUME_DESC8', 'D3DVIEWPORT8': 'D3DVIEWPORT9', 'D3DMATERIAL8': 'D3DMATERIAL9',
          'D3DLIGHT8': 'D3DLIGHT9'}
IFACES = [  # (name, prefix, implemented methods or 'all' or 'none')
    ('IDirect3D8', 'd8', 'all'),
    ('IDirect3DDevice8', 'dev8', """QueryInterface AddRef Release TestCooperativeLevel GetAvailableTextureMem
        ResourceManagerDiscardBytes GetDirect3D GetDeviceCaps GetDisplayMode GetCreationParameters SetCursorProperties
        SetCursorPosition ShowCursor Reset Present GetBackBuffer CreateTexture CreateCubeTexture CreateVertexBuffer
        CreateIndexBuffer CreateRenderTarget CreateDepthStencilSurface CreateImageSurface CopyRects UpdateTexture
        SetRenderTarget GetRenderTarget GetDepthStencilSurface BeginScene EndScene Clear SetTransform GetTransform
        MultiplyTransform SetViewport GetViewport SetMaterial GetMaterial SetLight GetLight LightEnable GetLightEnable
        SetClipPlane GetClipPlane SetRenderState GetRenderState BeginStateBlock EndStateBlock ApplyStateBlock
        CaptureStateBlock DeleteStateBlock CreateStateBlock GetTexture SetTexture GetTextureStageState
        SetTextureStageState ValidateDevice GetInfo DrawPrimitive DrawIndexedPrimitive DrawPrimitiveUP
        DrawIndexedPrimitiveUP CreateVertexShader SetVertexShader GetVertexShader DeleteVertexShader
        SetVertexShaderConstant GetVertexShaderConstant GetVertexShaderDeclaration GetVertexShaderFunction
        SetStreamSource GetStreamSource SetIndices GetIndices CreatePixelShader SetPixelShader GetPixelShader
        DeletePixelShader SetPixelShaderConstant GetPixelShaderConstant GetPixelShaderFunction GetRasterStatus
        SetGammaRamp GetGammaRamp"""),
    ('IDirect3DSurface8', 'surf8', 'all'),
    ('IDirect3DTexture8', 'tex8', 'all'),
    ('IDirect3DCubeTexture8', 'cube8', 'all'),
    ('IDirect3DVertexBuffer8', 'vb8', 'all'),
    ('IDirect3DIndexBuffer8', 'ib8', 'all'),
    ('IDirect3DSwapChain8', 'swap8', 'none'),
    ('IDirect3DVolumeTexture8', 'vtex8', 'none'),
    ('IDirect3DVolume8', 'vol8', 'none'),
    ('IDirect3DBaseTexture8', 'btex8', None),   # type only
]

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

def fix(args):
    args = ' '.join(args.split())
    args = re.sub(r'\bstruct\s+', '', args)
    for a, b in RENAME.items():
        args = re.sub(r'\b%s\b' % a, b, args)
    return args

def methods(iface):
    m = re.search(r'DECLARE_INTERFACE_IID_\(%s,.*?\n\{(.*?)\n\};' % iface, src, re.S)
    return [((r.group(1) or 'HRESULT').strip(), r.group(2) or r.group(3), fix(r.group(4) or '')) for r in rx.finditer(m.group(1))]

print('/* generated by gen_vtbl8.py from mingw d3d8.h; do not edit */')
print('#ifndef D3D8_VTBL_H\n#define D3D8_VTBL_H')
for name, _, _ in IFACES:
    print('typedef struct %s %s;' % (name, name))
for name, prefix, impl in IFACES:
    ms = methods(name)
    print('typedef struct %sVtbl {' % name)
    for ret, mname, args in ms:
        print('    %s (WINAPI *%s)(%s *This%s);' % (ret, mname, name, (', ' + args) if args else ''))
    print('} %sVtbl;\nstruct %s { const %sVtbl *lpVtbl; };' % (name, name, name))
    if impl is None:
        continue
    implset = set(ms_[1] for ms_ in ms) if impl == 'all' else set() if impl == 'none' else set(impl.split())
    print('/* --- %s: %d methods --- */' % (name, len(ms)))
    for ret, mname, args in ms:
        proto = '%s WINAPI %s_%s(%s *This%s)' % (ret, prefix, mname, name, (', ' + args) if args else '')
        if mname in implset:
            print('%s;' % proto)
        else:
            rv = 'E_NOTIMPL' if ret == 'HRESULT' else None if ret == 'void' else '0'
            print('static %s { D3DPT_STUB("%s::%s"); %s }' % (proto, name, mname, ('return %s;' % rv) if rv else ''))
    for ret, mname, args in ms:
        if traced(mname): print(wrapper(name, prefix, ret, mname, args))
    print('static const %sVtbl %s_vtbl = {' % (name, prefix))
    for ret, mname, args in ms:
        print('    %s%s_%s,' % ('t_' if traced(mname) else '', prefix, mname))
    print('};\n')
print('#endif')
