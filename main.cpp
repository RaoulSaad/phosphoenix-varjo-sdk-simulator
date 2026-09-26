////////////////////////////////////////////////////////////////////////////////
// main.cpp — Varjo XR-4 Mixed Reality: camera pass-through + gaze dot overlay
// + left camera capture in parallel using Varjo DataStream API.
//
// Thin orchestrator wiring three modules together:
//   frame source (varjo_source) -> transport (shared memory) -> renderer (GL).
//
// Runtime architecture:
//   1. Create a tiny Win32/WGL OpenGL context so Varjo GL swapchains can be used.
//   2. Start a Varjo session, enable MR video pass-through, and initialize gaze.
//   3. Subscribe to Varjo's distorted-color camera DataStream for both eyes.
//   4. Send captured NV12 camera frames and current gaze coordinates to Python.
//   5. Receive Python-generated grayscale phosphene masks back over shared memory.
//   6. Render a transparent overlay layer containing:
//        - a gaze-centered black scotoma mask, and
//        - a gaze-centered phosphene texture from Python.
//
// Startup sequence matters: the GL context must exist before the Varjo GL
// swapchain, and the camera stream must be started before the transport so the
// initial crop sizes can be derived from the stream metadata.
////////////////////////////////////////////////////////////////////////////////

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // GetAsyncKeyState (ESC to quit)

#include <chrono>
#include <cmath>    // lround (mask-opacity logging)
#include <cstdio>
#include <vector>
#include <algorithm>

#include "phase_timers.h"

#include "pipeline_types.h"
#include "transport.h"
#include "phx_shm.h"
#include "varjo_source.h"
#include "renderer.h"

#include "display.h"
#include <cstdlib>   // atoi, atof
#include <cstring>
#include <memory>
#include <string>
#ifdef PHX_HAVE_WEBCAM
#include "webcam_source.h"
#include "window_display.h"
#endif

int main(int argc, char** argv)
{
    // OpenGL must be ready before creating Varjo GL swapchains.
    OverlayRenderer renderer;
    if (!renderer.initGL()) return 1;

    // --- mode selection -----------------------------------------------------
    bool  webcamMode = false;
    int   webcamIndex = 0;
    std::string webcamUrl;          // --webcam <url|file> instead of a device index
    int   reqW = 1280, reqH = 720;
    float hfovDeg = 70.0f;
    int   windowViews = 1;          // --stereo draws both eyes side by side
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](float def) { return (i + 1 < argc) ? (float)atof(argv[++i]) : def; };
        if (!strcmp(a, "--webcam")) {
            webcamMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const char* v = argv[++i];
                // All digits = device index; anything else = URL or file path.
                if (v[0] != '\0' && strspn(v, "0123456789") == strlen(v)) webcamIndex = atoi(v);
                else webcamUrl = v;
            }
        } else if (!strcmp(a, "--fov"))    hfovDeg = next(hfovDeg);
        else if (!strcmp(a, "--width"))    reqW = (int)next((float)reqW);
        else if (!strcmp(a, "--height"))   reqH = (int)next((float)reqH);
        else if (!strcmp(a, "--stereo"))   windowViews = NUM_EYES;
        else if (!strcmp(a, "--conf"))     gYoloConf.store(next(gYoloConf.load()));
        else {
            fprintf(stderr, "usage: %s [--conf T] [--webcam [index|url|file] [--fov DEG] [--width W] [--height H] [--stereo]]\n", argv[0]);
            return 2;
        }
    }

    std::unique_ptr<VarjoFrameSource> varjo;
#ifdef PHX_HAVE_WEBCAM
    std::unique_ptr<WebcamFrameSource> webcam;
    std::unique_ptr<WindowDisplay>     window;
#endif
    IFrameSource* sourcePtr  = nullptr;
    IDisplay*     displayPtr = nullptr;

    if (webcamMode) {
#ifdef PHX_HAVE_WEBCAM
        webcam = std::make_unique<WebcamFrameSource>(webcamIndex, reqW, reqH, hfovDeg, webcamUrl);
        sourcePtr = webcam.get();
        // The window is sized from the actual capture size, known after start();
        // created below once the source is running.
#else
        fprintf(stderr, "built without webcam support (configure with -DOpenCV_DIR)\n");
        return 2;
#endif
    } else {
        varjo = std::make_unique<VarjoFrameSource>();
        if (!varjo->initSession()) return 1;
        sourcePtr = varjo.get(); displayPtr = varjo.get();
    }
    IFrameSource& source = *sourcePtr;

    // The bridge is the sink the camera callback writes into; it also carries
    // the per-eye gaze the render loop writes each frame.
    PhospheneBridge phospheneBridge{};

    const bool capStarted = source.start(&phospheneBridge);
    if (!capStarted) {
        printf("[WARN] camera capture not started\n");
    } else {
        printf("[OK] camera capture started (L+R on shared stream)\n");
    }

