"""ctypes wrapper over phx_shm (phx_shm/phx_shm.h) plus the ShmBridge the
pipeline talks to. All memory ordering lives in the C library; this module
never touches the control words itself.
"""
import ctypes as C
import os
import struct
import sys
import time

import numpy as np

import phx_geometry as geom

# ---------------------------------------------------------------------------
# Library loading
# ---------------------------------------------------------------------------
PHX_OK, PHX_NOTHING_NEW, PHX_TORN, PHX_TIMEOUT, PHX_SHUTDOWN, PHX_NOT_READY = 0, 1, 2, 3, 4, 5
PHX_BAD_VERSION, PHX_BAD_ARG, PHX_OS_ERROR, PHX_TOO_LARGE = -1, -2, -3, -4
PHX_PRODUCER, PHX_CONSUMER = 1, 2

_HERE = os.path.dirname(os.path.abspath(__file__))
_LIB_NAME = "phx_shm.dll" if sys.platform == "win32" else "libphx_shm.so"

class PhxError(RuntimeError):
    pass


class NotReady(PhxError):
    pass


class TornFrame(PhxError):
    pass

def _find_library(path=None):
    cands = [path, os.environ.get("PHX_SHM_LIB"),
             os.path.join(_HERE, _LIB_NAME),
             os.path.join(_HERE, "build-phx", "Release", _LIB_NAME),
             os.path.join(_HERE, "build", "Release", _LIB_NAME),
             os.path.join(_HERE, "build-phx", _LIB_NAME),
             os.path.join(_HERE, "build", "phx_shm", "Release", _LIB_NAME)]
    for c in cands:
        if c and os.path.exists(c):
            return c
    raise PhxError(f"{_LIB_NAME} not found; build phx_shm or set PHX_SHM_LIB")

class phx_layout(C.Structure):
    _fields_ = [("channels", C.c_uint32), ("slots", C.c_uint32), ("payload_bytes", C.c_uint32)]

class phx_meta(C.Structure):
    _fields_ = [("frame_number", C.c_uint64), ("timestamp_ns", C.c_uint64),
                ("width", C.c_uint32), ("height", C.c_uint32),
                ("row_stride", C.c_uint32), ("byte_size", C.c_uint32),
                ("gaze_tan_x", C.c_float), ("gaze_tan_y", C.c_float),
                ("eye", C.c_uint32), ("reserved", C.c_uint32)]

class phx_view(C.Structure):
    _fields_ = [("payload", C.POINTER(C.c_uint8)), ("meta", phx_meta),
                ("seq", C.c_uint64), ("slot", C.c_uint32), ("reserved", C.c_uint32)]


_META_KEYS = ("frame_number", "timestamp_ns", "width", "height", "row_stride",
              "byte_size", "gaze_tan_x", "gaze_tan_y", "eye")

_lib = None

def load_library(path=None):
    global _lib
    if _lib is not None:
        return _lib
    lib = C.CDLL(_find_library(path))
    H = C.c_void_p
    def sig(name, res, *args):
        f = getattr(lib, name); f.restype = res; f.argtypes = list(args); return f
    sig("phx_open", C.c_int, C.c_char_p, C.c_int, C.POINTER(phx_layout), C.POINTER(H))
    sig("phx_close", None, H)
    sig("phx_unlink", C.c_int, C.c_char_p)
    sig("phx_get_layout", C.c_int, H, C.POINTER(phx_layout))
    sig("phx_wait_ready", C.c_int, H, C.c_uint32)
    sig("phx_set_shutdown", None, H)
    sig("phx_is_shutdown", C.c_int, H)
    sig("phx_publish_begin", C.c_int, H, C.c_uint32, C.POINTER(C.POINTER(C.c_uint8)), C.POINTER(C.c_uint32))
    sig("phx_publish_commit", C.c_int, H, C.c_uint32, C.POINTER(phx_meta))
    sig("phx_consume_acquire", C.c_int, H, C.c_uint32, C.c_uint64, C.POINTER(phx_view))
    sig("phx_consume_release", C.c_int, H, C.c_uint32, C.POINTER(phx_view))
    sig("phx_wait", C.c_int, H, C.c_uint32, C.c_uint64, C.c_uint32)
    sig("phx_heartbeat", None, H)
    sig("phx_peer_alive", C.c_int, H, C.c_uint64)
    sig("phx_config_write", C.c_int, H, C.c_void_p, C.c_uint32)
    sig("phx_config_read", C.c_int, H, C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_uint64))
    sig("phx_field_announce", C.c_int, H, C.c_float)
    sig("phx_field_poll", C.c_int, H, C.POINTER(C.c_float), C.POINTER(C.c_uint64))
    sig("phx_now_ns", C.c_uint64)
    _lib = lib
    return lib

