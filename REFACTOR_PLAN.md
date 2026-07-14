# C++ Modularization Plan (main.cpp → modules)

Goal: split the monolithic `main.cpp` into focused modules so that (1) the Varjo
input can be swapped for a webcam when working remote, and (2) the Python pipeline
integration (shared memory) is a clean, isolated seam.

**Decision (agreed):** full 3-module split with an `IFrameSource` interface +
`VarjoFrameSource` now. Webcam is **seam-only for now** — build the interface so a
`WebcamFrameSource` drops in later; do **not** build webcam capture/display yet.

Everything must be **behavior-preserving** — this is a pure refactor. Only real
code change agreed: split the phosphene consume (shared-mem) from the GL upload.

---

## Target file layout

| File | Responsibility | Status |
|---|---|---|
| `pipeline_types.h/.cpp` | Shared types: `CameraFrame`, `BlindnessMode`, `OverlayGeometry`+`overlayGeometryFor`, `gBlindnessMode`, `GazeTan`, `Viewport`, `ViewTangents`, `NUM_EYES` | **DONE** |
| `transport.h/.cpp` | Shared-memory module (the Python seam): `PhospheneBridge` + all SHM code | **DONE** |
| `frame_source.h` | `IFrameSource` interface (the webcam seam) | **DONE** |
| `varjo_source.h/.cpp` | `VarjoFrameSource : IFrameSource` — Varjo session, camera DataStream, gaze, XR swapchain + frame loop | **DONE** |
| `renderer.h/.cpp` | GL context + loader + shaders + overlay draw + phosphene textures + FBOs | **DONE** |
| `main.cpp` | Thin orchestrator: wire source → transport → renderer, run loop | **DONE** |
| `CMakeLists.txt` | compile all `.cpp` | **DONE** |

> NOTE: `pipeline_types.cpp` already defines `gBlindnessMode` and
> `overlayGeometryFor`. These are **still also defined in `main.cpp`** (lines
> ~185–234). The build is fine today because CMake only compiles `main.cpp`.
> When wiring CMake, **delete those definitions from `main.cpp`** or you get a
> duplicate-symbol link error.

---

## Where each current `main.cpp` section goes

(Line numbers are approximate, as of this plan.)

| Current `main.cpp` section | Lines | Destination |
|---|---|---|
| GL function pointers + `getGLProcAddressAny` + `loadGLFunctions` | ~56–135 | `renderer.cpp` (file-static) |
| WGL context (`wndProc`, `createGLContext`, `destroyGLContext`, `g_hwnd/hdc/hglrc`) | ~137–179 | `renderer.cpp` |
| `BlindnessMode`, `gBlindnessMode`, `kDeviceFieldTan`, `OverlayGeometry`, `overlayGeometryFor` | ~181–234 | **already in `pipeline_types.*`** — delete from main |
| Shaders `g_vertSrc`, `g_fragSrc`, `g_phospheneFragSrc` | ~237–400 | `renderer.cpp` (file-static) |
| `CropRect`, `computeScotomaCropUV` | ~402–455 | **DEAD CODE** — grep first; if unused, delete. (Not called anywhere.) |
| `compileShader`, `createProgram` | ~457–492 | `renderer.cpp` |
| `calculateViewports`, `getTotalWidth/Height` | ~494–533 | `varjo_source.cpp` (uses `varjo_GetTextureSize`) — return `std::vector<Viewport>` |
| SHM constants/structs/helpers, `CapturedEyeFrame`, `PhospheneBridge`, `computePythonCropSize`, `shmCreate`, `writeHeaderConfig`, `startPhospheneBridge`, `stopPhospheneBridge`, `sendFrameToPython`, `uploadLatestPhospheneTexture` | ~536–1031 | `transport.h/.cpp` |
| Camera capture: `EyeCameraCapture`, `cameraSaverThreadMain`, `EyeCameraCaptureSet`, `onEyeCameraFrame`, `startEyeCameraCaptures`, `stopEyeCameraCaptures` | ~1033–1283 | `varjo_source.cpp` |
| `main()` — GL init, session, MR, gaze, swapchain, FBOs, shaders, textures, render loop, teardown | ~1289–1747 | split: GL→renderer, session/camera/gaze/swapchain/loop→varjo_source, orchestration stays in `main.cpp` |

---

## Module specs

### `transport.h/.cpp`  (lowest risk — move mostly verbatim)
Move the whole SHM section. Keep the SHM layout structs/constants
(`ShmHeader`, `ShmCtrl`, `ShmSlotMeta`, `SHM_*`, `CAM_*`, `PHOS_*`) and the
`shmChannel/shmSlot/shmPublish/shmConsume` helpers **file-static inside
`transport.cpp`** (implementation detail).

