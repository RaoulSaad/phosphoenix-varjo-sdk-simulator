"""run_varjo.py — the TensorRT phosphene pipeline driven by the Varjo C++ app.

Same compute pipeline as run_tensorRT.py, but the two ends are swapped:

    run_tensorRT.py :  cv2.VideoCapture  -> pipeline -> cv2.imshow
    run_varjo.py    :  shared memory     -> pipeline -> shared memory

The capture thread now pulls NV12 frames + gaze from the C++ app's `cam`
shared-memory section (one channel per eye), and the completed phosphene image
is published back into the `phos` section for the C++ renderer to display in
the headset.

Eye <-> stream mapping: eye 0 -> stream 0, eye 1 -> stream 1, so each eye gets
its own CUDA stream, buffers and dispatch thread (STREAM_COUNT must be 2).

Run (in the `phos-rtcv` env, with the C++ app already running):
    python run_varjo.py --engine-yolo engines/yolo11x-seg-static.engine \
                        --engine-enc engines/encoder-constrained.engine --fp16
"""

import argparse
import ctypes as C
import math
import os
import queue as q
import sys
import threading
import time
from typing import List

import pickle
import cv2
import numpy as np
import cupy as cp
import torch
import nvtx
import tensorrt as trt

from dynaphos.simulator import GaussianSimulator
from dynaphos.utils import Map, load_params

from model.ComputeManager import ComputeManager
from model.PhosphenePipeline import PhosphenePipeline
from misc.DrawModes import DrawModes

# --- Type definitions ---
HOSTFN = C.CFUNCTYPE(None, C.c_void_p)

# --- Variables ---
DEFAULT_SDK_DIR = r"D:\projects\simulation\Native_SDK"   # where shm_transport.py lives
STREAM_COUNT = 2                                # one stream per eye -- do not change
NMS_KEEP_MAX = 128
USE_GRAPH = False
DRAW_MODE = DrawModes.DRAW_PHOS                 # phosphene output is what the headset needs


def build_simulator(encoder_name: str = "constrained", map_scale: float = 1.0):
    """Initializes the Dynaphos simulator (unchanged from run_tensorRT.py)."""
    cfg_path = {
        "unconstrained": 'config_viseon/naturalistic_unconstrained.yaml',
        "constrained": 'config_viseon/naturalistic_constrained.yaml',
        "constrained_boundary": 'config_viseon/naturalistic_boundary.yaml'
    }[encoder_name]

    params_path = 'config_viseon/simulator_config.yaml'
    coords_path = 'config_viseon/DefaultCoordinateMap_1000_phosphenes.pickle'

    print(f"Loading Dynaphos cfg from: {cfg_path}")
    cfg = load_params(cfg_path)

    print(f"Loading Dynaphos params from: {params_path}")
    params = load_params(params_path)
    params['run'].update(cfg)
    params['thresholding'].update(cfg)

    print(f"Loading Dynaphos coordinates from: {coords_path}")
    with open(coords_path, 'rb') as handle:
        coordinates_visual_field = pickle.load(handle)

    if map_scale <= 0.0:
        raise ValueError("--phosphene-map-scale must be greater than zero")
    x, y = coordinates_visual_field.cartesian
    x = np.asarray(x)
    y = np.asarray(y)

    # GaussianSimulator removes coordinates outside +/- view_angle/2. The
    # encoder always emits exactly 1000 currents, so losing even one coordinate
    # makes its output impossible to reshape to the simulator state. Cap the
    # requested zoom just inside the field to preserve all electrodes.
    half_view = float(params["run"]["view_angle"]) / 2.0
    max_abs_coordinate = max(float(np.max(np.abs(x))), float(np.max(np.abs(y))))
    max_safe_scale = (half_view * 0.999) / max_abs_coordinate
    if map_scale > max_safe_scale:
        print(
            f"[WARN] phosphene map scale {map_scale:g} would clip electrodes; "
            f"using maximum safe scale {max_safe_scale:.4f}"
        )
        map_scale = max_safe_scale

    x = x * map_scale
    y = y * map_scale
    coordinates_visual_field = Map(x=x, y=y)
    print(
        f"Phosphene map scale: {map_scale:g} "
        f"(extent x=[{x.min():.1f},{x.max():.1f}] deg, "
        f"y=[{y.min():.1f},{y.max():.1f}] deg)"
    )

    simulator = GaussianSimulator(params, coordinates_visual_field)

    sim_res = tuple(params["run"]["resolution"])  # (W, H)
    print(f"Dynaphos simulator initialized for input resolution {sim_res}.")

    return simulator, params


