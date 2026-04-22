// Copyright 2020-2021 Varjo Technologies Oy. All rights reserved.

#include "AppView.hpp"

#include <unordered_map>
#include <tchar.h>
#include <iostream>
#include <fstream>
#include <imgui_internal.h>
#include <map>
#include <chrono>

// Application title text
#define APP_TITLE_TEXT "Varjo Masking Tool"
#define APP_COPYRIGHT_TEXT "(C) 2021 Varjo Technologies"

// Enable debug frame timing
#define DEBUG_FRAME_TIMING 0

// VarjoExamples namespace contains simple example wrappers for using Varjo API features.
// These are only meant to be used in SDK example applications. In your own application,
// use your own production quality integration layer.
using namespace VarjoExamples;

namespace
{
// Presets file
static const std::string c_presetsFilename = "maskingtool-presets.json";
static const std::string c_testPresetsFilename = "maskingtool-testpresets.json";

// Saved state file
static const std::string c_configStorageFilename = "maskingtool-saved.json";

//! Action info structure
struct ActionInfo {
    std::string name;  //!< Action name
    int keyCode;       //!< Shortcut keycode
    std::string help;  //!< Help string
};

// Key shortcut mapping
std::unordered_map<int, AppView::Action> c_keyMappings;

// clang-format off
// Action names
const std::map<AppView::Action, ActionInfo> c_actions = {
    {AppView::Action::None,                  {"None",                0,      "--   (no action)"}},
    {AppView::Action::Help,                  {"Help",                VK_F1,  "F1   Print help"}},
    {AppView::Action::Reset,                 {"Reset",               'R',    "R    Reset settings"}},
    {AppView::Action::MaskingMode,           {"Masking mode",        'M',    "M    Change masking mode"}},
    {AppView::Action::VisualizationMode,     {"Visualization mode",  'V',    "V    Change visualization mode"}},
    {AppView::Action::ToggleTestPresets,     {"Toggle test presets", 'T',    "T    Toggle test presets"}},
#ifdef USE_VIDEO_DEPTH_TEST
    {AppView::Action::DepthTestMode,         {"DepthTestMode",       'D',    "D    Toggle global depth test mode: Default, Full, Limited, Forced"}},
    {AppView::Action::DepthTestBehavior,     {"DepthTestBehavior",   'B',    "B    Toggle global depth test behavior: Prefer Layer, Prefer Video, Combine"}},
    {AppView::Action::DepthTestRange,        {"DepthTestRange",      'Z',    "Z    Toggle global depth test range: 3.0m, 1.5m, 0.5m, 0.0m"}},
#endif
    {AppView::Action::ApplyPreset_0,         {"Apply Preset 1",      '1',    "1    Apply preset 1"}},
    {AppView::Action::ApplyPreset_1,         {"Apply Preset 2",      '2',    "2    Apply preset 2"}},
    {AppView::Action::ApplyPreset_2,         {"Apply Preset 3",      '3',    "3    Apply preset 3"}},
    {AppView::Action::ApplyPreset_3,         {"Apply Preset 4",      '4',    "4    Apply preset 4"}},
    {AppView::Action::ApplyPreset_4,         {"Apply Preset 5",      '5',    "5    Apply preset 5"}},
    {AppView::Action::ApplyPreset_5,         {"Apply Preset 6",      '6',    "6    Apply preset 6"}},
    {AppView::Action::ApplyPreset_6,         {"Apply Preset 7",      '7',    "7    Apply preset 7"}},
    {AppView::Action::ApplyPreset_7,         {"Apply Preset 8",      '8',    "8    Apply preset 8"}},
    {AppView::Action::ApplyPreset_8,         {"Apply Preset 9",      '9',    "9    Apply preset 9"}},
};
// clang-format on

// Window client area margin
constexpr int c_windowMargin = 8;

// Window client area size and log height
constexpr glm::ivec2 c_windowClientSize(720, 992);
#ifdef USE_VIDEO_DEPTH_TEST
constexpr int c_logHeight = 320;
#else
constexpr int c_logHeight = 395;
#endif

static const std::vector<char*> c_maskingModeNames = {"None", "Restricted", "Extended", "Reduced", "DepthOrFail", "DepthOrPass"};

static const std::vector<char*> c_resolutionNames = {"Full", "1/2", "1/4", "1/8", "1/16"};
static const std::vector<int> c_resolutionValues = {1, 2, 4, 8, 16};

static const std::vector<char*> c_formatNames = {"Alpha", "RGBA"};
static const std::vector<varjo_TextureFormat> c_formatValues = {varjo_MaskTextureFormat_A8_UNORM, varjo_TextureFormat_R8G8B8A8_SRGB};

static const std::vector<char*> c_skipNames = {"None", "1", "2", "3"};
static const std::vector<int> c_skipValues = {0, 1, 2, 3};

static const std::vector<char*> c_videoDepthTestModeNames = {"Default", "Full Range", "Limited Range", "Force Test"};
static const std::vector<char*> c_videoDepthTestBehaviorNames = {"Prefer Layer", "Prefer Video", "Combined"};
static const std::vector<float> c_videoDepthTestRangeValues = {3.0f, 1.5f, 0.5f, 0.0f};

void applyPreset(const Presets::Preset& preset, AppState::State& state, bool keepPlanes)
{
    LOG_INFO("Apply preset: %s\n(%s)", preset.name.c_str(), preset.desc.c_str());
    auto prevState = state;
    state = preset.state;

    if (keepPlanes) {
        state.maskPlanes = prevState.maskPlanes;
    }
}

}  // namespace