def now_ns():
    return load_library().phx_now_ns()


def unlink(name):
    """Remove a section name (Linux /dev/shm file; no-op on Windows). For tests."""
    return load_library().phx_unlink(name.encode()) == PHX_OK

# ---------------------------------------------------------------------------
# Section: one direction
# ---------------------------------------------------------------------------
class Frame:
    """A view into a slot. Use payload, then call release(); True means the
    bytes were stable the whole time (keep any copy), False means discard."""
    __slots__ = ("payload", "meta", "seq", "slot", "_section", "_ch", "_view")

    def __init__(self, section, ch, view):
        self._section, self._ch, self._view = section, ch, view
        n = view.meta.byte_size
        self.payload = np.ctypeslib.as_array(view.payload, shape=(n,))   # zero copy
        self.meta = {k: getattr(view.meta, k) for k in _META_KEYS}
        self.seq, self.slot = view.seq, view.slot

    def release(self):
        return self._section._lib.phx_consume_release(self._section._h, self._ch, C.byref(self._view)) == PHX_OK


class Section:
    def __init__(self, handle, lib):
        self._h, self._lib = handle, lib
        lay = phx_layout()
        lib.phx_get_layout(handle, C.byref(lay))
        self.layout = (lay.channels, lay.slots, lay.payload_bytes)

    @classmethod
    def create(cls, name, channels, slots, payload_bytes):
        lib = load_library()
        lay = phx_layout(channels, slots, payload_bytes)
        h = C.c_void_p()
        r = lib.phx_open(name.encode(), PHX_PRODUCER, C.byref(lay), C.byref(h))
        if r != PHX_OK:
            raise PhxError(f"phx_open({name}, producer) failed: {r}")
        return cls(h, lib)

    @classmethod
    def open(cls, name, timeout=None):
        """Open an existing section; retry until it exists and is READY."""
        lib = load_library()
        t0 = time.time()
        while True:
            h = C.c_void_p()
            r = lib.phx_open(name.encode(), PHX_CONSUMER, None, C.byref(h))
            if r == PHX_OK:
                return cls(h, lib)
            if r == PHX_BAD_VERSION:
                raise PhxError(f"section {name} has a different layout version; rebuild both sides")
            if r != PHX_NOT_READY:
                raise PhxError(f"phx_open({name}, consumer) failed: {r}")
            if timeout is not None and time.time() - t0 >= timeout:
                raise NotReady(f"section {name} not created within {timeout}s")
            time.sleep(0.01)

    def close(self):
        if self._h:
            self._lib.phx_close(self._h)
            self._h = None

    # -- producer
    def publish_begin(self, ch):
        p = C.POINTER(C.c_uint8)(); cap = C.c_uint32()
        r = self._lib.phx_publish_begin(self._h, ch, C.byref(p), C.byref(cap))
        if r != PHX_OK:
            raise PhxError(f"publish_begin failed: {r}")
        return np.ctypeslib.as_array(p, shape=(cap.value,)), cap.value

    def publish_commit(self, ch, meta):
        m = phx_meta()
        for k in _META_KEYS:
            if k in meta:
                setattr(m, k, meta[k])
        r = self._lib.phx_publish_commit(self._h, ch, C.byref(m))
        if r != PHX_OK:
            raise PhxError(f"publish_commit failed: {r}")

    # -- consumer
    def consume_acquire(self, ch, last_seq):
        v = phx_view()
        r = self._lib.phx_consume_acquire(self._h, ch, last_seq, C.byref(v))
        if r == PHX_NOTHING_NEW:
            return None
        if r == PHX_TORN:
            raise TornFrame(f"channel {ch}: could not read a stable slot")
        if r != PHX_OK:
            raise PhxError(f"consume_acquire failed: {r}")
        return Frame(self, ch, v)

    def wait(self, ch, last_seq, timeout_ms):
        return self._lib.phx_wait(self._h, ch, last_seq, timeout_ms)

    # -- header
    def heartbeat(self):
        self._lib.phx_heartbeat(self._h)

    def peer_alive(self, max_age_ns):
        return self._lib.phx_peer_alive(self._h, max_age_ns) == 1

    def config_write(self, blob):
        buf = (C.c_uint8 * len(blob)).from_buffer_copy(blob)
        r = self._lib.phx_config_write(self._h, buf, len(blob))
        if r != PHX_OK:
            raise PhxError(f"config_write failed: {r}")

    def config_read(self, max_bytes=1024):
        buf = (C.c_uint8 * max_bytes)(); n = C.c_uint32(); seq = C.c_uint64()
        r = self._lib.phx_config_read(self._h, buf, max_bytes, C.byref(n), C.byref(seq))
        if r == PHX_NOTHING_NEW:
            return None
        if r != PHX_OK:
            raise PhxError(f"config_read failed: {r}")
        return bytes(buf[:n.value]), seq.value

    def field_announce(self, tan):
        r = self._lib.phx_field_announce(self._h, float(tan))
        if r != PHX_OK:
            raise PhxError(f"field_announce failed: {r}")

    def field_poll(self):
        tan = C.c_float(); seq = C.c_uint64()
        r = self._lib.phx_field_poll(self._h, C.byref(tan), C.byref(seq))
        if r == PHX_NOTHING_NEW:
            return None
        if r != PHX_OK:
            raise PhxError(f"field_poll failed: {r}")
        return tan.value, seq.value

    def set_shutdown(self):
        self._lib.phx_set_shutdown(self._h)

    def is_shutdown(self):
        return self._h is None or self._lib.phx_is_shutdown(self._h) == 1


