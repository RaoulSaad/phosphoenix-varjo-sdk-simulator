////////////////////////////////////////////////////////////////////////////////
// transport.cpp — adapter between the app and phx_shm. All protocol details
// (slots, sequence numbers, memory ordering) are in phx_shm; this file only
// sizes crops, mirrors config, and translates frames.
////////////////////////////////////////////////////////////////////////////////
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "phase_timers.h"
#include "phx_shm.h"
#include "transport.h"

static const char* kCamName  = "phx_cam";
static const char* kPhosName = "phx_phos";

// Fallback camera angular half-extents used only when Varjo intrinsics are not
// available. These approximate how a tangent-space radius maps to camera pixels.
constexpr float kCameraTanHalfX = 0.6f;
constexpr float kCameraTanHalfY = 0.6f;

void computePythonCropSize(
    int frameWidth,
    int frameHeight,
    float cropRadiusTan,
    const CameraFrame* frame,
    int& outCropWidth,
    int& outCropHeight)
{
    if (frame && frame->hasIntrinsics && frame->focalLengthX > 0.0 && frame->focalLengthY > 0.0) {
        // Preferred path: project tangent radius through the camera intrinsics.
        // A tangent of x corresponds to approximately fx * x pixels from center.
        double fx = frame->focalLengthX;
        double fy = frame->focalLengthY;
        if (fx < 1.0) { fx *= frameWidth; }   // normalized -> pixels
        if (fy < 1.0) { fy *= frameHeight; }
        outCropWidth  = (std::max)(32, (int)std::lround(2.0 * fx * cropRadiusTan));
        outCropHeight = (std::max)(32, (int)std::lround(2.0 * fy * cropRadiusTan));
    } else {
        // Fallback path for early startup or missing intrinsics.
        outCropWidth  = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfX) * frameWidth));
        outCropHeight = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfY) * frameHeight));
    }
    if (outCropWidth  & 1) ++outCropWidth;
    if (outCropHeight & 1) ++outCropHeight;
}

// Build this eye's config entry; intrinsics carry over from the last written
// config when the frame has none (Varjo does not attach them to every frame).
static void fillEyeConfig(PhospheneBridge& bridge, const CameraFrame* frame, int eye, ShmEyeConfig& cfg)
{
    cfg = ShmEyeConfig{};
    cfg.cropWidth   = bridge.cropWidth[eye];
    cfg.cropHeight  = bridge.cropHeight[eye];
    cfg.frameWidth  = bridge.frameWidth[eye];
    cfg.frameHeight = bridge.frameHeight[eye];
    cfg.rowStride   = bridge.rowStride[eye];
    if (frame && frame->hasIntrinsics) {
        cfg.intrinsicsModel = frame->intrinsicsModel;
        cfg.intrinsicsValid = 1;
        cfg.focalLengthX = frame->focalLengthX;
        cfg.focalLengthY = frame->focalLengthY;
        cfg.principalPointX = frame->principalPointX;
        cfg.principalPointY = frame->principalPointY;
        for (int i = 0; i < 8; ++i) cfg.distortionCoefficients[i] = frame->distortionCoefficients[i];
    } else {
        const ShmEyeConfig& prev = bridge.lastConfig.eye[eye];
        cfg.intrinsicsModel = prev.intrinsicsModel;
        cfg.intrinsicsValid = prev.intrinsicsValid;
        cfg.focalLengthX = prev.focalLengthX;
        cfg.focalLengthY = prev.focalLengthY;
        cfg.principalPointX = prev.principalPointX;
        cfg.principalPointY = prev.principalPointY;
        for (int i = 0; i < 8; ++i) cfg.distortionCoefficients[i] = prev.distortionCoefficients[i];
    }
}

