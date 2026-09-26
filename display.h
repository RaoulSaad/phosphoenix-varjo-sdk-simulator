////////////////////////////////////////////////////////////////////////////////
// display.h — the display seam.
//
// Everything main needs from "the thing frames are drawn on": view layout,
// per-view FOV tangents, the frame loop primitives and how to submit. The
// Varjo headset and a desktop window both implement it, so main never names
// either.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <vector>

#include "pipeline_types.h"

class IDisplay {
    public:
        virtual ~IDisplay() = default;

        virtual bool create() = 0;      // swapchain or window; after the GL context extists
        virtual void destroy() = 0;     // before the GL context is destroyed

        virtual int          viewCount() const = 0;
        virtual Viewport     viewport(int view) const = 0;
        virtual ViewTangents tangents(int view) const = 0;   // valid after beginFrame()
        virtual int          viewIndexToEye(int view) const = 0;

        // GL texture handles to wrap in FBOs; empty means "draw to framebuffer 0".
        virtual std::vector<unsigned int> swapchainTextures() const = 0;
        virtual int atlasWidth() const = 0;
        virtual int atlasHeight() const = 0;

        // Frame loop, in this order.
        virtual void pollEvents() = 0;
        virtual void waitSync() = 0;
        virtual void beginFrame() = 0;
        virtual int  acquireSwapchainImage() = 0;   // index into swapchainTextures(), or -1
        virtual void releaseSwapchainImage() = 0;
        virtual void endFrameAndSubmit() = 0;

        virtual bool wantsPassthroughPass() const = 0;   // window: yes; headset composites itself
        virtual bool shouldQuit() const = 0;             // ESC, window close
};