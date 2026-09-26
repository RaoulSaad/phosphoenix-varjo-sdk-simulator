#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#include "wglext.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "window_display.h"

static WindowDisplay* g_instance = nullptr;   // one window per process

WindowDisplay::WindowDisplay(void* hwnd, int viewWidth, int viewHeight, float hfovDeg, int views)
    : m_hwnd(hwnd), m_viewW(viewWidth), m_viewH(viewHeight),
      m_views(views < 1 ? 1 : (views > NUM_EYES ? NUM_EYES : views))
{
    // Same FOV for both views; vertical from the aspect ratio.
    const float right = (float)std::tan(hfovDeg * 0.5 * 3.14159265358979 / 180.0);
    m_tangents.right = right;  m_tangents.left = -right;
    m_tangents.top = right * (float)viewHeight / (float)viewWidth;
    m_tangents.bottom = -m_tangents.top;
}

long long __stdcall WindowDisplay::wndProcThunk(void* hwndv, unsigned msg, unsigned long long wp, long long lp)
{
    HWND hwnd = (HWND)hwndv;
    WindowDisplay* self = g_instance;
    switch (msg) {
    case WM_CLOSE:
        if (self) self->m_closeRequested.store(true);
        return 0;                                    // main exits the loop; no DestroyWindow here
    case WM_MOUSEMOVE:
        if (self) { self->m_mouseX.store(GET_X_LPARAM(lp)); self->m_mouseY.store(GET_Y_LPARAM(lp)); }
        return 0;
    case WM_MOUSELEAVE:
        if (self) { self->m_mouseX.store(-1); self->m_mouseY.store(-1); }
        return 0;
    default:
        break;
    }
    return self && self->m_prevWndProc
        ? CallWindowProcA((WNDPROC)self->m_prevWndProc, hwnd, msg, (WPARAM)wp, (LPARAM)lp)
        : DefWindowProcA(hwnd, msg, (WPARAM)wp, (LPARAM)lp);
}

bool WindowDisplay::create()
{
    HWND hwnd = (HWND)m_hwnd;
    if (!hwnd) return false;
    g_instance = this;
    m_prevWndProc = (void*)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)wndProcThunk);

    SetWindowLongPtrA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
    RECT r{0, 0, m_viewW * m_views, m_viewH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(hwnd, nullptr, 100, 100, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_FRAMECHANGED);
    SetWindowTextA(hwnd, "Phosphoenix simulator (webcam) - mouse = gaze, 0-9 mask, ESC quit");
    ShowWindow(hwnd, SW_SHOW);
    m_hdc = (void*)GetDC(hwnd);

    auto swapInterval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    m_vsync = swapInterval && swapInterval(1);
    printf("[OK] window %dx%d, %d view%s (%s)\n", m_viewW * m_views, m_viewH, m_views, m_views == 1 ? "" : "s",
           m_vsync ? "vsync" : "60 Hz sleep");
    return true;
}

void WindowDisplay::destroy()
{
    HWND hwnd = (HWND)m_hwnd;
    if (hwnd && m_prevWndProc) SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)m_prevWndProc);
    m_prevWndProc = nullptr;
    if (hwnd && m_hdc) ReleaseDC(hwnd, (HDC)m_hdc);
    m_hdc = nullptr;
    g_instance = nullptr;
}

Viewport WindowDisplay::viewport(int view) const
{
    return Viewport{view * m_viewW, 0, m_viewW, m_viewH};
}

void WindowDisplay::pollEvents()
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void WindowDisplay::waitSync()
{
    if (m_vsync) return;                          // SwapBuffers paces us
    static auto next = std::chrono::steady_clock::now();
    next += std::chrono::microseconds(16667);
    std::this_thread::sleep_until(next);
}

void WindowDisplay::endFrameAndSubmit()
{
    if (m_hdc) SwapBuffers((HDC)m_hdc);
}

bool WindowDisplay::shouldQuit() const
{
    return m_closeRequested.load() || (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
}

bool WindowDisplay::gazeTan(float& tanX, float& tanY) const
{
    const int mx = m_mouseX.load(), my = m_mouseY.load();
    if (mx < 0 || my < 0 || my >= m_viewH || mx >= m_viewW * m_views) return false;
    // Which view is the cursor over? Both share tangents, so only the
    // horizontal offset within that view matters.
    const int localX = mx % m_viewW;
    const float u = (float)localX / (float)m_viewW;
    const float v = (float)my / (float)m_viewH;
    tanX = m_tangents.left + u * (m_tangents.right - m_tangents.left);
    tanY = m_tangents.top  + v * (m_tangents.bottom - m_tangents.top);   // window y down = tangent y down
    return true;
}