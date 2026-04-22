// Copyright 2020 Varjo Technologies Oy. All rights reserved.

/* ChromaKey Example application
 *
 * - Showcases Varjo MR API features for configuring and using chroma keying
 * - Run example and press F1 for help
 */

// Standard includes
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cassert>
#include <atomic>

#include <glm/glm.hpp>

// Varjo includes
#include <Varjo.h>
#include <Varjo_events.h>
#include <Varjo_mr.h>

// Internal includes
#include "Globals.hpp"
#include "CameraManager.hpp"
#include "ChromaKeyManager.hpp"
#include "Scene.hpp"
#include "D3D11MultiLayerView.hpp"
#include "D3D11Renderer.hpp"

// VarjoExamples namespace contains simple example wrappers for using Varjo API features.
// These are only meant to be used in SDK example applications. In your own application,
// use your own production quality integration layer.
using namespace VarjoExamples;

//---------------------------------------------------------------------------

namespace
{
// Input action enumeration
enum class InputAction {
    None,
    Quit,
    PrintHelp,
    ToggleVideoRendering,
    LockConfig,
    UnlockConfig,
    ResetConfig,
    StartChromaKeying,
    StopChromaKeying,
    SelectChromaKeyIndex0,
    SelectChromaKeyIndex1,
    SelectChromaKeyIndex2,
    SelectChromaKeyIndex3,
    ChangeAdjustment,
    ToggleChromaKeyMode,
    IncParamValue0,
    DecParamValue0,
    IncParamValue1,
    DecParamValue1,
    IncParamValue2,
    DecParamValue2,
};

// Input action name mappings
const std::unordered_map<InputAction, std::string> c_inputNames = {
    {InputAction::None, "None"},
    {InputAction::Quit, "Quit"},
    {InputAction::PrintHelp, "PrintHelp"},
    {InputAction::ToggleVideoRendering, "ToggleVideoRendering"},
    {InputAction::LockConfig, "LockConfig"},
    {InputAction::UnlockConfig, "UnlockConfig"},
    {InputAction::ResetConfig, "ResetConfig"},
    {InputAction::StartChromaKeying, "StartChromaKeying"},
    {InputAction::StopChromaKeying, "StopChromaKeying"},
    {InputAction::SelectChromaKeyIndex0, "SelectChromaKeyIndex0"},
    {InputAction::SelectChromaKeyIndex1, "SelectChromaKeyIndex1"},
    {InputAction::SelectChromaKeyIndex2, "SelectChromaKeyIndex2"},
    {InputAction::SelectChromaKeyIndex3, "SelectChromaKeyIndex3"},
    {InputAction::ToggleChromaKeyMode, "ToggleChromaKeyMode"},
    {InputAction::ChangeAdjustment, "ChangeAdjustment"},
    {InputAction::IncParamValue0, "IncParamValue0"},
    {InputAction::DecParamValue0, "DecParamValue0"},
    {InputAction::IncParamValue1, "IncParamValue1"},
    {InputAction::DecParamValue1, "DecParamValue1"},
    {InputAction::IncParamValue2, "IncParamValue2"},
    {InputAction::DecParamValue2, "DecParamValue2"},
};

// Key to input mappings (WinUser.h)
// https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
const std::unordered_map<WORD, InputAction> s_inputActionMapping = {
    {VK_ESCAPE, InputAction::Quit},
    {VK_F1, InputAction::PrintHelp},
    {VK_F2, InputAction::ToggleVideoRendering},
    {VK_F3, InputAction::LockConfig},
    {VK_F4, InputAction::UnlockConfig},
    {VK_F5, InputAction::StartChromaKeying},
    {VK_F6, InputAction::StopChromaKeying},
    {'1', InputAction::SelectChromaKeyIndex0},
    {'2', InputAction::SelectChromaKeyIndex1},
    {'3', InputAction::SelectChromaKeyIndex2},
    {'4', InputAction::SelectChromaKeyIndex3},
    {'R', InputAction::ResetConfig},
    {'X', InputAction::ToggleChromaKeyMode},
    {'C', InputAction::ChangeAdjustment},
    {'Q', InputAction::IncParamValue0},
    {'A', InputAction::DecParamValue0},
    {'W', InputAction::IncParamValue1},
    {'S', InputAction::DecParamValue1},
    {'E', InputAction::IncParamValue2},
    {'D', InputAction::DecParamValue2},
};

// Usage text
const std::string s_usageText =
    "\n"
    "Usage:\n"
    "F1       - Print this help\n"
    "F2       - Toggle video rendering\n"
    "F3/F4    - Lock/unlock chromakey config\n"
    "F5/F6    - Start/stop chromakeying\n"
    "1,2,3,4  - Select chroma key index to edit\n"
    "R        - Reset all configs\n"
    "X        - Toggle chroma key mode\n"
    "C        - Toggle parameter to edit\n"
    "Q/A      - Inc/dec parameter value: Hue\n"
    "W/S      - Inc/dec parameter value: Sat\n"
    "E/D      - Inc/dec parameter value: Val\n"
    "\n";

// Editable parameters
enum class Adjustment { TargetColor = 0, Tolerance, Falloff, Count };

// Editable parameter names
const std::unordered_map<Adjustment, std::string> c_adjustmentNames = {
    {Adjustment::TargetColor, "ChromaKey Color (Hue, Sat, Val)"},
    {Adjustment::Tolerance, "ChromaKey Tolerance (Hue, Sat, Val)"},
    {Adjustment::Falloff, "ChromaKey Falloff (Hue, Sat, Val)"},
};

std::atomic_bool ctrlCPressed = false;

BOOL WINAPI ctrlHandler(DWORD /*dwCtrlType*/)
{
    ctrlCPressed = true;
    return TRUE;
}
}  // namespace