AppView::AppView(AppLogic& logic)
    : m_logic(logic)
{
    // Fill in key mappings
    for (auto& ai : c_actions) {
        c_keyMappings[ai.second.keyCode] = ai.first;
    }

    // Present UI with vsync OFF (We sync to Varjo API instead).
    constexpr bool c_vsync = false;

    // Create user interface instance
    m_ui = std::make_unique<UI>(std::bind(&AppView::onFrame, this, std::placeholders::_1),    //
        std::bind(&AppView::onKeyPress, this, std::placeholders::_1, std::placeholders::_2),  //
        _T(APP_TITLE_TEXT), c_windowClientSize.x, c_windowClientSize.y, c_vsync);

    // Set log function
    LOG_INIT(std::bind(&UI::writeLogEntry, m_ui.get(), std::placeholders::_1, std::placeholders::_2), LogLevel::Info);

    LOG_INFO(APP_TITLE_TEXT);
    LOG_INFO(APP_COPYRIGHT_TEXT);
    LOG_INFO("-------------------------------");

    // Create contexts
    m_context = std::make_unique<VarjoExamples::GfxContext>(m_ui->getWindowHandle());

    // Additional ImgUi setup
    auto& io = ImGui::GetIO();

    // Disable storing UI ini file
    io.IniFilename = NULL;
}

AppView::~AppView()
{
    // Deinit logger
    LOG_DEINIT();

    // Free UI
    m_ui.reset();
}

bool AppView::init()
{
    if (!m_logic.init(*m_context)) {
        LOG_ERROR("Initializing application failed.");
        return false;
    }

    // Load presets
    if (!m_presets.loadPresets(c_presetsFilename)) {
        LOG_ERROR("Loading presets failed: %s", c_presetsFilename.c_str());
        m_presets.reset();
    }

    // Reset states
    m_uiState = {};
    AppState appState = m_logic.getState();
    appState.state = m_presets.getResetState();

    bool initStateSet = false;

    // If config file found, load it
    std::ifstream file(c_configStorageFilename);
    if (file.good()) {
        if (!Presets::loadState(c_configStorageFilename, appState.state)) {
            LOG_ERROR("Loading initial config failed.");
        } else {
            initStateSet = true;
        }
    }

    // If not loaded, use default preset
    if (!initStateSet && !m_presets.getDefaultId().empty()) {
        const auto preset = m_presets.getPreset(m_presets.getDefaultId());
        if (preset) {
            applyPreset(*preset, appState.state, false);
        }
    }

    // Resolve UI indices
    resolveIndices(appState);

    // Force set initial state
    m_logic.setState(appState, true);

    return true;
}

void AppView::resolveIndices(const AppState& appState)
{
    // Set resolution index
    {
        auto it = std::find(c_resolutionValues.begin(), c_resolutionValues.end(), appState.state.options.resDivider);
        if (it != c_resolutionValues.end()) {
            m_uiState.resolutionIndex = glm::clamp(static_cast<int>(it - c_resolutionValues.begin()), 0, static_cast<int>(c_resolutionValues.size() - 1));
        }
    }

    // Set format index
    {
        auto it = std::find(c_formatValues.begin(), c_formatValues.end(), appState.state.options.maskFormat);
        if (it != c_formatValues.end()) {
            m_uiState.formatIndex = glm::clamp(static_cast<int>(it - c_formatValues.begin()), 0, static_cast<int>(c_formatValues.size() - 1));
        }
    }

    // Set skip index
    {
        auto it = std::find(c_skipValues.begin(), c_skipValues.end(), appState.state.options.frameSkip);
        if (it != c_skipValues.end()) {
            m_uiState.skipIndex = glm::clamp(static_cast<int>(it - c_skipValues.begin()), 0, static_cast<int>(c_skipValues.size() - 1));
        }
    }
}

