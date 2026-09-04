/*
 * win32_sdl.h: the two dozen Win32 calls guest-tools/src/d3dgame.h makes,
 * over SDL2, so the reference scene (doc 14 P0a) compiles unmodified as a
 * native program against DXVK's d3d9 (the host executor, ADR-007) and its
 * -dump BMPs diff against the rig goldens (reference/d3d). Include before
 * the scene source; C++ only (DXVK's native COM headers have no C mode).
 *
 * HWND is an SDL_Window* (what DXVK's SDL2 WSI expects). Keys, the
 * close button and the window title go through SDL; everything else is
 * the minimum that keeps the scene's logic and log format intact.
 */
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <SDL.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

typedef intptr_t LRESULT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef HANDLE HCURSOR;
typedef HANDLE HICON;
typedef HANDLE HBRUSH;
typedef HANDLE HMENU;
#define CALLBACK
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct tagWNDCLASSA {
  UINT style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra; HINSTANCE hInstance;
  HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName; LPCSTR lpszClassName;
} WNDCLASSA;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct _OSVERSIONINFOA { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; char szCSDVersion[128]; } OSVERSIONINFOA;

enum { WM_DESTROY = 0x0002, WM_QUIT = 0x0012, WM_SETCURSOR = 0x0020, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101 };
enum { VK_SPACE = 0x20, VK_ESCAPE = 0x1B, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_F1 = 0x70 };
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_POPUP            0x80000000L
#define WS_THICKFRAME       0x00040000L
#define WS_MAXIMIZEBOX      0x00010000L
#define CW_USEDEFAULT       ((int)0x80000000)
#define SW_SHOW 5
#define PM_REMOVE 1
#define IDC_ARROW 32512
#define GWLP_USERDATA (-21)

namespace win32sdl {
  static WNDPROC g_wndproc;
  static LONG_PTR g_userdata;
  static bool g_visible = false;     /* set WIN32SDL_SHOW=1 to see the window; default hidden (off-screen runs) */
  static bool g_keys[256];

  static int vk_from_sdl(SDL_Keycode k) {
    switch (k) {
      case SDLK_ESCAPE: return VK_ESCAPE; case SDLK_SPACE: return VK_SPACE; case SDLK_F1: return VK_F1;
      case SDLK_LEFT: return VK_LEFT; case SDLK_RIGHT: return VK_RIGHT; case SDLK_UP: return VK_UP; case SDLK_DOWN: return VK_DOWN;
      default: if (k >= 'a' && k <= 'z') return k - 'a' + 'A'; if (k >= '0' && k <= '9') return k; return 0;
    }
  }
}

inline LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline LONG_PTR GetWindowLongPtrA(HWND, int) { return win32sdl::g_userdata; }
inline LONG_PTR SetWindowLongPtrA(HWND, int, LONG_PTR v) { LONG_PTR o = win32sdl::g_userdata; win32sdl::g_userdata = v; return o; }
inline HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
inline HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return nullptr; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
inline BOOL AdjustWindowRect(RECT *, DWORD, BOOL) { return TRUE; }
inline BOOL ShowWindow(HWND, int) { return TRUE; }
inline BOOL SetForegroundWindow(HWND) { return TRUE; }
inline BOOL TranslateMessage(const MSG *) { return FALSE; }
inline LRESULT DispatchMessageA(const MSG *m) { return win32sdl::g_wndproc ? win32sdl::g_wndproc(m->hwnd, m->message, m->wParam, m->lParam) : 0; }
inline WORD RegisterClassA(const WNDCLASSA *wc) { win32sdl::g_wndproc = wc->lpfnWndProc; return 1; }
inline BOOL SetWindowTextA(HWND h, LPCSTR t) { SDL_SetWindowTitle((SDL_Window *)h, t); return TRUE; }
inline DWORD GetTickCount(void) { return SDL_GetTicks(); }
inline void Sleep(DWORD ms) { SDL_Delay(ms); }
inline short GetAsyncKeyState(int vk) { return (vk >= 0 && vk < 256 && win32sdl::g_keys[vk]) ? (short)0x8000 : 0; }
inline HMODULE LoadLibraryA(LPCSTR n) { return dlopen(n, RTLD_NOW); }
inline void *GetProcAddress(HMODULE m, LPCSTR n) { return m ? dlsym(m, n) : nullptr; }

inline HWND CreateWindowA(LPCSTR, LPCSTR name, DWORD style, int, int, int w, int h, HWND, HMENU, HINSTANCE, void *) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return nullptr; }
  const char *e = getenv("WIN32SDL_SHOW");
  win32sdl::g_visible = e && *e && *e != '0';
  Uint32 flags = SDL_WINDOW_VULKAN | (win32sdl::g_visible ? 0 : SDL_WINDOW_HIDDEN) | ((style & WS_POPUP) ? SDL_WINDOW_BORDERLESS : 0);
  return (HWND)SDL_CreateWindow(name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
}

inline BOOL PeekMessageA(MSG *m, HWND, UINT, UINT, UINT) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    memset(m, 0, sizeof(*m));
    switch (ev.type) {
      case SDL_QUIT: m->message = WM_QUIT; return TRUE;
      case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_CLOSE) { m->message = WM_DESTROY; m->hwnd = (HWND)SDL_GetWindowFromID(ev.window.windowID); return TRUE; }
        break;
      case SDL_KEYDOWN: case SDL_KEYUP: {
        int vk = win32sdl::vk_from_sdl(ev.key.keysym.sym);
        if (!vk) break;
        win32sdl::g_keys[vk] = ev.type == SDL_KEYDOWN;
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat) { m->message = WM_KEYDOWN; m->wParam = vk; m->hwnd = (HWND)SDL_GetWindowFromID(ev.key.windowID); return TRUE; }
        break;
      }
    }
  }
  return FALSE;
}

inline void GetLocalTime(SYSTEMTIME *st) {
  time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
  st->wYear = lt.tm_year + 1900; st->wMonth = lt.tm_mon + 1; st->wDay = lt.tm_mday; st->wDayOfWeek = lt.tm_wday;
  st->wHour = lt.tm_hour; st->wMinute = lt.tm_min; st->wSecond = lt.tm_sec; st->wMilliseconds = 0;
}
inline BOOL GetVersionExA(OSVERSIONINFOA *v) {
  v->dwMajorVersion = 0; v->dwMinorVersion = 0; v->dwBuildNumber = 0;
  snprintf(v->szCSDVersion, sizeof(v->szCSDVersion), "(native, DXVK d3d9 on %s)", SDL_GetPlatform());
  return TRUE;
}
inline DWORD GetModuleFileNameA(HMODULE, char *out, DWORD n) {
  /* the scene only splits on '\\' so a POSIX path yields "" and bare
   * log/dump names land in the current directory — intended */
#ifdef __APPLE__
  uint32_t sz = n; if (_NSGetExecutablePath(out, &sz) != 0) out[0] = 0;
#else
  ssize_t k = readlink("/proc/self/exe", out, n - 1); out[k > 0 ? k : 0] = 0;
#endif
  return (DWORD)strlen(out);
}

/* the scene's ID3DXBuffer stand-in names the C vtable type; only a pointer to it is formed */
struct IUnknownVtbl;

/* DXVK throws when no Vulkan device passes its checks; the scene expects NULL */
extern "C" IDirect3D9 *Direct3DCreate9(UINT);
inline IDirect3D9 *win32sdl_Direct3DCreate9(UINT v) { try { return Direct3DCreate9(v); } catch (...) { return nullptr; } }
#define Direct3DCreate9 win32sdl_Direct3DCreate9
