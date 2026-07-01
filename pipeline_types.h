////////////////////////////////////////////////////////////////////////////////
// pipeline_types.h — small shared types used across the modules.
//
// Kept deliberately free of Varjo / GL / Windows headers so every module can
// include it. The frame source, transport and renderer all speak these types.
////////////////////////////////////////////////////////////////////////////////
#pragma once

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

// Fixed implant's phosphene half-field (~24 deg). Must match params.yaml
// view_angle/2 on the Python side and generate_device_coords.py.
constexpr float kDeviceFieldTan = 0.45f;

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
