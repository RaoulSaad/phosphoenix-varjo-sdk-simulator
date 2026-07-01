import sys
import time
import cv2
import numpy as np
import mmap
import struct
import os
import threading
import tkinter as tk
from tkinter import ttk
import importlib
import inspect
import math

from collections import deque
from concurrent.futures import ThreadPoolExecutor

from dynaphos.image_processing import sobel_processor, canny_processor
from dynaphos.simulator import GaussianSimulator
from dynaphos.utils import load_params, load_coordinates_from_yaml, Map
from dynaphos.cortex_models import get_visual_field_coordinates_from_cortex_full

from base_processing_algorithm import BaseProcessingAlgorithm

print("Python is running!")

#########################
# Shared-memory layout  #
#########################
# Mirrors the C++ side in main.cpp exactly (same names/sizes). Two named,
# pagefile-backed sections: cam (C++ -> Python NV12 frames) and phos
# (Python -> C++ grayscale). Each is [ Header | eye0 channel | eye1 channel ];
# a channel is a lock-free double buffer [ Ctrl | slot0 | slot1 ], slot =
# [ SlotMeta | bytes ]. Publish/consume is the seqlock in shm_publish/shm_consume.
SHM_CAM_NAME  = "Local\\VarjoPhospheneCam"
SHM_PHOS_NAME = "Local\\VarjoPhospheneOut"

SHM_MAGIC   = 0x50484D31   # 'PHM1'
SHM_VERSION = 1
SHM_FLAG_READY    = 1 << 0
SHM_FLAG_SHUTDOWN = 1 << 1

SHM_NUM_EYES  = 2
SHM_NUM_SLOTS = 2          # double buffer

SHM_HEADER_SIZE   = 512
SHM_CTRL_SIZE     = 64
SHM_SLOTMETA_SIZE = 64

CAM_SLOT_CAP  = 8 * 1024 * 1024
PHOS_SLOT_CAP = 4 * 1024 * 1024

# struct formats (little-endian, packed) -- must match the C++ #pragma pack(1) structs
SHM_HEADER_FMT   = "<IIIIiI"    # magic, version, flags, configSeq, blindnessMode, reserved0  (24 B)
SHM_EYECFG_FMT   = "<8i12d"     # 8 int32 + 12 double                                          (128 B)
SHM_SLOTMETA_FMT = "<6I2f8I"    # frameId,eye,w,h,rowStride,byteSize, gazeX,gazeY, reserved[8] (64 B)


CAM_SLOT_STRIDE   = SHM_SLOTMETA_SIZE + CAM_SLOT_CAP
CAM_CHANNEL_SIZE  = SHM_CTRL_SIZE + SHM_NUM_SLOTS * CAM_SLOT_STRIDE
CAM_MAP_SIZE      = SHM_HEADER_SIZE + SHM_NUM_EYES * CAM_CHANNEL_SIZE
PHOS_SLOT_STRIDE  = SHM_SLOTMETA_SIZE + PHOS_SLOT_CAP
PHOS_CHANNEL_SIZE = SHM_CTRL_SIZE + SHM_NUM_SLOTS * PHOS_SLOT_STRIDE
PHOS_MAP_SIZE     = SHM_HEADER_SIZE + SHM_NUM_EYES * PHOS_CHANNEL_SIZE


def cam_channel_off(eye):
    return SHM_HEADER_SIZE + eye * CAM_CHANNEL_SIZE


def phos_channel_off(eye):
    return SHM_HEADER_SIZE + eye * PHOS_CHANNEL_SIZE


def read_header(mm):
    magic, version, flags, config_seq, mode, _res = struct.unpack_from(SHM_HEADER_FMT, mm, 0)
    return magic, version, flags, config_seq, mode


def read_eye_config(mm, eye):
    base = struct.calcsize(SHM_HEADER_FMT) + eye * struct.calcsize(SHM_EYECFG_FMT)
    v = struct.unpack_from(SHM_EYECFG_FMT, mm, base)
    return {
        "crop_w": v[0], "crop_h": v[1], "frame_w": v[2], "frame_h": v[3],
        "row_stride": v[4], "intr_model": v[5], "intr_valid": bool(v[6]),
        "focal_x": v[8], "focal_y": v[9], "pp_x": v[10], "pp_y": v[11],
        "coeffs": list(v[12:20]),
    }