// Write the config block if anything changed. Caller holds configMutex.
static void writeConfigIfChanged(PhospheneBridge& bridge, const CameraFrame* frame, int eye)
{
    if (!bridge.cam) return;
    CamConfigBlob next = bridge.lastConfig;
    next.blindnessMode = (int32_t)gBlindnessMode.load(std::memory_order_relaxed);
    next.yoloConf = gYoloConf.load(std::memory_order_relaxed);
    next.mapRequest = gMapRequest.load(std::memory_order_relaxed);
    fillEyeConfig(bridge, frame, eye, next.eye[eye]);
    if (bridge.configWritten && std::memcmp(&next, &bridge.lastConfig, sizeof(next)) == 0) return;
    if (phx_config_write(bridge.cam, &next, sizeof(next)) != PHX_OK) return;
    bridge.lastConfig = next;
    bridge.configWritten = true;
    const ShmEyeConfig& c = next.eye[eye];
    printf("[SHM] config eye=%d: blindness=%d conf=%.2f crop=%dx%d frame=%dx%d stride=%d intrinsics=%s fx=%.1f fy=%.1f\n",
           eye, next.blindnessMode, next.yoloConf, c.cropWidth, c.cropHeight, c.frameWidth, c.frameHeight,
           c.rowStride, c.intrinsicsValid ? "Y" : "N", c.focalLengthX, c.focalLengthY);
}

bool startPhospheneBridge(PhospheneBridge& bridge, uint32_t camPayloadBytes)
{
    phx_layout lay{ (uint32_t)NUM_EYES, 3u, camPayloadBytes };
    const int r = phx_open(kCamName, PHX_PRODUCER, &lay, &bridge.cam);
    if (r != PHX_OK) {
        fprintf(stderr, "[SHM] phx_open(%s) failed: %d\n", kCamName, r);
        bridge.cam = nullptr;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(bridge.configMutex);
        for (int e = 0; e < NUM_EYES; ++e) writeConfigIfChanged(bridge, nullptr, e);
    }
    phx_heartbeat(bridge.cam);
    bridge.running.store(true);
    phx_layout got{}; phx_get_layout(bridge.cam, &got);
    printf("[SHM] ready: %s (%u channels x %u slots x %.1f MB); waiting for Python to create %s\n",
           kCamName, got.channels, got.slots, got.payload_bytes / 1048576.0, kPhosName);
    return true;
}

void stopPhospheneBridge(PhospheneBridge& bridge)
{
    bridge.running.store(false);
    if (bridge.cam) phx_set_shutdown(bridge.cam);
    if (bridge.phos) { phx_close(bridge.phos); bridge.phos = nullptr; }
    if (bridge.cam)  { phx_close(bridge.cam);  bridge.cam  = nullptr; }
}

bool pollPhosOpen(PhospheneBridge& bridge)
{
    if (bridge.phos) return true;
    phx_handle* h = nullptr;
    const int r = phx_open(kPhosName, PHX_CONSUMER, nullptr, &h);
    if (r == PHX_OK) {
        bridge.phos = h;
        for (int e = 0; e < NUM_EYES; ++e) bridge.phosLastSeq[e] = 0;
        bridge.fieldSeq = 0;
        phx_layout got{}; phx_get_layout(h, &got);
        printf("[SHM] Python connected: %s (%u channels, %.2f MB slots)\n",
               kPhosName, got.channels, got.payload_bytes / 1048576.0);
        return true;
    }
    if (r == PHX_BAD_VERSION) {
        static bool warned = false;
        if (!warned) { warned = true; fprintf(stderr, "[SHM] %s has a different layout version; rebuild Python's phx_shm\n", kPhosName); }
    }
    return false;
}

uint8_t* PhospheneBridge::beginFrame(int eye, uint32_t* capacity)
{
    if (!running.load() || !cam || eye < 0 || eye >= NUM_EYES) return nullptr;
    uint8_t* dst = nullptr;
    if (phx_publish_begin(cam, (uint32_t)eye, &dst, capacity) != PHX_OK) return nullptr;
    return dst;
}

