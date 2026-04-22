// Copyright 2019-2020 Varjo Technologies Oy. All rights reserved.

/* Marker Example Application
 *
 * - Showcases Varjo Marker tracking.
 */

// Standard includes
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <atomic>

// Varjo includes
#include <Varjo.h>
#include <Varjo_events.h>
#include <Varjo_mr.h>

// Internal includes

#include "Globals.hpp"
#include "D3D11Renderer.hpp"
#include "D3D11MultiLayerView.hpp"

#include "MarkerScene.hpp"

// VarjoExamples namespace contains simple example wrappers for using Varjo API features.
// These are only meant to be used in SDK example applications. In your own application,
// use your own production quality integration layer.
using namespace VarjoExamples;

//---------------------------------------------------------------------------

namespace
{
// Input actions
enum class InputAction {
    None,         //
    Quit,         //
    LockMarkers,  //
    IncreaseMarkerVolume,
    DecreaseMarkerVolume
};

// Input action name mappings
const std::unordered_map<InputAction, std::string> c_inputNames = {
    {InputAction::None, "None"},
    {InputAction::Quit, "Quit"},
    {InputAction::LockMarkers, "Lock Markers"},
    {InputAction::IncreaseMarkerVolume, "Increase Marker Volume"},
    {InputAction::DecreaseMarkerVolume, "Decrease Marker Volume"},
};

// Virtual key to input mappings
// https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
const std::unordered_map<WORD, InputAction> s_inputActionMapping = {
    {VK_ESCAPE, InputAction::Quit},                //
    {VK_SPACE, InputAction::LockMarkers},          //
    {VK_UP, InputAction::IncreaseMarkerVolume},    //
    {VK_DOWN, InputAction::DecreaseMarkerVolume},  //
};

std::atomic_bool ctrlCPressed = false;

BOOL WINAPI ctrlHandler(DWORD /*dwCtrlType*/)
{
    ctrlCPressed = true;
    return TRUE;
}
}  // namespace

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
        m_scene = std::make_unique<MarkerScene>(m_session, *m_renderer);

        // Check if Mixed Reality features are available.
        varjo_Bool mixedRealityAvailable = varjo_False;
        varjo_SyncProperties(m_session);
        if (varjo_HasProperty(m_session, varjo_PropertyKey_MRAvailable)) {
            mixedRealityAvailable = varjo_GetPropertyBool(m_session, varjo_PropertyKey_MRAvailable);
        }

        if (mixedRealityAvailable == varjo_False) {
            LOG_ERROR("ERROR: Varjo Mixed Reality features not available!");
            exit(EXIT_FAILURE);
        }

        // Start video-see-through. VR content will be rendered on top of the video feed.
        varjo_MRSetVideoRender(m_session, varjo_True);
        if (CHECK_VARJO_ERR(m_session) == varjo_NoError) {
            LOG_INFO("VST rendering enabled.");
        }

        varjo_MRSetVideoDepthEstimation(m_session, varjo_True);
        if (CHECK_VARJO_ERR(m_session) == varjo_NoError) {
            LOG_INFO("VST depth estimation enabled.");
        }
    }

    // Destructor
    ~TestClient()
    {
        // Stop video-see-through.
        varjo_MRSetVideoRender(m_session, varjo_False);
        if (CHECK_VARJO_ERR(m_session) == varjo_NoError) {
            LOG_INFO("VST rendering disabled.");
        }

        // Free scene resources
        m_scene.reset();

        // Free view resources
        m_varjoView.reset();

        // Free renderer resources
        m_renderer.reset();

        // Reset session
        m_session = nullptr;
    }

    // Disable copy and assign
    TestClient(const TestClient& other) = delete;
    TestClient(const TestClient&& other) = delete;
    TestClient& operator=(const TestClient& other) = delete;
    TestClient& operator=(const TestClient&& other) = delete;

    // Client main loop
    void run()
    {
        InputAction input = InputAction::None;

        // Main loop
        while (true) {
            // Check for keyboard input
            input = checkInput();
            assert(c_inputNames.count(input));
            std::string inputName = c_inputNames.at(input);

            // Check for quit and Ctrl-C
            if (input == InputAction::Quit || ctrlCPressed) {
                LOG_INFO("Quitting main loop..");
                break;
            }

            if (input == InputAction::LockMarkers) {
                m_scene.get()->LockMarkerToggle();
            }

            if (input == InputAction::IncreaseMarkerVolume || input == InputAction::DecreaseMarkerVolume) {
                m_scene.get()->UpdateMarkerVolume(input == InputAction::IncreaseMarkerVolume);
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

                // Setup layer submit params
                MultiLayerView::Layer::SubmitParams submitParams{};
                submitParams.submitColor = true;
                submitParams.submitDepth = true;
                submitParams.alphaBlend = true;
                submitParams.depthTestEnabled = false;
                submitParams.depthTestRangeEnabled = false;
                submitParams.depthTestRangeLimits = {0.0, -1.0};
                submitParams.chromaKeyEnabled = false;

                // Begin layer rendering
                layer.begin(submitParams);

                // Clear frame
                layer.clear();

                // Render frame
                layer.renderScene(*m_scene);

                // End layer rendering
                layer.end();
            }

            // End and submit frmae
            m_varjoView->endFrame();
        }
    }

private:
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

private:
    // Varjo session
    varjo_Session* m_session = nullptr;

    // Renderer instance
    std::unique_ptr<Renderer> m_renderer;

    // Varjo view instance
    std::unique_ptr<MultiLayerView> m_varjoView;

    // MarkerScene instance
    std::unique_ptr<MarkerScene> m_scene;
};

//---------------------------------------------------------------------------

// Client application entry point
int main(int argc, char** argv)
{
    // Exit gracefully, when Ctrl-C signal is received
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    LOG_INFO("Varjo Marker Test Client");
    LOG_INFO("(C) 2019-2020 Varjo Technologies");

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
