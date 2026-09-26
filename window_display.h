////////////////////////////////////////////////////////////////////////////////
// window_display.h — WindowDisplay: a desktop window as the IDisplay.
//
// One view (left eye) or two side by side, drawn into the default
// framebuffer of the renderer's own WGL window, which is resized and shown.
// The transport stays stereo either way; a single view just skips drawing
// the duplicate right eye.
// Tangents come from a configured horizontal FOV; gaze comes from the mouse.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <vector>

#include "display.h"
#include "pipeline_types.h"

class WindowDisplay : public IDisplay {
public:
    WindowDisplay(void* hwnd, int viewWidth, int viewHeight, float hfovDeg, int views = 1);

    bool create() override;
    void destroy() override;
    int          viewCount() const override { return m_views; }
    Viewport     viewport(int view) const override;
    ViewTangents tangents(int) const override { return m_tangents; }
    int          viewIndexToEye(int view) const override { return view; }
    std::vector<unsigned int> swapchainTextures() const override { return {}; }
    int  atlasWidth() const override  { return m_viewW * m_views; }
    int  atlasHeight() const override { return m_viewH; }
    void pollEvents() override;
    void waitSync() override;
    void beginFrame() override {}
    int  acquireSwapchainImage() override { return -1; }
    void releaseSwapchainImage() override {}
    void endFrameAndSubmit() override;
    bool wantsPassthroughPass() const override { return true; }
    bool shouldQuit() const override;

    // Mouse position as a gaze tangent through the view it is over.
    // False when the cursor is outside the client area.
    bool gazeTan(float& tanX, float& tanY) const;

private:
    void*        m_hwnd;
    void*        m_hdc = nullptr;   // fetched once in create(), released in destroy()
    int          m_viewW, m_viewH;
    int          m_views;           // 1 or NUM_EYES
    ViewTangents m_tangents{};
    bool         m_vsync = false;
    std::atomic<bool> m_closeRequested{false};
    std::atomic<int>  m_mouseX{-1};
    std::atomic<int>  m_mouseY{-1};
    void*        m_prevWndProc = nullptr;

    static long long __stdcall wndProcThunk(void* hwnd, unsigned msg, unsigned long long wp, long long lp);
};