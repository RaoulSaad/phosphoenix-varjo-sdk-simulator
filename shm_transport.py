"""shm_transport.py — Python side of the C++ <-> Python shared-memory bridge.

This is the integration seam: any phosphene pipeline can import this module to
receive Varjo camera frames + gaze from the C++ app and send phosphene images
back, without knowing anything about Varjo or the C++ internals.

The layout mirrors `transport.cpp` exactly. Two named, pagefile-backed sections:
  cam  (C++ -> Python) : raw NV12 camera frames + per-frame gaze/metadata
  phos (Python -> C++) : 8-bit grayscale phosphene images

Each section is [ Header | eye0 channel | eye1 channel ]; a channel is a
lock-free double buffer [ Ctrl | slot0 | slot1 ], slot = [ SlotMeta | bytes ].
Publish/consume use a seqlock (see shm_publish / shm_consume).

Typical use:
    bridge = ShmBridge()
    bridge.open()                      # blocks until C++ marks READY
    cfg = bridge.configs[0]            # per-eye camera config from C++
    while not bridge.is_shutdown():
        item = bridge.poll_camera(eye) # None if nothing new
        if item:
            bgr = bridge.to_bgr(item)                       # NV12 -> BGR
            gy, gx = bridge.gaze_point_fraction(item, eye)  # for gaze cropping
            ...
            bridge.publish_phosphene(eye, item["frame_id"], gray_u8_2d)
"""

import mmap
import struct
import time

import numpy as np

try:
    import cv2
except ImportError:      # cv2 only needed for to_bgr()/undistort()
    cv2 = None


# ---------------------------------------------------------------------------
# Layout — must match transport.cpp
# ---------------------------------------------------------------------------

SHM_CAM_NAME  = "Local\\VarjoPhospheneCam"
SHM_PHOS_NAME = "Local\\VarjoPhospheneOut"

SHM_MAGIC   = 0x50484D31   # 'PHM1'
SHM_VERSION = 1
SHM_FLAG_READY    = 1 << 0
SHM_FLAG_SHUTDOWN = 1 << 1

NUM_EYES      = 2
EYE_LEFT      = 0
EYE_RIGHT     = 1
SHM_NUM_SLOTS = 2          # double buffer

SHM_HEADER_SIZE   = 512
SHM_CTRL_SIZE     = 64
SHM_SLOTMETA_SIZE = 64

CAM_SLOT_CAP  = 8 * 1024 * 1024
PHOS_SLOT_CAP = 4 * 1024 * 1024

# struct formats (little-endian, packed) -- match the C++ #pragma pack(1) structs
SHM_HEADER_FMT   = "<IIIIiI"    # magic, version, flags, configSeq, blindnessMode, reserved0
SHM_EYECFG_FMT   = "<8i12d"     # 8 int32 + 12 double
SHM_SLOTMETA_FMT = "<6I2f8I"    # frameId,eye,w,h,rowStride,byteSize, gazeX,gazeY, reserved[8]

CAM_SLOT_STRIDE   = SHM_SLOTMETA_SIZE + CAM_SLOT_CAP
CAM_CHANNEL_SIZE  = SHM_CTRL_SIZE + SHM_NUM_SLOTS * CAM_SLOT_STRIDE
CAM_MAP_SIZE      = SHM_HEADER_SIZE + NUM_EYES * CAM_CHANNEL_SIZE
PHOS_SLOT_STRIDE  = SHM_SLOTMETA_SIZE + PHOS_SLOT_CAP
PHOS_CHANNEL_SIZE = SHM_CTRL_SIZE + SHM_NUM_SLOTS * PHOS_SLOT_STRIDE
PHOS_MAP_SIZE     = SHM_HEADER_SIZE + NUM_EYES * PHOS_CHANNEL_SIZE

BLINDNESS_MACULAR  = 0
BLINDNESS_GLAUCOMA = 1
BLINDNESS_FULL     = 2
BLINDNESS_NAMES = {BLINDNESS_MACULAR: "macular",
                   BLINDNESS_GLAUCOMA: "glaucoma",
                   BLINDNESS_FULL: "full"}

VARJO_INTRINSICS_MODEL_OMNIDIR  = 1
VARJO_INTRINSICS_MODEL_RATIONAL = 2


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
    struct.pack_into("<I", mm, channel_off + 0, w)                        # latestIndex
    seq = struct.unpack_from("<I", mm, channel_off + 4)[0]
    struct.pack_into("<I", mm, channel_off + 4, (seq + 1) & 0xFFFFFFFF)   # publishSeq (publish)
    return (w + 1) % SHM_NUM_SLOTS


# ---------------------------------------------------------------------------
# Camera geometry helpers (interpret the C++ per-eye config)
# ---------------------------------------------------------------------------

