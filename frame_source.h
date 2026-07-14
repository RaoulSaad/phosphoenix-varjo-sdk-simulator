////////////////////////////////////////////////////////////////////////////////
// frame_source.h — the camera-source seam.
//
// Abstracts "something that produces per-eye camera frames and gaze" so the
// Varjo headset input can be swapped for a webcam when working remote. Only
// the capture side is behind this interface; Varjo-XR display methods live on
// VarjoFrameSource directly (a webcam build has no headset to submit to).
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <functional>

#include "pipeline_types.h"

// Delivered from the source's capture thread; wire to transport's
// publishCameraFrame.
using FrameCallback = std::function<void(const CameraFrame&, int eye)>;

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual bool start(FrameCallback onFrame) = 0;  // begin capture; frames via callback
    virtual void stop() = 0;
    virtual GazeTan getGaze() = 0;                   // per-eye gaze in tangent space
    // Stream metadata (known after start) for initial crop/texture sizing:
    virtual int frameWidth(int eye) const = 0;
    virtual int frameHeight(int eye) const = 0;
    virtual int rowStride(int eye) const = 0;
};
