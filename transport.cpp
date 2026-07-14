////////////////////////////////////////////////////////////////////////////////
// transport.cpp — shared-memory bridge to Python (replaces the old UDP
// transport).
//
// Each section is [ Header | eye0 channel | eye1 channel ]; a channel is a
// lock-free double buffer [ Ctrl | slot0 | slot1 ], slot = [ SlotMeta | bytes ].
// Publish (single-producer/single-consumer, latest-wins): the producer writes
// the free slot, stores latestIndex, then bumps publishSeq (release). The
// consumer reads publishSeq, copies slot[latestIndex], re-reads publishSeq; if
// it moved mid-copy it retries (seqlock). Double buffering means the producer
// never overwrites the slot being read unless it laps the consumer, which the
// re-check catches. No locks, no kernel events -- consumers poll publishSeq.
//
// The layout is fixed at compile time so both sides agree without negotiating;
// mappings are sized to generous maxima and only touched pages use RAM.
////////////////////////////////////////////////////////////////////////////////

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // CreateFileMapping/MapViewOfFile

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "transport.h"

static const char* kCamMapName  = "Local\\VarjoPhospheneCam";   // C++  -> Python
static const char* kPhosMapName = "Local\\VarjoPhospheneOut";   // Python -> C++

constexpr uint32_t SHM_MAGIC   = 0x50484D31;   // 'PHM1'
constexpr uint32_t SHM_VERSION = 1;

constexpr uint32_t SHM_FLAG_READY    = 1u << 0;
constexpr uint32_t SHM_FLAG_SHUTDOWN = 1u << 1;

constexpr uint32_t SHM_NUM_EYES  = 2;
constexpr uint32_t SHM_NUM_SLOTS = 2;          // double buffer

constexpr uint32_t SHM_HEADER_SIZE   = 512;
constexpr uint32_t SHM_CTRL_SIZE     = 64;
constexpr uint32_t SHM_SLOTMETA_SIZE = 64;

// Per-slot payload capacities (generous upper bounds; the actual bytes per frame
// are carried in SlotMeta.byteSize). NV12 ~ w*h*3/2; grayscale ~ w*h.
constexpr uint32_t CAM_SLOT_CAP  = 8u * 1024 * 1024;
constexpr uint32_t PHOS_SLOT_CAP = 4u * 1024 * 1024;

constexpr uint32_t CAM_SLOT_STRIDE   = SHM_SLOTMETA_SIZE + CAM_SLOT_CAP;
constexpr uint32_t CAM_CHANNEL_SIZE  = SHM_CTRL_SIZE + SHM_NUM_SLOTS * CAM_SLOT_STRIDE;
constexpr uint32_t CAM_MAP_SIZE      = SHM_HEADER_SIZE + SHM_NUM_EYES * CAM_CHANNEL_SIZE;

constexpr uint32_t PHOS_SLOT_STRIDE  = SHM_SLOTMETA_SIZE + PHOS_SLOT_CAP;
constexpr uint32_t PHOS_CHANNEL_SIZE = SHM_CTRL_SIZE + SHM_NUM_SLOTS * PHOS_SLOT_STRIDE;
constexpr uint32_t PHOS_MAP_SIZE     = SHM_HEADER_SIZE + SHM_NUM_EYES * PHOS_CHANNEL_SIZE;

// Fallback camera angular half-extents used only when Varjo intrinsics are not
// available. These approximate how a tangent-space radius maps to camera pixels.
constexpr float kCameraTanHalfX = 0.6f;
constexpr float kCameraTanHalfY = 0.6f;
constexpr double kPythonSendIntervalMs = 0.0; // ~15 FPS --> switch to 16.6 for 60 FPS if needed

