# 🗺️ Quest: The Headless Headset

**Win condition:** `VarjoGazeDot.exe --webcam` (or however you spell it) runs on your
laptop with no Varjo anywhere — webcam frames flow to Python, phosphenes flow back,
and you can *see* the simulation follow your fake gaze.

Rules of the hunt: clues and checkpoints only, no solutions. Each level says *where*
to dig and *how you'll know* you've got it. Ask Claude for a hint, not a walkthrough.

**Process rule:** new commits on top of `modular` (or a branch off it), and nothing in
`transport.*`, `renderer.*`, or `varjo_source.*` should need to change — if a level
seems to demand it, re-read the clue, because the seam was built so it doesn't.

---

## Level 0 — Choose your weapon
Decide: OpenCV or Windows Media Foundation. Clue: the Python side already depends on
OpenCV, and `cv::VideoCapture` + one color-conversion call + a preview window covers
three later levels in one dependency. The price is getting CMake to find it on Windows
(`find_package(OpenCV)` and a prefix path — this is a real mini-boss, fight it *now*
in a throwaway target, not later inside your real build).

**✅ Checkpoint:** a 20-line scratch program that opens your webcam and shows live
video in a window.

## Level 1 — Read the map before walking it
No code this level. Answer four questions by reading, and write the answers down:

1. In [varjo_source.cpp](varjo_source.cpp), *which thread* calls
   `capture->onFrame(...)`, and what does it do when the consumer is slow?
   (Study `cameraSaverThreadMain` + `hasNewFrame`.)
2. In [transport.cpp](transport.cpp), which fields of `CameraFrame` actually matter
   to `publishCameraFrame`? What happens if `rowStride != width`?
3. In [main.cpp](main.cpp), what are `frameWidth(e)`/`frameHeight(e)` used for
   *before the first frame ever arrives*? What does that mean your source must know
   immediately after `start()`?
4. In [PhospheneHandler_intrinsics.py](PhospheneHandler_intrinsics.py) around
   line 543: what does Python wait for before it processes *anything*? This one is a
   trap detector — it will save you an hour of "why is Python hanging."

**✅ Checkpoint:** you can explain why a webcam source with only one physical camera
still must publish **two** eyes.

## Level 2 — The color cipher
Your webcam speaks BGR. The pipe speaks NV12. Somewhere in the Python file is the
exact line that decodes your bytes back into an image — find it first; it is the
specification for what you must produce. Then write BGR → NV12. You may find a single
OpenCV conversion that *almost* does it and needs one extra rearranging step —
figuring out which planes go where is the puzzle. Traps: odd frame dimensions, and
deciding what `rowStride` is for your buffer.

**✅ Checkpoint:** a scratch test that converts one BGR frame → NV12 → (Python's
decode path or `cv::cvtColor` back) and the colors survive the round trip. Hold
something red in front of the camera; if it comes back blue, you've swapped exactly
the thing NV12 exists to interleave.

## Level 3 — Forge the source
Now the real artifact: `webcam_source.h/.cpp`,
`class WebcamFrameSource : public IFrameSource`. You already have a working reference
implementation of the *shape* — capture thread, latest-frame handoff, clean `running`
flag — in [varjo_source.cpp](varjo_source.cpp). Steal its skeleton, replace its
heart. Design decisions you own: one capture thread or two? Same `CameraFrame` to
both eyes, or copies? What's your `frameNumber`?

**✅ Checkpoint:** a temporary `main`-like scratch that does `source.start(callback)`
and prints frame dimensions + a counter from the callback at ~30/s, then stops
cleanly *ten times in a row* without hanging. (If it hangs on stop, revisit how
`stopEyeCameraCaptures` orders flag → notify → join.)

