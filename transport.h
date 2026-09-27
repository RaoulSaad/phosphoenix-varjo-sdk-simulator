////////////////////////////////////////////////////////////////////////////////
// transport.h — shared-memory bridge to Python, built on phx_shm.
//
//   phx_cam  (C++ producer -> Python consumer): NV12 camera frames + gaze,
//            with the per-eye config (crop, intrinsics, blindness mode) in
//            the section's config block.
//   phx_phos (Python producer -> C++ consumer): 8-bit phosphene images, with
//            Python's device-field announcement in the section's field block.
//
// The bridge is the IFrameSink the camera source writes into.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "frame_source.h"
#include "pipeline_types.h"

struct phx_handle;

#pragma pack(push, 1)
struct ShmEyeConfig {
    int32_t cropWidth;
    int32_t cropHeight;
    int32_t frameWidth;
    int32_t frameHeight;
    int32_t rowStride;
    int32_t intrinsicsModel;
    int32_t intrinsicsValid;
    int32_t reserved;
    double  focalLengthX;
    double  focalLengthY;
    double  principalPointX;
    double  principalPointY;
    double  distortionCoefficients[8];
};
// Must match phx_shm.py CAM_CONFIG_FMT ("<if" + "8i12d" * 2), 264 bytes.
struct CamConfigBlob {
    int32_t       blindnessMode;
    float         yoloConf;       // live YOLO confidence threshold (negative = not set)
    ShmEyeConfig  eye[NUM_EYES];
};
#pragma pack(pop)
static_assert(sizeof(ShmEyeConfig) == 128, "ShmEyeConfig must be 128 bytes");
static_assert(sizeof(CamConfigBlob) == 264, "CamConfigBlob must be 264 bytes");

struct PhospheneBridge : public IFrameSink {
    std::atomic<bool> running{false};

    phx_handle* cam  = nullptr;   // producer, created by startPhospheneBridge
    phx_handle* phos = nullptr;   // consumer, opened lazily by pollPhosOpen

    uint64_t phosLastSeq[NUM_EYES] = {0, 0};
    uint64_t fieldSeq = 0;        // last device-field announcement adopted

    // Written by the render loop, read by the capture thread when stamping frames.
    std::atomic<float> gazeTanX[NUM_EYES];
    std::atomic<float> gazeTanY[NUM_EYES];

    int cropWidth[NUM_EYES]    = {0, 0};
    int cropHeight[NUM_EYES]   = {0, 0};
    int frameWidth[NUM_EYES]   = {0, 0};
    int frameHeight[NUM_EYES]  = {0, 0};
    int rowStride[NUM_EYES]    = {0, 0};

    // Last config written to the cam section, for change detection.
    std::mutex     configMutex;
    CamConfigBlob  lastConfig{};
    bool           configWritten = false;

    PhospheneBridge() {
        for (int e = 0; e < NUM_EYES; ++e) { gazeTanX[e].store(0.0f); gazeTanY[e].store(0.0f); }
    }

    // IFrameSink
    uint8_t* beginFrame(int eye, uint32_t* capacity) override;
    void     commitFrame(int eye, const CameraFrame& meta, uint32_t byteSize) override;
};

// Convert a tangent-space crop radius into camera pixels, preferring the
// frame's intrinsics; falls back to an approximate camera FOV when absent.
void computePythonCropSize(
    int frameWidth,
    int frameHeight,
    float cropRadiusTan,
    const CameraFrame* frame,
    int& outCropWidth,
    int& outCropHeight);

// Create the cam section (2 channels, 3 slots, camPayloadBytes per slot) and
// publish the initial config. Python creates phos; see pollPhosOpen.
bool startPhospheneBridge(PhospheneBridge& bridge, uint32_t camPayloadBytes);

// Flag shutdown on cam, close both sections.
void stopPhospheneBridge(PhospheneBridge& bridge);

// Try to open the phos section if Python has created it. Cheap when it is
// not there yet; a no-op once open. Returns true when phos is open.
bool pollPhosOpen(PhospheneBridge& bridge);

// Copy the newest phosphene image for `eye` if newer than the last call
// (render thread only). False if nothing new, torn, or phos not open.
bool consumePhosphene(PhospheneBridge& bridge, int eye,
                      std::vector<uint8_t>& outGray, int& outWidth, int& outHeight,
                      uint64_t& outFrameNumber, uint64_t& outCaptureTs);

// Adopt Python's announced device field into gDeviceFieldTan. Once per frame.
bool pollAnnouncedDeviceField(PhospheneBridge& bridge);

// Liveness. Call bridgeHeartbeat once per render frame.
void bridgeHeartbeat(PhospheneBridge& bridge);
bool pythonAlive(PhospheneBridge& bridge, double maxAgeSeconds);