def intrinsics_pixels(cfg):
    """Varjo may report intrinsics normalised to [0,1]; convert to pixels.
    Returns (fx, fy, cx, cy) in pixels, or None when intrinsics are unavailable."""
    if not cfg["intr_valid"]:
        return None
    w, h = cfg["frame_w"], cfg["frame_h"]
    fx, fy = cfg["focal_x"], cfg["focal_y"]
    cx, cy = cfg["pp_x"], cfg["pp_y"]
    if fx < 1.0: fx *= w
    if fy < 1.0: fy *= h
    if cx < 1.0: cx *= w
    if cy < 1.0: cy *= h
    return fx, fy, cx, cy


def build_undistort_maps(cfg):
    """cv2 remap tables for one eye, or (None, None, None) if not applicable."""
    if not cfg["intr_valid"] or cv2 is None:
        return None, None, None

    width, height = cfg["frame_w"], cfg["frame_h"]
    coeffs, model = cfg["coeffs"], cfg["intr_model"]

    if model == VARJO_INTRINSICS_MODEL_RATIONAL:
        fx, fy, cx, cy = intrinsics_pixels(cfg)
        K_px = np.array([[fx, 0.0, cx], [0.0, fy, cy], [0.0, 0.0, 1.0]], dtype=np.float64)
        k1, k2, k3, k4, p1, p2, k5, k6 = coeffs[:8]
        D = np.array([k1, k2, p1, p2, k3, k4, k5, k6], dtype=np.float64)
        map1, map2 = cv2.initUndistortRectifyMap(
            K_px, D, np.eye(3, dtype=np.float64), K_px, (width, height), cv2.CV_32FC1)
        return map1, map2, K_px

    if model == VARJO_INTRINSICS_MODEL_OMNIDIR and hasattr(cv2, "omnidir"):
        # cv2.omnidir expects the camera matrix in PIXELS. Varjo reports
        # normalized intrinsics, so convert like the rational branch does
        # (intrinsics_pixels is a no-op if values are already pixel-scale).
        # With the raw normalized values (fx ~ 0.7 "pixels") the remap
        # collapses the whole image into a couple of source pixels and the
        # undistorted frame comes out black.
        fx, fy, cx, cy = intrinsics_pixels(cfg)
        K = np.array([[fx, 0.0, cx], [0.0, fy, cy], [0.0, 0.0, 1.0]], dtype=np.float64)
        K_omni = np.array([[fx, coeffs[2], cx], [0.0, fy, cy], [0.0, 0.0, 1.0]], dtype=np.float64)
        xi = np.array([coeffs[3]], dtype=np.float64)
        D = np.array([coeffs[0], coeffs[1], coeffs[4], coeffs[5]], dtype=np.float64).reshape(1, 4)
        flags = getattr(cv2.omnidir, "RECTIFY_PERSPECTIVE", 1)
        map1, map2 = cv2.omnidir.initUndistortRectifyMap(
            K_omni, D, xi, np.eye(3, dtype=np.float64), K, (width, height), cv2.CV_32FC1, flags)
        return map1, map2, K

    return None, None, None


# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------

