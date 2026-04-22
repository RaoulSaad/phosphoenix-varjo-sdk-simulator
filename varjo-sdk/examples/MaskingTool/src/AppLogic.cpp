// Copyright 2020-2021 Varjo Technologies Oy. All rights reserved.

#include "AppLogic.hpp"

#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <set>
#include <thread>

#include <Varjo.h>
#include <Varjo_events.h>
#include <Varjo_mr.h>
#ifdef USE_EXPERIMENTAL_API
#include <Varjo_mr_experimental.h>
#endif
#include <Varjo_d3d11.h>
#include <json/json.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "D3D11MultiLayerView.hpp"

#include "MaskScene.hpp"
#include "Presets.hpp"

// VarjoExamples namespace contains simple example wrappers for using Varjo API features.
// These are only meant to be used in SDK example applications. In your own application,
// use your own production quality integration layer.
using namespace VarjoExamples;

namespace
{
// High priority to keep app on top
constexpr int32_t c_sessionPriorityTop = 9999;
}  // namespace

//---------------------------------------------------------------------------

AppLogic::~AppLogic()
{
    // Free marker tracker
    m_markerTracker.reset();

    // Free scene resources
    m_maskScene.reset();

    // Free view resources
    m_varjoView.reset();

    // Free renderer resources
    m_renderer.reset();

    // Shutdown the varjo session. Can't check errors anymore after this.
    LOG_DEBUG("Shutting down Varjo session..");
    varjo_SessionShutDown(m_session);
    m_session = nullptr;
}

bool AppLogic::init(VarjoExamples::GfxContext& context)
{
    // Initialize varjo utility app session.
    LOG_DEBUG("Initializing Varjo session..");
    m_session = varjo_SessionInit();
    if (CHECK_VARJO_ERR(m_session) != varjo_NoError) {
        LOG_ERROR("Creating Varjo session failed.");
        return false;
    }

    // Get graphics adapter used by Varjo session
    auto dxgiAdapter = D3D11MultiLayerView::getAdapter(m_session);

    // Init graphics
    context.init(dxgiAdapter.Get());

    // Create D3D11 renderer instance.
    m_renderer = std::make_unique<D3D11Renderer>(dxgiAdapter.Get());

    // Create mask scene instance
    m_maskScene = std::make_unique<MaskScene>(*m_renderer);

    // Check if Mixed Reality features are available.
    varjo_Bool mixedRealityAvailable = varjo_False;
    varjo_SyncProperties(m_session);
    CHECK_VARJO_ERR(m_session);
    if (varjo_HasProperty(m_session, varjo_PropertyKey_MRAvailable)) {
        mixedRealityAvailable = varjo_GetPropertyBool(m_session, varjo_PropertyKey_MRAvailable);
    }

    // Handle mixed reality availability
    onMixedRealityAvailable(mixedRealityAvailable == varjo_True, false);

    // Set session priority
    varjo_SessionSetPriority(m_session, c_sessionPriorityTop);

    return true;
}

void AppLogic::setVSTRendering(bool enabled)
{
    varjo_MRSetVideoRender(m_session, enabled ? varjo_True : varjo_False);
    if (CHECK_VARJO_ERR(m_session) == varjo_NoError) {
        LOG_INFO("VST rendering: %s", enabled ? "ON" : "OFF");
    }
    m_appState.state.options.vstRendering = enabled;
}

void AppLogic::setVSTDepthEstimation(bool enabled)
{
    varjo_MRSetVideoDepthEstimation(m_session, enabled ? varjo_True : varjo_False);
    if (CHECK_VARJO_ERR(m_session) == varjo_NoError) {
        LOG_INFO("VST depth estimation: %s", enabled ? "ON" : "OFF");
    }
    m_appState.general.vstDepthEstimation = enabled;
}