#pragma pack(push, 1)
struct ShmEyeConfig {
    // Per-eye config, carried in the cam header (replaces the old config packet).
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

struct ShmHeader {
    // Lives at offset 0 of each mapping. The cam header additionally carries the
    // per-eye config that used to travel in the UDP config packet.
    uint32_t magic;
    uint32_t version;
    uint32_t flags;            // SHM_FLAG_READY | SHM_FLAG_SHUTDOWN
    uint32_t configSeq;        // bumped when any eye config changes
    int32_t  blindnessMode;    // C++ is the single source of truth
    uint32_t reserved0;
    ShmEyeConfig eye[SHM_NUM_EYES];
};

struct ShmCtrl {
    // One per channel, at the channel's base offset.
    uint32_t latestIndex;      // index of the most-recently published slot
    uint32_t publishSeq;       // monotonically increasing; consumer compares
    uint32_t reserved[14];
};

struct ShmSlotMeta {
    // Prefix of each slot; describes the payload bytes that follow it.
    uint32_t frameId;
    uint32_t eye;
    uint32_t width;
    uint32_t height;
    uint32_t rowStride;
    uint32_t byteSize;         // actual payload bytes in this slot
    float    gazeTanX;
    float    gazeTanY;
    uint32_t reserved[8];
};
#pragma pack(pop)

static_assert(sizeof(ShmHeader)   <= SHM_HEADER_SIZE,   "ShmHeader exceeds SHM_HEADER_SIZE");
static_assert(sizeof(ShmCtrl)     == SHM_CTRL_SIZE,     "ShmCtrl must equal SHM_CTRL_SIZE");
static_assert(sizeof(ShmSlotMeta) == SHM_SLOTMETA_SIZE, "ShmSlotMeta must equal SHM_SLOTMETA_SIZE");

// The headers live at offset 0 of each mapping.
static ShmHeader* camHeader(PhospheneBridge& bridge)  { return reinterpret_cast<ShmHeader*>(bridge.camBase); }
static ShmHeader* phosHeader(PhospheneBridge& bridge) { return reinterpret_cast<ShmHeader*>(bridge.phosBase); }

// --- lock-free double-buffer helpers (x86 TSO; fences are the compiler barrier) ---
static inline uint8_t* shmChannel(uint8_t* base, int eye, uint32_t channelSize) {
    return base + SHM_HEADER_SIZE + (uint32_t)eye * channelSize;
}
static inline uint8_t* shmSlot(uint8_t* channel, uint32_t slot, uint32_t slotStride) {
    return channel + SHM_CTRL_SIZE + slot * slotStride;
}

// Producer: write meta + payload into the free slot, then publish it.
static void shmPublish(uint8_t* channel, uint32_t slotStride, uint32_t slotCap,
                       uint32_t& writeIndex, const ShmSlotMeta& metaIn,
                       const void* payload, uint32_t byteSize)
{
    ShmCtrl* ctrl = reinterpret_cast<ShmCtrl*>(channel);
    const uint32_t w = writeIndex;
    uint8_t* slot = shmSlot(channel, w, slotStride);
    const uint32_t n = (byteSize <= slotCap) ? byteSize : slotCap;

    ShmSlotMeta* meta = reinterpret_cast<ShmSlotMeta*>(slot);
    *meta = metaIn;
    meta->byteSize = n;
    std::memcpy(slot + SHM_SLOTMETA_SIZE, payload, n);

    // Ensure payload + meta are visible before we advertise the slot.
    std::atomic_thread_fence(std::memory_order_release);
    ctrl->latestIndex = w;
    std::atomic_thread_fence(std::memory_order_release);
    ctrl->publishSeq  = ctrl->publishSeq + 1;   // the publish

    writeIndex = (w + 1) % SHM_NUM_SLOTS;
}

// Consumer: return the latest published frame if it is newer than lastSeq.
static bool shmConsume(uint8_t* channel, uint32_t slotStride,
                       uint32_t& lastSeq, ShmSlotMeta& metaOut,
                       std::vector<uint8_t>& payloadOut)
{
    ShmCtrl* ctrl = reinterpret_cast<ShmCtrl*>(channel);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = ctrl->publishSeq;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s1 == lastSeq) return false;                 // nothing new
        const uint32_t idx = ctrl->latestIndex;
        if (idx >= SHM_NUM_SLOTS) return false;

        uint8_t* slot = shmSlot(channel, idx, slotStride);
        ShmSlotMeta m = *reinterpret_cast<const ShmSlotMeta*>(slot);
        const uint32_t n = (m.byteSize <= slotStride - SHM_SLOTMETA_SIZE)
                               ? m.byteSize : 0;
        payloadOut.resize(n);
        if (n) std::memcpy(payloadOut.data(), slot + SHM_SLOTMETA_SIZE, n);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (ctrl->publishSeq == s1) {                    // no tear during copy
            lastSeq = s1;
            metaOut = m;
            return true;
        }
        // producer published mid-copy; retry for a consistent snapshot
    }
    return false;   // producer racing very fast; pick it up next poll
}

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
        // Handle both pixel-scale and normalized intrinsics
        double fx = frame->focalLengthX;
        double fy = frame->focalLengthY;
        if (fx < 1.0) { fx *= frameWidth; }   // normalized → pixels
        if (fy < 1.0) { fy *= frameHeight; }
        outCropWidth  = (std::max)(32, (int)std::lround(2.0 * fx * cropRadiusTan));
        outCropHeight = (std::max)(32, (int)std::lround(2.0 * fy * cropRadiusTan));
    } else {
        // Fallback path for early startup or missing intrinsics: scale by an
        // approximate camera tangent half-FOV.
        outCropWidth  = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfX) * frameWidth));
        outCropHeight = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfY) * frameHeight));
    }
    if (outCropWidth  & 1) ++outCropWidth;
    if (outCropHeight & 1) ++outCropHeight;
}