#ifdef PHX_HAVE_WEBCAM
    if (webcamMode) {
        window = std::make_unique<WindowDisplay>(renderer.nativeWindow(),
                                                 source.frameWidth(0), source.frameHeight(0), hfovDeg,
                                                 windowViews);
        displayPtr = window.get();
    }
#endif
    IDisplay* display = displayPtr;

    // Startup copy only: the render loop re-fetches it every frame because the
    // device field can change when Python announces its loaded map.
    const OverlayGeometry startupGeom = overlayGeometryFor(gBlindnessMode.load());

    uint32_t camPayloadBytes = 0;
    for (int e = 0; e < NUM_EYES; ++e) {
        computePythonCropSize(
            source.frameWidth(e),
            source.frameHeight(e),
            startupGeom.phospheneRadiusTan,
            nullptr,
            phospheneBridge.cropWidth[e],
            phospheneBridge.cropHeight[e]);

        phospheneBridge.frameWidth[e]  = source.frameWidth(e);
        phospheneBridge.frameHeight[e] = source.frameHeight(e);
        phospheneBridge.rowStride[e]   = source.rowStride(e);
        // NV12: rowStride * height luma + half that chroma.
        const uint32_t bytes = (uint32_t)source.rowStride(e) * (uint32_t)source.frameHeight(e) * 3u / 2u;
        camPayloadBytes = (std::max)(camPayloadBytes, bytes);
    }
    if (camPayloadBytes == 0) camPayloadBytes = 4u * 1024 * 1024;   // no stream: still create the section

    if (!startPhospheneBridge(phospheneBridge, camPayloadBytes)) {
        printf("[WARN] shared-memory bridge not started\n");
    }

    if (!display->create()) {
        source.stop();
        return 1;
    }

    if (!renderer.setupSwapchainFbos(display->swapchainTextures(),
                                     display->atlasWidth(), display->atlasHeight())) {
        source.stop();
        return 1;
    }

    renderer.initShaders();
    renderer.initPhospheneTextures(phospheneBridge.cropWidth, phospheneBridge.cropHeight);
    renderer.setPhospheneOpacity(0.0f);   // nothing to show until Python's heartbeat arrives

    if (display->wantsPassthroughPass())
        renderer.initPassthroughTextures(source.frameWidth(0), source.frameHeight(0));
    std::vector<uint8_t> passthroughNv12;

    printf("     Parallel camera capture forwards frames to Python over shared memory\n");
    printf("     Press ESC to quit.\n");
    printf("     Keys 1-9 fade the blindness mask to 10%%-90%% so the real world\n"
           "     shows through (for checking phosphene alignment); 0 restores the\n"
           "     full simulation.\n");
    printf("     Keys M / G / F switch the blindness mode: macular / glaucoma / full.\n");
    printf("     Keys [ / ] shrink / grow the scotoma (macular) or tunnel (glaucoma)\n"
           "     radius by %.2f tan per press.\n", kSpotRadiusStepTan);
    printf("     Keys , / . lower / raise the YOLO confidence threshold by %.2f per press\n"
           "     (forwarded to Python). A [STATE] line prints on every change.\n",
           kYoloConfStep);
    printf("     Key N asks Python for the next phosphene map in its --coords list\n"
           "     (the phosphene layer goes dark for a few seconds while it rebuilds).\n");

    // Status line: one printf whenever any user-facing state changes (keys or
    // Python announcing a map), so "what am I looking at" is always the last
    // [STATE] line in the console.
    struct StateSnapshot {
        BlindnessMode mode; float spotRadiusTan; float maskOpacity; float deviceFieldTan; float yoloConf;
        bool operator!=(const StateSnapshot& o) const {
            return mode != o.mode || spotRadiusTan != o.spotRadiusTan ||
                   maskOpacity != o.maskOpacity || deviceFieldTan != o.deviceFieldTan ||
                   yoloConf != o.yoloConf;
        }
    };
    StateSnapshot lastPrinted{ (BlindnessMode)-1, -1.0f, -1.0f, -1.0f, -1.0f };   // forces the first print
    auto printStateIfChanged = [&](const StateSnapshot& s) {
        if (!(s != lastPrinted)) return;
        lastPrinted = s;
        const double rad2deg = 180.0 / 3.14159265358979;
        printf("[STATE] mode=%s spot=%.2f tan (%.1f deg) opacity=%d%% field=%.4f tan (+/-%.1f deg) conf=%.2f\n",
               blindnessModeName(s.mode),
               s.spotRadiusTan, std::atan(s.spotRadiusTan) * rad2deg,
               (int)lround(s.maskOpacity * 100.0f),
               s.deviceFieldTan, std::atan(s.deviceFieldTan) * rad2deg,
               s.yoloConf);
    };

    // One press = one step, for keys that accumulate (a held key would
    // otherwise sweep the whole range in a second). Returns true on the
    // down-edge only.
    struct EdgeKey {
        int vk; bool prevDown = false;
        bool pressed() {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool edge = down && !prevDown;
            prevDown = down;
            return edge;
        }
    };
    auto stepAtomic = [](std::atomic<float>& v, float delta, float lo, float hi) {
        v.store((std::min)((std::max)(v.load() + delta, lo), hi));
    };
    EdgeKey keyRadiusDown{ VK_OEM_4 }, keyRadiusUp{ VK_OEM_6 };       // '[' ']'
    EdgeKey keyConfDown{ VK_OEM_COMMA }, keyConfUp{ VK_OEM_PERIOD };  // ',' '.'
    EdgeKey keyMapNext{ 'N' };

    // Reused scratch for consumed phosphene bytes; the render thread is single.
    std::vector<uint8_t> phospheneGray;

    auto lastTimerPrint = std::chrono::steady_clock::now();

    // Liveness: Python heartbeats the sections it uses; if it stops for longer
    // than this the phosphene layer is faded out rather than freezing on the
    // last image, and restored when it returns.
    constexpr double kPythonDeadAfterSeconds = 2.0;
    bool pythonWasAlive = false;

    while (true) {
        // Simple local exit condition for the sample program.
        if (display->shouldQuit()) break;

        // Alignment aid: dim the mask so the MR passthrough is visible behind
        // the phosphenes. Assigning the same value repeatedly is harmless, so
        // no key-edge detection is needed.
        for (int k = 0; k <= 9; ++k) {
            if (GetAsyncKeyState('0' + k) & 0x8000) {
                const float opacity = (k == 0) ? 1.0f : (float)k * 0.1f;
                if (opacity != renderer.maskOpacity()) renderer.setMaskOpacity(opacity);
            }
        }

        // Blindness mode: direct-set keys, so repeated assignment is harmless
        // (no edge detection). Camera threads read the mode too, hence atomic.
        {
            static const struct { int key; BlindnessMode mode; } kModeKeys[] = {
                { 'M', BLINDNESS_MACULAR  },
                { 'G', BLINDNESS_GLAUCOMA },
                { 'F', BLINDNESS_FULL     },
            };
            for (const auto& mk : kModeKeys) {
                if ((GetAsyncKeyState(mk.key) & 0x8000) && gBlindnessMode.load() != mk.mode)
                    gBlindnessMode.store(mk.mode);
            }
        }

        // Scotoma / tunnel radius of the current mode. No effect in full
        // blindness, which has no boundary (the edge state is still consumed
        // so the press does not fire later after a mode switch).
        {
            const BlindnessMode m = gBlindnessMode.load();
            const bool dn = keyRadiusDown.pressed(), up = keyRadiusUp.pressed();
            if (m != BLINDNESS_FULL) {
                if (dn) stepAtomic(gSpotRadiusTan[m], -kSpotRadiusStepTan, kSpotRadiusMinTan, kSpotRadiusMaxTan);
                if (up) stepAtomic(gSpotRadiusTan[m], +kSpotRadiusStepTan, kSpotRadiusMinTan, kSpotRadiusMaxTan);
            }
        }

        // YOLO confidence threshold; the transport mirrors it into the cam
        // header on the next camera frame and Python applies it per dispatch.
        if (keyConfDown.pressed()) stepAtomic(gYoloConf, -kYoloConfStep, kYoloConfMin, kYoloConfMax);
        if (keyConfUp.pressed())   stepAtomic(gYoloConf, +kYoloConfStep, kYoloConfMin, kYoloConfMax);

        // Next phosphene map: Python owns the list; we only count presses.
        if (keyMapNext.pressed()) {
            const int32_t n = gMapRequest.fetch_add(1) + 1;
            printf("[MAP] next map requested (#%d); waiting for Python to rebuild\n", n);
        }

        display->pollEvents();
        display->waitSync();

        // Adopt the device field of whatever map Python announced (no-op until
        // it does); the camera threads then derive the matching crop and the
        // geometry below drives the shader with the same span.
        bridgeHeartbeat(phospheneBridge);
        pollPhosOpen(phospheneBridge);
        pollAnnouncedDeviceField(phospheneBridge);
        {
            const bool alive = pythonAlive(phospheneBridge, kPythonDeadAfterSeconds);
            if (alive && !pythonWasAlive) {
                renderer.setPhospheneOpacity(1.0f);
                printf("[OK] Python alive: phosphene layer on\n");
            } else if (!alive && pythonWasAlive) {
                renderer.setPhospheneOpacity(0.0f);
                printf("[WARN] Python heartbeat stopped (> %.0f s): phosphene layer off until it returns\n",
                       kPythonDeadAfterSeconds);
            }
            pythonWasAlive = alive;
        }
        // Load once per frame so the geometry and the shader uniform agree.
        const BlindnessMode   mode = gBlindnessMode.load();
        const OverlayGeometry geom = overlayGeometryFor(mode);
        printStateIfChanged(StateSnapshot{ mode, geom.spotRadiusTan,
                                           renderer.maskOpacity(), geom.phospheneRadiusTan,
                                           gYoloConf.load() });

    #ifdef PHX_HAVE_WEBCAM
        if (webcam && window) {
            float gx, gy;
            if (window->gazeTan(gx, gy)) webcam->setGaze(gx, gy);
            int pw, ph, ps;
            if (webcam->latestFrame(passthroughNv12, pw, ph, ps))
                renderer.uploadPassthrough(passthroughNv12.data(), pw, ph, ps);
        }
    #endif
        // Per-eye gaze in tangent space (keeps last-known values while invalid).
        const GazeTan gaze = source.getGaze();

    

        // Share per-eye gaze with bridge (camera capture threads read these)
        for (int e = 0; e < NUM_EYES; ++e) {
            phospheneBridge.gazeTanX[e].store(gaze.x[e]);
            phospheneBridge.gazeTanY[e].store(gaze.y[e]);
        }

        // Consume the newest phosphene image per eye and upload it. Done
        // before the frame block so the phases stay disjoint: the SHM copy
        // reports itself (shm_con inside consumePhosphene), the GL upload
        // counts toward "render". Upload only touches our own texture, so it
        // is safe outside the begin/submit bracket.
        long long renderNs = 0;
        for (int e = 0; e < NUM_EYES; ++e) {
            int w = 0, h = 0; uint64_t frameNumber = 0, captureTs = 0;
            if (consumePhosphene(phospheneBridge, e, phospheneGray, w, h, frameNumber, captureTs)) {
                const auto tUpload = std::chrono::steady_clock::now();
                renderer.uploadPhosphene(e, phospheneGray.data(), w, h);
                renderNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - tUpload).count();
                // End-to-end: camera capture (stamped by commitFrame) -> phosphene on GPU.
                if (captureTs != 0) {
                    const uint64_t nowNs = phx_now_ns();
                    if (nowNs > captureTs) gPhaseTimers.add(TIMER_E2E, (long long)(nowNs - captureTs));
                }
            }
        }

        const auto tFrame = std::chrono::steady_clock::now();
        display->beginFrame();
        const int sci = display->acquireSwapchainImage();
        renderer.beginFrame(sci);

        for (int i = 0; i < display->viewCount(); i++) {
            const int eyeIdx = display->viewIndexToEye(i);
            if (display->wantsPassthroughPass()) 
                renderer.drawPassthrough(display->viewport(i));

            renderer.drawView(
                display->viewport(i),
                display->tangents(i),
                gaze.x[eyeIdx],
                gaze.y[eyeIdx],
                eyeIdx,
                mode,
                geom);
        }

        renderer.endFrame();
        display->releaseSwapchainImage();
        display->endFrameAndSubmit();

        renderNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tFrame).count();
        gPhaseTimers.add(TIMER_RENDER, renderNs);

        // Averaged phase latencies, once per second (mirrors Python's [TIME]).
        const auto tNow = std::chrono::steady_clock::now();
        if (tNow - lastTimerPrint >= std::chrono::seconds(1)) {
            gPhaseTimers.printAndReset();
            lastTimerPrint = tNow;
        }
    }

    printf("Shutting down...\n");

    // Teardown order: stop camera stream -> stop transport -> free GL objects
    // -> shut down the Varjo session -> destroy the GL context last.
    source.stop();
    stopPhospheneBridge(phospheneBridge);
    renderer.shutdownGL();
    display->destroy();
    renderer.destroyContext();
    printf("Done.\n");
    return 0;
}
