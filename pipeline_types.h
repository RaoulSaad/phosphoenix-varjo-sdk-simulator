////////////////////////////////////////////////////////////////////////////////
// pipeline_types.h — small shared types used across the modules.
//
// Kept deliberately free of Varjo / GL / Windows headers so every module can
// include it. The frame source, transport and renderer all speak these types.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

constexpr int NUM_EYES = 2;   // 0 = left, 1 = right

// ---------------------------------------------------------------------------
// Blindness mode + overlay geometry
// ---------------------------------------------------------------------------

enum BlindnessMode {
    BLINDNESS_MACULAR  = 0,
    BLINDNESS_GLAUCOMA = 1,
    BLINDNESS_FULL     = 2
};

// Single source of truth for the active blindness type (defined in
// pipeline_types.cpp). C++ owns it; Python follows it via the cam header.
extern BlindnessMode gBlindnessMode;

// Fixed implant's phosphene half-field, as tan(half-FOV). This one constant
// sets BOTH ends of the loop: computePythonCropSize() asks Python for a camera
// crop spanning +/- this tangent, and the phosphene shader draws the image it
// gets back across +/- this same tangent. So it must equal tan(view_angle / 2)
// of the simulator producing that image AND the angular extent of that
// simulator's phosphene coordinate map -- all three, or the world arrives at
// the wrong scale (a crop wider than the array looks zoomed out).
//   run_varjo.py -> config_viseon/simulator_config.yaml : view_angle 48
//     for the +/-24 deg maps (generate_lgn_map.py output, or the viseon map
//     with --phosphene-map-scale 3). For the unscaled viseon map (+/-7.97 deg)
//     use 0.1405f (tan 8) + view_angle 16 instead.
//   PhospheneHandler_intrinsics.py -> params.yaml + grid_coords_full_field.yaml
//     (+/-24 deg array) also matches this value.
// run_varjo.py re-derives this from the crop size and warns on mismatch.
constexpr float kDeviceFieldTan = 0.4452f;   // tan(24 deg) -> +/-24 deg device field

// Runtime device field, initialised to kDeviceFieldTan. Python derives the
// real span from the phosphene map it loaded and announces it through the
// phos SHM header (see pollAnnouncedDeviceField in transport.h); once adopted,
// the camera crop and the shader both follow it — no rebuild needed when the
// map changes. Atomic: written by the render loop, read by camera threads.
extern std::atomic<float> gDeviceFieldTan;

struct OverlayGeometry {
    float spotRadiusTan;        // scotoma (macular) / clear tunnel (glaucoma) boundary
    float softEdgeTan;          // feather width of that boundary
    float phospheneRadiusTan;   // gaze-centred phosphene disc (the device field)
};

OverlayGeometry overlayGeometryFor(BlindnessMode mode);

// ---------------------------------------------------------------------------
// Camera frame + gaze + view geometry (source/renderer-agnostic)
// ---------------------------------------------------------------------------

// One CPU camera frame for a single eye, in NV12. Produced by an IFrameSource,
// consumed by the transport.
struct CameraFrame {
    int eye = 0;
    int width = 0;
    int height = 0;
    int rowStride = 0;
    int64_t frameNumber = 0;
    bool hasIntrinsics = false;
    int intrinsicsModel = 0;
    double focalLengthX = 0.0;
    double focalLengthY = 0.0;
    double principalPointX = 0.0;
    double principalPointY = 0.0;
    double distortionCoefficients[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::vector<uint8_t> nv12;
};

// Per-eye gaze in tangent space (tan of the angle from view forward).
struct GazeTan {
    float x[NUM_EYES] = {0.0f, 0.0f};
    float y[NUM_EYES] = {0.0f, 0.0f};
    bool  valid = false;
};

// A pixel rectangle in the swapchain atlas (a single packed view).
struct Viewport {
    int x = 0, y = 0, width = 0, height = 0;
};

// A view's FOV expressed as edge tangents (matches Varjo's FovTangents).
struct ViewTangents {
    float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
};