void PhospheneBridge::commitFrame(int eye, const CameraFrame& frame, uint32_t byteSize)
{
    if (!running.load() || !cam || eye < 0 || eye >= NUM_EYES) return;
    ScopedPhaseTimer timePublish(TIMER_SHM_PUB);

    frameWidth[eye]  = frame.width;
    frameHeight[eye] = frame.height;
    rowStride[eye]   = frame.rowStride;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        const OverlayGeometry geom = overlayGeometryFor(gBlindnessMode.load(std::memory_order_relaxed));
        computePythonCropSize(frameWidth[eye], frameHeight[eye], geom.phospheneRadiusTan,
                              &frame, cropWidth[eye], cropHeight[eye]);
        writeConfigIfChanged(*this, &frame, eye);
    }

    phx_meta m{};
    m.frame_number = (uint64_t)frame.frameNumber;
    m.timestamp_ns = phx_now_ns();
    m.width = (uint32_t)frame.width; m.height = (uint32_t)frame.height;
    m.row_stride = (uint32_t)frame.rowStride; m.byte_size = byteSize;
    m.gaze_tan_x = gazeTanX[eye].load(); m.gaze_tan_y = gazeTanY[eye].load();
    m.eye = (uint32_t)eye;
    const int r = phx_publish_commit(cam, (uint32_t)eye, &m);
    if (r != PHX_OK) fprintf(stderr, "[SHM] commit eye=%d failed: %d\n", eye, r);
}

bool consumePhosphene(PhospheneBridge& bridge, int eye,
                      std::vector<uint8_t>& outGray, int& outWidth, int& outHeight,
                      uint64_t& outFrameNumber, uint64_t& outCaptureTs)
{
    if (!bridge.phos) return false;
    phx_view v{};
    const int r = phx_consume_acquire(bridge.phos, (uint32_t)eye, bridge.phosLastSeq[eye], &v);
    if (r != PHX_OK) return false;                       // nothing new or torn
    bridge.phosLastSeq[eye] = v.seq;
    if (v.meta.byte_size == 0 || v.meta.width == 0 || v.meta.height == 0) {
        phx_consume_release(bridge.phos, (uint32_t)eye, &v);
        return false;                                    // Python published an empty marker
    }
    if (v.meta.byte_size != v.meta.width * v.meta.height) {
        phx_consume_release(bridge.phos, (uint32_t)eye, &v);
        return false;
    }
    const auto conStart = std::chrono::steady_clock::now();
    outGray.resize(v.meta.byte_size);
    std::memcpy(outGray.data(), v.payload, v.meta.byte_size);
    if (phx_consume_release(bridge.phos, (uint32_t)eye, &v) != PHX_OK) return false;   // torn: discard copy
    outWidth = (int)v.meta.width; outHeight = (int)v.meta.height;
    outFrameNumber = v.meta.frame_number; outCaptureTs = v.meta.timestamp_ns;
    gPhaseTimers.add(TIMER_SHM_CON, std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - conStart).count());
    return true;
}

bool pollAnnouncedDeviceField(PhospheneBridge& bridge)
{
    if (!bridge.phos) return false;
    float tan = 0.0f; uint64_t seq = 0;
    if (phx_field_poll(bridge.phos, &tan, &seq) != PHX_OK) return false;
    if (seq == bridge.fieldSeq) return false;
    bridge.fieldSeq = seq;
    // Sanity window: tan of ~0.6..80 deg half-field.
    if (!(tan > 0.01f && tan < 6.0f)) return false;
    const float current = gDeviceFieldTan.load(std::memory_order_relaxed);
    if (std::fabs(tan - current) < 1e-4f) return false;
    gDeviceFieldTan.store(tan, std::memory_order_relaxed);
    printf("[SHM] device field announced by Python: tan=%.4f (+/-%.1f deg). Crop and shader follow.\n",
           tan, std::atan(tan) * 180.0 / 3.14159265358979);
    return true;
}

void bridgeHeartbeat(PhospheneBridge& bridge)
{
    if (bridge.cam)  phx_heartbeat(bridge.cam);
    if (bridge.phos) phx_heartbeat(bridge.phos);
}

bool pythonAlive(PhospheneBridge& bridge, double maxAgeSeconds)
{
    // Python beats on both sections; the phos one is the authoritative "it is
    // producing" signal, cam covers the window before phos exists.
    const uint64_t maxAge = (uint64_t)(maxAgeSeconds * 1e9);
    if (bridge.phos && phx_peer_alive(bridge.phos, maxAge)) return true;
    return bridge.cam && phx_peer_alive(bridge.cam, maxAge) == 1;
}