# ---------------------------------------------------------------------------
# Cam config blob: must match transport.cpp CamConfigBlob
# ---------------------------------------------------------------------------
NUM_EYES = 2
CAM_NAME, PHOS_NAME = "phx_cam", "phx_phos"
BLINDNESS_MACULAR, BLINDNESS_GLAUCOMA, BLINDNESS_FULL = 0, 1, 2
BLINDNESS_NAMES = {BLINDNESS_MACULAR: "macular", BLINDNESS_GLAUCOMA: "glaucoma", BLINDNESS_FULL: "full"}
_EYE_FMT = "<8i12d"
# int blindness_mode, float yolo_conf (-1 = C++ has not set one), then per-eye blocks.
CAM_CONFIG_FMT = "<if" + _EYE_FMT[1:] * NUM_EYES
CAM_CONFIG_SIZE = struct.calcsize(CAM_CONFIG_FMT)


def pack_cam_config(blindness_mode, eyes, yolo_conf=-1.0):
    vals = [int(blindness_mode), float(yolo_conf)]
    for e in eyes:
        vals += [e["crop_w"], e["crop_h"], e["frame_w"], e["frame_h"], e["row_stride"],
                 e["intr_model"], 1 if e["intr_valid"] else 0, 0,
                 e["focal_x"], e["focal_y"], e["pp_x"], e["pp_y"]] + list(e["coeffs"])[:8]
    return struct.pack(CAM_CONFIG_FMT, *vals)


def unpack_cam_config(blob):
    v = struct.unpack(CAM_CONFIG_FMT, blob[:CAM_CONFIG_SIZE])
    mode, yolo_conf = v[0], v[1]
    eyes = []
    for e in range(NUM_EYES):
        b = 2 + e * 20
        eyes.append({"crop_w": v[b], "crop_h": v[b + 1], "frame_w": v[b + 2], "frame_h": v[b + 3],
                     "row_stride": v[b + 4], "intr_model": v[b + 5], "intr_valid": bool(v[b + 6]),
                     "focal_x": v[b + 8], "focal_y": v[b + 9], "pp_x": v[b + 10], "pp_y": v[b + 11],
                     "coeffs": list(v[b + 12:b + 20])})
    return mode, yolo_conf, eyes


