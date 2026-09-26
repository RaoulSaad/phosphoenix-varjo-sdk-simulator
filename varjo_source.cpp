////////////////////////////////////////////////////////////////////////////////
// varjo_source.cpp — VarjoFrameSource implementation.
//
// Capture side: subscribe to Varjo's distorted-color camera DataStream (one
// shared subscription, both eye channels), copy each frame on the callback,
// and hand it to a per-eye saver thread that forwards it through the
// FrameCallback (the transport seam).
//
// Display side: GL swapchain atlas, per-view FOV tangents, and the
// begin/acquire/release/submit frame loop primitives used by main.
////////////////////////////////////////////////////////////////////////////////

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <GL/gl.h>

#include <Varjo.h>
#include <Varjo_gl.h>
#include <Varjo_mr.h>
#include <Varjo_datastream.h>
#include <Varjo_layers.h>
#include <Varjo_math.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "phase_timers.h"
#include "varjo_source.h"

// ---------------------------------------------------------------------------
// Atlas viewports
// ---------------------------------------------------------------------------

static std::vector<varjo_Viewport> calculateViewports(
    varjo_Session* session, varjo_TextureSize_Type type)
{
    // Varjo swapchains are rendered as an atlas: multiple eye/focus/context
    // views are packed into one large texture. This function asks Varjo for each
    // view's recommended texture size, then lays them out two per row.
    const int32_t viewCount = 4;
    std::vector<varjo_Viewport> viewports;
    viewports.reserve(viewCount);
    int x = 0, y = 0;
    for (int32_t i = 0; i < viewCount; i++) {
        int32_t width = 0, height = 0;
        varjo_GetTextureSize(session, type, i, &width, &height);
        const varjo_Viewport viewport = varjo_Viewport{x, y, width, height};
        viewports.push_back(viewport);
        x += viewport.width;
        if (i > 0 && viewports.size() % 2 == 0) {
            x = 0;
            y += viewport.height;
        }
    }
    return viewports;
}

static int32_t getTotalWidth(const std::vector<varjo_Viewport>& vp) {
    // Atlas width is the largest right edge among all packed viewports.
    int32_t m = 0;
    for (auto& v : vp) m = (std::max)(m, (int32_t)(v.x + v.width));
    return m;
}
static int32_t getTotalHeight(const std::vector<varjo_Viewport>& vp) {
    // Atlas height is the largest bottom edge among all packed viewports.
    int32_t m = 0;
    for (auto& v : vp) m = (std::max)(m, (int32_t)(v.y + v.height));
    return m;
}

// ---------------------------------------------------------------------------
// Eye camera capture pipeline
// ---------------------------------------------------------------------------
static void onEyeCameraFrame(
    const varjo_StreamFrame* frame,
    varjo_Session* session,
    void* userData)
{
    // Varjo invokes this from its DataStream thread whenever a distorted-color
    // frame is available. The one memcpy out of Varjo's locked buffer goes
    // straight into the sink's slot.
    if (!frame || !userData) return;
    if (frame->type != varjo_StreamType_DistortedColor) return;
    if (!(frame->dataFlags & varjo_DataFlag_Buffer)) return;

    EyeCameraCaptureSet* set = reinterpret_cast<EyeCameraCaptureSet*>(userData);

    for (int eye = 0; eye < NUM_EYES; ++eye) {
        EyeCameraCapture* capture = set->captures[eye];
        if (!capture || !capture->running.load() || !capture->sink) continue;

        const varjo_ChannelFlag wantFlag =
            (eye == 0) ? varjo_ChannelFlag_Left : varjo_ChannelFlag_Right;
        const varjo_ChannelIndex channel =
            (eye == 0) ? varjo_ChannelIndex_Left : varjo_ChannelIndex_Right;
        if (!(frame->channels & wantFlag)) continue;

        const varjo_BufferId bufferId =
            varjo_GetBufferId(session, frame->id, frame->frameNumber, channel);
        if (bufferId == varjo_InvalidId) continue;

        varjo_LockDataStreamBuffer(session, bufferId);
        varjo_BufferMetadata meta = varjo_GetBufferMetadata(session, bufferId);

        if (meta.type == varjo_BufferType_CPU && meta.format == varjo_TextureFormat_NV12) {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(
                varjo_GetBufferCPUData(session, bufferId));
            if (src && meta.byteSize > 0) {
                ScopedPhaseTimer timeCapture(TIMER_CAPTURE);
                uint32_t capacity = 0;
                uint8_t* dst = capture->sink->beginFrame(capture->eye, &capacity);
                if (dst && (uint32_t)meta.byteSize <= capacity) {
                    std::memcpy(dst, src, (size_t)meta.byteSize);

                    CameraFrame cf;
                    cf.eye = capture->eye;
                    cf.width = meta.width;
                    cf.height = meta.height;
                    cf.rowStride = meta.rowStride;
                    cf.frameNumber = frame->frameNumber;
                    cf.hasIntrinsics = false;
                    if (frame->dataFlags & varjo_DataFlag_Intrinsics) {
                        varjo_CameraIntrinsics2 intr = varjo_GetCameraIntrinsics2(session, frame->id, frame->frameNumber, channel);
                        cf.hasIntrinsics = true;
                        cf.intrinsicsModel = (int)intr.model;
                        cf.focalLengthX = intr.focalLengthX;
                        cf.focalLengthY = intr.focalLengthY;
                        cf.principalPointX = intr.principalPointX;
                        cf.principalPointY = intr.principalPointY;
                        for (int i = 0; i < 8; ++i) cf.distortionCoefficients[i] = intr.distortionCoefficients[i];
                    }
                    capture->sink->commitFrame(capture->eye, cf, (uint32_t)meta.byteSize);
                } else if (dst) {
                    static bool warned = false;
                    if (!warned) { warned = true; fprintf(stderr, "[CAM] frame %d bytes exceeds slot capacity %u; dropping\n", (int)meta.byteSize, capacity); }
                }
            }
        }
        varjo_UnlockDataStreamBuffer(session, bufferId);
    }
}

