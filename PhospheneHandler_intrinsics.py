import sys
import time
import cv2
import numpy as np
import socket
import struct
import os
import threading
import tkinter as tk
from tkinter import ttk
import importlib
import inspect
import math

from collections import deque

from dynaphos.image_processing import sobel_processor, canny_processor
from dynaphos.simulator import GaussianSimulator
from dynaphos.utils import load_params, load_coordinates_from_yaml, Map
from dynaphos.cortex_models import get_visual_field_coordinates_from_cortex_full

from base_processing_algorithm import BaseProcessingAlgorithm

print("Python is running!")

#####################
# UDP Socket Values #
#####################
FRAME_START = 0xFF
SHUTDOWN_BYTE = 0xFE
CONFIG_OPCODE = 0xFD
CONFIG_ACK_OPCODE = 0xFC

# Packet layouts (all little-endian, struct-packed on the C++ side).
#
# PythonFrameChunkHeader (C++ -> Python frame packets):
#   opcode(1) eye(1) frameId(4) chunkIndex(4) chunkCount(4) gazeTanX(4) gazeTanY(4)  = 22 bytes
# CppFrameChunkHeader (Python -> C++ phosphene packets):
#   opcode(1) eye(1) frameId(4) chunkIndex(4) chunkCount(4)                          = 14 bytes
# PythonConfigPacket (C++ -> Python):
#   opcode(1) eye(1) + 7 int32 + 12 double                                           = 126 bytes
FRAME_HDR_FMT = "<BBIIIff"
FRAME_HDR = struct.calcsize(FRAME_HDR_FMT)    # 22
CPP_HDR_FMT = "<BBIII"
CONFIG_FMT = "<BB7i12d"
CONFIG_ACK_FMT = "<BB"
CHUNK_SIZE = 8000

EYE_LEFT = 0
EYE_RIGHT = 1
NUM_EYES = 2

VARJO_INTRINSICS_MODEL_OMNIDIR = 1
VARJO_INTRINSICS_MODEL_RATIONAL = 2

recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
recv_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
recv_sock.bind(("0.0.0.0", 5000))
recv_sock.settimeout(0.5)

# Per-eye in-flight chunk assembly: frame_chunks[eye][frame_id] -> state dict
frame_chunks = [{}, {}]
IN_QUEUE_MAX = 8

if len(sys.argv) == 2:
    python_dir = sys.argv[1].strip('"')
else:
    print("Incorrect program arguments! We need path to the directory.", flush=True)
    sys.exit(1)


def parse_config_packet(pkt: bytes):
    if len(pkt) < struct.calcsize(CONFIG_FMT):
        raise ValueError("Config packet too small")

    unpacked = struct.unpack_from(CONFIG_FMT, pkt, 0)
    if unpacked[0] != CONFIG_OPCODE:
        raise ValueError("Not a config packet")

    eye = unpacked[1]
    crop_w, crop_h, frame_w, frame_h, row_stride, intr_model, intr_valid = unpacked[2:9]
    focal_x, focal_y, pp_x, pp_y = unpacked[9:13]
    coeffs = list(unpacked[13:21])
    print(f"[DEBUG] Raw Varjo coeffs (eye={eye}): {coeffs}")

    return {
        "eye": eye,
        "crop_w": crop_w,
        "crop_h": crop_h,
        "frame_w": frame_w,
        "frame_h": frame_h,
        "row_stride": row_stride,
        "intr_model": intr_model,
        "intr_valid": bool(intr_valid),
        "focal_x": focal_x,
        "focal_y": focal_y,
        "pp_x": pp_x,
        "pp_y": pp_y,
        "coeffs": coeffs,
    }


def send_config_ack(sock, eye, repeats=3):
    pkt = struct.pack(CONFIG_ACK_FMT, CONFIG_ACK_OPCODE, eye)
    for _ in range(repeats):
        sock.sendto(pkt, ("127.0.0.1", 5001))
    print(f"[PYTHON] Sent config ACK for eye={eye}")


def wait_for_configs(sock, timeout=60.0):
    """Block until we've received a config packet from both eyes."""
    print("Waiting for config packets from C++ (both eyes)...")
    configs = [None, None]
    t_start = time.time()
    while (configs[0] is None or configs[1] is None) and (time.time() - t_start < timeout):
        try:
            pkt, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue

        if pkt and pkt[0] == CONFIG_OPCODE:
            cfg = parse_config_packet(pkt)
            eye = cfg["eye"]
            if eye not in (EYE_LEFT, EYE_RIGHT):
                print(f"[PYTHON] Ignoring config with bad eye index {eye}")
                continue
            configs[eye] = cfg
            print(
                f"Received config from C++ (eye={eye}): crop=({cfg['crop_w']}, {cfg['crop_h']}) "
                f"frame=({cfg['frame_w']}, {cfg['frame_h']}) stride={cfg['row_stride']} "
                f"intrinsics={cfg['intr_valid']} model={cfg['intr_model']} "
                f"fx={cfg['focal_x']:.2f} fy={cfg['focal_y']:.2f} cx={cfg['pp_x']:.2f} cy={cfg['pp_y']:.2f}"
            )

    if configs[0] is None or configs[1] is None:
        missing = [i for i, c in enumerate(configs) if c is None]
        raise TimeoutError(f"No config packet received for eye(s) {missing} within {timeout}s")

    return configs