# ------------------------ Shared-memory capture thread ------------------------
def start_shm_capture_thread(bridge, queue_list: List[q.Queue], stop_event: threading.Event,
                             undistort: bool):
    """Replaces the webcam capture thread: pulls frames + gaze from the C++ app.

    Each eye is polled independently and pushed to its own queue (latest-wins,
    so a slow GPU never builds a backlog).
    """

    def capture_loop():
        while not stop_event.is_set():
            if bridge.is_shutdown():
                print("[SHM] shutdown signalled by C++")
                stop_event.set()
                break

            # Pick up crop-size / intrinsics / blindness-mode changes from C++.
            bridge.refresh_config()

            got_any = False
            for eye in range(STREAM_COUNT):
                item = bridge.poll_camera(eye)
                if item is None:
                    continue
                got_any = True

                nvtx.mark("capture_frame", color="green")

                frame = bridge.to_bgr(item)
                if undistort:
                    frame = bridge.undistort(frame, eye)

                # The crop kernel wants (dy, dx) offsets from the frame centre,
                # as fractions of frame height/width.
                gaze_point = bridge.gaze_point_fraction(item, eye)

                payload = (frame, gaze_point, item["frame_id"])

                # cv2.imshow(f"SHM capture eye {eye}", frame)
                # cv2.waitKey(1)

                try:
                    queue_list[eye].put_nowait(payload)
                except q.Full:
                    try:
                        queue_list[eye].get_nowait()
                    except q.Empty:
                        pass
                    try:
                        queue_list[eye].put_nowait(payload)
                    except q.Full:
                        pass

            if not got_any:
                time.sleep(0.001)   # nothing new; poll gently

    capture_thread = threading.Thread(target=capture_loop, name="shm_capture_thread")
    capture_thread.start()
    print("Capture thread started (shared memory)")
    return capture_thread


# ------------------------ Async dispatch thread ------------------------
def start_dispatch_thread(queue_list: List[q.Queue], dispatch_lock: threading.Lock,
                          stop_event: threading.Event, compute_manager: ComputeManager,
                          fn_dispatch_async: callable):
    """One dispatch thread per stream/eye (unchanged apart from carrying frame_id)."""

    def dispatch_loop(stream_idx: int):
        stream_wrapper = compute_manager.streams[stream_idx]

        while not stop_event.is_set():
            if stream_wrapper.inflight:
                time.sleep(0.005)
                continue

            try:
                frame, gaze_point, frame_id = queue_list[stream_idx].get(timeout=0.01)
            except q.Empty:
                continue

            dispatch_lock.acquire()
            try:
                stream_wrapper.gaze_point = gaze_point
                stream_wrapper.frame_id = frame_id      # so the result can be matched back
                np.copyto(stream_wrapper.h2d_buffer_host_np, frame)

                stream_wrapper.inflight = True
                
                if USE_GRAPH:
                    stream_wrapper.launch_graph()
                else:
                    fn_dispatch_async(stream_idx)

                
            finally:
                dispatch_lock.release()

    dispatch_threads_list = []
    for i in range(STREAM_COUNT):
        t = threading.Thread(target=dispatch_loop, args=(i,), name=f"dispatch_thread_{i}")
        t.start()
        dispatch_threads_list.append(t)

    print("Dispatch threads started")
    return dispatch_threads_list