// Single-subscription start: one varjo_StartDataStream with both channel
// flags. Varjo rejects a second subscription to the same stream with
// "Requested stream already in use", so we must share the subscription.
static bool startEyeCameraCaptures(
    varjo_Session* session,
    EyeCameraCapture& leftCapture,
    EyeCameraCapture& rightCapture,
    EyeCameraCaptureSet& set,
    varjo_StreamId& outStreamId)
{
    // Find the best available CPU-readable NV12 distorted-color stream that has
    // both eye channels. CPU/NV12 is chosen because it can be copied directly to
    // Python.
    outStreamId = varjo_InvalidId;

    const int32_t count = varjo_GetDataStreamConfigCount(session);
    if (count <= 0) {
        fprintf(stderr, "[CAM] no data stream configs available\n");
        return false;
    }

    std::vector<varjo_StreamConfig> configs((size_t)count);
    varjo_GetDataStreamConfigs(session, configs.data(), count);

    const varjo_ChannelFlag wantFlags = varjo_ChannelFlag_Left | varjo_ChannelFlag_Right;

    const varjo_StreamConfig* best = nullptr;
    int64_t bestScore = -1;

    for (const auto& cfg : configs) {
        // Score prefers higher resolution, with frame rate as a small tie-break.
        if (cfg.streamType != varjo_StreamType_DistortedColor) continue;
        if (cfg.bufferType != varjo_BufferType_CPU) continue;
        if ((cfg.channelFlags & wantFlags) != wantFlags) continue;
        if (cfg.format != varjo_TextureFormat_NV12) continue;

        const int64_t score = (int64_t)cfg.width * (int64_t)cfg.height * 1000 + cfg.frameRate;
        if (score > bestScore) {
            best = &cfg;
            bestScore = score;
        }
    }

    if (!best) {
        fprintf(stderr, "[CAM] could not find a CPU/NV12 distorted-color stream with both channels\n");
        return false;
    }

    // Both eye captures share the chosen stream metadata.
    for (EyeCameraCapture* cap : {&leftCapture, &rightCapture}) {
        cap->streamId = best->streamId;
        cap->streamWidth = best->width;
        cap->streamHeight = best->height;
        cap->streamRowStride = best->rowStride;
        cap->running.store(true);
    }

    set.captures[0] = &leftCapture;
    set.captures[1] = &rightCapture;

    printf("[CAM] starting shared camera stream (L+R): %dx%d @ %d Hz (rowStride=%d)\n",
           best->width, best->height, best->frameRate, best->rowStride);

    varjo_StartDataStream(
        session,
        best->streamId,
        wantFlags,
        onEyeCameraFrame,
        &set);

    varjo_Error e = varjo_GetError(session);
    if (e != varjo_NoError) {
        fprintf(stderr, "[CAM] StartDataStream failed: %s\n", varjo_GetErrorDesc(e));
        for (EyeCameraCapture* cap : {&leftCapture, &rightCapture}) {
            cap->running.store(false);
            cap->streamId = varjo_InvalidId;
        }
        set.captures[0] = nullptr;
        set.captures[1] = nullptr;
        return false;
    }

    outStreamId = best->streamId;
    return true;
}

