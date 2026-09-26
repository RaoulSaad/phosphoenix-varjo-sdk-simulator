# Interactivity / Quality-of-Life Roadmap

Handoff notes for making the phosphene simulation interactive at runtime.
Context: C++ app (this repo) renders in the Varjo headset and owns the
keyboard; Python (`realtime_phosphene_pipeline/run_varjo.py`) computes the
phosphenes. The two communicate over shared memory (`transport.cpp` /
`phx_shm.py`, both on the `phx_shm` library).

**Design rule:** keep all user-facing keys in the C++ app (`GetAsyncKeyState`
is system-wide, so they work regardless of window focus — the person wearing
the headset cannot see which window has focus) and forward state to Python
through the SHM cam header, the way `blindnessMode` already travels. Avoid
splitting controls between the C++ window and the Python preview window.

Already done, for reference: all of Bundle 1 (mask-opacity keys `0`–`9`,
blindness-mode keys `M`/`G`/`F`, scotoma/tunnel radius keys `[`/`]`, and the
`[STATE]` status line, all in `main.cpp`), and the automatic device-field
handshake (Python announces the loaded map's span via the phos header;
C++ adapts crop + shader — `pollAnnouncedDeviceField`).

---

## Bundle 1 — daily-driver controls (C++ only, one sitting) — DONE

### 1. Blindness-mode keys (`M` / `G` / `F`) — DONE
Switch macular / glaucoma / full at runtime instead of editing
`pipeline_types.cpp` and rebuilding.
- Nearly free: `writeHeaderConfig` already detects mode changes and bumps
  `configSeq`; Python already re-reads it; the shader takes the mode as a
  uniform per frame.
- Change: make `gBlindnessMode` assignable from the key handler in
  `main.cpp`'s loop (pattern: the opacity keys). It is read by camera threads
  via `overlayGeometryFor`, so make it `std::atomic<BlindnessMode>` like
  `gDeviceFieldTan`.

### 2. Live scotoma / tunnel radius (`[` / `]`) — DONE
Grow/shrink `spotRadiusTan` to sweep disease severity (mild -> severe
glaucoma) in one session.
- Change: the per-mode radii in `overlayGeometryFor` become runtime state
  (atomic floats, same pattern as `gDeviceFieldTan`), keys adjust in steps of
  ~0.01 tan, clamp to [0, ~0.6].
- Python does not need to know: the radii only affect the shader mask.
  (The crop depends only on the device field, not the scotoma radius.)

### 3. Status line — DONE
One `printf` on any state change: mode, spot radius, mask opacity, device
field, map announced by Python. Answers "what am I looking at right now"
during experiments. Pattern exists in the opacity handler.

## Bundle 2 — stimulation controls (Python side)

### 4. Live stimulus gain (`+` / `-`) — DROPPED (2026-09-26)
Not worth a key. dynaphos does not cap amplitude, but its brightness sigmoid
(calibrated on Fernandez et al. 2021) is already saturated at the default
100 uA stimulus_scale: brightness goes from below detection threshold at
~25 uA to 0.98 at ~30 uA and 1.0 above. So a gain knob is a cliff between
0.24x and 0.32x, and above 1x it only grows phosphene size (sqrt of
amplitude, unbounded) and speeds habituation. The paper's only stated bound
is the 0-128 uA range used in its constrained experiment. If a size sweep is
ever wanted, that is a separate feature with a 128 uA ceiling.
Original notes kept below for reference.
Scale `stimulus_scale` at runtime — overall phosphene brightness; real
implants expose exactly this knob to the patient.
- Sample mode: multiply the value used by `sample_stimulus(rescale=True)`.
- Encoder mode: multiply the stim tensor before `simulator(stim)`.
- Keys via `cv2.waitKey` in the preview loop, or (better, per the design
  rule) a C++ key forwarded through a reserved field in the cam header.

### 5. Live YOLO confidence threshold (`,` / `.`) — DONE
Not baked into the engine (the EfficientNMS export path is commented out);
the threshold is a per-launch argument of the custom nms_pre kernel. C++ owns
it (`gYoloConf`, `--conf` flag, keys step 0.05 in [0.05, 0.95]) and forwards
it through the former `reserved` float in `CamConfigBlob`; Python's
`ShmBridge.yolo_conf` feeds a holder the dispatch closure reads per frame.
Python's own `--conf` only applies when C++ sends a negative value (never,
today) — set the threshold on the C++ command line when running together.
Adjust `--conf` at runtime while watching the scene; tune clutter vs
completeness. Threshold is passed per-dispatch (`segmentation_postprocess`),
so it can simply be a mutable variable read by the dispatch closure.

### 6. Per-class rendering toggles
`--edge-classes` / `--solid-classes` exist as flags; a runtime cycle key
(e.g. per-class solid -> edge -> contour -> ignore) would help the staircase
experiments. Requires rebuilding the `class_mode` array on the GPU (cheap).

## Bundle 3 — bigger, highest value

### 7. Map cycling (`N`) — DONE
`--coords` takes several maps (paths or globs, expanded in Python). C++ key
`N` bumps a `mapRequest` counter in `CamConfigBlob` (now 268 B); the capture
thread raises an event, the main thread swaps: publishers pause, both eyes
get a black image, the two new simulators are built BEFORE the old are
dropped (failed load = current map stays), the field is re-announced via
`announce_field` (shared with startup), and each stream's `shape_cropped`
tuple follows the new crop under the dispatch lock (the crop kernel
resamples into a fixed buffer, so no GPU reallocation). Peak GPU memory is
briefly two extra simulators during the swap.
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

1 + 2 + 3 + 5 + 7 done, 4 dropped. Next 8 (per-eye simulators already exist), then 6 or 9.
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