def shm_consume(mm, channel_off, slot_stride, last_seq):
    """Latest-wins seqlock read. Returns (new_last_seq, meta_tuple, payload) or
    (last_seq, None, None) when there is nothing newer / a torn read persists."""
    for _ in range(8):
        s1 = struct.unpack_from("<I", mm, channel_off + 4)[0]   # publishSeq
        if s1 == last_seq:
            return last_seq, None, None
        idx = struct.unpack_from("<I", mm, channel_off + 0)[0]  # latestIndex
        if idx >= SHM_NUM_SLOTS:
            return last_seq, None, None
        so = channel_off + SHM_CTRL_SIZE + idx * slot_stride
        meta = struct.unpack_from(SHM_SLOTMETA_FMT, mm, so)
        n = meta[5]
        if n > slot_stride - SHM_SLOTMETA_SIZE:
            n = 0
        payload = bytes(mm[so + SHM_SLOTMETA_SIZE: so + SHM_SLOTMETA_SIZE + n])
        if struct.unpack_from("<I", mm, channel_off + 4)[0] == s1:  # no publish mid-copy
            return s1, meta, payload
        # producer published while we copied; retry for a consistent snapshot
    return last_seq, None, None


def shm_publish(mm, channel_off, slot_stride, slot_cap, write_index,
                frame_id, eye, width, height, row_stride, gaze_x, gaze_y, payload):
    """Write meta + payload into the free slot and publish it (x86 store order)."""
    w = write_index
    so = channel_off + SHM_CTRL_SIZE + w * slot_stride
    n = min(len(payload), slot_cap)
    struct.pack_into(SHM_SLOTMETA_FMT, mm, so,
                     frame_id, eye, width, height, row_stride, n,
                     gaze_x, gaze_y, 0, 0, 0, 0, 0, 0, 0, 0)
    mm[so + SHM_SLOTMETA_SIZE: so + SHM_SLOTMETA_SIZE + n] = payload[:n]
    struct.pack_into("<I", mm, channel_off + 0, w)                 # latestIndex
    seq = struct.unpack_from("<I", mm, channel_off + 4)[0]
    struct.pack_into("<I", mm, channel_off + 4, (seq + 1) & 0xFFFFFFFF)  # publishSeq (publish)
    return (w + 1) % SHM_NUM_SLOTS

EYE_LEFT = 0
EYE_RIGHT = 1
NUM_EYES = 2

VARJO_INTRINSICS_MODEL_OMNIDIR = 1
VARJO_INTRINSICS_MODEL_RATIONAL = 2

IN_QUEUE_MAX = 8


def open_shared_memory(timeout=60.0):
    """Open both mappings and block until C++ marks them READY.

    Either process may create the section first; the fixed size means both agree,
    and the READY flag (set by C++ last) gates use. mmap(-1, tagname=...) opens the
    existing section or creates a zero-filled one of the same size.
    """
    print("Opening shared memory, waiting for C++ (READY)...")
    cam = mmap.mmap(-1, CAM_MAP_SIZE, tagname=SHM_CAM_NAME, access=mmap.ACCESS_WRITE)
    phos = mmap.mmap(-1, PHOS_MAP_SIZE, tagname=SHM_PHOS_NAME, access=mmap.ACCESS_WRITE)
    t0 = time.time()
    while time.time() - t0 < timeout:
        magic, _version, flags, _seq, _mode = read_header(cam)
        if magic == SHM_MAGIC and (flags & SHM_FLAG_READY):
            return cam, phos
        time.sleep(0.01)
    raise TimeoutError("C++ shared memory not READY within timeout")

#########################
# Blindness Mode Values #
#########################
BLINDNESS_MACULAR = 0
BLINDNESS_GLAUCOMA = 1
BLINDNESS_FULL = 2
BLINDNESS_NAMES = {BLINDNESS_MACULAR: "macular", BLINDNESS_GLAUCOMA: "glaucoma", BLINDNESS_FULL: "full"}

# C++ (gBlindnessMode) is the SINGLE SOURCE OF TRUTH for the blindness type: it
# sends the mode in every config packet and Python adopts whatever it receives.
# This value is only a fallback for the brief window before the first config.
blindness_mode = BLINDNESS_MACULAR