class ShmBridge:
    """Owns both mappings plus the per-eye publish/consume cursors."""

    def __init__(self):
        self.cam_mm = None
        self.phos_mm = None
        self.configs = [None, None]
        self.blindness_mode = BLINDNESS_MACULAR
        self.config_seq = -1
        self._cam_last_seq = [0, 0]
        self._phos_write_index = [0, 0]
        self._undistort_cache = [None, None]   # (key, map1, map2, K)

    # -- lifecycle ----------------------------------------------------------
    def open(self, timeout=60.0, require_intrinsics=True, verbose=True):
        """Open both mappings and block until C++ marks them READY.

        Either process may create the section first; the fixed size means both
        agree, and the READY flag (set by C++ last) gates use.

        require_intrinsics: also wait for the config that carries real camera
        intrinsics, so crop sizes are final before a pipeline sizes its buffers.
        """
        if verbose:
            print("[SHM] opening shared memory, waiting for C++ (READY)...")
        self.cam_mm = mmap.mmap(-1, CAM_MAP_SIZE, tagname=SHM_CAM_NAME, access=mmap.ACCESS_WRITE)
        self.phos_mm = mmap.mmap(-1, PHOS_MAP_SIZE, tagname=SHM_PHOS_NAME, access=mmap.ACCESS_WRITE)

        t0 = time.time()
        ready = False
        while time.time() - t0 < timeout:
            magic, _v, flags, _seq, _mode = read_header(self.cam_mm)
            if magic == SHM_MAGIC and (flags & SHM_FLAG_READY):
                ready = True
                break
            time.sleep(0.01)
        if not ready:
            raise TimeoutError("C++ shared memory not READY within timeout")

        self.refresh_config()
        if require_intrinsics:
            # The first config carries fallback sizes; the crop size is finalised
            # once a camera frame with intrinsics arrives. Wait for that so
            # buffer sizes derived from crop_w/crop_h are stable.
            while time.time() - t0 < timeout:
                self.refresh_config()
                if all(c and c["intr_valid"] for c in self.configs):
                    break
                time.sleep(0.02)
            else:
                if verbose:
                    print("[SHM] WARNING: no intrinsics within timeout; using fallback crop sizes")

        if verbose:
            for e, c in enumerate(self.configs):
                print(f"[SHM] eye={e} frame={c['frame_w']}x{c['frame_h']} "
                      f"crop={c['crop_w']}x{c['crop_h']} stride={c['row_stride']} "
                      f"intrinsics={c['intr_valid']}")
            print(f"[SHM] blindness mode from C++: {self.blindness_mode} "
                  f"({BLINDNESS_NAMES.get(self.blindness_mode, '?')})")
        return self

    def close(self):
        for mm in (self.cam_mm, self.phos_mm):
            try:
                if mm is not None:
                    mm.close()
            except Exception:
                pass
        self.cam_mm = self.phos_mm = None

    def is_shutdown(self):
        if self.cam_mm is None:
            return True
        _m, _v, flags, _s, _mode = read_header(self.cam_mm)
        return bool(flags & SHM_FLAG_SHUTDOWN)

    # -- config -------------------------------------------------------------
    def refresh_config(self):
        """Re-read the config header. Returns True if it changed since last call."""
        _m, _v, _f, seq, mode = read_header(self.cam_mm)
        if seq == self.config_seq:
            return False
        self.configs = [read_eye_config(self.cam_mm, e) for e in range(NUM_EYES)]
        self.blindness_mode = mode
        self.config_seq = seq
        return True

    # -- camera (C++ -> Python) --------------------------------------------
    def poll_camera(self, eye):
        """Latest unread camera frame for `eye`, or None. Dict with keys:
        frame_id, nv12, width, height, row_stride, gaze_tan_x, gaze_tan_y."""
        self._cam_last_seq[eye], meta, payload = shm_consume(
            self.cam_mm, cam_channel_off(eye), CAM_SLOT_STRIDE, self._cam_last_seq[eye])
        if meta is None:
            return None
        frame_id, _eye, w, h, stride, _n, gx, gy = meta[:8]
        if w <= 0 or h <= 0 or len(payload) != stride * h * 3 // 2:
            return None      # torn / mis-sized frame
        return {"frame_id": frame_id, "nv12": payload, "width": w, "height": h,
                "row_stride": stride, "gaze_tan_x": gx, "gaze_tan_y": gy}

    @staticmethod
    def to_bgr(item):
        """NV12 payload -> contiguous BGR uint8 (H, W, 3)."""
        if cv2 is None:
            raise RuntimeError("cv2 is required for to_bgr()")
        w, h, stride = item["width"], item["height"], item["row_stride"]
        nv12 = np.frombuffer(item["nv12"], np.uint8)
        y_plane = nv12[:stride * h].reshape(h, stride)[:, :w]
        uv_plane = nv12[stride * h:].reshape(h // 2, stride)[:, :w]
        return cv2.cvtColor(np.vstack((y_plane, uv_plane)), cv2.COLOR_YUV2BGR_NV12)

    def undistort(self, img, eye):
        """Undistort with cached remap tables (no-op without valid intrinsics)."""
        cfg = self.configs[eye]
        if not cfg["intr_valid"] or cv2 is None:
            return img
        key = (cfg["frame_w"], cfg["frame_h"], cfg["intr_model"],
               round(cfg["focal_x"], 6), round(cfg["focal_y"], 6),
               round(cfg["pp_x"], 6), round(cfg["pp_y"], 6),
               tuple(round(float(c), 6) for c in cfg["coeffs"]))
        cached = self._undistort_cache[eye]
        if cached is None or cached[0] != key:
            m1, m2, K = build_undistort_maps(cfg)
            self._undistort_cache[eye] = (key, m1, m2, K)
            cached = self._undistort_cache[eye]
        _k, map1, map2, _K = cached
        if map1 is None:
            return img
        return cv2.remap(img, map1, map2, interpolation=cv2.INTER_LINEAR,
                         borderMode=cv2.BORDER_CONSTANT)

    def gaze_pixel(self, item, eye):
        """Gaze position in camera pixels for this frame."""
        cfg = self.configs[eye]
        intr = intrinsics_pixels(cfg)
        w, h = item["width"], item["height"]
        if intr is None:
            return w * 0.5, h * 0.5
        fx, fy, cx, cy = intr
        # tangent y is up, image y is down
        return cx + fx * item["gaze_tan_x"], cy - fy * item["gaze_tan_y"]

    def gaze_point_fraction(self, item, eye):
        """Gaze as (dy, dx) offsets from frame centre, in fractions of frame
        height/width — the convention the TensorRT crop kernel expects
        (range approximately [-0.5, 0.5])."""
        px, py = self.gaze_pixel(item, eye)
        w, h = item["width"], item["height"]
        return (py - h * 0.5) / h, (px - w * 0.5) / w

    def device_field_tan(self, eye):
        """Half-field the C++ expects the phosphene image to span, derived from
        the crop size it asked for: crop_w = 2 * fx * tan(halfFov)."""
        cfg = self.configs[eye]
        intr = intrinsics_pixels(cfg)
        if intr is None or cfg["crop_w"] <= 0:
            return None
        fx = intr[0]
        return cfg["crop_w"] / (2.0 * fx)

    # -- device-field negotiation (Python -> C++) ---------------------------
    # C++ compiles a default kDeviceFieldTan, but the authoritative span is
    # whatever phosphene map Python actually loaded. announce_device_field()
    # bit-casts that tangent into the phos header's reserved word; C++ polls it
    # every frame (pollAnnouncedDeviceField) and adapts its camera crop and
    # shader. wait_for_crop_match() then confirms the round trip by watching
    # the cam header until the crop sizes match the announced field.

    _FIELD_TAN_OFFSET = 20   # ShmHeader.reserved0 (after magic/version/flags/configSeq/mode)

    def announce_device_field(self, field_tan):
        """Advertise the half-field tangent of the loaded phosphene map."""
        struct.pack_into("<f", self.phos_mm, self._FIELD_TAN_OFFSET, float(field_tan))

    def expected_crop(self, eye, field_tan):
        """Crop size C++ should ask for once it adopts field_tan (its
        computePythonCropSize: round(2*fx*tan), min 32, bumped to even)."""
        cfg = self.configs[eye]
        intr = intrinsics_pixels(cfg)
        if intr is None:
            return None
        w = max(32, int(round(2.0 * intr[0] * field_tan)))
        h = max(32, int(round(2.0 * intr[1] * field_tan)))
        return (w + (w & 1), h + (h & 1))

    def wait_for_crop_match(self, field_tan, timeout=5.0, verbose=True):
        """Block until C++'s crop sizes reflect the announced field (or timeout).
        Call before sizing any buffers from configs, so they use the final crop."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.refresh_config()
            ok = True
            for eye in range(NUM_EYES):
                expected = self.expected_crop(eye, field_tan)
                if expected is None:
                    ok = False
                    break
                cfg = self.configs[eye]
                if (abs(cfg["crop_w"] - expected[0]) > 2 or
                        abs(cfg["crop_h"] - expected[1]) > 2):
                    ok = False
                    break
            if ok:
                if verbose:
                    print(f"[SHM] C++ adopted device field tan={field_tan:.4f}: "
                          f"crop {self.configs[0]['crop_w']}x{self.configs[0]['crop_h']}")
                return True
            time.sleep(0.02)
        print(f"[SHM] WARNING: C++ did not adopt device field tan={field_tan:.4f} "
              f"within {timeout:.0f}s (old C++ build?); continuing with crop "
              f"{self.configs[0]['crop_w']}x{self.configs[0]['crop_h']} — geometry may be off")
        return False

    # -- phosphenes (Python -> C++) ----------------------------------------
    def publish_phosphene(self, eye, frame_id, gray, flip_vertical=True):
        """Publish a 2-D uint8 phosphene image for `eye`.

        The image is interpreted by the C++ shader as spanning the full device
        field (+/- kDeviceFieldTan) centred on gaze, so any resolution works —
        the meta carries its dimensions.
        """
        gray = np.asarray(gray)
        if gray.ndim == 3 and gray.shape[2] == 1:
            gray = gray[:, :, 0]
        if gray.ndim != 2:
            raise ValueError(f"phosphene image must be 2-D grayscale, got shape {gray.shape}")
        if gray.dtype != np.uint8:
            gray = np.clip(gray, 0, 255).astype(np.uint8)
        if flip_vertical:
            gray = gray[::-1]     # GL samples the texture bottom-up
        gray = np.ascontiguousarray(gray)
        h, w = gray.shape[:2]
        self._phos_write_index[eye] = shm_publish(
            self.phos_mm, phos_channel_off(eye), PHOS_SLOT_STRIDE, PHOS_SLOT_CAP,
            self._phos_write_index[eye], int(frame_id), eye, w, h, w,
            0.0, 0.0, gray.tobytes())