# ------------------------ Main ------------------------
def parse_args():
    ap = argparse.ArgumentParser("Real-time Phosphene Pipeline (Varjo shared-memory I/O)")
    ap.add_argument("--engine-yolo", type=str, required=True, help="Path to YOLO TensorRT engine (.engine)")
    ap.add_argument("--engine-enc", type=str, required=True, help="Path to encoder TensorRT engine (.engine)")
    ap.add_argument("--conf", type=float, default=0.25, help="Confidence threshold")
    ap.add_argument("--iou", type=float, default=0.5, help="NMS IoU threshold")
    ap.add_argument("--fp32", action="store_true", help="Use full precision FP32")
    ap.add_argument("--fp16", action="store_true", help="Use half precision FP16")
    ap.add_argument("--int8", action="store_true", help="Use quarter precision INT8")
    # The SHM bridge always hands us BGR (cv2.COLOR_YUV2BGR_NV12), and YOLO
    # wants RGB, so the swap belongs on by default here -- unlike
    # run_tensorRT.py, where the source's colour order varies.
    ap.add_argument("--bgr", dest="bgr", action="store_true", default=True,
                    help="Frame is BGR; swap to RGB for YOLO (default: on)")
    ap.add_argument("--no-bgr", dest="bgr", action="store_false",
                    help="Frame is already RGB; do not swap channels")
    ap.add_argument("--mirror", action="store_true", help="Flip the captured frame horizontally")
    ap.add_argument("--sdk-dir", type=str, default=DEFAULT_SDK_DIR,
                    help="Directory containing shm_transport.py (the C++ app's folder)")
    ap.add_argument("--encoder-config", type=str, default="constrained",
                    choices=["constrained", "unconstrained", "constrained_boundary"])
    ap.add_argument("--phosphene-map-scale", type=float, default=1.0,
                    help="Scale VIsEON electrode coordinates in visual degrees; "
                         "use about 1.9 to fill the same 30-degree field as the old handler")
    ap.add_argument("--no-undistort", action="store_true",
                    help="Skip camera undistortion (faster, slightly wrong geometry off-centre)")
    ap.add_argument("--preview", action="store_true", help="Show a desktop preview window")
    return ap.parse_args()