//---------------------------------------------------------------------------

// Dummy test scene with just solid background color to visualize chroma key mask
class DummyScene : public VarjoExamples::Scene
{
public:
    // Constructor
    DummyScene() = default;

protected:
    // Update scene animation
    void onUpdate(double frameTime, double deltaTime, int64_t frameCounter, const UpdateParams& params) override {}

    // Render scene to given view
    void onRender(VarjoExamples::Renderer& renderer, VarjoExamples::Renderer::ColorDepthRenderTarget& target, int viewIndex, const glm::mat4x4& viewMat,
        const glm::mat4x4& projMat, void* userData) const override
    {
        // Nothing to render
    }
};

//---------------------------------------------------------------------------

class TestClient
{
public:
    // Construct client app
    TestClient(varjo_Session* session)
        : m_session(session)
    {
        // Create D3D11 renderer and view
        {
            auto dxgiAdapter = D3D11MultiLayerView::getAdapter(session);
            auto d3d11Renderer = std::make_unique<D3D11Renderer>(dxgiAdapter.Get());
            m_varjoView = std::make_unique<D3D11MultiLayerView>(session, *d3d11Renderer);
            m_renderer = std::move(d3d11Renderer);
        }

        // Create scene instance
        m_scene = std::make_unique<DummyScene>();

        // Create mixed reality camera manager instance
        m_camera = std::make_unique<CameraManager>(session);

        // Create mixed reality chroma key manager instance
        m_chromakey = std::make_unique<ChromaKeyManager>(session);

        // Check if Mixed Reality features are available.
        varjo_Bool mixedRealityAvailable = varjo_False;
        varjo_SyncProperties(m_session);
        if (varjo_HasProperty(m_session, varjo_PropertyKey_MRAvailable)) {
            mixedRealityAvailable = varjo_GetPropertyBool(m_session, varjo_PropertyKey_MRAvailable);
        }

        if (mixedRealityAvailable == varjo_True) {
            LOG_INFO("Varjo Mixed Reality features available!");

            // Reset camera properties to defaults and set auto exposure and WB
            m_camera->resetPropertiesToDefaults();
            m_camera->setAutoMode(varjo_CameraPropertyType_ExposureTime);
            m_camera->setAutoMode(varjo_CameraPropertyType_WhiteBalance);

        } else {
            LOG_ERROR("ERROR: Varjo Mixed Reality features not available!");
        }
    }

    // Destruictor
    ~TestClient()
    {
        // Reset scene
        m_scene.reset();

        // Reset layer view
        m_varjoView.reset();

        // Reset renderer
        m_renderer.reset();

        // Free camera manager resources
        m_camera.reset();

        // Free chroma key resources
        m_chromakey.reset();

        // Reset session
        m_session = nullptr;
    }

    // Disable copy and assign
    TestClient(const TestClient& other) = delete;
    TestClient(const TestClient&& other) = delete;
    TestClient& operator=(const TestClient& other) = delete;
    TestClient& operator=(const TestClient&& other) = delete;

