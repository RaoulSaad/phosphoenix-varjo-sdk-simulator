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

// ---------------------------------------------------------------------------
// Launcher live panel. Two tiny sections whose config blocks carry one packed
// struct each: phx_ctl (launcher -> us; we consume) and phx_stat (us ->
// launcher; we produce). Must match phx_shm.py CTL_FMT ("<iifffiii") and
// STAT_FMT ("<iiffffiiffffff").
// ---------------------------------------------------------------------------
constexpr int32_t kCtlVersion  = 1;
constexpr int32_t kStatVersion = 1;
#pragma pack(push, 1)
struct CtlBlock {
    int32_t version;          // kCtlVersion; other versions are ignored
    int32_t blindnessMode;    // BlindnessMode
    float   spotRadiusTan;    // radius of the CURRENT mode
    float   maskOpacity;      // 0..1
    float   yoloConf;         // kYoloConfMin..kYoloConfMax
    int32_t keyboardEnabled;  // 1 = keys act too; 0 = keys ignored
    int32_t habituation;      // reserved (0)
    int32_t recording;        // reserved (0)
};
struct StatBlock {
    int32_t version;          // kStatVersion
    int32_t blindnessMode;
    float   spotRadiusTan;
    float   maskOpacity;
    float   yoloConf;
    float   deviceFieldTan;
    int32_t pythonAlive;
    int32_t keyboardEnabled;  // what we are actually doing (1 when no launcher)
    float   renderFps;        // render loop frames per second, last second
    float   captureMs, shmPubMs, shmConMs, renderMs, e2eMs;   // [TIME] averages, last second
};
#pragma pack(pop)
static_assert(sizeof(CtlBlock) == 32, "CtlBlock must be 32 bytes");
static_assert(sizeof(StatBlock) == 56, "StatBlock must be 56 bytes");

struct PhospheneBridge : public IFrameSink {
    std::atomic<bool> running{false};

    phx_handle* cam  = nullptr;   // producer, created by startPhospheneBridge
    phx_handle* phos = nullptr;   // consumer, opened lazily by pollPhosOpen
    phx_handle* ctl  = nullptr;   // consumer, opened lazily by pollControl (launcher creates it)
    phx_handle* stat = nullptr;   // producer, created by startPhospheneBridge (launcher reads it)
    uint64_t    ctlSeq = 0;
    bool        ctlVersionWarned = false;
    uint64_t    ctlRetryAfterNs = 0;   // F-5: throttle reconnect attempts after a dead peer

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

// Launcher live panel. pollControl: open phx_ctl when a launcher has created
// it, apply a changed control block to the runtime atomics, and hand the
// opacity back through outOpacity (NaN when unchanged; it lives in the
// renderer). Returns true while a launcher is connected and its heartbeat is
// fresher than peerDeadAfterSeconds; when the heartbeat goes stale the section
// is dropped and gKeyboardEnabled is set back to true. Once per render frame.
bool pollControl(PhospheneBridge& bridge, double peerDeadAfterSeconds, float* outOpacity);

// Publish the status block into phx_stat (no-op when the section failed to open).
void writeStatus(PhospheneBridge& bridge, const StatBlock& s);