# ---------------------------------------------------------------------------
# Launcher live panel: control block (launcher -> C++) and status block
# (C++ -> launcher), one packed struct per section config block.
# Must match transport.h CtlBlock / StatBlock.
# ---------------------------------------------------------------------------
CTL_NAME, STAT_NAME = "phx_ctl", "phx_stat"
CTL_VERSION, STAT_VERSION = 1, 1
# version, mode, spot_radius_tan, mask_opacity, yolo_conf, keyboard_enabled, habituation, recording
CTL_FMT = "<iifffiii"
STAT_FIELDS = ("version", "mode", "spot_radius_tan", "mask_opacity", "yolo_conf", "device_field_tan",
               "python_alive", "keyboard_enabled", "render_fps", "capture_ms", "shm_pub_ms",
               "shm_con_ms", "render_ms", "e2e_ms")
STAT_FMT = "<iiffffiiffffff"
CTL_SIZE, STAT_SIZE = struct.calcsize(CTL_FMT), struct.calcsize(STAT_FMT)
assert len(STAT_FIELDS) == 14 and STAT_SIZE == 56 and CTL_SIZE == 32


def pack_ctl(mode, spot_radius_tan, mask_opacity, yolo_conf, keyboard_enabled):
    return struct.pack(CTL_FMT, CTL_VERSION, int(mode), float(spot_radius_tan), float(mask_opacity),
                       float(yolo_conf), 1 if keyboard_enabled else 0, 0, 0)


def unpack_ctl(blob):
    v = struct.unpack(CTL_FMT, blob[:CTL_SIZE])
    return {"version": v[0], "mode": v[1], "spot_radius_tan": v[2], "mask_opacity": v[3],
            "yolo_conf": v[4], "keyboard_enabled": bool(v[5]), "habituation": v[6], "recording": v[7]}


def pack_stat(**fields):
    """Test helper / fake C++ side. Missing fields are 0; version defaults."""
    fields.setdefault("version", STAT_VERSION)
    return struct.pack(STAT_FMT, *[fields.get(k, 0) for k in STAT_FIELDS])


def unpack_stat(blob):
    s = dict(zip(STAT_FIELDS, struct.unpack(STAT_FMT, blob[:STAT_SIZE])))
    s["python_alive"] = bool(s["python_alive"])
    s["keyboard_enabled"] = bool(s["keyboard_enabled"])
    return s


class ControlLink:
    """The launcher's end of the live panel: producer of phx_ctl, consumer of
    phx_stat. C++ opens phx_ctl lazily and creates phx_stat at startup."""

    def __init__(self):
        self.ctl = None
        self.stat = None
        self._last_blob = None
        self._stat_seq = 0
        self._version_warned = False   # F-7: print the status-version mismatch once

    def create(self):
        try:
            self.ctl = Section.create(CTL_NAME, channels=1, slots=2, payload_bytes=64)
        except PhxError:
            unlink(CTL_NAME)                      # stale name from a crashed launcher (POSIX)
            self.ctl = Section.create(CTL_NAME, channels=1, slots=2, payload_bytes=64)
        self.ctl.heartbeat()                      # so C++ sees a live peer on its first poll
        return self

    def send(self, mode, spot_radius_tan, mask_opacity, yolo_conf, keyboard_enabled):
        """Write the control block; returns False when it equals the last one sent."""
        blob = pack_ctl(mode, spot_radius_tan, mask_opacity, yolo_conf, keyboard_enabled)
        if blob == self._last_blob or self.ctl is None:
            return False
        self.ctl.config_write(blob)
        self._last_blob = blob
        return True

    def heartbeat(self):
        if self.ctl:
            self.ctl.heartbeat()
        if self.stat:
            self.stat.heartbeat()

    def open_status(self, timeout=0.0):
        if self.stat is not None:
            return True
        try:
            self.stat = Section.open(STAT_NAME, timeout=timeout)
        except NotReady:
            return False
        self._stat_seq = 0
        return True

    def read_status(self):
        """Newest status block, or None when nothing new / not connected / wrong version."""
        if self.stat is None:
            return None
        r = self.stat.config_read()
        if r is None:
            return None
        blob, seq = r
        if seq == self._stat_seq or len(blob) < STAT_SIZE:
            return None
        self._stat_seq = seq
        s = unpack_stat(blob)
        if s["version"] != STAT_VERSION:
            if not self._version_warned:
                self._version_warned = True
                print(f"[SHM] status block version {s['version']} (expected {STAT_VERSION}): ignoring")
            return None
        return s

    def cpp_alive(self, max_age_s=2.0):
        return self.stat is not None and bool(self.stat.peer_alive(int(max_age_s * 1e9)))

    def close(self):
        if self.stat:
            self.stat.close()
        if self.ctl:
            self.ctl.close()
        self.stat = self.ctl = None
        self._last_blob = None
        unlink(CTL_NAME)