void AppLogic::setState(const AppState& appState, bool force)
{
    // Store previous state and set new one
    const auto prevState = m_appState;
    m_appState = appState;

    // Check for mixed reality availability
    if (!m_appState.general.mrAvailable) {
        // Toggle video rendering off
        if (m_appState.state.options.vstRendering) {
            setVSTRendering(false);
        }
        if (m_appState.general.vstDepthEstimation) {
            setVSTDepthEstimation(false);
        }
        return;
    }

    // Toggle video-see-through
    if (force || appState.state.options.vstRendering != prevState.state.options.vstRendering) {
        setVSTRendering(appState.state.options.vstRendering);
    }

    // Toggle video depth estimation
    m_appState.general.vstDepthEstimation = (appState.state.options.vrLayerSubmitDepth && appState.state.options.vrLayerDepthTestMask);
    if (force || m_appState.general.vstDepthEstimation != prevState.general.vstDepthEstimation) {
        setVSTDepthEstimation(m_appState.general.vstDepthEstimation);
    }

    // Masking mode changed
    if (force || appState.state.options.maskingMode != prevState.state.options.maskingMode) {
        switch (appState.state.options.maskingMode) {
            case AppState::MaskMode::None: LOG_INFO("Masking mode: None"); break;
            case AppState::MaskMode::Restricted: LOG_INFO("Masking mode: Restrict"); break;
            case AppState::MaskMode::Extended: LOG_INFO("Masking mode: Extend"); break;
            case AppState::MaskMode::Reduced: LOG_INFO("Masking mode: Reduced"); break;
            case AppState::MaskMode::DepthTestOrFail: LOG_INFO("Masking mode: DepthTestOrFail"); break;
            case AppState::MaskMode::DepthTestOrPass: LOG_INFO("Masking mode: DepthTestOrPass"); break;
        }
    }

    // Debug mode mode changed
    if (force || appState.general.debugMode != prevState.general.debugMode) {
        switch (appState.general.debugMode) {
            case AppState::DebugMode::None: LOG_INFO("Visualization mode: None"); break;
            case AppState::DebugMode::VisualizeMask: LOG_INFO("Visualization mode: Mask"); break;
            case AppState::DebugMode::VisualizeColors: LOG_INFO("Visualization mode: Plane Colors"); break;
        }
    }

    // Create view if resolution changed or we don't have one
    if (m_varjoView == nullptr || (appState.state.options.resDivider != prevState.state.options.resDivider) ||
        (appState.state.options.maskFormat != prevState.state.options.maskFormat)) {
        // Notice that we use double divider for focus here to reduce it's size even more
        const int contextDiv = m_appState.state.options.resDivider;
        const int focusDiv = 2 * m_appState.state.options.resDivider;

        createView({contextDiv, focusDiv, m_appState.state.options.maskFormat});
    }

    if (force || appState.state.options.vrViewOffset != prevState.state.options.vrViewOffset) {
        varjo_MRSetVRViewOffset(m_session, appState.state.options.vrViewOffset);
        CHECK_VARJO_ERR(m_session);
    }

#ifdef USE_VIDEO_DEPTH_TEST
    // Video depth test mode
    {
        const bool needLock = (force ||                                                                                              //
                               (appState.state.options.videoDepthTestMode != prevState.state.options.videoDepthTestMode) ||          //
                               (appState.state.options.videoDepthTestBehavior != prevState.state.options.videoDepthTestBehavior) ||  //
                               (appState.state.options.videoDepthTestRange != prevState.state.options.videoDepthTestRange));

        // Acquire lock
        if (needLock) {
            auto ret = varjo_Lock(m_session, varjo_LockType_VideoDepthTest);
            CHECK_VARJO_ERR(m_session);
            if (ret == varjo_False) {
                LOG_ERROR("Could not change video depth test settings.");
                return;
            }
        }

        // Video depth test mode
        bool modeChanged = false;
        if (force || appState.state.options.videoDepthTestMode != prevState.state.options.videoDepthTestMode ||
            appState.state.options.videoDepthTestBehavior != prevState.state.options.videoDepthTestBehavior) {
            if (appState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::Default) {
                varjo_MRResetVideoDepthTest(m_session);
            } else {
                const std::unordered_map<AppState::VideoDepthTestMode, varjo_VideoDepthTestMode> c_modeMappings = {
                    {AppState::VideoDepthTestMode::FullRange, varjo_VideoDepthTestMode_Full},
                    {AppState::VideoDepthTestMode::LimitedRange, varjo_VideoDepthTestMode_LimitedRange},
                    {AppState::VideoDepthTestMode::ForcedRange, varjo_VideoDepthTestMode_ForcedRange},
                };

                const std::unordered_map<AppState::VideoDepthTestBehavior, varjo_VideoDepthTestBehavior> c_behaviorMappings = {
                    {AppState::VideoDepthTestBehavior::PreferLayerRange, varjo_VideoDepthTestBehavior_PreferLayerRange},
                    {AppState::VideoDepthTestBehavior::PreferVideoRange, varjo_VideoDepthTestBehavior_PreferVideoRange},
                    {AppState::VideoDepthTestBehavior::CombineRanges, varjo_VideoDepthTestBehavior_CombineRanges},
                };

                if (c_modeMappings.count(appState.state.options.videoDepthTestMode) &&
                    c_behaviorMappings.count(appState.state.options.videoDepthTestBehavior)) {
                    varjo_MRSetVideoDepthTestMode(m_session, c_modeMappings.at(appState.state.options.videoDepthTestMode),
                        c_behaviorMappings.at(appState.state.options.videoDepthTestBehavior));
                } else {
                    LOG_ERROR("Unsupported video depth test mode: mode=%d, behavior=%d", appState.state.options.videoDepthTestMode,
                        appState.state.options.videoDepthTestBehavior);
                }
            }

            if (CHECK_VARJO_ERR(m_session) == varjo_NoError) {
                LOG_INFO(
                    "Video depth test mode: mode=%d, behavior=%d", appState.state.options.videoDepthTestMode, appState.state.options.videoDepthTestBehavior);
                modeChanged = true;
            }
        }

        // Video depth test range
        if ((appState.state.options.videoDepthTestMode != AppState::VideoDepthTestMode::Default) &&
            (force || (prevState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::Default) ||
                (appState.state.options.videoDepthTestRange != prevState.state.options.videoDepthTestRange))) {
            varjo_MRSetVideoDepthTestRange(m_session, appState.state.options.videoDepthTestRange[0], appState.state.options.videoDepthTestRange[1]);
            if (CHECK_VARJO_ERR(m_session) == varjo_NoError) {
                if (modeChanged) {
                    LOG_INFO("Video depth test range: [%f, %f]", appState.state.options.videoDepthTestRange[0], appState.state.options.videoDepthTestRange[1]);
                }
            }
        }

        // Release lock
        if (needLock) {
            varjo_Unlock(m_session, varjo_LockType_VideoDepthTest);
            CHECK_VARJO_ERR(m_session);
        }
    }
#endif
}