static void stopEyeCameraCaptures(
    varjo_Session* session,
    EyeCameraCapture& leftCapture,
    EyeCameraCapture& rightCapture,
    EyeCameraCaptureSet& set,
    varjo_StreamId streamId)
{
    // Stop the Varjo stream first so no new callbacks race with teardown.
    if (streamId != varjo_InvalidId) {
        varjo_StopDataStream(session, streamId);
    }

    set.captures[0] = nullptr;
    set.captures[1] = nullptr;

    for (EyeCameraCapture* cap : {&leftCapture, &rightCapture}) {
        cap->streamId = varjo_InvalidId;
        cap->running.store(false);
    }
}

// ---------------------------------------------------------------------------
// VarjoFrameSource — lifecycle
// ---------------------------------------------------------------------------

bool VarjoFrameSource::initSession()
{
    m_session = varjo_SessionInit();
    if (!m_session) { fprintf(stderr, "SessionInit failed\n"); return false; }
    printf("[OK] Varjo session\n");

    m_mrAvailable = false;
    if (varjo_HasProperty(m_session, varjo_PropertyKey_MRAvailable)) {
        m_mrAvailable = varjo_GetPropertyBool(m_session, varjo_PropertyKey_MRAvailable);
    }
    if (m_mrAvailable) {
        // This asks Varjo Base to composite the real camera video behind our
        // submitted transparent layer.
        varjo_MRSetVideoRender(m_session, varjo_True);
        varjo_Error e = varjo_GetError(m_session);
        if (e != varjo_NoError)
            printf("[WARN] MRSetVideoRender: %s\n", varjo_GetErrorDesc(e));
        else
            printf("[OK] Mixed Reality video pass-through enabled\n");
    } else {
        printf("[WARN] MR not available — will render VR-only (black + dot)\n");
    }

    varjo_GazeInit(m_session);
    {
        // Gaze is optional for session startup, but the overlay is gaze-driven,
        // so report any initialization issue clearly.
        varjo_Error e = varjo_GetError(m_session);
        if (e != varjo_NoError)
            printf("[WARN] GazeInit: %s\n", varjo_GetErrorDesc(e));
        else
            printf("[OK] Gaze initialised\n");
    }
    return true;
}

bool VarjoFrameSource::create()
{
    m_viewCount = varjo_GetViewCount(m_session);
    printf("[OK] viewCount=%d\n", m_viewCount);

    // Dynamic foveation can expose four views: left/right context and
    // left/right focus. All of them need overlay rendering.
    m_viewports = calculateViewports(m_session, varjo_TextureSize_Type_DynamicFoveation);
    m_atlasWidth  = getTotalWidth(m_viewports);
    m_atlasHeight = getTotalHeight(m_viewports);
    printf("[OK] Atlas %dx%d\n", m_atlasWidth, m_atlasHeight);

    // Triple buffering lets Varjo consume one image while the app renders into
    // another, reducing stalls.
    m_swapchainConfig.numberOfTextures = 3;
    m_swapchainConfig.textureWidth     = m_atlasWidth;
    m_swapchainConfig.textureHeight    = m_atlasHeight;
    m_swapchainConfig.textureFormat    = varjo_TextureFormat_R8G8B8A8_SRGB;
    m_swapchainConfig.textureArraySize = 1;

    m_swapchain = varjo_GLCreateSwapChain(m_session, &m_swapchainConfig);
    {
        varjo_Error e = varjo_GetError(m_session);
        if (e != varjo_NoError) {
            fprintf(stderr, "Swapchain: %s\n", varjo_GetErrorDesc(e));
            return false;
        }
    }
    printf("[OK] GL swap chain\n");

    m_views.resize(m_viewCount);
    for (int i = 0; i < m_viewCount; i++) {
        // Each Varjo view points at a sub-rectangle of the same swapchain atlas.
        // Projection and view matrices are filled every frame in
        // endFrameAndSubmit().
        memset(&m_views[i], 0, sizeof(m_views[i]));
        const varjo_Viewport& vp = m_viewports[i];

        m_views[i].viewport = varjo_SwapChainViewport{
            m_swapchain, vp.x, vp.y, vp.width, vp.height, 0, 0};
        m_views[i].extension = nullptr;
    }

    // The layer is transparent except where the shaders draw alpha. With MR
    // video pass-through enabled, transparent pixels reveal the real cameras.
    m_projLayer = varjo_LayerMultiProj{};
    m_projLayer.header.type  = varjo_LayerMultiProjType;
    m_projLayer.header.flags = varjo_LayerFlag_BlendMode_AlphaBlend;
    m_projLayer.space        = varjo_SpaceLocal;
    m_projLayer.viewCount    = m_viewCount;
    m_projLayer.views        = m_views.data();

    m_layerPtrs[0] = &m_projLayer.header;
    m_submitInfo = varjo_SubmitInfoLayers{};
    m_submitInfo.layerCount = 1;
    m_submitInfo.layers     = m_layerPtrs;

    m_frameTangents.resize(m_viewCount);

    m_frameInfo = varjo_CreateFrameInfo(m_session);
    return true;
}

