////////////////////////////////////////////////////////////////////////////////
// transport.h — shared-memory bridge to Python (the pipeline seam).
//
// Two named, pagefile-backed sections, one per direction:
//   cam  (C++ -> Python) : raw NV12 camera frames + per-frame gaze/metadata
//   phos (Python -> C++) : 8-bit grayscale phosphene images
//
// The SHM wire layout (headers, control blocks, slots) is an implementation
// detail of transport.cpp; this header only exposes the bridge state and the
// operations the rest of the program needs. No Varjo, GL or Windows headers.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include "pipeline_types.h"

struct PhospheneBridge {
    // Owns the two shared-memory sections and the state shared between:
    //   - camera saver/sender threads (produce into the cam section),
    //   - the render loop (consumes the phos section, writes gaze).
    std::atomic<bool> running{false};

    void* camMap  = nullptr;   // HANDLE, C++ -> Python (producer)
    void* phosMap = nullptr;   // HANDLE, Python -> C++ (consumer)
    uint8_t* camBase  = nullptr;
    uint8_t* phosBase = nullptr;

    // Per-eye publish/consume cursors (process-local, not shared).
    uint32_t camWriteIndex[NUM_EYES] = {0, 0};   // producer's next cam slot
    uint32_t phosLastSeq[NUM_EYES]   = {0, 0};   // last phos publishSeq we consumed

    // Gaze values are atomics because the render loop writes them while the
    // camera sender threads read them to annotate outgoing frames.
    std::atomic<float> gazeTanX[NUM_EYES];   // [0]=L, [1]=R
    std::atomic<float> gazeTanY[NUM_EYES];

    int cropWidth[NUM_EYES]    = {0, 0};
    int cropHeight[NUM_EYES]   = {0, 0};
    int frameWidth[NUM_EYES]   = {0, 0};
    int frameHeight[NUM_EYES]  = {0, 0};
    int rowStride[NUM_EYES]    = {0, 0};

    std::chrono::steady_clock::time_point lastSendTime[NUM_EYES];

    PhospheneBridge() {
        for (int e = 0; e < NUM_EYES; ++e) {
            gazeTanX[e].store(0.0f);
            gazeTanY[e].store(0.0f);
            lastSendTime[e] = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        }
    }
};

// Convert a tangent-space crop radius into camera pixels, preferring the
// frame's intrinsics; falls back to an approximate camera FOV when absent.
// Used by main's startup sizing (frame == nullptr) and per published frame.
void computePythonCropSize(
    int frameWidth,
    int frameHeight,
    float cropRadiusTan,
    const CameraFrame* frame,
    int& outCropWidth,
    int& outCropHeight);

// Create/map both sections, reset the per-eye control blocks, publish the
// initial per-eye config (from the bridge's crop/frame sizes), set READY last.
bool startPhospheneBridge(PhospheneBridge& bridge);

// Signal SHUTDOWN to Python via both headers, then unmap/close the sections.
void stopPhospheneBridge(PhospheneBridge& bridge);

// Publish one camera frame (+ current gaze and config) to Python. Called from
// the per-eye camera saver threads.
void publishCameraFrame(PhospheneBridge& bridge, const CameraFrame& frame, int eye);

// Consume the latest phosphene image for one eye if it is newer than the last
// call (render thread only). Returns bytes + dimensions; no GL here — the
// texture upload is the renderer's half of the old uploadLatestPhospheneTexture.
bool consumePhosphene(PhospheneBridge& bridge, int eye,
                      std::vector<uint8_t>& outGray, int& outWidth, int& outHeight);