def main():
    args = parse_args()

    print("tensorrt version:", trt.__version__)

    assert args.fp32 | args.fp16 | args.int8, "At least one of --fp32, --fp16, --int8 must be set"
    assert sum([args.fp32, args.fp16, args.int8]) == 1, "Exactly one of --fp32, --fp16, --int8 must be set"

    dtype = np.float32
    if args.fp16:
        dtype = np.float16
    elif args.int8:
        dtype = np.int8

    # --- connect to the C++ app first: it defines frame size, crop size, gaze ---
    if args.sdk_dir not in sys.path:
        sys.path.insert(0, args.sdk_dir)
    try:
        import shm_transport
    except ImportError as e:
        raise SystemExit(f"Cannot import shm_transport from {args.sdk_dir}: {e}\n"
                         f"Pass --sdk-dir <path to Native_SDK>")

    bridge = shm_transport.ShmBridge().open()
    cfg_l, cfg_r = bridge.configs

    if (cfg_l["frame_w"], cfg_l["frame_h"]) != (cfg_r["frame_w"], cfg_r["frame_h"]):
        print(f"[WARN] eyes report different frame sizes "
              f"({cfg_l['frame_w']}x{cfg_l['frame_h']} vs {cfg_r['frame_w']}x{cfg_r['frame_h']}); "
              f"using the left eye's for buffer allocation")

    frame_shape = (cfg_l["frame_h"], cfg_l["frame_w"], 3)
    crop_shape = (cfg_l["crop_h"], cfg_l["crop_w"])   # gaze-centred crop, in camera pixels

    metadata = {
        "draw_mode": DRAW_MODE,
        "frame_shape": frame_shape,
        "frame_dtype": np.uint8,
        "crop_shape": crop_shape,
    }
    print(f"[SHM] pipeline sized from C++: frame={frame_shape}, crop={crop_shape}")

    queue_dispatch_list = [q.Queue(maxsize=1) for _ in range(STREAM_COUNT)]
    queue_display = q.Queue(maxsize=STREAM_COUNT * 2)

    @C.CFUNCTYPE(None, C.c_void_p)
    def cuda_host_callback(userdata):
        with nvtx.annotate("cuda_host_callback", color="red"):
            try:
                stream_done_idx = C.cast(userdata, C.POINTER(C.c_int)).contents.value
                stream_wrapper = compute_manager.streams[stream_done_idx]
                frame_completed = stream_wrapper.d2h_buffer_host_np.copy()
                # Read frame_id before releasing the slot, or the dispatch thread
                # can overwrite it with the next frame's id.
                frame_id = getattr(stream_wrapper, "frame_id", 0)

                stream_wrapper.inflight = False

                payload = (frame_completed, stream_done_idx, frame_id)
                try:
                    queue_display.put_nowait(payload)
                except q.Full:
                    queue_display.get_nowait()
                    queue_display.put_nowait(payload)

            except Exception as e:
                print(f"ERROR: cuda_host_callback: {e}")

    c_cuda_host_callback = HOSTFN(cuda_host_callback)

    contour_classes = list(range(80))
    whitelists = {"solid": [], "contour": contour_classes, "edge": []}

    compute_manager = ComputeManager(
        args.engine_yolo,
        args.engine_enc,
        metadata,
        dtype=dtype,
        num_streams=STREAM_COUNT,
        whitelists=whitelists,
    )
    phosphene_pipeline = PhosphenePipeline(compute_manager, c_cuda_host_callback)

    for sw in compute_manager.streams:
        sw.frame_id = 0

    simulator, sim_params = build_simulator(
        args.encoder_config,
        map_scale=args.phosphene_map_scale,
    )
    torch.set_grad_enabled(False)

    # --- geometry sanity check: the phosphene image the C++ shader draws spans
    # +/- kDeviceFieldTan, and the simulator renders +/- view_angle/2. The crop
    # C++ asked for encodes its expectation; warn loudly if they disagree.
    view_angle = float(sim_params["run"]["view_angle"])
    sim_tan = math.tan(math.radians(view_angle / 2.0))
    dev_tan = bridge.device_field_tan(0)
    if dev_tan is None:
        print("[WARN] no intrinsics: cannot verify field-of-view match")
    else:
        rel = abs(dev_tan - sim_tan) / sim_tan
        msg = (f"[GEOM] simulator view_angle={view_angle:.1f} deg (half-field tan={sim_tan:.3f}) "
               f"vs C++ device field tan={dev_tan:.3f} "
               f"({2 * math.degrees(math.atan(dev_tan)):.1f} deg)")
        if rel > 0.05:
            print(msg)
            print(f"[WARN] FIELD MISMATCH ({rel * 100:.0f}%): phosphenes will be drawn at the wrong "
                  f"scale.\n       Fix: set kDeviceFieldTan = {sim_tan:.4f}f in "
                  f"Native_SDK/pipeline_types.h and rebuild,\n"
                  f"       or set view_angle = {2 * math.degrees(math.atan(dev_tan)):.0f} in "
                  f"config_viseon/simulator_config.yaml.")
        else:
            print(msg + "  [OK]")

    def fn_dispatch_async(dispatch_stream_idx: int):
        # --- Start of Async segment (unchanged from run_tensorRT.py) ---
        phosphene_pipeline.transfer_frame(dispatch_stream_idx)
        phosphene_pipeline.segmentation_preprocess(dispatch_stream_idx, args.bgr, args.mirror)
        phosphene_pipeline.segmentation_process(dispatch_stream_idx)
        phosphene_pipeline.segmentation_postprocess(
            dispatch_stream_idx,
            score_threshold=args.conf,
            iou_threshold=args.iou
        )

        # DRAW_PHOS path: composite masks -> gaze crop -> encoder -> stim vector
        phosphene_pipeline.composite_mask(dispatch_stream_idx, 1, draw_mode=DRAW_MODE,
                                          is_bgr=args.bgr, mirror=args.mirror)
        phosphene_pipeline.encoder_preprocess(dispatch_stream_idx)
        phosphene_pipeline.encoder_process(dispatch_stream_idx)
        phosphene_pipeline.encoder_postprocess(dispatch_stream_idx)

        phosphene_pipeline.finalize(dispatch_stream_idx, draw_mode=DRAW_MODE)
        # --- End of Async segment ---

    if USE_GRAPH:
        cp.cuda.runtime.deviceSynchronize()
        cp.get_default_memory_pool().free_all_blocks()
        cp.get_default_pinned_memory_pool().free_all_blocks()
        cp.cuda.set_allocator(None)
        cp.cuda.set_pinned_memory_allocator(None)
        for stream in compute_manager.streams:
            fn_dispatch_async(stream.index)
            from cuda.bindings import runtime as cudart
            cudart.cudaStreamSynchronize(stream.stream)
            compute_manager.record_graph(stream, fn_dispatch_async)
        cp.cuda.set_allocator(compute_manager.memory_pool.malloc)
        cp.cuda.set_pinned_memory_allocator(compute_manager.memory_pool_pinned.malloc)

    stop_event = threading.Event()
    dispatch_lock = threading.Lock()
    capture_thread = start_shm_capture_thread(bridge, queue_dispatch_list, stop_event,
                                              undistort=not args.no_undistort)
    dispatch_threads_list = start_dispatch_thread(queue_dispatch_list, dispatch_lock, stop_event,
                                                  compute_manager, fn_dispatch_async=fn_dispatch_async)

    win = "Varjo phosphene pipeline (q to quit)"
    if args.preview:
        cv2.namedWindow(win, cv2.WINDOW_NORMAL | cv2.WINDOW_KEEPRATIO)

    print("Running. Ctrl+C (or 'q' in the preview window) to quit.")

    frame_count = [0, 0]
    t_last = time.time()

    try:
        while not stop_event.is_set():
            with nvtx.annotate("main::publish_loop", color="red"):
                try:
                    frame_completed, stream_idx, frame_id = queue_display.get(timeout=0.01)
                except q.Empty:
                    if bridge.is_shutdown():
                        break
                    continue

                eye = stream_idx     # eye <-> stream mapping

                # Dynaphos: stimulation vector -> phosphene image
                nvtx_range = nvtx.start_range(message="main::simulator", color="red")
                simulator.reset()
                stim = torch.from_numpy(frame_completed).cuda()
                phos = simulator(stim)
                phos_img = (phos.numpy(force=True) * 255).astype("uint8")
                nvtx.end_range(nvtx_range)

                # Hand the phosphene image back to the C++ renderer.
                bridge.publish_phosphene(eye, frame_id, phos_img)

                frame_count[eye] += 1
                t_now = time.time()
                delta_t = t_now - t_last
                if delta_t >= 1.0:
                    print(f"[FPS] left={frame_count[0] / delta_t:5.1f}  "
                          f"right={frame_count[1] / delta_t:5.1f}  "
                          f"(phos {phos_img.shape[1]}x{phos_img.shape[0]})")
                    # Where in the chain the signal dies (see interpretation
                    # notes in the repo discussion / commit message):
                    #   detections=0 forever      -> YOLO never sees anything
                    #   stim max ~0.5..1.0        -> encoder engine lacks the
                    #                                128e-6 output scaling
                    #   above_rheobase ~constant  -> encoder input is constant
                    stim_np = frame_completed.reshape(-1).astype(np.float32)
                    rheobase = float(sim_params["thresholding"]["rheobase"])
                    sw_dbg = compute_manager.streams[stream_idx]
                    n_det = int(sw_dbg.nms_survivors_count.get()[0])
                    sc = sw_dbg.scores.get()
                    print(f"[DBG] eye={eye} detections={n_det} "
                          f"stim min={stim_np.min():.3e} max={stim_np.max():.3e} "
                          f"mean={stim_np.mean():.3e} "
                          f"above_rheobase={int((stim_np > rheobase).sum())}/{stim_np.size}")
                    # scores min ~0.5 and max <=0.731 means the class scores got
                    # sigmoid()-ed twice (ultralytics already activates them on
                    # export), so every anchor clears any threshold <= 0.5.
                    print(f"[DBG] scores min={sc.min():.3f} max={sc.max():.3f} "
                          f"above_conf={int((sc >= args.conf).sum())}/{sc.size}")
                    frame_count = [0, 0]
                    t_last = t_now

                if args.preview:
                    disp = phos_img if phos_img.ndim == 3 else np.stack([phos_img] * 3, axis=2)
                    cv2.imshow(win, disp)
                    # What the encoder actually received for this frame (float
                    # [0,1]; may tear if the next frame is already in flight —
                    # debug only). Black here = the problem is upstream of the
                    # encoder (YOLO input / composite / gaze crop).
                    enc_in = compute_manager.streams[stream_idx].input_buffer_encoder[0, 0].get()
                    cv2.imshow(f"encoder input (eye {eye})", enc_in.astype(np.float32))
                    if (cv2.waitKey(1) & 0xFF) == ord('q'):
                        break
    except KeyboardInterrupt:
        print("\ninterrupted")

    print("\nexiting...")
    stop_event.set()

    for thread in dispatch_threads_list:
        thread.join(timeout=0.5)
    capture_thread.join(timeout=0.5)

    bridge.close()
    if args.preview:
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
