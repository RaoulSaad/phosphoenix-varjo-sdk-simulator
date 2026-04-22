// Copyright 2020-2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <memory>
#include <glm/glm.hpp>
#include <imgui.h>

#include "Globals.hpp"
#include "UI.hpp"

#include "GfxContext.hpp"
#include "AppLogic.hpp"
#include "Presets.hpp"

//! Application view class
class AppView
{
public:
    // Input actions
    enum class Action {
        None = 0,
        Help,
        Reset,
        MaskingMode,
        VisualizationMode,
#ifdef USE_VIDEO_DEPTH_TEST
        DepthTestMode,
        DepthTestBehavior,
        DepthTestRange,
#endif
        ToggleTestPresets,
        ApplyPreset_0,
        ApplyPreset_1,
        ApplyPreset_2,
        ApplyPreset_3,
        ApplyPreset_4,
        ApplyPreset_5,
        ApplyPreset_6,
        ApplyPreset_7,
        ApplyPreset_8,
    };

    //! UI specific state
    struct UIState {
        int planeIndex{0};       //!< Currently active plane index
        int formatIndex{0};      //!< Currently active format index
        int resolutionIndex{0};  //!< Currently active resolution index
        int skipIndex{0};        //!< Currently active frame skip index
#ifdef USE_VIDEO_DEPTH_TEST
        int depthTestRangeIndex{0};  //!< Currently active depth test range index
#endif
        bool testPresets{false};    //!< Test presets active
        bool anyItemActive{false};  //!< True if any UI item active. Ignore keys then.
    };

public:
    //! Constructor
    AppView(AppLogic& logic);

    //! Destructor
    ~AppView();

    // Disable copy and assign
    AppView(const AppView& other) = delete;
    AppView(const AppView&& other) = delete;
    AppView& operator=(const AppView& other) = delete;
    AppView& operator=(const AppView&& other) = delete;

    //! Initialize application
    bool init();

    //! Application main loop
    void run();

private:
    //! UI frame callback
    bool onFrame(VarjoExamples::UI& ui);

    //! UI key press callback
    void onKeyPress(VarjoExamples::UI& ui, int keyCode);

    //! Updates UI based on logic state and writes changes back to it.
    void updateUI();

    //! Handle UI action. Return true if state changed and should be handled.
    bool onAction(Action actionType, AppState& appState);

    //! Resolve combo and list indices
    void resolveIndices(const AppState& appState);

private:
    AppLogic& m_logic;                                     //!< App logic instance
    std::unique_ptr<VarjoExamples::UI> m_ui;               //!< User interface wrapper
    std::unique_ptr<VarjoExamples::GfxContext> m_context;  //!< Graphics contexts
    UIState m_uiState{};                                   //!< UI specific states
    Presets m_presets;                                     //!< Presets

    // FPS statistics
    using FpsClock = std::chrono::high_resolution_clock;
    struct {
        FpsClock::time_point startTime{FpsClock::now()};
        int64_t frameCount{0};
        double frameRate{0.0};
    } m_fpsStats;
};
