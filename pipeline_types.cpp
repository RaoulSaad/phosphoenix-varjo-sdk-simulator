////////////////////////////////////////////////////////////////////////////////
// pipeline_types.cpp — definitions for the shared config.
////////////////////////////////////////////////////////////////////////////////
#include "pipeline_types.h"

// The active blindness mode. C++ is the single source of truth; the transport
// mirrors this into the cam header so Python follows it.
BlindnessMode gBlindnessMode = BLINDNESS_FULL;

// Runtime device field; starts at the compiled default and is overridden when
// Python announces the span of its loaded phosphene map through the phos
// header (pollAnnouncedDeviceField in transport.cpp).
std::atomic<float> gDeviceFieldTan{kDeviceFieldTan};

OverlayGeometry overlayGeometryFor(BlindnessMode mode)
{
    // The device field is whatever map Python is actually running, so the crop
    // (transport) and the shader (renderer) both read the runtime value.
    const float deviceFieldTan = gDeviceFieldTan.load(std::memory_order_relaxed);

    switch (mode) {
    case BLINDNESS_GLAUCOMA:
        // Clear central tunnel (residual vision); the device's phosphenes show in
        // the blind ring between the tunnel edge and the device field (~8.5..24 deg).
        // Widen the tunnel (spotRadiusTan) for milder glaucoma.
        return OverlayGeometry{ 0.15f, 0.04f, deviceFieldTan };
    case BLINDNESS_FULL:
        // Full blindness: black everywhere; the device field is the same fixed disc.
        return OverlayGeometry{ 0.0f, 0.0f, deviceFieldTan };
    case BLINDNESS_MACULAR:
    default:
        // Black central scotoma; the fixed central device fills it.
        return OverlayGeometry{ 0.24f, 0.04f, deviceFieldTan };
    }
}
