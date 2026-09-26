////////////////////////////////////////////////////////////////////////////////
// renderer.h — OverlayRenderer: all OpenGL in one place.
//
// Owns the WGL context, the runtime-loaded GL function pointers, the two
// overlay shader programs, the per-eye phosphene textures and the swapchain
// FBOs. Deliberately free of GL/Windows types in this header (plain unsigned
// int for GL handles) so main stays header-light.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstdint>
#include <vector>

#include "pipeline_types.h"

class OverlayRenderer {
public:
    // WGL context + loadGLFunctions. Must run BEFORE the Varjo GL swapchain
    // is created (varjo_GLCreateSwapChain needs a current context).
    bool initGL();

    // Wrap the Varjo swapchain's GL textures in FBOs so normal GL rendering
    // can draw into them. Fails if any FBO is incomplete.
    bool setupSwapchainFbos(const std::vector<unsigned int>& swapchainTextures,
                            int atlasWidth, int atlasHeight);

    // Compile both overlay programs, look up uniforms, create the VAO.
    bool initShaders();

    // One GL_R8 texture per eye, initially sized to the transport's crop size.
    void initPhospheneTextures(const int cropWidth[NUM_EYES], const int cropHeight[NUM_EYES]);

    // GL half of the old uploadLatestPhospheneTexture: reallocate on size
    // change, then sub-image upload. Render thread only.
    void uploadPhosphene(int eye, const uint8_t* data, int width, int height);

    // Bind the acquired swapchain FBO, clear to transparent, set up blending.
    void beginFrame(int swapchainImageIndex);

    // Draw one atlas viewport: black scotoma pass, then the gaze-anchored
    // phosphene pass for that eye.
    void drawView(const Viewport& vp, const ViewTangents& t,
                  float gazeTanX, float gazeTanY, int eyeIdx,
                  BlindnessMode mode, const OverlayGeometry& geom);

    // Disable blending and unbind the FBO.
    void endFrame();

    // Opacity of the black blindness mask, 0..1. 1 = normal simulation (the
    // mask fully occludes the MR passthrough); lower values let the real world
    // show through underneath so phosphene alignment can be checked against it.
    // Does not affect phosphene brightness.
    void setMaskOpacity(float opacity);
    float maskOpacity() const { return m_maskOpacity; }

    // Phosphene layer brightness multiplier, 0..1. main drops it to 0 while
    // Python is not alive so a stale image is not shown as if it were live.
    void setPhospheneOpacity(float opacity);
    float phospheneOpacity() const { return m_phospheneOpacity; }

    // The hidden 1x1 Win32 window the GL context was created on, as an HWND.
    // WindowDisplay resizes and shows it in webcam mode.
    void* nativeWindow() const;

    // Passthrough stand-in for windowed mode: the latest camera frame drawn
    // as an opaque background before the overlay passes. NV12 in, converted
    // in the shader. Unused on the headset (Varjo composites the cameras).
    void initPassthroughTextures(int width, int height);
    void uploadPassthrough(const uint8_t* nv12, int width, int height, int rowStride);
    void drawPassthrough(const Viewport& vp);

    // Delete GL objects (programs, VAO, textures, FBOs). Call before the Varjo
    // swapchain/session are freed; the context stays alive for that.
    void shutdownGL();

    // Tear down the WGL context. Last step of program teardown.
    void destroyContext();

private:
    // Swapchain FBOs (one per swapchain image) + atlas size for beginFrame.
    std::vector<unsigned int> m_fbos;
    int m_atlasWidth = 0;
    int m_atlasHeight = 0;

    // Programs + VAO.
    unsigned int m_blackSpotProgram = 0;
    unsigned int m_phospheneProgram = 0;
    unsigned int m_vao = 0;

    // Per-eye phosphene texture and its current allocation size.
    unsigned int m_phospheneTexture[NUM_EYES] = {0, 0};
    int m_texWidth[NUM_EYES]  = {0, 0};
    int m_texHeight[NUM_EYES] = {0, 0};

    // Debug/alignment aid; 1.0 is the normal simulation.
    float m_maskOpacity = 1.0f;
    float m_phospheneOpacity = 1.0f;


    bool m_defaultFramebuffer = false;   // draw to FBO 0 (window) instead of swapchain FBOs

    unsigned int m_passthroughProgram = 0;
    unsigned int m_passthroughTexY  = 0;   // GL_R8, full res
    unsigned int m_passthroughTexUV = 0;   // GL_RG8, half res
    int m_passthroughWidth = 0, m_passthroughHeight = 0;
    int m_locPtTexY = -1, m_locPtTexUV = -1;

    // Uniform locations — black spot program.
    int m_locGazeTanX = -1, m_locGazeTanY = -1;
    int m_locViewLeft = -1, m_locViewRight = -1, m_locViewTop = -1, m_locViewBottom = -1;
    int m_locSpotRadiusTan = -1, m_locSoftEdgeTan = -1, m_locBlindnessMode = -1;
    int m_locMaskOpacity = -1;

    // Uniform locations — phosphene program.
    int m_locPGazeTanX = -1, m_locPGazeTanY = -1;
    int m_locPViewLeft = -1, m_locPViewRight = -1, m_locPViewTop = -1, m_locPViewBottom = -1;
    int m_locPPhospheneRadius = -1, m_locPPhospheneOp = -1, m_locPPhospheneTex = -1;
    int m_locPSpotRadiusTan = -1, m_locPSoftEdgeTan = -1, m_locPBlindnessMode = -1;
};