## Level 4 — The seeing cursor
`getGaze()` v1: return the default `GazeTan{}` and move on. v2 (the fun one):
mouse-driven gaze. The riddle is the coordinate mapping — window pixels → tangent
space. Clue: the fallback constants `kCameraTanHalfX/Y` in
[transport.cpp](transport.cpp) already define how the pipeline maps tangents to your
camera's pixels; if your mouse mapping uses the same constants, the scotoma will
track your cursor *exactly*.

**✅ Checkpoint:** print `getGaze()` while moving the mouse: center of window ≈ (0,0),
edges ≈ ±0.6.

## Level 5 — Two doors into main
Wire source selection into [main.cpp](main.cpp) — a CLI flag, with **no args =
today's Varjo path, character-for-character**. You'll discover quickly that you can't
just swap the object behind `IFrameSource`: half of main's calls (`createSwapchain`,
`waitSync`, `endFrameAndSubmit`) don't exist on the interface. That's not an
accident — it's the seam telling you the webcam mode needs its *own run loop*,
sharing only the transport wiring. Whether that's a second function in main.cpp or a
second file is your architectural call.

**✅ Checkpoint:** no-arg run still prints the exact old startup lines (`git diff`
your console output against a saved log if you're feeling rigorous); `--webcam`
reaches `[SHM] ready` with zero Varjo calls executed.

## Level 6 — The mirror
The payoff level. Python is now receiving your webcam frames and publishing phosphene
masks back — but nobody's looking. Build the preview: consume with
`consumePhosphene(...)` (it hands you plain grayscale bytes + dimensions — no GL
needed, this is exactly why it was split from the upload) and composite onto the
camera frame in a desktop window. Decide how faithful to be: quick version is
alpha-blending the mask at the gaze point; faithful version re-reads the shader math
in [renderer.cpp](renderer.cpp) (`blindnessMask`, the `diff / (2.0 * radius) + 0.5`
UV mapping) and reproduces mask + phosphene placement on the CPU.

**✅ Checkpoint:** you see your own room through a scotoma that follows your mouse,
with phosphenes inside it. Screenshot it. That's the trophy.

## Level 7 — Boss: the graceful death
Ctrl-out cleanly: stop capture threads → `stopPhospheneBridge` → windows closed, and
Python prints `Python is done <3` on its own (find *why* it does — the SHUTDOWN flag
path you studied in Level 1). Run start/quit five times back to back; stale shared
memory from a crashed run is the boss's second phase.

**✅ Checkpoint:** five consecutive clean runs, no orphaned threads, no zombie
windows, Python exits by itself every time.

---

## ⭐ Bonus stars (optional, in difficulty order)

- **Throttle star:** frame-rate-limit publishing using the mechanism already sitting
  in `publishCameraFrame` (find the constant).
- **Intrinsics star:** calibrate your webcam (OpenCV chessboard), fill
  `hasIntrinsics` + focal lengths in your `CameraFrame`, and catch the moment the
  `[SHM] config` log flips `intrinsics=N` → `intrinsics=Y` — Python then computes
  exact crops instead of the 0.6-tangent guess.
- **Honesty star:** you'll be the first *real* user of `GazeTan.valid`. Decide what
  the pipeline should do with an explicitly-invalid gaze and make one consumer
  respect it.

---

## Progress tracker

- [x] Level 0 — weapon chosen (OpenCV via vcpkg), hello-webcam runs
- [x] Level 1 — four answers written down (latest-wins, stride, dims-at-open, both-eyes)
- [x] Level 2 — NV12 round-trip survives (I420 → interleave UV; decode-constant trap defeated)
- [x] Level 3 — WebcamFrameSource stops cleanly 10/10 (pull loop, self-death path, 10-cycle harness)
- [ ] Level 4 — mouse gaze maps to ±0.6
- [ ] Level 5 — `--webcam` flag, Varjo path untouched
- [ ] Level 6 — 🏆 screenshot taken
- [ ] Level 7 — boss down, 5 clean runs
- [ ] ⭐ Throttle
- [ ] ⭐ Intrinsics
- [ ] ⭐ Honesty
