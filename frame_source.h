////////////////////////////////////////////////////////////////////////////////
// frame_source.h — the camera-source seam.
//
// A source produces per-eye camera frames and gaze. It writes each frame
// straight into a buffer the sink hands out (beginFrame), then commits it with
// the frame's metadata. No intermediate copy: for the Varjo source the memcpy
// out of Varjo's locked stream buffer lands directly in shared memory.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstdint>

#include "pipeline_types.h"

class IFrameSink {
    public:
        virtual ~IFrameSink() = default;
        // A writable buffer for `eye` of at least *capacity bytes, or nullptr if
        // the sink is not accepting frames right now (drop the frame).
        virtual uint8_t* beginFrame(int eye, uint32_t* capacity) = 0;
        // Publish the frame written into the buffer from beginFrame. `meta`
        // carries dimensions, frame number and intrinsics; byteSize is the count
        // of bytes written.
        virtual void commitFrame(int eye, const CameraFrame& meta, uint32_t byteSize) = 0;
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual bool start(IFrameSink* sink) = 0;           // begin capture; frames via callback
    virtual void stop() = 0;
    virtual GazeTan getGaze() = 0;                      // per-eye gaze in tangent space
    // Stream metadata (known after start) for initial crop/texture sizing:
    virtual int frameWidth(int eye) const = 0;
    virtual int frameHeight(int eye) const = 0;
    virtual int rowStride(int eye) const = 0;
};