////////////////////////////////////////////////////////////////////////////////
// varjo_source.h — VarjoFrameSource: the Varjo headset as an IFrameSource.
//
// Owns the Varjo session, the distorted-color camera DataStream (both eyes),
// gaze, and the XR display side (GL swapchain atlas + frame loop + layer
// submission). Only the capture half is behind IFrameSource; the display
// methods are Varjo-only and used directly by main (a future WebcamFrameSource
// implements just the interface).
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <Varjo_types.h>
#include <Varjo_types_layers.h>
#include <Varjo_types_datastream.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "frame_source.h"
#include "pipeline_types.h"

struct EyeCameraCapture {
    // One capture state per eye. The Varjo callback writes "latest"; the saver
    // thread waits on cv, copies the frame, and forwards it via onFrame.
    std::mutex mutex;
    std::condition_variable cv;
    CameraFrame latest;
    bool hasNewFrame = false;
    std::atomic<bool> running{false};
    std::thread saverThread;
    varjo_StreamId streamId = varjo_InvalidId;
    int streamWidth = 0;
    int streamHeight = 0;
    int streamRowStride = 0;
    int eye = 0;              // 0 = left, 1 = right
    FrameCallback onFrame;    // delivers frames to the transport
};

// Wraps the 2-eye capture array so a single stream subscription can fan
// out to both eyes. Varjo only allows one subscriber per streamId, and the
// DistortedColor stream delivers both channels in one callback.
struct EyeCameraCaptureSet {
    EyeCameraCapture* captures[NUM_EYES] = {nullptr, nullptr};
};

class VarjoFrameSource : public IFrameSource {
public:
    // --- lifecycle (sequence: renderer.initGL() -> initSession() -> start()
    //     -> createSwapchain(); the GL context must exist before the swapchain).
    bool initSession();       // session + MR video pass-through + gaze init
    bool createSwapchain();   // viewports, GL swapchain, layer/submit structs
    void shutdown();          // MR off, frameInfo, swapchain, session

    // --- IFrameSource (the capture seam)
    bool start(FrameCallback onFrame) override;
    void stop() override;
    GazeTan getGaze() override;
    int frameWidth(int eye) const override  { return m_captures[eye].streamWidth; }
    int frameHeight(int eye) const override { return m_captures[eye].streamHeight; }
    int rowStride(int eye) const override   { return m_captures[eye].streamRowStride; }

    // --- XR display (Varjo-only, not on the interface)
    int viewCount() const { return m_viewCount; }
    Viewport viewport(int viewIndex) const;
    ViewTangents tangents(int viewIndex) const;   // this frame's FOV tangents
    int viewIndexToEye(int viewIndex) const;
    std::vector<unsigned int> swapchainTextures() const;   // GL handles for FBOs
    int atlasWidth() const  { return m_atlasWidth; }
    int atlasHeight() const { return m_atlasHeight; }

    // --- frame loop primitives (called in this order each frame)
    void pollEvents();               // MR device status prints
    void waitSync();
    void beginFrame();               // varjo_BeginFrameWithLayers + tangent snapshot
    int  acquireSwapchainImage();
    void releaseSwapchainImage();
    void endFrameAndSubmit();        // fill per-view proj/view matrices, submit layer

private:
    varjo_Session* m_session = nullptr;
    bool m_mrAvailable = false;

    // Camera capture (shared DataStream subscription, one saver thread per eye).
    EyeCameraCapture m_captures[NUM_EYES];
    EyeCameraCaptureSet m_captureSet;
    varjo_StreamId m_cameraStreamId = varjo_InvalidId;

    // Display / layer submission.
    varjo_SwapChain* m_swapchain = nullptr;
    varjo_SwapChainConfig2 m_swapchainConfig{};
    std::vector<varjo_Viewport> m_viewports;
    int m_viewCount = 0;
    int m_atlasWidth = 0;
    int m_atlasHeight = 0;
    varjo_FrameInfo* m_frameInfo = nullptr;
    std::vector<varjo_LayerMultiProjView> m_views;
    varjo_LayerMultiProj m_projLayer{};
    varjo_LayerHeader* m_layerPtrs[1] = {nullptr};
    varjo_SubmitInfoLayers m_submitInfo{};

    // Per-frame FOV tangents, snapshotted once in beginFrame() so the shader
    // uniforms (via tangents()) and the submitted projection matrices (via
    // endFrameAndSubmit()) are guaranteed to use identical values.
    std::vector<varjo_FovTangents> m_frameTangents;

    // Last-known combined gaze in tangent space. Persistent across frames so
    // an invalid gaze sample keeps the previous values and the overlay does
    // not jump (matches the monolithic render loop's behavior).
    float m_gazeTanX = 0.0f;
    float m_gazeTanY = 0.0f;
};