void AppView::run()
{
    LOG_DEBUG("Entering main loop.");

    // Run UI main loop
    m_ui->run();
}

bool AppView::onFrame(UI& ui)
{
    // Check UI instance
    if (&ui != m_ui.get()) {
        return false;
    }

#if DEBUG_FRAME_TIMING
    static std::chrono::nanoseconds maxDuration{0};
    static std::chrono::nanoseconds totDuration{0};
    static int frameCount = 0;
    const auto frameStartTime = std::chrono::high_resolution_clock::now();
#endif

    // Check for varjo events
    m_logic.checkEvents();

    // Update state to logic state
    updateUI();

    // Update application logic
    const bool submit = m_logic.update();

    // Update submit rate
    {
        const std::chrono::seconds fpsInterval{1};
        const auto nowTime = FpsClock::now();
        const auto fpsDuration = nowTime - m_fpsStats.startTime;
        m_fpsStats.frameCount += submit;
        if (fpsDuration > fpsInterval) {
            double fps = (1000000000.0 * m_fpsStats.frameCount) / fpsDuration.count();
            m_fpsStats.frameRate = fps;
            m_fpsStats.frameCount = 0;
            m_fpsStats.startTime = nowTime;
        }
    }

#if DEBUG_FRAME_TIMING
    // Frame timing
    const auto frameEndTime = std::chrono::high_resolution_clock::now();
    const auto frameDuration = frameEndTime - frameStartTime;
    totDuration += frameDuration;
    maxDuration = std::max(frameDuration, maxDuration);
    frameCount++;

    // Report frame timing
    static const std::chrono::seconds c_frameReportInterval(1);
    if (totDuration >= c_frameReportInterval) {
        LOG_INFO("Timing: frames=%d, fps=%f, avg=%f ms, max=%f ms, tot=%f ms", frameCount, 1e9f * frameCount / totDuration.count(),
            0.000001 * totDuration.count() / frameCount, 0.000001 * maxDuration.count(), 0.000001 * totDuration.count());
        maxDuration = {0};
        totDuration = {0};
        frameCount = 0;
    }
#endif

    // Return true to continue running
    return true;
}

bool AppView::onAction(Action actionType, AppState& appState)
{
    bool stateDirty = false;

    if (actionType != Action::None) {
        LOG_INFO("Action: %s", c_actions.at(actionType).name.c_str());
    }

    // Handle input actions
    switch (actionType) {
        case Action::None: {
            // Ignore
        } break;

        case Action::Help: {
            LOG_INFO("\nKeyboard Shortcuts:\n");
            for (const auto& ai : c_actions) {
                if (ai.first != Action::None) {
                    LOG_INFO("  %s", ai.second.help.c_str());
                }
            }
            LOG_INFO("");
        } break;

        case Action::Reset: {
            const auto& resetState = m_presets.getResetState();
            appState.state = resetState;
            stateDirty = true;
        } break;

        case Action::MaskingMode: {
            int mode = (static_cast<int>(appState.state.options.maskingMode) + 1) % c_maskingModeNames.size();
            appState.state.options.maskingMode = static_cast<AppState::MaskMode>(mode);
            // LOG_INFO("Masking mode: %s", c_modeNames[mode].c_str());
            stateDirty = true;
        } break;

        case Action::VisualizationMode: {
            const std::vector<std::string> c_modeNames = {"None", "Planes", "Mask"};
            int mode = (static_cast<int>(appState.general.debugMode) + 1) % c_modeNames.size();
            appState.general.debugMode = static_cast<AppState::DebugMode>(mode);
            // LOG_INFO("Debug mode: %s", c_modeNames[mode].c_str());
            stateDirty = true;
        } break;

        case Action::ToggleTestPresets: {
            m_uiState.testPresets = !m_uiState.testPresets;
            LOG_INFO("Test presets: %s", (m_uiState.testPresets ? "ON" : "OFF"));
            // Reload presets
            const auto presetsFilename = m_uiState.testPresets ? c_testPresetsFilename : c_presetsFilename;
            if (!m_presets.loadPresets(presetsFilename)) {
                LOG_ERROR("Loading presets failed: %s", presetsFilename.c_str());
            } else {
                // LOG_INFO("Reset to defaults.");
                // appState.state = m_presets.getResetState();
                // stateDirty = true;
            }
        } break;
#ifdef USE_VIDEO_DEPTH_TEST
        case AppView::Action::DepthTestMode: {
            appState.state.options.videoDepthTestMode =
                static_cast<AppState::VideoDepthTestMode>((static_cast<int>(appState.state.options.videoDepthTestMode) + 1) % c_videoDepthTestModeNames.size());
            stateDirty = true;
        } break;

        case AppView::Action::DepthTestBehavior: {
            appState.state.options.videoDepthTestBehavior = static_cast<AppState::VideoDepthTestBehavior>(
                (static_cast<int>(appState.state.options.videoDepthTestBehavior) + 1) % c_videoDepthTestBehaviorNames.size());
            stateDirty = true;
        } break;

        case AppView::Action::DepthTestRange: {
            m_uiState.depthTestRangeIndex = (m_uiState.depthTestRangeIndex + 1) % c_videoDepthTestRangeValues.size();
            appState.state.options.videoDepthTestRange = {0.0f, c_videoDepthTestRangeValues[m_uiState.depthTestRangeIndex]};
            stateDirty = true;
        } break;
#endif

        case Action::ApplyPreset_0:
        case Action::ApplyPreset_1:
        case Action::ApplyPreset_2:
        case Action::ApplyPreset_3:
        case Action::ApplyPreset_4:
        case Action::ApplyPreset_5:
        case Action::ApplyPreset_6:
        case Action::ApplyPreset_7:
        case Action::ApplyPreset_8: {
            int i = static_cast<int>(actionType) - static_cast<int>(Action::ApplyPreset_0);
            if (i >= 0 && i < static_cast<int>(m_presets.getPresetCount())) {
                const auto preset = m_presets.getPreset(m_presets.getPresetId(i));
                if (preset) {
                    applyPreset(*preset, appState.state, !m_uiState.testPresets);
                }
                stateDirty = true;
            } else {
                LOG_WARNING("No preset to apply: index=%d", i);
            }
        } break;

        default: {
            // Ignore unknown action
            LOG_ERROR("Unknown action: %d", actionType);
            assert(false);
        } break;
    }

    return stateDirty;
}

