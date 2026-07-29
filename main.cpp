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

#include <cmath>    // lround (mask-opacity logging)
#include <cstdio>
#include <vector>

#include "pipeline_types.h"
#include "transport.h"
#include "varjo_source.h"
#include "renderer.h"

int main()
{
    // OpenGL must be ready before creating Varjo GL swapchains.
    OverlayRenderer renderer;
    if (!renderer.initGL()) return 1;

    VarjoFrameSource source;
    if (!source.initSession()) return 1;

    // The camera saver threads feed every captured frame straight into the
    // shared-memory transport; the bridge also carries the per-eye gaze the
    // render loop writes each frame.
    PhospheneBridge phospheneBridge{};

    const bool capStarted = source.start(
        [&phospheneBridge](const CameraFrame& frame, int eye) {
            publishCameraFrame(phospheneBridge, frame, eye);
        });
    if (!capStarted) {
        printf("[WARN] camera capture not started\n");
    } else {
        printf("[OK] camera capture started (L+R on shared stream)\n");
    }

    // Overlay geometry depends on the blindness type (see overlayGeometryFor):
    //   Macular  -> black scotoma + phosphenes at the gaze centre.
    //   Glaucoma -> clear central tunnel + phosphenes in the surrounding ring.
    // Both keep the phosphenes anchored to gaze; only the mask/radii differ.
    const OverlayGeometry geom = overlayGeometryFor(gBlindnessMode);

    for (int e = 0; e < NUM_EYES; ++e) {
        // Before real frames arrive, initialize crop/texture sizes from stream
        // metadata. Later frames with intrinsics may refine these values.
        computePythonCropSize(
            source.frameWidth(e),
            source.frameHeight(e),
            geom.phospheneRadiusTan,
            nullptr,
            phospheneBridge.cropWidth[e],
            phospheneBridge.cropHeight[e]);

        phospheneBridge.frameWidth[e]  = source.frameWidth(e);
        phospheneBridge.frameHeight[e] = source.frameHeight(e);
        phospheneBridge.rowStride[e]   = source.rowStride(e);
    }

    if (!startPhospheneBridge(phospheneBridge)) {
        printf("[WARN] UDP phosphene bridge not started\n");
    } else {
        /* config will be sent with the first camera frame (per eye) */
    }

    if (!source.createSwapchain()) {
        source.stop();
        return 1;
    }

    if (!renderer.setupSwapchainFbos(source.swapchainTextures(),
                                     source.atlasWidth(), source.atlasHeight())) {
        source.stop();
        return 1;
    }

    renderer.initShaders();
    renderer.initPhospheneTextures(phospheneBridge.cropWidth, phospheneBridge.cropHeight);

    printf("     Parallel camera capture forwards frames to Python over shared memory\n");
    printf("     Press ESC to quit.\n");
    printf("     Keys 1-9 fade the blindness mask to 10%%-90%% so the real world\n"
           "     shows through (for checking phosphene alignment); 0 restores the\n"
           "     full simulation.\n");

    // Reused scratch for consumed phosphene bytes; the render thread is single.
    std::vector<uint8_t> phospheneGray;

    while (true) {
        // Simple local exit condition for the sample program.
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        // Alignment aid: dim the mask so the MR passthrough is visible behind
        // the phosphenes. Assigning the same value repeatedly is harmless, so
        // no key-edge detection is needed.
        for (int k = 0; k <= 9; ++k) {
            if (GetAsyncKeyState('0' + k) & 0x8000) {
                const float opacity = (k == 0) ? 1.0f : (float)k * 0.1f;
                if (opacity != renderer.maskOpacity()) {
                    renderer.setMaskOpacity(opacity);
                    printf("[VIEW] blindness mask opacity = %d%% (passthrough %d%%)\n",
                           (int)lround(opacity * 100.0f),
                           (int)lround((1.0f - opacity) * 100.0f));
                }
            }
        }

        source.pollEvents();
        source.waitSync();

        // Per-eye gaze in tangent space (keeps last-known values while invalid).
        const GazeTan gaze = source.getGaze();

        // Share per-eye gaze with bridge (camera capture threads read these)
        for (int e = 0; e < NUM_EYES; ++e) {
            phospheneBridge.gazeTanX[e].store(gaze.x[e]);
            phospheneBridge.gazeTanY[e].store(gaze.y[e]);
        }

        source.beginFrame();
        const int sci = source.acquireSwapchainImage();
        renderer.beginFrame(sci);

        // Upload latest phosphene texture for each eye
        for (int e = 0; e < NUM_EYES; ++e) {
            int w = 0, h = 0;
            if (consumePhosphene(phospheneBridge, e, phospheneGray, w, h)) {
                renderer.uploadPhosphene(e, phospheneGray.data(), w, h);
            }
        }

        for (int i = 0; i < source.viewCount(); i++) {
            const int eyeIdx = source.viewIndexToEye(i);
            renderer.drawView(
                source.viewport(i),
                source.tangents(i),
                gaze.x[eyeIdx],
                gaze.y[eyeIdx],
                eyeIdx,
                gBlindnessMode,
                geom);
        }

        renderer.endFrame();
        source.releaseSwapchainImage();
        source.endFrameAndSubmit();
    }

    printf("Shutting down...\n");

    // Teardown order: stop camera stream -> stop transport -> free GL objects
    // -> shut down the Varjo session -> destroy the GL context last.
    source.stop();
    stopPhospheneBridge(phospheneBridge);
    renderer.shutdownGL();
    source.shutdown();
    renderer.destroyContext();
    printf("Done.\n");
    return 0;
}
