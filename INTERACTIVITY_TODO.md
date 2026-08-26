# Interactivity / Quality-of-Life Roadmap

Handoff notes for making the phosphene simulation interactive at runtime.
Context: C++ app (this repo) renders in the Varjo headset and owns the
keyboard; Python (`realtime_phosphene_pipeline/run_varjo.py`) computes the
phosphenes. The two communicate over shared memory (`transport.cpp` /
`shm_transport.py`).

**Design rule:** keep all user-facing keys in the C++ app (`GetAsyncKeyState`
is system-wide, so they work regardless of window focus — the person wearing
the headset cannot see which window has focus) and forward state to Python
through the SHM cam header, the way `blindnessMode` already travels. Avoid
splitting controls between the C++ window and the Python preview window.

Already done, for reference: mask-opacity keys `0`–`9` in `main.cpp`
(passthrough blend for alignment checks), and the automatic device-field
handshake (Python announces the loaded map's span via the phos header;
C++ adapts crop + shader — `pollAnnouncedDeviceField`).

---

## Bundle 1 — daily-driver controls (C++ only, one sitting)

### 1. Blindness-mode keys (`M` / `G` / `F`)
Switch macular / glaucoma / full at runtime instead of editing
`pipeline_types.cpp` and rebuilding.
- Nearly free: `writeHeaderConfig` already detects mode changes and bumps
  `configSeq`; Python already re-reads it; the shader takes the mode as a
  uniform per frame.
- Change: make `gBlindnessMode` assignable from the key handler in
  `main.cpp`'s loop (pattern: the opacity keys). It is read by camera threads
  via `overlayGeometryFor`, so make it `std::atomic<BlindnessMode>` like
  `gDeviceFieldTan`.

### 2. Live scotoma / tunnel radius (`[` / `]`)
Grow/shrink `spotRadiusTan` to sweep disease severity (mild -> severe
glaucoma) in one session.
- Change: the per-mode radii in `overlayGeometryFor` become runtime state
  (atomic floats, same pattern as `gDeviceFieldTan`), keys adjust in steps of
  ~0.01 tan, clamp to [0, ~0.6].
- Python does not need to know: the radii only affect the shader mask.
  (The crop depends only on the device field, not the scotoma radius.)

### 3. Status line
One `printf` on any state change: mode, spot radius, mask opacity, device
field, map announced by Python. Answers "what am I looking at right now"
during experiments. Pattern exists in the opacity handler.

## Bundle 2 — stimulation controls (Python side)

### 4. Live stimulus gain (`+` / `-`)
Scale `stimulus_scale` at runtime — overall phosphene brightness; real
implants expose exactly this knob to the patient.
- Sample mode: multiply the value used by `sample_stimulus(rescale=True)`.
- Encoder mode: multiply the stim tensor before `simulator(stim)`.
- Keys via `cv2.waitKey` in the preview loop, or (better, per the design
  rule) a C++ key forwarded through a reserved field in the cam header.

### 5. Live YOLO confidence threshold
Adjust `--conf` at runtime while watching the scene; tune clutter vs
completeness. Threshold is passed per-dispatch (`segmentation_postprocess`),
so it can simply be a mutable variable read by the dispatch closure.

### 6. Per-class rendering toggles
`--edge-classes` / `--solid-classes` exist as flags; a runtime cycle key
(e.g. per-class solid -> edge -> contour -> ignore) would help the staircase
experiments. Requires rebuilding the `class_mode` array on the GPU (cheap).

## Bundle 3 — bigger, highest value

### 7. Map cycling (`n`)
Swap phosphene maps (seeds = "different patients") without restarting
anything: rebuild the simulator, re-announce the device field; C++ adapts
crop + shader live thanks to the handshake.
- Caveats: rebuild takes ~1–2 s (phosphene maps on GPU); block the publish
  loop during swap; announce the new field only after the simulator is ready.

### 8. Habituation toggle
Switch between reset-per-frame (current) and persistent temporal dynamics.
Phosphenes visibly fading under constant stimulation is the single most
convincing realism feature dynaphos offers.
- Prerequisite: one simulator **per eye** (state must not mix across eyes;
  the old handler did this — `simulators = [GaussianSimulator(...), ...]`).
- Cost: doubles simulator GPU memory (~260 MB per instance at 1000 x 256²).

### 9. Session recording (`r`)
Dump per frame: camera frame, composite crop, stim vector, phosphene image,
gaze, `[TIME]` phases -> a run directory. Turns demo impressions into paper
figures. Write asynchronously (queue + writer thread) so recording does not
disturb the `[TIME]` numbers it is recording.

---

## Suggested order

1 + 2 + 3 first (one C++ session, transforms daily use), then 4, then 7.
8 and 9 are the most valuable but are each a real chunk of work.

## Notes for whoever implements

- SHM header layout: `ShmHeader.reserved0` on the **phos** side is taken
  (device-field announcement). The cam header has `ShmEyeConfig.reserved`
  per eye plus slack up to `SHM_HEADER_SIZE` (512 B) — room for forwarding
  new runtime values C++ -> Python. Bump-on-change via `configSeq` is the
  established pattern (`writeHeaderConfig`).
- Anything read by the camera threads must be atomic (they run concurrently
  with the render loop): see `gDeviceFieldTan` / `gBlindnessMode` usage.
- Key handling: `GetAsyncKeyState` polling in the render loop; assigning the
  same value repeatedly is harmless, so no edge detection needed (see the
  opacity keys). Add edge detection only for *cycling* keys (mode, per-class,
  map), or one press will cycle every frame.