void AppView::onKeyPress(UI& ui, int keyCode)
{
    if (m_uiState.anyItemActive) {
        // Ignore key handling if UI items active
        return;
    }

    auto appState = m_logic.getState();

    // Check for input action
    Action action = Action::None;
    if (c_keyMappings.count(keyCode)) {
        action = c_keyMappings.at(keyCode);
    }

    // Handle action
    const bool stateDirty = onAction(action, appState);

    // Update state if changed
    if (stateDirty) {
        // Resolve UI indices
        resolveIndices(appState);

        // Set app state
        m_logic.setState(appState, false);
    }
}

void AppView::updateUI()
{
    // --- UI helper definitions --->
#define _VSPACE ImGui::Dummy(ImVec2(0.0f, 8.0f));

#define _HSPACE                        \
    ImGui::Dummy(ImVec2(8.0f, 12.0f)); \
    ImGui::SameLine();

#define _SEPARATOR      \
    _VSPACE;            \
    ImGui::Separator(); \
    _VSPACE;

#define _PUSHSTYLE_ALPHA(X) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, X);

#define _POPSTYLE ImGui::PopStyleVar();

#define _PUSHDISABLEDIF(C)                                  \
    if (C) {                                                \
        _PUSHSTYLE_ALPHA(0.5f);                             \
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true); \
    }

#define _POPDISABLEDIF(C)     \
    if (C) {                  \
        _POPSTYLE;            \
        ImGui::PopItemFlag(); \
    }
    // <--- UI helper definitions ---

    // Update from logic state
    AppState appState = m_logic.getState();

    // VST Post Process window
    ImGui::Begin(APP_TITLE_TEXT);

    // Set initial size and pos
    {
        const float m = static_cast<float>(c_windowMargin);
        const float w = static_cast<float>(c_windowClientSize.x);
        const float h = static_cast<float>(c_windowClientSize.y - c_logHeight);
        ImGui::SetWindowPos(ImVec2(m, m), ImGuiCond_FirstUseEver);
        ImGui::SetWindowSize(ImVec2(w - 2 * m, h - 2 * m), ImGuiCond_FirstUseEver);
    }