# Electrode grid.
# A single fixed implant, used by every blindness type -- the electrodes do not
# move with the diagnosis. grid_coords_full_field.yaml is a realistic V1 array:
# mostly foveal (dense centre) but extending to ~24 deg, so the SAME device serves
# central loss (macular fills the scotoma) and peripheral loss (glaucoma gets a
# peripheral ring). The disease only changes the C++ scotoma mask -- which of these
# fixed, gaze-locked phosphenes fall in the blind region and are therefore shown.
# (dynaphos models a V1 implant; we use it as a retinotopic proxy for the LGN
# target, so phosphene positions are approximate.)
# Regenerate/tune the grid with generate_device_coords.py; its DEVICE_HALF_FOV_DEG
# must match params.yaml view_angle (2x) and the C++ kDeviceFieldTan (tan24~=0.45).

if len(sys.argv) == 2:
    python_dir = sys.argv[1].strip('"')
else:
    print("Incorrect program arguments! We need path to the directory.", flush=True)
    sys.exit(1)


cam_mm, phos_mm = open_shared_memory()

# Read the initial per-eye config + blindness mode straight from the cam header.
_, _, _, last_config_seq, blindness_mode = read_header(cam_mm)
runtime_cfgs = [read_eye_config(cam_mm, e) for e in range(NUM_EYES)]
for _e, _c in enumerate(runtime_cfgs):
    print(
        f"[eye={_e}] Using crop={_c['crop_w']}x{_c['crop_h']}, "
        f"frame={_c['frame_w']}x{_c['frame_h']}, stride={_c['row_stride']}"
    )
print(f"[PYTHON] Blindness mode from C++: {blindness_mode} "
      f"({BLINDNESS_NAMES.get(blindness_mode, '?')})")

config_lock = threading.Lock()
# Per-eye undistort maps cache.
undistort_lock = threading.Lock()
undistort_maps = [
    {"key": None, "map1": None, "map2": None, "new_K": None},
    {"key": None, "map1": None, "map2": None, "new_K": None},
]


# Per-eye buffers and control variables
buffers = [
    [
        np.zeros((runtime_cfgs[e]["crop_h"], runtime_cfgs[e]["crop_w"]), dtype=np.uint8)
        for _ in range(2)
    ]
    for e in range(NUM_EYES)
]
most_recent_image = [0, 0]
is_reading_image_buffer = [False, False]
image_being_written = [1, 1]


def build_undistort_maps(cfg):
    if not cfg["intr_valid"]:
        return None, None, None

    width = cfg["frame_w"]
    height = cfg["frame_h"]
    fx = cfg["focal_x"]
    fy = cfg["focal_y"]
    cx = cfg["pp_x"]
    cy = cfg["pp_y"]
    coeffs = cfg["coeffs"]
    model = cfg["intr_model"]

    if model == VARJO_INTRINSICS_MODEL_RATIONAL:
        fx_px = fx * width
        fy_px = fy * height
        cx_px = cx * width
        cy_px = cy * height

        K_px = np.array([[fx_px, 0.0, cx_px],
                         [0.0, fy_px, cy_px],
                         [0.0, 0.0, 1.0]], dtype=np.float64)

        k1, k2, k3, k4, p1, p2, k5, k6 = coeffs[:8]
        D = np.array([k1, k2, p1, p2, k3, k4, k5, k6], dtype=np.float64)

        new_K, roi = cv2.getOptimalNewCameraMatrix(K_px, D, (width, height), alpha=0.0)
        map1, map2 = cv2.initUndistortRectifyMap(
            K_px, D, np.eye(3, dtype=np.float64),
            K_px, (width, height), cv2.CV_32FC1,
        )
        return map1, map2, K_px

    if model == VARJO_INTRINSICS_MODEL_OMNIDIR and hasattr(cv2, "omnidir"):
        K = np.array([[fx, 0.0, cx],
                  [0.0, fy, cy],
                  [0.0, 0.0, 1.0]], dtype=np.float64)
        skew = coeffs[2]
        xi = np.array([coeffs[3]], dtype=np.float64)
        p1 = coeffs[4]
        p2 = coeffs[5]
        K_omni = np.array([[fx, skew, cx],
                           [0.0, fy, cy],
                           [0.0, 0.0, 1.0]], dtype=np.float64)
        D = np.array([coeffs[0], coeffs[1], p1, p2], dtype=np.float64).reshape(1, 4)
        flags = getattr(cv2.omnidir, "RECTIFY_PERSPECTIVE", 1)
        map1, map2 = cv2.omnidir.initUndistortRectifyMap(
            K_omni,
            D,
            xi,
            np.eye(3, dtype=np.float64),
            K,
            (width, height),
            cv2.CV_32FC1,
            flags,
        )
        return map1, map2, K

    print("[PYTHON] Warning: omnidir intrinsics received but cv2.omnidir is unavailable; skipping undistortion")
    return None, None, None