`transport.h` exposes `struct PhospheneBridge` + free functions:
- `bool startPhospheneBridge(PhospheneBridge&)`
- `void stopPhospheneBridge(PhospheneBridge&)`
- `void publishCameraFrame(PhospheneBridge&, const CameraFrame&, int eye)` (was `sendFrameToPython`)
- `bool consumePhosphene(PhospheneBridge&, int eye, std::vector<uint8_t>& out, int& w, int& h)` — **NEW split**: does the `shmConsume` half of the old `uploadLatestPhospheneTexture` and returns bytes+dims. No GL here.
- `void computePythonCropSize(...)` (used by main's startup sizing)

Gotcha — **remove the SHM-layout types from `PhospheneBridge`** so it can live in
the header without exposing `#pragma pack` structs:
- Drop `ShmEyeConfig lastConfig[2]` + `haveLastConfig[2]`. Replace the change
  detection in `writeHeaderConfig` to compare the freshly-built `ShmEyeConfig`
  against **the current header value** `h->eye[eye]` (memcmp) and read "previous
  intrinsics" from `h->eye[eye]` too. (Header always holds the last-written config.)
- Drop `camHeader()/phosHeader()` members; make them file-static helpers in `.cpp`.
- `CapturedEyeFrame` is replaced by `CameraFrame` (in `pipeline_types.h`); it drops
  the unused `varjo_Nanoseconds timestamp` field.

`gBlindnessMode` / `overlayGeometryFor` come from `pipeline_types.h` (extern).

### `frame_source.h`  (interface — the webcam seam)
```cpp
#pragma once
#include <functional>
#include "pipeline_types.h"

// Delivered from the source's capture thread; wire to transport.publishCameraFrame.
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
```
`VarjoFrameSource` also exposes Varjo-XR display methods (NOT on the interface —
they're Varjo-only, used directly by main for the seam-only build). A future
`WebcamFrameSource` implements only `IFrameSource`.

### `varjo_source.h/.cpp`  (Varjo Handling — largest)
`class VarjoFrameSource : public IFrameSource` owns:
- `varjo_Session*` (create, `MRSetVideoRender`, `GazeInit`, shutdown)
- Camera: `EyeCameraCapture[2]` + `cameraSaverThreadMain` (now calls the
  `FrameCallback` instead of `sendFrameToPython`), `onEyeCameraFrame`,
  `startEyeCameraCaptures`/`stopEyeCameraCaptures`. The saver thread builds a
  `CameraFrame` and calls `onFrame(frame, eye)`.
- Gaze: `getGaze()` returns `GazeTan` — move the `safeTan` + focus-distance
  per-eye logic from main's loop (lines ~1579–1630) here.
- XR display: swapchain create (`varjo_GLCreateSwapChain`), `viewCount()`,
  `viewports()` (via `calculateViewports`), `tangents(i)`, `viewIndexToEye(i)`,
  swapchain GL texture handles (`swapchainTextures()` for the renderer's FBOs),
  and the frame loop primitives: `waitSync()`, `beginFrame()`,
  `acquireSwapchainImage()->int`, `releaseSwapchainImage()`,
  `endFrameAndSubmit()` (fills per-view proj/view matrices from
  `varjo_GetFovTangents`/`GetProjectionMatrix`/`frameInfo` and submits the layer),
  plus `pollEvents()` (MR device status prints).

Needs `varjo_Session*` before the swapchain; GL context must exist first
(renderer.initGL()). Sequence in main below.

### `renderer.h/.cpp`  (all GL)
Owns GL context + all GL function pointers + shaders + programs + uniform
locations + VAO + the 2 phosphene textures + the swapchain FBOs.
```cpp
class OverlayRenderer {
public:
    bool initGL();                 // WGL context + loadGLFunctions (BEFORE varjo swapchain)
    bool initShaders();            // compile both programs, VAO, get uniform locations
    void initPhospheneTextures(const int cropW[2], const int cropH[2]);
    void setupSwapchainFbos(const std::vector<unsigned>& glTextures, int atlasW, int atlasH);
    void uploadPhosphene(int eye, const uint8_t* data, int w, int h);  // GL half of old upload
    void beginFrame(int swapchainImageIndex); // bind fbo, viewport(atlas), clear, blend, VAO, no depth
    void drawView(const Viewport& vp, const ViewTangents& t,
                  float gazeTanX, float gazeTanY, int eyeIdx,
                  BlindnessMode mode, const OverlayGeometry& geom);
    void endFrame();               // disable blend, unbind fbo
    void shutdownGL();
};
```
`beginFrame/drawView/endFrame` are the GL body of main's render loop
(lines ~1563–1711). `drawView` sets the black-spot + phosphene uniforms and
issues the two `glDrawArrays`. `phospheneTexture[eye]` is owned here and bound
inside `drawView`. `uploadPhosphene` is the GL half of the old
`uploadLatestPhospheneTexture` (reallocate on size change + `glTexSubImage2D`).

### `main.cpp`  (thin orchestrator)
```
renderer.initGL();                                  // GL context first
VarjoFrameSource source; source initVarjo();        // session, MR, gaze
transport.start()  ... but needs sizes first:
  source.start(onFrame = [&](f,e){ publishCameraFrame(bridge,f,e); });
  for e: computePythonCropSize(source.frameWidth(e), ..., geom.phospheneRadiusTan, ...) -> bridge sizes
  startPhospheneBridge(bridge)
source.createSwapchain();  renderer.initShaders();
renderer.initPhospheneTextures(bridge.cropWidth, bridge.cropHeight);
renderer.setupSwapchainFbos(source.swapchainTextures(), totalW, totalH);
loop:
  source.pollEvents();
  source.waitSync();
  GazeTan g = source.getGaze();
  bridge.setGaze per eye (store atomics)             // camera threads read these
  for eye: if consumePhosphene(bridge,eye,buf,w,h): renderer.uploadPhosphene(eye,buf.data(),w,h);
  source.beginFrame(); int idx = source.acquireSwapchainImage();
  renderer.beginFrame(idx);
  for i in viewCount: renderer.drawView(source.viewport(i), source.tangents(i),
                        g.x[eye], g.y[eye], eye=source.viewIndexToEye(i),
                        gBlindnessMode, overlayGeometryFor(gBlindnessMode));
  renderer.endFrame(); source.releaseSwapchainImage(); source.endFrameAndSubmit();
teardown: source.stop(); stopPhospheneBridge(bridge); renderer.shutdownGL();
```
`main.cpp` keeps `#include "pipeline_types.h"` and must NOT redefine
`gBlindnessMode`/`overlayGeometryFor` (now in pipeline_types).

### `CMakeLists.txt`
```cmake
add_executable(VarjoGazeDot
    main.cpp
    pipeline_types.cpp
    transport.cpp
    varjo_source.cpp
    renderer.cpp)
```
(Everything else in the file stays: includes, `VarjoLib`/`opengl32` link, DLL copy.)

---

## Key gotchas / invariants to preserve
1. **GL context before Varjo swapchain** — `varjo_GLCreateSwapChain` needs a current WGL context.
2. **Camera saver threads read `bridge.gazeTanX/Y`** — the render loop must keep writing gaze into the bridge each frame (atomics).
3. **Config header written before READY** — `startPhospheneBridge` fills fallback config + publishes `READY` last (fence order matters; keep as-is).
4. **`writeHeaderConfig` change-detection** — after dropping `lastConfig`, compare against `h->eye[eye]` and read prev intrinsics from there. Keep the "bump configSeq only on change" behavior.
5. **`viewIndexToEye`** mapping (2 views = [L,R]; 4 views = [Lctx,Rctx,Lfoc,Rfoc]) — preserve exactly.
6. **`uploadLatestPhospheneTexture` split** is the only intended behavior change: transport `consumePhosphene` (no GL) + renderer `uploadPhosphene` (GL). Net effect identical.
7. **`computeScotomaCropUV`/`CropRect`** appear unused — `grep` before deleting.
8. Drop `CameraFrame.timestamp` (unused) and the `capture->latest.timestamp = ...` line.

---

## Build & verify
```
cmake --build build --config Release
```
- Fix duplicate-symbol (delete gBlindnessMode/overlayGeometryFor from main.cpp).
- Fix include/linkage errors iteratively.
- Cannot run the Varjo app here (needs headset). Verification = **clean build** +
  a read-through diff confirming no logic changed. The Python round-trip test
  (`native_sdk` env) still validates the transport wire format independently.

## Behavior-preservation checklist (after it builds)
- [ ] Startup log lines unchanged (`[OK] OpenGL`, `[OK] Varjo session`, `[SHM] ready`, etc.)
- [ ] `gBlindnessMode` still drives crop size (transport) + shader uniforms (renderer).
- [ ] Both eyes still publish/consume independently.
- [ ] Gaze still written to bridge every frame; camera threads still read it.
- [ ] Teardown order: stop camera stream → stop transport → free GL → shutdown session.
