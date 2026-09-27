////////////////////////////////////////////////////////////////////////////////
// pipeline_types.cpp — definitions for the shared config.
////////////////////////////////////////////////////////////////////////////////
#include "pipeline_types.h"

// The active blindness mode. C++ is the single source of truth; the transport
// mirrors this into the cam header so Python follows it.
std::atomic<BlindnessMode> gBlindnessMode{BLINDNESS_MACULAR};

// Runtime device field; starts at the compiled default and is overridden when
// Python announces the span of its loaded phosphene map through the phos
// header (pollAnnouncedDeviceField in transport.cpp).
std::atomic<float> gDeviceFieldTan{kDeviceFieldTan};

// Startup radii. Macular: black central scotoma the central device fills.
// Glaucoma: clear central tunnel (residual vision); phosphenes show in the
// blind ring between the tunnel edge and the device field (~8.5..24 deg).
// Larger = more severe macular / milder glaucoma. Live-adjustable ([ / ]).
std::atomic<float> gSpotRadiusTan[NUM_BLINDNESS_MODES] = {
    0.24f,   // BLINDNESS_MACULAR
    0.15f,   // BLINDNESS_GLAUCOMA
    0.0f,    // BLINDNESS_FULL (unused: black everywhere)
};

std::atomic<float> gYoloConf{kYoloConfDefault};
std::atomic<bool> gQuitRequested{false};

const char* blindnessModeName(BlindnessMode mode)
{
    switch (mode) {
    case BLINDNESS_MACULAR:  return "macular";
    case BLINDNESS_GLAUCOMA: return "glaucoma";
    case BLINDNESS_FULL:     return "full";
    default:                 return "?";
    }
}

OverlayGeometry overlayGeometryFor(BlindnessMode mode)
{
    // The device field is whatever map Python is actually running, so the crop
    // (transport) and the shader (renderer) both read the runtime value.
    const float deviceFieldTan = gDeviceFieldTan.load(std::memory_order_relaxed);

    switch (mode) {
    case BLINDNESS_GLAUCOMA:
        return OverlayGeometry{ gSpotRadiusTan[BLINDNESS_GLAUCOMA].load(std::memory_order_relaxed),
                                0.04f, deviceFieldTan };
    case BLINDNESS_FULL:
        // Full blindness: black everywhere; the device field is the same fixed disc.
        return OverlayGeometry{ 0.0f, 0.0f, deviceFieldTan };
    case BLINDNESS_MACULAR:
    default:
        return OverlayGeometry{ gSpotRadiusTan[BLINDNESS_MACULAR].load(std::memory_order_relaxed),
                                0.04f, deviceFieldTan };
    }
}