    // Reset configuration
    void resetConfig()
    {
        // Reset edit state
        m_edit = {};

        // Allocate editable configs
        m_edit.m_configs.resize(m_chromakey->getCount());

        // Reset all configs
        for (int i = 0; i < m_chromakey->getCount(); i++) {
            // This is just an example configuration for green screen use. Actual values depend on
            // your chroma surfaces and environment lighting.

            // Adjust target hue to match your chroma surface and light temperature.
            // Saturation and value should usually be 1.
            const glm::vec3 targetColorHSV(0.355, 1, 1);

            // Adjust tolerances for balancing between chroma leak, reflections, and shadows. These
            // settings are highly dependent on your environment and can only be fine tuned on the location.
            const glm::vec3 toleranceHSV(0.15, 0.60, 0.92);

            // Adjust falloffs for gradual fade out of reflections and shadows.
            const glm::vec3 falloffHSV(0.03, 0.03, 0.03);

            // Set initial green screen config to index 0, disable others.
            varjo_ChromaKeyConfig config = (i == 0)                                                                      //
                                               ? m_chromakey->createConfigHSV(targetColorHSV, toleranceHSV, falloffHSV)  //
                                               : m_chromakey->createConfigDisabled();

            // Apply configuration
            m_chromakey->setConfig(i, config);
            m_edit.m_configs[i] = config;
        }
    }