void AppLogic::createView(const VarjoExamples::D3D11MultiLayerView::D3D11Layer::Config& maskLayerConfig)
{
    assert(maskLayerConfig.format == varjo_TextureFormat_R8G8B8A8_SRGB || maskLayerConfig.format == varjo_MaskTextureFormat_A8_UNORM);

    LOG_INFO("Create view: format=%s, Resolution: ctx=1/%d, fcs=1/%d", (maskLayerConfig.format == varjo_TextureFormat_R8G8B8A8_SRGB) ? "RGBA" : "Alpha",
        maskLayerConfig.contextDivider, maskLayerConfig.focusDivider);

    // Destruct previous view
    m_varjoView.reset();

    // Create new view
    const std::vector<D3D11MultiLayerView::D3D11Layer::Config> configs = {
        {1, 1, varjo_TextureFormat_R8G8B8A8_SRGB},  // Always use full size RGBA for color layer
        maskLayerConfig,
    };

    m_varjoView = std::make_unique<D3D11MultiLayerView>(m_session, *m_renderer, configs);
}

bool AppLogic::update()
{
    // Check for new mixed reality events
    checkEvents();

    // We must have view here
    if (m_varjoView == nullptr) {
        return false;
    }

    if (m_appState.state.options.vrFrameSync) {
        // Sync frame
        m_varjoView->syncFrame();
    } else {
        constexpr int64_t desiredFps = 90;
        const auto frameDuration = std::chrono::nanoseconds(1000000000 / desiredFps);
        const auto sleepTime = frameDuration - (std::chrono::high_resolution_clock::now() - m_updateTime);
        std::this_thread::sleep_for(sleepTime);
        m_updateTime = std::chrono::high_resolution_clock::now();
    }

    // Early exit if no frame update enabled
    if (!m_appState.state.options.vrFrameSync || !m_appState.state.options.vrFrameUpdate) {
        // Invalidate frame by submitting empty frame on the first call, then no op
        m_varjoView->invalidateFrame();
        return false;
    }

    // Update frame time
    m_appState.general.frameTime += m_varjoView->getDeltaTime();
    m_appState.general.frameCount = m_varjoView->getFrameNumber();

    // Check if tracking enabled for any plane
    bool tracking = false;
    for (size_t i = 0; i < m_appState.state.maskPlanes.size(); i++) {
        auto& plane = m_appState.state.maskPlanes[i];
        tracking |= plane.tracking;
    }

    if (tracking) {
        // Create tracker instance if we need one
        if (!m_markerTracker) {
            LOG_INFO("Constructing marker tracker.");
            m_markerTracker = std::make_unique<MarkerTracker>(m_session);
        }

        // Clear markers
        m_markerTracker->reset();

        // Update visual markers
        m_markerTracker->update();

        // Handle visual markers
        const auto& markers = m_markerTracker->getObjects();
        if (!markers.empty()) {
            // LOG_INFO("Markers visible: %d", markers.size());

            // Set of available markers
            std::set<MarkerTracker::MarkerId> availableIds;
            for (const auto& it : markers) {
                availableIds.insert(it.second.id);
                // LOG_INFO("id=%d", it.second.id);
            }

            // Iterate planes to update
            for (size_t i = 0; i < m_appState.state.maskPlanes.size(); i++) {
                auto& plane = m_appState.state.maskPlanes[i];
                if (m_markerTracker->isValidId(plane.trackedId)) {
                    // Remove from set of available markers
                    availableIds.erase(plane.trackedId);

                    // If tracking, update position
                    if (plane.tracking) {
                        auto object = m_markerTracker->getObject(plane.trackedId);
                        if (object) {
                            // LOG_INFO("Update tracked pose: marker=%d, plane=%d", object->id, i);

                            // Just store tracked pose to be used in object transform. Manual controls are relative to this pose.
                            plane.trackedPose = object->pose;

                        } else {
                            // Ignore update, marker not currently visible
                        }

                        if (plane.resetMarkerPrediction) {
                            LOG_INFO("Reset marker prediction for plane-%d", i);
                            // Reset marker filtering by first enabling and then disabling it
                            m_markerTracker->setPrediction(true, {plane.trackedId});
                            m_markerTracker->setPrediction(false, {plane.trackedId});
                            plane.resetMarkerPrediction = false;
                        }
                    }
                } else {
                    if (plane.tracking && plane.trackedId <= 0 && !availableIds.empty()) {
                        auto assignedId = *availableIds.begin();
                        availableIds.erase(assignedId);
                        plane.trackedId = static_cast<int>(assignedId);
                        plane.resetMarkerPrediction = true;
                        plane.position = {0.0f, 0.0f, 0.0f};
                        plane.rotation = {0.0f, 0.0f, 0.0f};
                        LOG_INFO("Marker auto assigned to plane-%d: id=%d", i, assignedId);
                    }
                }
            }
        }
    } else {
        if (m_markerTracker) {
            LOG_INFO("Destructing marker tracker.");
            m_markerTracker.reset();
        }
    }

    // Update mask planes
    m_maskScene->updatePlanes(m_appState.state.maskPlanes);

    // Update scenes
    Scene::UpdateParams updateParams{};
    m_maskScene->update(m_varjoView->getFrameTime(), m_varjoView->getDeltaTime(), m_varjoView->getFrameNumber(), updateParams);

    // Skip frame submits
    if (m_appState.state.options.frameSkip > 0) {
        int mod = m_varjoView->getFrameNumber() % (m_appState.state.options.frameSkip + 1);
        if (mod != 0) {
            // Don't invalidate as we want to keep the previous frame
            return false;
        }
    }

    // Early exit if no frame submit
    if (!m_appState.state.options.vrFrameSubmit) {
        // Invalidate frame by submitting empty frame on the first call, then no op
        m_varjoView->invalidateFrame();
        return false;
    }

    // Begin varjo frame
    m_varjoView->beginFrame();

    // Render color layer
    if (m_appState.state.options.vrLayerSubmitColor) {
        // Get layer for rendering
        constexpr int c_layerIndex = 0;
        auto& layer = m_varjoView->getLayer(c_layerIndex);

        // Setup submit params
        MultiLayerView::Layer::SubmitParams submitParams{};
        submitParams.submitColor = true;
        submitParams.submitDepth = m_appState.state.options.vrLayerSubmitDepth;
        submitParams.depthTestEnabled = m_appState.state.options.vrLayerDepthTestMask;
        submitParams.depthTestRangeEnabled = false;
        submitParams.depthTestRangeLimits = {0.0f, 1.5f};
        submitParams.chromaKeyEnabled = false;
        submitParams.alphaBlend = true;

        // Begin layer rendering
        layer.begin(submitParams);

        if (m_appState.state.options.vrRenderMask) {
            // Clear color and depth
            const auto clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
            MultiLayerView::Layer::ClearParams clearParams(clearColor);
            clearParams.depthValue = 1.0f;

            // Clear buffer solid color or transparent
            layer.clear(clearParams);

            // Render mask scene to layer
            layer.renderScene(*m_maskScene);
        }

        // End layer rendering
        layer.end();
    }

    // Render mask layer
    if (m_appState.state.options.vrLayerSubmitMask) {
        // Setup submit params
        MultiLayerView::Layer::SubmitParams submitParams{};
        submitParams.submitColor = true;
        submitParams.submitDepth = m_appState.state.options.vrLayerSubmitDepth;
        submitParams.depthTestEnabled = m_appState.state.options.vrLayerDepthTestMask;
        submitParams.depthTestRangeEnabled = false;
        submitParams.depthTestRangeLimits = {0.0f, 1.5f};
        submitParams.chromaKeyEnabled = false;
        submitParams.alphaBlend = false;

        // Add view extensions
        m_blendControlViewExts.resize(m_varjoView->getViewCount());
        for (int i = 0; i < static_cast<int>(m_blendControlViewExts.size()); i++) {
            auto& viewExt = m_blendControlViewExts[i];
            viewExt.header.type = varjo_ViewExtensionBlendControlMaskType;
            viewExt.header.next = nullptr;
            viewExt.forceGlobalViewOffset = m_appState.state.options.forceGlobalViewOffset ? varjo_True : varjo_False;
            viewExt.maskingMode = static_cast<varjo_BlendControlMaskingMode>(m_appState.state.options.maskingMode);
            viewExt.debugMode = static_cast<varjo_BlendControlDebugMode>(m_appState.general.debugMode);

            // Add view extension
            submitParams.viewExtensions.push_back(&viewExt.header);
        }

        // Get layer for rendering
        constexpr int c_layerIndex = 1;
        auto& layer = m_varjoView->getLayer(c_layerIndex);

        // Begin layer rendering
        layer.begin(submitParams);

        if (m_appState.state.options.vrRenderMask) {
            // Clear color and depth
            const auto clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
            MultiLayerView::Layer::ClearParams clearParams(clearColor);
            clearParams.depthValue = 1.0f;

            // Clear buffer solid color or transparent
            layer.clear(clearParams);

            // Render mask scene to layer
            layer.renderScene(*m_maskScene);
        }

        // End layer rendering
        layer.end();
    }

    // Submit varjo frame with rendered layers
    m_varjoView->endFrame();

    // Return true for submitted frame
    return true;
}