static uint8_t* shmCreate(HANDLE& outMap, const char* name, uint32_t size)
{
    // Pagefile-backed named section. Either process may create it first; with a
    // fixed size both agree, and the READY flag (set by C++ last) gates use.
    outMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, size, name);
    if (!outMap) {
        fprintf(stderr, "[SHM] CreateFileMapping('%s') failed: %lu\n", name, GetLastError());
        return nullptr;
    }
    uint8_t* view = reinterpret_cast<uint8_t*>(
        MapViewOfFile(outMap, FILE_MAP_ALL_ACCESS, 0, 0, size));
    if (!view) {
        fprintf(stderr, "[SHM] MapViewOfFile('%s') failed: %lu\n", name, GetLastError());
        CloseHandle(outMap); outMap = nullptr;
        return nullptr;
    }
    return view;
}

// Mirror the per-eye config into the cam header, bumping configSeq only when it
// actually changes (so Python re-reads intrinsics/crop sizes on demand). The
// header always holds the last-written config, so it doubles as the "previous"
// value for change detection and for carrying intrinsics across frames.
static void writeHeaderConfig(PhospheneBridge& bridge, const CameraFrame* frame, int eye)
{
    if (!bridge.camBase) return;

    ShmHeader* h = camHeader(bridge);

    ShmEyeConfig cfg{};
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
        // No intrinsics on this frame: keep the last known ones (in the header).
        const ShmEyeConfig& prev = h->eye[eye];
        cfg.intrinsicsModel = prev.intrinsicsModel;
        cfg.intrinsicsValid = prev.intrinsicsValid;
        cfg.focalLengthX = prev.focalLengthX;
        cfg.focalLengthY = prev.focalLengthY;
        cfg.principalPointX = prev.principalPointX;
        cfg.principalPointY = prev.principalPointY;
        for (int i = 0; i < 8; ++i) cfg.distortionCoefficients[i] = prev.distortionCoefficients[i];
    }

    const bool modeChanged = (h->blindnessMode != (int)gBlindnessMode);
    if (!modeChanged && std::memcmp(&cfg, &h->eye[eye], sizeof(cfg)) == 0) {
        return;   // nothing changed
    }

    h->eye[eye] = cfg;
    h->blindnessMode = (int)gBlindnessMode;
    std::atomic_thread_fence(std::memory_order_release);
    h->configSeq = h->configSeq + 1;

    printf("[SHM] config eye=%d: blindness=%d crop=%dx%d frame=%dx%d stride=%d intrinsics=%s fx=%.1f fy=%.1f\n",
           eye, h->blindnessMode, cfg.cropWidth, cfg.cropHeight, cfg.frameWidth, cfg.frameHeight,
           cfg.rowStride, cfg.intrinsicsValid ? "Y" : "N", cfg.focalLengthX, cfg.focalLengthY);
}

bool startPhospheneBridge(PhospheneBridge& bridge)
{
    bridge.camBase  = shmCreate(bridge.camMap,  kCamMapName,  CAM_MAP_SIZE);
    bridge.phosBase = shmCreate(bridge.phosMap, kPhosMapName, PHOS_MAP_SIZE);
    if (!bridge.camBase || !bridge.phosBase) return false;

    // Reset control blocks for both directions/eyes (fresh pages are zero-filled,
    // but a stale mapping surviving a crash would not be).
    for (int eye = 0; eye < (int)SHM_NUM_EYES; ++eye) {
        ShmCtrl* cc = reinterpret_cast<ShmCtrl*>(shmChannel(bridge.camBase,  eye, CAM_CHANNEL_SIZE));
        ShmCtrl* pc = reinterpret_cast<ShmCtrl*>(shmChannel(bridge.phosBase, eye, PHOS_CHANNEL_SIZE));
        cc->latestIndex = 0; cc->publishSeq = 0;
        pc->latestIndex = 0; pc->publishSeq = 0;
        bridge.camWriteIndex[eye] = 0;
        bridge.phosLastSeq[eye]   = 0;
    }

    // Fill the initial config (fallback sizes from main), then publish READY last.
    camHeader(bridge)->configSeq = 0;
    camHeader(bridge)->blindnessMode = (int)gBlindnessMode;
    for (int eye = 0; eye < (int)SHM_NUM_EYES; ++eye) writeHeaderConfig(bridge, nullptr, eye);

    for (uint8_t* base : {bridge.camBase, bridge.phosBase}) {
        ShmHeader* h = reinterpret_cast<ShmHeader*>(base);
        h->version = SHM_VERSION;
        std::atomic_thread_fence(std::memory_order_release);
        h->magic = SHM_MAGIC;
        std::atomic_thread_fence(std::memory_order_release);
        h->flags = SHM_FLAG_READY;
    }

    bridge.running.store(true);
    printf("[SHM] ready: cam='%s' (%.1f MB), phos='%s' (%.1f MB)\n",
           kCamMapName, CAM_MAP_SIZE / 1048576.0,
           kPhosMapName, PHOS_MAP_SIZE / 1048576.0);
    return true;
}