def get_undistort_maps(cfg, eye):
    key = (
        cfg["frame_w"], cfg["frame_h"], cfg["intr_model"], cfg["intr_valid"],
        round(cfg["focal_x"], 6), round(cfg["focal_y"], 6),
        round(cfg["pp_x"], 6), round(cfg["pp_y"], 6),
        tuple(round(float(c), 6) for c in cfg["coeffs"]),
    )
    with undistort_lock:
        slot = undistort_maps[eye]
        if slot["key"] != key:
            map1, map2, new_K = build_undistort_maps(cfg)
            slot["key"] = key
            slot["map1"] = map1
            slot["map2"] = map2
            slot["new_K"] = new_K
        return slot["map1"], slot["map2"], slot["new_K"]


def undistort_bgr(img, cfg, eye):
    if not cfg["intr_valid"]:
        return img
    map1, map2, _ = get_undistort_maps(cfg, eye)
    if map1 is None or map2 is None:
        return img
    return cv2.remap(img, map1, map2, interpolation=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)


def crop_with_black(img, gaze_tan_x, gaze_tan_y, crop_w, crop_h, focal_x, focal_y, pp_x, pp_y):
    """
    img: HxW or HxWxC (uint8), assumed undistorted with projection matrix K.
    gaze_tan_x, gaze_tan_y: gaze center in tangent-space coordinates.
    Returns: crop_h x crop_w (or crop_h x crop_w x C) with black padding.
    """
    H, W = img.shape[:2]
    C = 1 if img.ndim == 2 else img.shape[2]

    out = np.zeros((crop_h, crop_w, C), dtype=img.dtype) if C > 1 else np.zeros((crop_h, crop_w), dtype=img.dtype)

    cx = pp_x + focal_x * gaze_tan_x
    cy = pp_y - focal_y * gaze_tan_y
    print(f"[GAZE] gx = {gaze_tan_x}, gy = {gaze_tan_y}; cx = {cx}, cy = {cy}")

    left = int(math.floor(cx - crop_w / 2))
    top = int(math.floor(cy - crop_h / 2))

    src_x0 = max(0, left)
    src_y0 = max(0, top)

    dst_x0 = max(0, -left)
    dst_y0 = max(0, -top)

    copy_w = max(0, min(crop_w - dst_x0, W - src_x0))
    copy_h = max(0, min(crop_h - dst_y0, H - src_y0))
    if copy_w <= 0 or copy_h <= 0:
        return out

    if C == 1:
        out[dst_y0:dst_y0 + copy_h, dst_x0:dst_x0 + copy_w] = img[src_y0:src_y0 + copy_h, src_x0:src_x0 + copy_w]
    else:
        out[dst_y0:dst_y0 + copy_h, dst_x0:dst_x0 + copy_w, :] = img[src_y0:src_y0 + copy_h, src_x0:src_x0 + copy_w, :]

    return out


class FilterApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Algorithm Selection")

        self.filter_var = tk.StringVar(self)
        self.filter_dropdown = ttk.Combobox(self, textvariable=self.filter_var)
        self.filter_dropdown.set("Select Algorithm")
        self.filter_dropdown.pack(pady=10)

        self.start_button = ttk.Button(self, text="Start Simulation", command=self.start_processing)
        self.start_button.pack(pady=10)

        self.algorithms = self.get_algorithms()
        self.filter_dropdown['values'] = list(self.algorithms.keys())

    def get_algorithms(self):
        algorithms_dict = {}
        for file in [f for f in os.listdir(python_dir + "/processing_algorithms") if f.endswith(".py") and not f.startswith("__")]:
            module_name = file.split(".")[0]
            if module_name:
                try:
                    module = importlib.import_module(f"processing_algorithms.{module_name}")
                    classes = inspect.getmembers(module, inspect.isclass)
                    for name, algorithm_class in classes:
                        if issubclass(algorithm_class, BaseProcessingAlgorithm) and name != "BaseProcessingAlgorithm":
                            algorithms_dict[name] = algorithm_class
                except ModuleNotFoundError as e:
                    print(f"Error importing {module_name}: {e}")
        return algorithms_dict

    def start_processing(self):
        algorithm_name = self.filter_var.get()
        if algorithm_name in self.algorithms:
            algorithm_class = self.algorithms[algorithm_name]
            algorithm = algorithm_class()
            _params = load_params(python_dir + '/params.yaml')
            main(_params, algorithm, self)
        else:
            print("Please select a valid filter!")