#define _TAG "##appgeneric"

    {
        _VSPACE;

        ImGui::Text("Settings: ");
        ImGui::SameLine();
        if (ImGui::Button("Reset" _TAG)) {
            const auto& resetState = m_presets.getResetState();
            LOG_INFO("Reset to defaults.");
            appState.state = resetState;

            // Resolve UI indices
            resolveIndices(appState);
        }
        ImGui::SameLine();
        _HSPACE;
        if (ImGui::Button("Load" _TAG)) {
            // Load config from file to logic state
            auto logicState = m_logic.getState().state;
            if (Presets::loadState(c_configStorageFilename, logicState)) {
                // Update loaded config from logic state
                appState.state = logicState;

                // Resolve UI indices
                resolveIndices(appState);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save" _TAG)) {
            // Save current config to file
            if (Presets::saveState(c_configStorageFilename, m_logic.getState().state)) {
                // State saved
            }
        }

        _VSPACE;

        ImGui::Text("Apply preset: ");

        const auto presetCount = m_presets.getPresetCount();
        for (size_t i = 0; i < presetCount; i++) {
            const auto preset = m_presets.getPreset(m_presets.getPresetId(i));
            ImGui::SameLine();
            if (ImGui::Button(preset->name.c_str())) {
                onAction(static_cast<Action>(static_cast<int>(Action::ApplyPreset_0) + i), appState);
            }
        }

        _VSPACE;

        {
            const char* items[] = {"None", "Show Mask", "Show Colors"};
            ImGui::Text("Visualization Mode: ");
            ImGui::SameLine();
            ImGui::PushItemWidth(110);
            ImGui::Combo("##debugvisumode" _TAG, (int*)&appState.general.debugMode, items, 3);
            ImGui::PopItemWidth();

            ImGui::SameLine();
            switch (appState.general.debugMode) {
                case AppState::DebugMode::None: ImGui::Text("(No debug visualization)"); break;
                case AppState::DebugMode::VisualizeMask: ImGui::Text("(Visualize mask alpha channel)"); break;
                case AppState::DebugMode::VisualizeColors: ImGui::Text("(Visualize masking plane colors)"); break;
            }
        }
    }
#undef _TAG

    _SEPARATOR;

#define _TAG "##maskingoptions"
    {
        {
            ImGui::Text("Masking mode: ");
            ImGui::SameLine();
            ImGui::PushItemWidth(100);
            ImGui::Combo(
                "##maskingmode" _TAG, (int*)&appState.state.options.maskingMode, c_maskingModeNames.data(), static_cast<int>(c_maskingModeNames.size()));
            ImGui::PopItemWidth();

            ImGui::SameLine();
            switch (appState.state.options.maskingMode) {
                case AppState::MaskMode::None: ImGui::Text("(Chromakey everywhere, No additional masking)"); break;
                case AppState::MaskMode::Restricted: ImGui::Text("(Chroma key restricted to the masked areas, VR everywhere else)"); break;
                case AppState::MaskMode::Extended: ImGui::Text("(Chroma key everywhere, VR extended to masked areas)"); break;
                case AppState::MaskMode::Reduced: ImGui::Text("(Chroma key everywhere, Video on masked areas)"); break;
                case AppState::MaskMode::DepthTestOrFail: ImGui::Text("(Depth test on masked areas, fail outside)"); break;
                case AppState::MaskMode::DepthTestOrPass: ImGui::Text("(Depth test on masked areas, pass outside)"); break;
            }
        }
    }

#undef _TAG

    _VSPACE;

    _PUSHDISABLEDIF(appState.state.options.maskingMode == AppState::MaskMode::None);

#define _TAG "##planes"
    {
        bool anyTracking = false;
        ImGui::Text("Masking Planes: ");
        for (size_t i = 0; i < appState.state.maskPlanes.size(); i++) {
            auto& plane = appState.state.maskPlanes[i];
            ImGui::SameLine();
            const bool isSelected = (i == m_uiState.planeIndex);
            const auto colTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const auto colBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(colTxt.x, colTxt.y, colTxt.z, colTxt.w * (isSelected ? 1.0f : 0.5f)));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colBtn.x, colBtn.y, colBtn.z, colBtn.w * (isSelected ? 1.2f : 0.8f)));
            if (ImGui::Button(("#" + std::to_string(i) + _TAG).c_str())) {
                if (m_uiState.planeIndex != i) {
                    LOG_INFO("Current plane: %d", i);
                    m_uiState.planeIndex = static_cast<int>(i);
                }
            }
            ImGui::PopStyleColor(2);
            anyTracking |= (plane.enabled && plane.tracking);
        }

        ImGui::SameLine();
        _HSPACE;
        if (ImGui::Button("Track All")) {
            LOG_INFO("Tracking all planes.");
            appState.general.debugMode = AppState::DebugMode::VisualizeMask;
            for (size_t i = 0; i < appState.state.maskPlanes.size(); i++) {
                auto& plane = appState.state.maskPlanes[i];
                plane.tracking = true;
                plane.resetMarkerPrediction = true;
                plane.trackedId = 0;
                plane.enabled = true;
            }
        }
        ImGui::SameLine();
        _PUSHDISABLEDIF(!anyTracking)
        if (ImGui::Button("Stop All")) {
            LOG_INFO("Stop tracking planes.");
            for (size_t i = 0; i < appState.state.maskPlanes.size(); i++) {
                auto& plane = appState.state.maskPlanes[i];
                plane.tracking = false;
                plane.enabled = (plane.trackedId > 0);
            }
        }
        _POPDISABLEDIF(!anyTracking)

        ImGui::SameLine();
        _HSPACE;
        if (ImGui::Button("Reset All")) {
            LOG_INFO("Reset all planes.");
            appState.state.maskPlanes = m_presets.getResetState().maskPlanes;
        }
    }

    _VSPACE;

    // Current plane
    {
        int i = m_uiState.planeIndex;
        std::string tag = "##plane" + std::to_string(i);

        auto& plane = appState.state.maskPlanes[i];
        _HSPACE;
        ImGui::Text("Plane #%d:", i);
        ImGui::SameLine();
        ImGui::Checkbox((tag + "Enabled").c_str(), &plane.enabled);

        const bool planeDisabled = !plane.enabled;
        _PUSHDISABLEDIF(planeDisabled);

        ImGui::SameLine();
        _HSPACE;
        ImGui::Text("Tracking:");
        ImGui::SameLine();
        if (ImGui::Checkbox((tag + "Tracking").c_str(), &plane.tracking)) {
            // If tracking enabled, request pose prediction reset
            plane.resetMarkerPrediction = plane.tracking;
        }

        _PUSHDISABLEDIF(!plane.tracking);
        ImGui::SameLine();
        ImGui::Text("ID:");
        ImGui::SameLine();
        ImGui::PushItemWidth(120);
        ImGui::InputInt((tag + "Marker ID").c_str(), &plane.trackedId, 1, 10, ImGuiInputTextFlags_CharsDecimal);
        ImGui::PopItemWidth();
        _POPDISABLEDIF(!plane.tracking);
        ImGui::SameLine();
        if (ImGui::Button(("Reset ID" + tag).c_str())) {
            plane.trackedId = 0;
        }

        ImGui::SameLine();
        _HSPACE;
        if (ImGui::Button(("Reset Plane" + tag + "plane").c_str())) {
            LOG_INFO("Reset plane #%d", i);
            const bool enabled = plane.enabled;
            plane = m_presets.getResetState().maskPlanes[i];
            plane.enabled = enabled;
        }

        _HSPACE;
        ImGui::Text("Position:");
        ImGui::SameLine();
        ImGui::DragFloat3((tag + "Position").c_str(), (float*)&plane.position, 0.01f, -10.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (ImGui::Button(("Reset" + tag + "position").c_str())) {
            LOG_INFO("Reset position for plane #%d", i);
            plane.position = glm::vec3(0.0f);
        }

        _HSPACE;
        ImGui::Text("Rotation:");
        ImGui::SameLine();
        ImGui::DragFloat3((tag + "Rotation").c_str(), (float*)&plane.rotation, 0.5f, -180.0f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (ImGui::Button(("Reset" + tag + "rotation").c_str())) {
            LOG_INFO("Reset rotation for plane #%d", i);
            plane.rotation = glm::vec3(0.0f);
        }

        _HSPACE;
        ImGui::Text("Scale:");
        ImGui::SameLine();
        ImGui::DragFloat2((tag + "Scale").c_str(), (float*)&plane.scale, 0.01f, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (ImGui::Button(("Reset" + tag + "scale").c_str())) {
            LOG_INFO("Reset scale for plane #%d", i);
            plane.scale = m_presets.getResetState().maskPlanes[i].scale;
        }

        _HSPACE;
        ImGui::Text("Color:");
        ImGui::SameLine();
        // ImGui::DragFloat4((tag + "Color").c_str(), (float*)&plane.color, 0.005f, 0.0f, 1.0f, "%.3f", 1.0f);
        ImGui::ColorEdit3((tag + "Color").c_str(), (float*)&plane.color, 0);
        ImGui::SameLine();
        if (ImGui::Button(("Reset" + tag + "color").c_str())) {
            LOG_INFO("Reset color for plane #%d", i);
            plane.color = m_presets.getResetState().maskPlanes[i].color;
        }

        _POPDISABLEDIF(planeDisabled);
    }
#undef _TAG

    _POPDISABLEDIF(appState.state.options.maskingMode == AppState::MaskMode::None);

    _SEPARATOR;

#define _TAG "##vrtoggles"
    {
        {
            ImGui::Text("Feature Toggles:");
            _HSPACE;
            ImGui::Checkbox("Video Rendering" _TAG, &appState.state.options.vstRendering);

            ImGui::SameLine();
            _HSPACE;
            float viewOffset = static_cast<float>(appState.state.options.vrViewOffset);
            ImGui::PushItemWidth(100);
            ImGui::SliderFloat("Mask View Offset" _TAG, &viewOffset, 0.0, 1.0, "%.1f");
            ImGui::PopItemWidth();
            appState.state.options.vrViewOffset = viewOffset;

            ImGui::SameLine();
            _HSPACE;
            ImGui::Checkbox("Global View Offset" _TAG, &appState.state.options.forceGlobalViewOffset);

            _VSPACE;

            _HSPACE;
            ImGui::Checkbox("Sync Frame" _TAG, &appState.state.options.vrFrameSync);
            _PUSHDISABLEDIF(!appState.state.options.vrFrameSync);

            ImGui::SameLine();
            ImGui::Checkbox("Update Frame" _TAG, &appState.state.options.vrFrameUpdate);
            _PUSHDISABLEDIF(!appState.state.options.vrFrameUpdate);

            ImGui::SameLine();
            ImGui::Checkbox("Submit Frame" _TAG, &appState.state.options.vrFrameSubmit);
            _PUSHDISABLEDIF(!appState.state.options.vrFrameSubmit);

            _HSPACE;
            ImGui::Checkbox("Layer: Color" _TAG, &appState.state.options.vrLayerSubmitColor);
            ImGui::SameLine();
            ImGui::Checkbox("Layer: Mask" _TAG, &appState.state.options.vrLayerSubmitMask);
            ImGui::SameLine();

            _PUSHDISABLEDIF(!(appState.state.options.vrLayerSubmitColor || appState.state.options.vrLayerSubmitMask));

            ImGui::Checkbox("Render mask" _TAG, &appState.state.options.vrRenderMask);
            ImGui::SameLine();
            ImGui::Checkbox("Depth Submit" _TAG, &appState.state.options.vrLayerSubmitDepth);

            _PUSHDISABLEDIF(!appState.state.options.vrLayerSubmitDepth);
            ImGui::SameLine();
            ImGui::Checkbox("Depth Test" _TAG, &appState.state.options.vrLayerDepthTestMask);
            _POPDISABLEDIF(!appState.state.options.vrLayerSubmitDepth);

            _POPDISABLEDIF(!(appState.state.options.vrLayerSubmitColor || appState.state.options.vrLayerSubmitMask));
            _POPDISABLEDIF(!appState.state.options.vrFrameSubmit);
            _POPDISABLEDIF(!appState.state.options.vrFrameUpdate);
            _POPDISABLEDIF(!appState.state.options.vrFrameSync);
        }

        _VSPACE;

        {
            ImGui::Text("Perf Toggles:");

            _PUSHDISABLEDIF(!appState.state.options.vrFrameSubmit);

            _HSPACE;

            m_uiState.formatIndex = glm::clamp(m_uiState.formatIndex, 0, static_cast<int>(c_formatNames.size() - 1));
            ImGui::Text("Format: ");
            ImGui::SameLine();
            ImGui::PushItemWidth(70);
            ImGui::Combo("##format" _TAG, &m_uiState.formatIndex, c_formatNames.data(), static_cast<int>(c_formatNames.size()));
            ImGui::PopItemWidth();
            appState.state.options.maskFormat = c_formatValues[m_uiState.formatIndex];

            ImGui::SameLine();
            _HSPACE;

            m_uiState.resolutionIndex = glm::clamp(m_uiState.resolutionIndex, 0, static_cast<int>(c_resolutionNames.size() - 1));
            ImGui::Text("Resolution: ");
            ImGui::SameLine();
            ImGui::PushItemWidth(70);
            ImGui::Combo("##resolution" _TAG, &m_uiState.resolutionIndex, c_resolutionNames.data(), static_cast<int>(c_resolutionNames.size()));
            ImGui::PopItemWidth();
            appState.state.options.resDivider = c_resolutionValues[m_uiState.resolutionIndex];

            ImGui::SameLine();
            _HSPACE;

            m_uiState.skipIndex = glm::clamp(m_uiState.skipIndex, 0, static_cast<int>(c_skipNames.size() - 1));
            ImGui::Text("Frame skip: ");
            ImGui::SameLine();
            ImGui::PushItemWidth(70);
            ImGui::Combo("##frameskip" _TAG, &m_uiState.skipIndex, c_skipNames.data(), static_cast<int>(c_skipNames.size()));
            ImGui::PopItemWidth();
            appState.state.options.frameSkip = c_skipValues[m_uiState.skipIndex];

            _POPDISABLEDIF(!appState.state.options.vrFrameSubmit);
        }
    }
#undef _TAG

#ifdef USE_VIDEO_DEPTH_TEST

    _SEPARATOR;

#define _TAG "##videodepthtest"

    {
        ImGui::Text("Global Video Depth Testing:");

        ImGui::SameLine();
        _HSPACE;

        ImGui::PushItemWidth(120);
        ImGui::Combo("##video depth test mode" _TAG, (int*)&appState.state.options.videoDepthTestMode, c_videoDepthTestModeNames.data(),
            static_cast<int>(c_videoDepthTestModeNames.size()));
        ImGui::PopItemWidth();

        // TODO: Add description text of selected mode

        _VSPACE;
        _HSPACE;
        ImGui::BeginGroup();

        _PUSHDISABLEDIF(appState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::Default)

        ImGui::Text("Behavior:");
        ImGui::SameLine();

        ImGui::PushItemWidth(120);
        ImGui::Combo("##video depth test behavior" _TAG, (int*)&appState.state.options.videoDepthTestBehavior, c_videoDepthTestBehaviorNames.data(),
            static_cast<int>(c_videoDepthTestBehaviorNames.size()));
        ImGui::PopItemWidth();

        // TODO: Add description text of selected mode

        _POPDISABLEDIF(appState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::Default);

        _PUSHDISABLEDIF(appState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::Default ||
                        appState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::FullRange);

        ImGui::SameLine();
        _HSPACE;

        ImGui::Text("Range Near:");
        ImGui::SameLine();

        ImGui::SameLine();
        ImGui::PushItemWidth(120);
        float nearZ =
            std::min(static_cast<float>(appState.state.options.videoDepthTestRange[0]), static_cast<float>(appState.state.options.videoDepthTestRange[1]));
        float farZ = static_cast<float>(appState.state.options.videoDepthTestRange[1]);
        ImGui::SliderFloat("##Depth range value near" _TAG, &nearZ, 0.0f, 5.0f, "%.2f");
        ImGui::SameLine();
        ImGui::Text(" Far:");
        ImGui::SameLine();
        ImGui::SliderFloat("##Depth range value far" _TAG, &farZ, 0.0f, 5.0f, "%.2f");
        appState.state.options.videoDepthTestRange[0] = std::max(std::min(nearZ, farZ), 0.0f);
        appState.state.options.videoDepthTestRange[1] = std::max(farZ, 0.0f);
        ImGui::PopItemWidth();

        _POPDISABLEDIF(appState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::Default ||
                       appState.state.options.videoDepthTestMode == AppState::VideoDepthTestMode::FullRange);

        ImGui::EndGroup();
    }
#endif

#undef _TAG

    _SEPARATOR;

    {
        ImGui::Text("Frame rate: %.1f fps (%.1f ms), Submit rate: %.1f fps (%.1f ms), Total: %d frames (%.1f s)",  //
            ImGui::GetIO().Framerate,                                                                              //
            1000.0f / ImGui::GetIO().Framerate,                                                                    //
            m_fpsStats.frameRate,                                                                                  //
            1000.0f / m_fpsStats.frameRate,                                                                        //
            appState.general.frameCount, appState.general.frameTime);

        ImGui::End();
    }

    // Log window
    {
        ImGui::Begin("Log");

        // Set initial size and pos
        {
            const float m = static_cast<float>(c_windowMargin);
            const float w = static_cast<float>(c_windowClientSize.x);
            const float h0 = static_cast<float>(c_windowClientSize.y - c_logHeight);
            const float h1 = static_cast<float>(c_logHeight);
            ImGui::SetWindowPos(ImVec2(m, h0), ImGuiCond_FirstUseEver);
            ImGui::SetWindowSize(ImVec2(w - 2 * m, h1 - m), ImGuiCond_FirstUseEver);
        }

        m_ui->drawLog();
        ImGui::End();
    }

// --- UI helper definitions --->
#undef _VSPACE
#undef _HSPACE
#undef _SEPARATOR
#undef _PUSHSTYLE_ALPHA
#undef _POPSTYLE
#undef _PUSHDISABLEDIF
#undef _POPDISABLEDIF
#undef _TAG
    // <--- UI helper definitions ---

    // Set UI item active flag
    m_uiState.anyItemActive = ImGui::IsAnyItemActive();

    // Update state from UI back to logic
    m_logic.setState(appState, false);
}