void stopPhospheneBridge(PhospheneBridge& bridge)
{
    // Signal shutdown to Python via both headers, then tear down the mappings.
    if (bridge.camBase)  camHeader(bridge)->flags  |= SHM_FLAG_SHUTDOWN;
    if (bridge.phosBase) phosHeader(bridge)->flags |= SHM_FLAG_SHUTDOWN;
    std::atomic_thread_fence(std::memory_order_release);

    bridge.running.store(false);

    if (bridge.camBase)  { UnmapViewOfFile(bridge.camBase);  bridge.camBase  = nullptr; }
    if (bridge.phosBase) { UnmapViewOfFile(bridge.phosBase); bridge.phosBase = nullptr; }
    if (bridge.camMap)   { CloseHandle(bridge.camMap);  bridge.camMap  = nullptr; }
    if (bridge.phosMap)  { CloseHandle(bridge.phosMap); bridge.phosMap = nullptr; }
}

void publishCameraFrame(PhospheneBridge& bridge, const CameraFrame& frame, int eye)
{
    if (!bridge.running.load() || !bridge.camBase) return;

    // Throttle Python work so camera capture can run faster than the phosphene
    // processing loop (currently 0 = publish every frame).
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration<double, std::milli>(now - bridge.lastSendTime[eye]).count();
    if (elapsedMs < kPythonSendIntervalMs) {
        return;
    }
    bridge.lastSendTime[eye] = now;

    bridge.frameWidth[eye] = frame.width;
    bridge.frameHeight[eye] = frame.height;
    bridge.rowStride[eye] = frame.rowStride;
    {
        // Every mode uses the same gaze-centred crop sized to the fixed device
        // field, so the phosphenes follow the eye. The per-disease mask (C++
        // shader) decides which of them are shown.
        const OverlayGeometry geom = overlayGeometryFor(gBlindnessMode);
        computePythonCropSize(
            bridge.frameWidth[eye],
            bridge.frameHeight[eye],
            geom.phospheneRadiusTan,
            &frame,
            bridge.cropWidth[eye],
            bridge.cropHeight[eye]);
    }

    // Publish current config (crop size / intrinsics / mode) into the cam header;
    // this no-ops unless something changed and bumps configSeq when it does.
    writeHeaderConfig(bridge, &frame, eye);

    if (frame.nv12.empty()) return;
    if (frame.nv12.size() > CAM_SLOT_CAP) {
        fprintf(stderr, "[SHM] cam frame eye=%d too large (%zu > %u); dropping\n",
                eye, frame.nv12.size(), CAM_SLOT_CAP);
        return;
    }

    ShmSlotMeta meta{};
    meta.frameId   = (uint32_t)(frame.frameNumber & 0xffffffffu);
    meta.eye       = (uint32_t)eye;
    meta.width     = (uint32_t)frame.width;
    meta.height    = (uint32_t)frame.height;
    meta.rowStride = (uint32_t)frame.rowStride;
    // Latest gaze written by the render loop; tells Python where to crop.
    meta.gazeTanX  = bridge.gazeTanX[eye].load();
    meta.gazeTanY  = bridge.gazeTanY[eye].load();

    uint8_t* channel = shmChannel(bridge.camBase, eye, CAM_CHANNEL_SIZE);
    shmPublish(channel, CAM_SLOT_STRIDE, CAM_SLOT_CAP,
               bridge.camWriteIndex[eye], meta,
               frame.nv12.data(), (uint32_t)frame.nv12.size());
}

bool consumePhosphene(PhospheneBridge& bridge, int eye,
                      std::vector<uint8_t>& outGray, int& outWidth, int& outHeight)
{
    // Consume the latest phosphene image from shared memory (render thread only).
    // The GL texture upload is the renderer's job (uploadPhosphene).
    if (!bridge.phosBase) return false;

    ShmSlotMeta meta{};
    uint8_t* channel = shmChannel(bridge.phosBase, eye, PHOS_CHANNEL_SIZE);
    if (!shmConsume(channel, PHOS_SLOT_STRIDE, bridge.phosLastSeq[eye], meta, outGray)) {
        return false;   // nothing newer than what we last consumed
    }
    if (outGray.empty() || meta.width == 0 || meta.height == 0) return false;
    if (outGray.size() != (size_t)meta.width * meta.height) return false;   // size mismatch

    outWidth  = (int)meta.width;
    outHeight = (int)meta.height;
    return true;
}