# ---------------------------------------------------------------------------
# ShmBridge: what run_varjo.py talks to
# ---------------------------------------------------------------------------
class ShmBridge:
    """cam section (C++ producer) consumed here; phos section created here."""

    def __init__(self):
        self.cam = None
        self.phos = None
        self.configs = [None, None]
        self.blindness_mode = BLINDNESS_MACULAR
        self.yolo_conf = -1.0   # live YOLO confidence from C++; negative = keep --conf
        self.config_seq = 0
        self._cam_last_seq = [0, 0]
        self._undistort_cache = [None, None]
        self._phos_too_large_logged = [False, False]

    def open(self, timeout=60.0, require_intrinsics=True, verbose=True):
        if verbose:
            print("[SHM] waiting for the C++ app (phx_cam)...")
        self.cam = Section.open(CAM_NAME, timeout=timeout)
        t0 = time.time()
        while self.cam.config_read() is None:
            if time.time() - t0 > timeout:
                raise TimeoutError("C++ never published a camera config")
            time.sleep(0.01)
        self.refresh_config()
        if require_intrinsics:
            while time.time() - t0 < timeout:
                self.refresh_config()
                if all(c and c["intr_valid"] for c in self.configs):
                    break
                time.sleep(0.02)
            else:
                if verbose:
                    print("[SHM] WARNING: no intrinsics within timeout; using fallback crop sizes")
        # phos: a phosphene image can never exceed the camera frame
        cfg = self.configs[0]
        self.phos = Section.create(PHOS_NAME, channels=NUM_EYES, slots=3,
                                   payload_bytes=cfg["frame_w"] * cfg["frame_h"])
        self.heartbeat()
        if verbose:
            for e, c in enumerate(self.configs):
                print(f"[SHM] eye={e} frame={c['frame_w']}x{c['frame_h']} "
                      f"crop={c['crop_w']}x{c['crop_h']} stride={c['row_stride']} "
                      f"intrinsics={c['intr_valid']}")
            print(f"[SHM] blindness mode from C++: {self.blindness_mode} "
                  f"({BLINDNESS_NAMES.get(self.blindness_mode, '?')})")
        return self

    def close(self):
        if self.phos:
            self.phos.set_shutdown()
            self.phos.close()
        if self.cam:
            self.cam.close()
        self.cam = self.phos = None

    def is_shutdown(self):
        return self.cam is None or self.cam.is_shutdown()

    def heartbeat(self):
        if self.cam: self.cam.heartbeat()
        if self.phos: self.phos.heartbeat()

    def cpp_alive(self, max_age_s=2.0):
        return self.cam is not None and self.cam.peer_alive(int(max_age_s * 1e9))

    # -- config
    def refresh_config(self):
        """Re-read the config block. Returns True if it changed since last call."""
        r = self.cam.config_read()
        if r is None:
            return False
        blob, seq = r
        if seq == self.config_seq:
            return False
        self.blindness_mode, self.yolo_conf, self.configs = unpack_cam_config(blob)
        self.config_seq = seq
        return True

    # -- camera (C++ -> Python)
    def poll_camera(self, eye, wait_ms=0):
        """Latest unread camera frame for `eye` as a Frame, or None.
        Frame.payload is a zero-copy NV12 view; call Frame.release() after
        the first copy (nv12_to_bgr) and drop the frame if it returns False."""
        if wait_ms:
            self.cam.wait(eye, self._cam_last_seq[eye], wait_ms)
        try:
            f = self.cam.consume_acquire(eye, self._cam_last_seq[eye])
        except TornFrame:
            return None          # producer racing us; the next poll gets a stable slot
        if f is None:
            return None
        self._cam_last_seq[eye] = f.seq
        m = f.meta
        if m["width"] <= 0 or m["height"] <= 0 or m["byte_size"] != m["row_stride"] * m["height"] * 3 // 2:
            f.release()
            return None
        return f

    def wait_camera(self, eye, timeout_ms):
        """Block until a camera frame newer than the last poll_camera() for
        `eye` exists, the timeout passes, or C++ signals shutdown. Returns the
        PHX_* status. Both eyes are published from one callback, so waiting on
        eye 0 is enough for the capture loop."""
        return self.cam.wait(eye, self._cam_last_seq[eye], timeout_ms)

    @staticmethod
    def to_bgr(frame):
        m = frame.meta
        return geom.nv12_to_bgr(frame.payload, m["width"], m["height"], m["row_stride"])

    def undistort(self, img, eye):
        cfg = self.configs[eye]
        key = (cfg["frame_w"], cfg["frame_h"], cfg["intr_model"], cfg["intr_valid"],
               round(cfg["focal_x"], 6), round(cfg["focal_y"], 6),
               round(cfg["pp_x"], 6), round(cfg["pp_y"], 6), tuple(cfg["coeffs"]))
        cached = self._undistort_cache[eye]
        if cached is None or cached[0] != key:
            m1, m2, K = geom.build_undistort_maps(cfg)
            self._undistort_cache[eye] = (key, m1, m2, K)
            cached = self._undistort_cache[eye]
        _, m1, m2, _K = cached
        if m1 is None:
            return img
        import cv2
        return cv2.remap(img, m1, m2, interpolation=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)

    def gaze_pixel(self, frame, eye):
        m = frame.meta
        return geom.gaze_pixel(self.configs[eye], m["width"], m["height"], m["gaze_tan_x"], m["gaze_tan_y"])

    def gaze_point_fraction(self, frame, eye):
        m = frame.meta
        return geom.gaze_point_fraction(self.configs[eye], m["width"], m["height"], m["gaze_tan_x"], m["gaze_tan_y"])

    def device_field_tan(self, eye):
        return geom.device_field_tan(self.configs[eye])

    def expected_crop(self, eye, field_tan):
        return geom.expected_crop(self.configs[eye], field_tan)

    # -- device-field negotiation (Python -> C++)
    def announce_device_field(self, field_tan):
        self.phos.field_announce(field_tan)

    def wait_for_crop_match(self, field_tan, timeout=5.0, verbose=True):
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.refresh_config()
            ok = True
            for eye in range(NUM_EYES):
                expected = self.expected_crop(eye, field_tan)
                if expected is None:
                    ok = False; break
                cfg = self.configs[eye]
                if abs(cfg["crop_w"] - expected[0]) > 2 or abs(cfg["crop_h"] - expected[1]) > 2:
                    ok = False; break
            if ok:
                if verbose:
                    print(f"[SHM] C++ adopted device field tan={field_tan:.4f}: "
                          f"crop={self.configs[0]['crop_w']}x{self.configs[0]['crop_h']}")
                return True
            time.sleep(0.02)
        if verbose:
            print(f"[SHM] WARNING: C++ did not adopt device field tan={field_tan:.4f} within {timeout}s")
        return False

    # -- phosphenes (Python -> C++)
    def publish_phosphene(self, eye, frame_number, timestamp_ns, gray, flip_vertical=True):
        """Publish a 2-D uint8 phosphene image for `eye`, echoing the camera
        frame's number and capture timestamp so C++ can pair and time them."""
        gray = np.asarray(gray)
        if gray.ndim == 3 and gray.shape[2] == 1:
            gray = gray[:, :, 0]
        if gray.ndim != 2:
            raise ValueError(f"phosphene image must be 2-D grayscale, got shape {gray.shape}")
        if gray.dtype != np.uint8:
            gray = np.clip(gray, 0, 255).astype(np.uint8)
        if flip_vertical:
            gray = gray[::-1]     # GL samples the texture bottom-up
        h, w = gray.shape[:2]
        view, cap = self.phos.publish_begin(eye)
        if h * w > cap:
            if not self._phos_too_large_logged[eye]:
                self._phos_too_large_logged[eye] = True
                print(f"[SHM] phosphene image {w}x{h} exceeds slot capacity {cap}; dropping")
            self.phos.publish_commit(eye, {"byte_size": 0, "eye": eye})   # keeps seq consistent, C++ ignores empty
            return
        np.copyto(view[:h * w].reshape(h, w), gray)   # the one copy on this path (handles the flipped view)
        self.phos.publish_commit(eye, {"frame_number": int(frame_number), "timestamp_ns": int(timestamp_ns),
                                       "width": w, "height": h, "row_stride": w, "byte_size": h * w,
                                       "eye": eye})