def main(params: dict, algorithm, FilterApp):
    FilterApp.destroy()
    # Single fixed implant: the same full-field electrode grid for every mode.
    coordinates_cortex = load_coordinates_from_yaml(python_dir + '/grid_coords_full_field.yaml')
    coordinates_cortex = Map(*coordinates_cortex)
    coordinates_visual_field = get_visual_field_coordinates_from_cortex_full(params['cortex_model'], coordinates_cortex)
    # One simulator per eye so temporal state (charge accumulation, etc.) stays independent.
    simulators = [
        GaussianSimulator(params, coordinates_visual_field),
        GaussianSimulator(params, coordinates_visual_field),
    ]
    resolution = params['run']['resolution']
    fps = params['run']['fps']
    print("Cortex Model: ", params['cortex_model'])
    print("Resolution: ", resolution)

    # Latest unprocessed camera frame per eye (drop-stale). Processed as a
    # synchronised pair, so we only ever keep the newest frame for each eye.
    pending = [None, None]   # pending[eye] = (img_in, gx, gy, frame_id)
    q_lock = threading.Lock()
    q_cond = threading.Condition(q_lock)
    stop_event = threading.Event()

    def shm_reader_loop():
        global blindness_mode, last_config_seq
        cam_last_seq = [0, 0]
        while not stop_event.is_set():
            _magic, _ver, flags, config_seq, mode = read_header(cam_mm)
            if flags & SHM_FLAG_SHUTDOWN:
                print("Received shutdown from C++")
                stop_event.set()
                break

            # Config change (crop size / intrinsics / blindness mode)?
            if config_seq != last_config_seq:
                with config_lock:
                    for e in range(NUM_EYES):
                        runtime_cfgs[e] = read_eye_config(cam_mm, e)
                blindness_mode = mode
                last_config_seq = config_seq
                c0 = runtime_cfgs[EYE_LEFT]
                print(f"[PYTHON] Config updated (seq={config_seq}) blindness={mode} "
                      f"crop=({c0['crop_w']},{c0['crop_h']}) frame=({c0['frame_w']},{c0['frame_h']}) "
                      f"intrinsics={c0['intr_valid']}")

            # Poll both eyes for a newer camera frame (latest-wins).
            got = False
            for eye in (EYE_LEFT, EYE_RIGHT):
                cam_last_seq[eye], meta, payload = shm_consume(
                    cam_mm, cam_channel_off(eye), CAM_SLOT_STRIDE, cam_last_seq[eye])
                if meta is None:
                    continue
                got = True
                frame_id, _m_eye, fw, fh, rs, _n, gx, gy = meta[:8]
                expected = rs * fh * 3 // 2
                if fw <= 0 or fh <= 0 or len(payload) != expected:
                    print(f"[PYTHON] Dropping frame eye={eye} id={frame_id}: "
                          f"expected {expected} bytes, got {len(payload)}")
                    continue

                nv12 = np.frombuffer(payload, np.uint8)
                y_plane = nv12[:rs * fh].reshape(fh, rs)[:, :fw]
                uv_plane = nv12[rs * fh:].reshape(fh // 2, rs)[:, :fw]
                tight_nv12 = np.vstack((y_plane, uv_plane))
                img_in = cv2.cvtColor(tight_nv12, cv2.COLOR_YUV2BGR_NV12)

                with q_cond:
                    pending[eye] = (img_in, gx, gy, frame_id)   # keep only newest
                    q_cond.notify()

            if not got:
                time.sleep(0.001)   # poll gently when there is nothing new

    reader_thread = threading.Thread(target=shm_reader_loop, daemon=True)
    reader_thread.start()

    # Producer cursors for the phos (Python -> C++) channels.
    phos_write_index = [0, 0]

    import torch

    print("CUDA available:", torch.cuda.is_available())
    if torch.cuda.is_available():
        print("CUDA device:", torch.cuda.get_device_name(0))

    print("Resolution:", resolution)
    print("Simulator type:", type(simulators[0]))

    def process_one_eye(eye, img_in, gx, gy, frame_id):
        """Full per-eye pipeline: undistort -> gaze crop -> phosphenes -> bytes.
        Returns (eye, gray_bytes, crop_w, crop_h, frame_id). Pure compute + no I/O,
        so the two eyes can run concurrently and be published together."""
        with config_lock:
            cfg = dict(runtime_cfgs[eye])

        undistorted = undistort_bgr(img_in, cfg, eye)
        _, _, new_K = get_undistort_maps(cfg, eye)
        if new_K is not None:
            crop_fx, crop_fy = new_K[0, 0], new_K[1, 1]
            crop_cx, crop_cy = new_K[0, 2], new_K[1, 2]
        else:
            crop_fx = cfg["focal_x"] * cfg["frame_w"]
            crop_fy = cfg["focal_y"] * cfg["frame_h"]
            crop_cx = cfg["pp_x"] * cfg["frame_w"]
            crop_cy = cfg["pp_y"] * cfg["frame_h"]

        # Same gaze-centred crop for every mode; the C++ scotoma mask decides which
        # phosphenes are actually shown (central disc / peripheral ring / whole field).
        frame_in = crop_with_black(
            undistorted, gx, gy,
            cfg["crop_w"], cfg["crop_h"],
            crop_fx, crop_fy, crop_cx, crop_cy,
        )
        frame = cv2.resize(frame_in, resolution, cv2.INTER_LINEAR)
        stim_pattern = algorithm.process(frame, params, simulators[eye])
        phosphenes = simulators[eye](stim_pattern)
        phosphenes = np.round(phosphenes.cpu().numpy() * 255).astype('uint8')
        resized = cv2.resize(phosphenes, (cfg["crop_w"], cfg["crop_h"]), interpolation=cv2.INTER_LINEAR)
        gray = np.ascontiguousarray(resized[::-1]).tobytes()
        return eye, gray, cfg["crop_w"], cfg["crop_h"], frame_id

    # Both eyes are processed as a synchronised pair: wait until a fresh frame is
    # available for BOTH eyes, run them concurrently (cv2/torch release the GIL,
    # so the work overlaps), then publish both back to back. This keeps L/R in
    # lock-step and prevents either eye from starving.
    pool = ThreadPoolExecutor(max_workers=NUM_EYES)
    t_pair = time.time()
    while True:
        with q_cond:
            while not stop_event.is_set() and (pending[EYE_LEFT] is None or pending[EYE_RIGHT] is None):
                q_cond.wait(timeout=0.05)
            if stop_event.is_set():
                break
            pair = list(pending)
            pending[EYE_LEFT] = None
            pending[EYE_RIGHT] = None

        futures = [pool.submit(process_one_eye, e, *pair[e]) for e in range(NUM_EYES)]
        results = [f.result() for f in futures]   # both eyes done
        for eye, gray, cw, ch, frame_id in results:   # publish the pair together
            phos_write_index[eye] = shm_publish(
                phos_mm, phos_channel_off(eye), PHOS_SLOT_STRIDE, PHOS_SLOT_CAP,
                phos_write_index[eye], frame_id, eye, cw, ch, cw, 0.0, 0.0, gray)

        now = time.time()
        print(f"[PYTHON] pair processed+published: {(now - t_pair) * 1000:.1f} ms "
              f"({1.0 / max(now - t_pair, 1e-6):.0f} FPS/eye)")
        t_pair = now

    stop_event.set()
    with q_cond:
        q_cond.notify_all()
    pool.shutdown(wait=False)
    try:
        reader_thread.join(timeout=1.0)
    except Exception:
        pass

    print("Python is done <3")


if __name__ == '__main__':
    app = FilterApp()
    app.mainloop()
    sys.exit()