void AppLogic::onMixedRealityAvailable(bool available, bool forceSetState)
{
    m_appState.general.mrAvailable = available;

    if (available) {
        // Update stuff here if needed
    } else {
        LOG_ERROR("Mixed Reality features not available.");
        // Update stuff here if needed
    }

    // Force set state when MR becomes active
    if (forceSetState) {
        setState(m_appState, true);
    }
}

void AppLogic::checkEvents()
{
    varjo_Bool ret = varjo_False;

    do {
        varjo_Event evt{};
        ret = varjo_PollEvent(m_session, &evt);
        CHECK_VARJO_ERR(m_session);

        if (ret == varjo_True) {
            switch (evt.header.type) {
                case varjo_EventType_MRDeviceStatus: {
                    switch (evt.data.mrDeviceStatus.status) {
                        // Occurs when Mixed Reality features are enabled
                        case varjo_MRDeviceStatus_Connected: {
                            LOG_INFO("EVENT: Mixed reality device status: %s", "Connected");
                            constexpr bool forceSetState = true;
                            onMixedRealityAvailable(true, forceSetState);
                        } break;
                        // Occurs when Mixed Reality features are disabled
                        case varjo_MRDeviceStatus_Disconnected: {
                            LOG_INFO("EVENT: Mixed reality device status: %s", "Disconnected");
                            constexpr bool forceSetState = false;
                            onMixedRealityAvailable(false, forceSetState);
                        } break;
                        default: {
                            // Ignore unknown status.
                        } break;
                    }
                } break;

                default: {
                    // Ignore unknown event.
                } break;
            }
        }
    } while (ret);
}