    // Client main loop
    void run()
    {
        // Startup procedure
        {
            // Lock confifuration
            m_chromakey->lockConfig();

            // Reset configurations
            resetConfig();

            // Enable video rendering
            varjo_MRSetVideoRender(m_session, varjo_True);
            CHECK_VARJO_ERR(m_session);
            m_videoEnabled = true;

            // Enable chroma keying
            m_chromakey->toggleChromaKeying(true);
        }

        // Main loop
        while (true) {
            // Check for keyboard input
            InputAction input = checkInput();
            assert(c_inputNames.count(input));
            std::string inputName = c_inputNames.at(input);
            if (input != InputAction::None) {
                LOG_INFO("INPUT: %s", inputName.c_str());
            }

            // Check for quit and Ctrl-C
            if (input == InputAction::Quit || ctrlCPressed) {
                LOG_INFO("Quitting main loop..");
                break;
            }

            // Active config and editable param
            double* editParam = nullptr;
            std::array<glm::dvec2, 3> editMinMax = {glm::dvec2(0.0f, 1.0f), glm::dvec2(0.0f, 1.0f), glm::dvec2(0.0f, 1.0f)};
            std::array<double, 3> editStep = {0.01, 0.01, 0.01};

            {
                varjo_ChromaKeyConfig& config = m_edit.m_configs[m_edit.m_activeIndex];

                switch (m_edit.m_activeAdjustment) {
                    case Adjustment::TargetColor: {
                        if (config.type == varjo_ChromaKeyType_HSV) {
                            editParam = config.params.hsv.targetColor;
                            editStep = {0.005, 0.01, 0.01};
                        }
                    } break;
                    case Adjustment::Tolerance: {
                        if (config.type == varjo_ChromaKeyType_HSV) {
                            editParam = config.params.hsv.tolerance;
                        }
                    } break;
                    case Adjustment::Falloff: {
                        if (config.type == varjo_ChromaKeyType_HSV) {
                            editParam = config.params.hsv.falloff;
                        }
                    } break;
                    default: {
                        LOG_ERROR("ERROR: Unsupported adjustment: %d", static_cast<int>(m_edit.m_activeAdjustment));
                    } break;
                }
            }

            // Flag for storing edited config
            bool configValueEdited = false;

            // Handle input
            switch (input) {
                case InputAction::PrintHelp: {
                    printHelp();
                    break;
                }
                case InputAction::ToggleVideoRendering: {
                    m_videoEnabled = !m_videoEnabled;
                    LOG_INFO("Video rendering: %s", m_videoEnabled ? "ON" : "OFF");
                    varjo_MRSetVideoRender(m_session, m_videoEnabled ? varjo_True : varjo_False);
                    CHECK_VARJO_ERR(m_session);
                    break;
                }
                case InputAction::LockConfig: {
                    if (!m_chromakey->lockConfig()) {
                        LOG_ERROR("ERROR: Getting chroma key config lock failed.");
                    }
                } break;
                case InputAction::UnlockConfig: {
                    m_chromakey->unlockConfig();
                } break;
                case InputAction::ResetConfig: {
                    resetConfig();
                } break;
                case InputAction::StartChromaKeying: {
                    m_chromakey->toggleChromaKeying(true);
                } break;
                case InputAction::StopChromaKeying: {
                    m_chromakey->toggleChromaKeying(false);
                } break;

                case InputAction::SelectChromaKeyIndex0:
                case InputAction::SelectChromaKeyIndex1:
                case InputAction::SelectChromaKeyIndex2:
                case InputAction::SelectChromaKeyIndex3: {
                    int index = static_cast<int>(input) - static_cast<int>(InputAction::SelectChromaKeyIndex0);
                    assert(index >= 0 && index < m_chromakey->getCount());
                    m_edit.m_activeIndex = index;
                    std::string prefix = "ChromaKey config (" + std::to_string(m_edit.m_activeIndex) + "):";
                    ChromaKeyManager::print(m_edit.m_configs[m_edit.m_activeIndex], prefix);
                } break;

                case InputAction::ChangeAdjustment: {
                    m_edit.m_activeAdjustment =
                        static_cast<Adjustment>((static_cast<int>(m_edit.m_activeAdjustment) + 1) % static_cast<int>(Adjustment::Count));
                    LOG_INFO("Adjustment: %s", c_adjustmentNames.at(m_edit.m_activeAdjustment).c_str());
                } break;

                case InputAction::ToggleChromaKeyMode: {
                    auto& config = m_edit.m_configs[m_edit.m_activeIndex];
                    switch (config.type) {
                        case varjo_ChromaKeyType_Disabled: config.type = varjo_ChromaKeyType_HSV; break;
                        case varjo_ChromaKeyType_HSV: config.type = varjo_ChromaKeyType_Disabled; break;
                        default:
                            LOG_ERROR("Unsupported type: %d", static_cast<int>(config.type));
                            config.type = varjo_ChromaKeyType_Disabled;
                            break;
                    }
                    m_chromakey->setConfig(m_edit.m_activeIndex, m_edit.m_configs[m_edit.m_activeIndex]);
                } break;

                case InputAction::IncParamValue0:
                case InputAction::DecParamValue0: {
                    if (editParam) {
                        const double sign = (input == InputAction::IncParamValue0) ? 1.0 : -1.0;
                        const int component = 0;
                        if (m_edit.m_activeAdjustment == Adjustment::TargetColor) {
                            // Cyclic wrap around for hue angle
                            editParam[component] = glm::fract(editParam[component] + 1.0 + editStep[component] * sign);
                        } else {
                            editParam[component] =
                                glm::clamp(editParam[component] + editStep[component] * sign, editMinMax[component].x, editMinMax[component].y);
                        }
                        configValueEdited = true;
                    }
                } break;
                case InputAction::IncParamValue1:
                case InputAction::DecParamValue1: {
                    if (editParam) {
                        const double sign = (input == InputAction::IncParamValue1) ? 1.0 : -1.0;
                        constexpr int component = 1;
                        editParam[component] = glm::clamp(editParam[component] + editStep[component] * sign, editMinMax[component].x, editMinMax[component].y);
                        configValueEdited = true;
                    }
                } break;
                case InputAction::IncParamValue2:
                case InputAction::DecParamValue2: {
                    if (editParam) {
                        const double sign = (input == InputAction::IncParamValue2) ? 1.0 : -1.0;
                        const int component = 2;
                        editParam[component] = glm::clamp(editParam[component] + editStep[component] * sign, editMinMax[component].x, editMinMax[component].y);
                        configValueEdited = true;
                    }
                } break;

                default: {
                    // Ignore unknown input
                } break;
            }

            // Apply edited config
            if (configValueEdited) {
                m_chromakey->setConfig(m_edit.m_activeIndex, m_edit.m_configs[m_edit.m_activeIndex]);
            }

            // Sync frame
            m_varjoView->syncFrame();

            // Update scene
            m_scene->update(m_varjoView->getFrameTime(), m_varjoView->getDeltaTime(), m_varjoView->getFrameNumber(), Scene::UpdateParams());

            // Begin frame
            m_varjoView->beginFrame();

            // Render layer
            {
                // Get layer for rendering
                constexpr int layerIndex = 0;
                auto& layer = m_varjoView->getLayer(layerIndex);

                // Setup render params
                MultiLayerView::Layer::SubmitParams submitParams{};
                submitParams.submitColor = true;
                submitParams.submitDepth = false;
                submitParams.depthTestEnabled = false;
                submitParams.depthTestRangeEnabled = false;
                submitParams.depthTestRangeLimits = {0.0, -1.0};
                submitParams.chromaKeyEnabled = true;
                submitParams.alphaBlend = false;

                // Begin layer rendering
                layer.begin(submitParams);

                // Clear frame with solid yellow. That will be shown on chromakeyed areas
                const glm::vec4 bgColor{1.0f, 1.0f, 0.0f, 1.0f};
                layer.clear(bgColor);

                // Render frame
                layer.renderScene(*m_scene);

                // End layer rendering
                layer.end();
            }

            // End and submit frame
            m_varjoView->endFrame();

            // Check for new mixed reality related events
            checkEvents();
        }
    }

private:
    // Print usage help text
    void printHelp()
    {
        // Print usage text
        LOG_INFO("%s", s_usageText.c_str());
    }