runtime_cfgs = wait_for_configs(recv_sock)
for _e, _c in enumerate(runtime_cfgs):
    print(
        f"[eye={_e}] Using crop={_c['crop_w']}x{_c['crop_h']}, "
        f"frame={_c['frame_w']}x{_c['frame_h']}, stride={_c['row_stride']}"
    )

for eye in (EYE_LEFT, EYE_RIGHT):
    send_config_ack(recv_sock, eye)

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
    coordinates_cortex = load_coordinates_from_yaml(python_dir + '/grid_coords_dipole_valid.yaml', n_coordinates=1500)
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

    frame_queue = deque(maxlen=IN_QUEUE_MAX)
    q_lock = threading.Lock()
    q_cond = threading.Condition(q_lock)
    stop_event = threading.Event()

    def shm_reader_loop():
        while not stop_event.is_set():
            try:
                pkt, _ = recv_sock.recvfrom(65535)
            except socket.timeout:
                continue

            if len(pkt) == 1 and pkt[0] == SHUTDOWN_BYTE:
                print("Received shutdown from C++")
                stop_event.set()
                break

            if pkt and pkt[0] == CONFIG_OPCODE:
                try:
                    new_cfg = parse_config_packet(pkt)
                    eye = new_cfg["eye"]
                    if eye not in (EYE_LEFT, EYE_RIGHT):
                        print(f"[PYTHON] Ignoring config with bad eye index {eye}")
                        continue
                    with config_lock:
                        runtime_cfgs[eye].update(new_cfg)
                    print(
                        f"[PYTHON] Updated config (eye={eye}): crop=({new_cfg['crop_w']}, {new_cfg['crop_h']}) "
                        f"frame=({new_cfg['frame_w']}, {new_cfg['frame_h']}) stride={new_cfg['row_stride']} "
                        f"intrinsics={new_cfg['intr_valid']} model={new_cfg['intr_model']}"
                    )
                    send_config_ack(recv_sock, eye, repeats=1)
                except Exception as e:
                    print(f"[PYTHON] Failed to parse config: {e}")
                continue

            if len(pkt) < FRAME_HDR or pkt[0] != FRAME_START:
                continue

            # Header: <BBIIIff>  opcode, eye, frameId, chunkIndex, chunkCount, gazeTanX, gazeTanY
            _opcode, eye, frame_id, chunk_i, total, gx, gy = struct.unpack_from(FRAME_HDR_FMT, pkt, 0)
            if eye not in (EYE_LEFT, EYE_RIGHT):
                continue
            payload = pkt[FRAME_HDR:]

            eye_chunks = frame_chunks[eye]
            st = eye_chunks.get(frame_id)
            if st is None:
                st = {"total": total, "gx": gx, "gy": gy, "chunks": [None] * total}
                eye_chunks[frame_id] = st

            if 0 <= chunk_i < st["total"]:
                st["chunks"][chunk_i] = payload

            if all(c is not None for c in st["chunks"]):
                raw_nv12 = b"".join(st["chunks"])
                del eye_chunks[frame_id]

                with config_lock:
                    cfg = dict(runtime_cfgs[eye])
                expected_bytes = cfg["row_stride"] * cfg["frame_h"] * 3 // 2
                if len(raw_nv12) != expected_bytes:
                    print(f"[PYTHON] Dropping frame eye={eye} id={frame_id}: expected {expected_bytes} bytes, got {len(raw_nv12)}")
                    continue

                nv12 = np.frombuffer(raw_nv12, np.uint8)
                y_plane = nv12[:cfg["row_stride"] * cfg["frame_h"]].reshape(cfg["frame_h"], cfg["row_stride"])[:, :cfg["frame_w"]]
                uv_plane = nv12[cfg["row_stride"] * cfg["frame_h"]:].reshape(cfg["frame_h"] // 2, cfg["row_stride"])[:, :cfg["frame_w"]]
                tight_nv12 = np.vstack((y_plane, uv_plane))
                img_in = cv2.cvtColor(tight_nv12, cv2.COLOR_YUV2BGR_NV12)

                with q_cond:
                    frame_queue.append((eye, img_in, gx, gy, frame_id))
                    q_cond.notify()

    reader_thread = threading.Thread(target=shm_reader_loop, daemon=True)
    reader_thread.start()

    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    send_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 2 * 1024 * 1024)
    CPP_IP = "127.0.0.1"
    CPP_PORT = 5001

    def send_gray_in_chunks(gray_bytes: bytes, frame_id: int, eye: int):
        total = (len(gray_bytes) + CHUNK_SIZE - 1) // CHUNK_SIZE
        for i in range(total):
            off = i * CHUNK_SIZE
            chunk = gray_bytes[off:off + CHUNK_SIZE]
            pkt = struct.pack(CPP_HDR_FMT, FRAME_START, eye, frame_id, i, total) + chunk
            send_sock.sendto(pkt, (CPP_IP, CPP_PORT))

    import torch

    print("CUDA available:", torch.cuda.is_available())
    if torch.cuda.is_available():
        print("CUDA device:", torch.cuda.get_device_name(0))

    print("Resolution:", resolution)
    print("Simulator type:", type(simulators[0]))
    first = [True, True]
    start_frame_in_time = time.time()
    while True:
        with q_cond:
            if stop_event.is_set():
                break
            if not bool(frame_queue):
                q_cond.wait(timeout=0.005)
                continue
            eye, img_in, gx, gy, frame_id = frame_queue.popleft()
            print(f"[PYTHON] Time to capture frame from queue (eye={eye}): {(time.time() - start_frame_in_time) * 1000} ms")
            start_frame_in_time = time.time()

        with config_lock:
            cfg = dict(runtime_cfgs[eye])

        start_time = time.time()
        undistorted = undistort_bgr(img_in, cfg, eye)

        # Get the new_K that was used for undistortion
        _, _, new_K = get_undistort_maps(cfg, eye)

        if new_K is not None:
            crop_fx = new_K[0, 0]
            crop_fy = new_K[1, 1]
            crop_cx = new_K[0, 2]
            crop_cy = new_K[1, 2]
        else:
            crop_fx = cfg["focal_x"] * cfg["frame_w"]
            crop_fy = cfg["focal_y"] * cfg["frame_h"]
            crop_cx = cfg["pp_x"] * cfg["frame_w"]
            crop_cy = cfg["pp_y"] * cfg["frame_h"]

        frame_in = crop_with_black(
            undistorted,
            gx, gy,
            cfg["crop_w"], cfg["crop_h"],
            crop_fx, crop_fy,
            crop_cx, crop_cy,
        )
        end_time = time.time()

        start_time = time.time()
        frame = cv2.resize(frame_in, resolution, cv2.INTER_LINEAR)
        end_time = time.time()

        start_time = time.time()
        stim_pattern = algorithm.process(frame, params, simulators[eye])
        end_time = time.time()
        print(f"[PYTHON] Algorithm processing time (eye={eye}): {(end_time - start_time) * 1000} ms")

        start_time = time.time()
        phosphenes = simulators[eye](stim_pattern)
        phosphenes = phosphenes.cpu().numpy() * 255
        phosphenes = np.round(phosphenes).astype('uint8')
        end_time = time.time()
        print(f"[PYTHON] Phosphene generation time (eye={eye}): {(end_time - start_time) * 1000} ms")

        start_time = time.time()
        resizedPhosphenes = cv2.resize(phosphenes, (cfg["crop_w"], cfg["crop_h"]), interpolation=cv2.INTER_LINEAR)
        end_time = time.time()
        print(f"[PYTHON] Time to resize phosphenes (eye={eye}): {(end_time - start_time) * 1000} ms")

        if first[eye]:
            print(f"[eye={eye}] frame shape:", frame.shape, frame.dtype)
            print(f"[eye={eye}] stim type:", type(stim_pattern))
            print(f"[eye={eye}] stim shape:", getattr(stim_pattern, "shape", None))
            print(f"[eye={eye}] stim dtype:", getattr(stim_pattern, "dtype", None))
            print(f"[eye={eye}] stim device:", getattr(stim_pattern, "device", None))
            first[eye] = False

        start_time = time.time()
        gray = np.ascontiguousarray(resizedPhosphenes[::-1]).tobytes()
        send_gray_in_chunks(gray, frame_id, eye)
        end_time = time.time()
        print(f"[PYTHON] Send phosphenes to C++ (eye={eye}): {(end_time - start_time) * 1000} ms")

    stop_event.set()
    with q_cond:
        q_cond.notify_all()
    try:
        reader_thread.join(timeout=1.0)
    except Exception:
        pass

    print("Python is done <3")


if __name__ == '__main__':
    app = FilterApp()
    app.mainloop()
    sys.exit()