bool VarjoFrameSource::shouldQuit() const
{
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

void VarjoFrameSource::shutdown()
{
    if (!m_session) return;
    if (m_mrAvailable) {
        varjo_MRSetVideoRender(m_session, varjo_False);
    }
    if (m_frameInfo) { varjo_FreeFrameInfo(m_frameInfo); m_frameInfo = nullptr; }
    if (m_swapchain) { varjo_FreeSwapChain(m_swapchain); m_swapchain = nullptr; }
    varjo_SessionShutDown(m_session);
    m_session = nullptr;
}

// ---------------------------------------------------------------------------
// VarjoFrameSource — IFrameSource (capture)
// ---------------------------------------------------------------------------

bool VarjoFrameSource::start(IFrameSink* sink)
{
    for (int e = 0; e < NUM_EYES; ++e) {
        m_captures[e].eye = e;
        m_captures[e].sink = sink;
    }
    return startEyeCameraCaptures(
        m_session, m_captures[0], m_captures[1], m_captureSet, m_cameraStreamId);
}

void VarjoFrameSource::stop()
{
    stopEyeCameraCaptures(
        m_session, m_captures[0], m_captures[1], m_captureSet, m_cameraStreamId);
}

GazeTan VarjoFrameSource::getGaze()
{
    // Start from the last-known combined gaze so an invalid sample keeps the
    // previous tangent values and the overlay does not jump.
    GazeTan out;
    out.x[0] = out.x[1] = m_gazeTanX;
    out.y[0] = out.y[1] = m_gazeTanY;

    varjo_Gaze gaze{};
    bool gazeValid = false;
    {
        // Grab the latest combined/per-eye gaze state.
        varjo_Gaze g = varjo_GetGaze(m_session);
        if (g.status == varjo_GazeStatus_Valid) { gaze = g; gazeValid = true; }
    }
    if (!gazeValid) return out;

    // Convert a 3D gaze ray into tangent space. This is roughly the same
    // coordinate system Varjo uses to describe each view frustum:
    // tanX = x / z, tanY = y / z.

    const float kMinGz = 0.35f;
    const float kMaxTan = 1.5f;

    auto safeTan = [&](double fx, double fy, double fz, float& outX, float& outY) -> bool {
        // Reject near-parallel or extreme rays; otherwise one bad gaze
        // sample could fling the overlay far outside the useful view.
        if (fabs(fz) < kMinGz) return false;
        const float tx = (float)(fx / fz);
        const float ty = (float)(fy / fz);
        if (fabs(tx) > kMaxTan || fabs(ty) > kMaxTan) return false;
        outX = tx; outY = ty;
        return true;
    };

    // fallback from combined gaze direction
    float cx = 0.0f, cy = 0.0f;
    if (safeTan(gaze.gaze.forward[0], gaze.gaze.forward[1], gaze.gaze.forward[2], cx, cy)) {
        m_gazeTanX = cx;
        m_gazeTanY = cy;
        out.x[0] = out.x[1] = cx;
        out.y[0] = out.y[1] = cy;
    }

    // better binocular fixation if focus distance is usable
    if (gaze.focusDistance > 0.05 && gaze.focusDistance < 2.0) {
        // If Varjo provides a usable fixation distance, compute the 3D
        // fixation point and then express that same point relative to
        // each eye. This improves binocular alignment.
        const double px = gaze.gaze.origin[0] + gaze.gaze.forward[0] * gaze.focusDistance;
        const double py = gaze.gaze.origin[1] + gaze.gaze.forward[1] * gaze.focusDistance;
        const double pz = gaze.gaze.origin[2] + gaze.gaze.forward[2] * gaze.focusDistance;

        auto pointToEyeTan = [&](const varjo_Ray& eyeRay, float& tx, float& ty) -> bool {
            const double dx = px - eyeRay.origin[0];
            const double dy = py - eyeRay.origin[1];
            const double dz = pz - eyeRay.origin[2];
            return safeTan(dx, dy, dz, tx, ty);
        };

        if (gaze.leftStatus == varjo_GazeEyeStatus_Tracked) {
            pointToEyeTan(gaze.leftEye, out.x[0], out.y[0]);
        }
        if (gaze.rightStatus == varjo_GazeEyeStatus_Tracked) {
            pointToEyeTan(gaze.rightEye, out.x[1], out.y[1]);
        }
    }

    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// VarjoFrameSource — XR display
// ---------------------------------------------------------------------------

Viewport VarjoFrameSource::viewport(int viewIndex) const
{
    const varjo_Viewport& vp = m_viewports[viewIndex];
    return Viewport{vp.x, vp.y, vp.width, vp.height};
}

ViewTangents VarjoFrameSource::tangents(int viewIndex) const
{
    const varjo_FovTangents& t = m_frameTangents[viewIndex];
    return ViewTangents{(float)t.left, (float)t.right, (float)t.top, (float)t.bottom};
}

int VarjoFrameSource::viewIndexToEye(int viewIndex) const
{
    if (m_viewCount == 2) {
        // [LEFT, RIGHT]
        return viewIndex;
    }
    if (m_viewCount == 4) {
        // [LEFT_CONTEXT, RIGHT_CONTEXT, LEFT_FOCUS, RIGHT_FOCUS]
        return (viewIndex == 0 || viewIndex == 2) ? 0 : 1;
    }
    return (viewIndex & 1);
}

std::vector<unsigned int> VarjoFrameSource::swapchainTextures() const
{
    // Varjo owns the swapchain images; convert each one to its OpenGL texture
    // handle so the renderer can attach them to FBOs.
    std::vector<unsigned int> textures((size_t)m_swapchainConfig.numberOfTextures);
    for (int i = 0; i < m_swapchainConfig.numberOfTextures; i++) {
        varjo_Texture vTex = varjo_GetSwapChainImage(m_swapchain, i);
        textures[i] = varjo_ToGLTexture(vTex);
    }
    return textures;
}

// ---------------------------------------------------------------------------
// VarjoFrameSource — frame loop primitives
// ---------------------------------------------------------------------------

void VarjoFrameSource::pollEvents()
{
    varjo_Event evt{};
    while (varjo_PollEvent(m_session, &evt)) {
        // MR camera status changes are useful diagnostics when pass-through
        // suddenly disappears.
        if (evt.header.type == varjo_EventType_MRDeviceStatus) {
            if (evt.data.mrDeviceStatus.status == varjo_MRDeviceStatus_Connected)
                printf("[EVT] MR cameras connected\n");
            else if (evt.data.mrDeviceStatus.status == varjo_MRDeviceStatus_Disconnected)
                printf("[EVT] MR cameras disconnected!\n");
        }
    }
}

void VarjoFrameSource::waitSync()
{
    varjo_WaitSync(m_session, m_frameInfo);
}

void VarjoFrameSource::beginFrame()
{
    varjo_BeginFrameWithLayers(m_session);

    // Snapshot this frame's FOV tangents once so the overlay uniforms and the
    // submitted projection matrices are built from the same values.
    for (int i = 0; i < m_viewCount; i++) {
        m_frameTangents[i] = varjo_GetFovTangents(m_session, i);
    }
}

int VarjoFrameSource::acquireSwapchainImage()
{
    int32_t sci = 0;
    varjo_AcquireSwapChainImage(m_swapchain, &sci);
    return (int)sci;
}

void VarjoFrameSource::releaseSwapchainImage()
{
    varjo_ReleaseSwapChainImage(m_swapchain);
}

void VarjoFrameSource::endFrameAndSubmit()
{
    for (int i = 0; i < m_viewCount; i++) {
        // The FOV tangents and frameInfo matrices are per-view, so every
        // atlas viewport gets its own projection/view metadata.
        varjo_FovTangents tangents = m_frameTangents[i];
        varjo_Matrix proj = varjo_GetProjectionMatrix(&tangents);
        varjo_UpdateNearFarPlanes(proj.value, varjo_ClipRangeZeroToOne, 0.01, 300.0);

        std::copy(proj.value, proj.value + 16, m_views[i].projection.value);
        std::copy(m_frameInfo->views[i].viewMatrix,
                  m_frameInfo->views[i].viewMatrix + 16,
                  m_views[i].view.value);
    }

    m_projLayer.header.flags = varjo_LayerFlag_BlendMode_AlphaBlend;

    // Submit this frame's transparent overlay layer to Varjo.
    m_submitInfo.frameNumber = m_frameInfo->frameNumber;
    varjo_EndFrameWithLayers(m_session, &m_submitInfo);
}