    // Check for keyboard input
    InputAction checkInput()
    {
        // Check for keyboard input
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        INPUT_RECORD input;
        DWORD cnt = 0;
        while (GetNumberOfConsoleInputEvents(in, &cnt) && cnt > 0) {
            if (ReadConsoleInputA(in, &input, 1, &cnt) && cnt > 0 && input.EventType == KEY_EVENT && input.Event.KeyEvent.bKeyDown) {
                auto it = s_inputActionMapping.find(input.Event.KeyEvent.wVirtualKeyCode);
                if (it != s_inputActionMapping.end()) {
                    return it->second;
                }
            }
        }
        return InputAction::None;
    }

    // Check for Varjo API events
    void checkEvents()
    {
        varjo_Event evt{};
        while (varjo_PollEvent(m_session, &evt)) {
            switch (evt.header.type) {
                case varjo_EventType_MRDeviceStatus: {
                    switch (evt.data.mrDeviceStatus.status) {
                        case varjo_MRDeviceStatus_Connected: {
                            LOG_INFO("EVENT: Mixed reality device status: %s", "Connected");
                            // TODO: Handle this!
                            break;
                        }
                        case varjo_MRDeviceStatus_Disconnected: {
                            LOG_INFO("EVENT: Mixed reality device status: %s", "Disconnected");
                            // TODO: Handle this!
                            break;
                        }
                    }
                } break;

                case varjo_EventType_MRChromaKeyConfigChange: {
                    LOG_INFO("EVENT: Chroma key config changed");
                } break;

                default: break;
            }
        }
    }

private:
    // Varjo session
    varjo_Session* m_session = nullptr;

    // Renderer instance
    std::unique_ptr<Renderer> m_renderer;

    // Varjo view instance
    std::unique_ptr<MultiLayerView> m_varjoView;

    // Scene instance
    std::unique_ptr<DummyScene> m_scene;

    // Camera manager instance
    std::unique_ptr<CameraManager> m_camera;

    // Chroma key manager instance
    std::unique_ptr<ChromaKeyManager> m_chromakey;

    // Struct wrapping edit state
    struct EditState {
        // Currently active chroma key index
        int m_activeIndex = 0;

        // Currently active adjustment
        Adjustment m_activeAdjustment = Adjustment::TargetColor;

        // Editable configs
        std::vector<varjo_ChromaKeyConfig> m_configs;
    };

    // Edit state
    EditState m_edit;

    // Video rendering enabled flag
    bool m_videoEnabled = false;
};

//---------------------------------------------------------------------------

// Client application entry point
int main(int argc, char** argv)
{
    // Exit gracefully, when Ctrl-C signal is received
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    LOG_INFO("Varjo ChromaKey Config Tool");
    LOG_INFO("(C) 2020 Varjo Technologies");

    // Initialize the varjo session
    LOG_INFO("Initializing varjo session..");
    varjo_Session* session = varjo_SessionInit();
    CHECK_VARJO_ERR(session);

    // Instantiate test client. Client takes session ownership.
    LOG_INFO("Initializing client app..");
    auto client = std::make_unique<TestClient>(session);

    // Run client main loop
    LOG_INFO("Running client app..");
    client->run();

    // Deinitialize client app
    LOG_INFO("Deinitializing client app..");
    client.reset();

    // Shutdown the varjo session. Can't check errors anymore after this.
    LOG_INFO("Shutting down varjo session..");
    varjo_SessionShutDown(session);

    // Exit successfully
    LOG_INFO("Done!");
    return EXIT_SUCCESS;
}
