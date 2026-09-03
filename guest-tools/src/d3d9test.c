/*
 * d3d9test: the Direct3D 9 counterpart of wglgears for the guest-tools ISO.
 * Creates a HAL device on a window, prints the adapter identifier and a
 * few caps (so you can tell WineD3D-over-pass-through from the Microsoft
 * software path or a failure), then spins a colored triangle and reports
 * the frame rate in the title bar and on the console every second.
 * Also reports the x87 control word after CreateDevice: D3D sets 24-bit
 * precision unless D3DCREATE_FPU_PRESERVE, which is the case our QEMU
 * x87 inline mode 2 (doc 13) is for.
 *
 * Build (mingw-w64, msvcrt, Pentium III floor as the rest of the ISO):
 *   i686-w64-mingw32-gcc -O2 -o d3d9test.exe d3d9test.c -ld3d9 -lgdi32 -luser32
 * Run next to D3D9.DLL + WINED3D.DLL (+ OPENGL32.DLL) in the game folder.
 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <math.h>

#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
struct vtx { float x, y, z, rhw; DWORD color; };

static unsigned short x87_cw(void)
{
    unsigned short cw;
    __asm__ volatile ("fnstcw %0" : "=m" (cw));
    return cw;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    int W = 640, H = 480, frames = 0, total = 0;
    int limit = argc > 1 ? atoi(argv[1]) : 0;   /* frames to run, 0 = until closed */
    WNDCLASSA wc = { 0 };
    HWND hwnd;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DADAPTER_IDENTIFIER9 id;
    D3DCAPS9 caps;
    D3DPRESENT_PARAMETERS pp = { 0 };
    D3DDISPLAYMODE mode;
    HRESULT hr;
    DWORD t0, tlast;
    unsigned short cw_before = x87_cw(), cw_after;
    MSG msg;
    float ang = 0.0f;

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "d3d9test";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("d3d9test", "d3d9test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, wc.hInstance, NULL);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        printf("d3d9test: Direct3DCreate9 failed (no d3d9.dll?)\n");
        return 1;
    }
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id))) {
        printf("d3d9test: adapter \"%s\" driver \"%s\" version %lu.%lu.%lu.%lu vendor %04x device %04x\n",
               id.Description, id.Driver,
               (unsigned long)HIWORD(id.DriverVersion.HighPart), (unsigned long)LOWORD(id.DriverVersion.HighPart),
               (unsigned long)HIWORD(id.DriverVersion.LowPart), (unsigned long)LOWORD(id.DriverVersion.LowPart),
               (unsigned)id.VendorId, (unsigned)id.DeviceId);
    }
    if (SUCCEEDED(IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps))) {
        printf("d3d9test: HAL caps: vs %lu.%lu ps %lu.%lu, max texture %lux%lu, HW T&L %s\n",
               (unsigned long)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion),
               (unsigned long)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
               (unsigned long)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion),
               (unsigned long)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
               (unsigned long)caps.MaxTextureWidth, (unsigned long)caps.MaxTextureHeight,
               (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? "yes" : "no");
    } else {
        printf("d3d9test: no HAL device type (software/reference only)\n");
    }
    IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        printf("d3d9test: CreateDevice(HAL) failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    cw_after = x87_cw();
    printf("d3d9test: HAL device ok; x87 control word %04x -> %04x (PC=%s)\n",
           cw_before, cw_after,
           (cw_after & 0x300) == 0x000 ? "24" : (cw_after & 0x300) == 0x200 ? "53" : "64");
    fflush(stdout);

    t0 = tlast = GetTickCount();
    for (;;) {
        struct vtx v[3];
        int i;
        DWORD now;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                goto done;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        for (i = 0; i < 3; i++) {
            float a = ang + (float)i * 2.0943951f;
            v[i].x = 320.0f + 180.0f * cosf(a);
            v[i].y = 240.0f + 180.0f * sinf(a);
            v[i].z = 0.5f;
            v[i].rhw = 1.0f;
            v[i].color = i == 0 ? 0xffff0000 : i == 1 ? 0xff00ff00 : 0xff0000ff;
        }
        ang += 0.02f;
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff202040, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetFVF(dev, FVF);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(v[0]));
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        frames++;
        total++;
        now = GetTickCount();
        if (now - tlast >= 1000) {
            char title[64];
            snprintf(title, sizeof(title), "d3d9test: %d fps", frames);
            SetWindowTextA(hwnd, title);
            printf("%s\n", title);
            fflush(stdout);
            frames = 0;
            tlast = now;
        }
        if (limit && total >= limit) {
            break;
        }
    }
done:
    printf("d3d9test: %d frames in %lu ms\n", total, (unsigned long)(GetTickCount() - t0));
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
