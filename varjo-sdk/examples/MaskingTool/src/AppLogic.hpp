// Copyright 2020-2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <memory>
#include <chrono>
#include <glm/glm.hpp>
#include <Varjo_types_layers.h>

#include "Globals.hpp"
#include "D3D11Renderer.hpp"
#include "D3D11MultiLayerView.hpp"
#include "Scene.hpp"
#include "MarkerTracker.hpp"

#include "AppState.hpp"
#include "GfxContext.hpp"
#include "MaskScene.hpp"

//! Application logic class
class AppLogic
{
public:
    //! Constructor
    AppLogic() = default;

    //! Destruictor
    ~AppLogic();

    // Disable copy and assign
    AppLogic(const AppLogic& other) = delete;
    AppLogic(const AppLogic&& other) = delete;
    AppLogic& operator=(const AppLogic& other) = delete;
    AppLogic& operator=(const AppLogic&& other) = delete;

    //! Initialize application
    bool init(VarjoExamples::GfxContext& context);

    //! Check for Varjo API events
    void checkEvents();

    //! Update application state
    void setState(const AppState& appState, bool force);

    //! Returns application state
    const AppState& getState() const { return m_appState; }

    //! Update application. Return true if frame submitted.
    bool update();

private:
    //! Toggle VST rendering
    void setVSTRendering(bool enabled);

    //! Toggle VST depth
    void setVSTDepthEstimation(bool enabled);

    //! Create varjo view with given resolution divider
    void createView(const VarjoExamples::D3D11MultiLayerView::D3D11Layer::Config& maskLayerConfig);

private:
    //! Handle mixed reality availablity
    void onMixedRealityAvailable(bool available, bool forceSetState);

private:
    varjo_Session* m_session{nullptr};                                        //!< Varjo session
    std::unique_ptr<VarjoExamples::D3D11Renderer> m_renderer;                 //!< Renderer instance
    std::unique_ptr<VarjoExamples::MultiLayerView> m_varjoView;               //!< Varjo layer ext view instance
    std::unique_ptr<MaskScene> m_maskScene;                                   //!< Application mask scene instance
    std::unique_ptr<VarjoExamples::MarkerTracker> m_markerTracker;            //!< Visual markers instance
    AppState m_appState{};                                                    //!< Application state
    std::vector<varjo_ViewExtensionBlendControlMask> m_blendControlViewExts;  //!< Blend control mask view extensions
    std::chrono::high_resolution_clock::time_point m_updateTime{};            //!< Last update time for frame sync
};
