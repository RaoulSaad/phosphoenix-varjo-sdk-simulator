"""Camera geometry helpers shared by the Python pipeline.

Pure functions over the per-eye config dict that C++ publishes:
  crop_w, crop_h, frame_w, frame_h, row_stride, intr_model, intr_valid,
  focal_x, focal_y, pp_x, pp_y, coeffs (8 floats)
Moved verbatim from shm_transport.py; only the `self` parameter became `cfg`.
"""
import numpy as np

try:
    import cv2
except ImportError:      # cv2 only needed for nv12_to_bgr()/undistort maps
    cv2 = None

VARJO_INTRINSICS_MODEL_OMNIDIR = 1
VARJO_INTRINSICS_MODEL_RATIONAL = 2

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


def gaze_pixel(cfg, width, height, gaze_tan_x, gaze_tan_y):
    """Gaze position in camera pixels for this frame."""
    intr = intrinsics_pixels(cfg)
    if intr is None:
        return width * 0.5, height * 0.5
    fx, fy, cx, cy = intr
    # tangent y is up, image y is down
    return cx + fx * gaze_tan_x, cy - fy * gaze_tan_y


def gaze_point_fraction(cfg, width, height, gaze_tan_x, gaze_tan_y):
    """Gaze as (dy, dx) offsets from frame centre, in fractions of frame
    height/width — the convention the TensorRT crop kernel expects."""
    px, py = gaze_pixel(cfg, width, height, gaze_tan_x, gaze_tan_y)
    return (py - height * 0.5) / height, (px - width * 0.5) / width


def device_field_tan(cfg):
    """Half-field the C++ expects the phosphene image to span, derived from
    the crop size it asked for: crop_w = 2 * fx * tan(halfFov)."""
    intr = intrinsics_pixels(cfg)
    if intr is None or cfg["crop_w"] <= 0:
        return None
    return cfg["crop_w"] / (2.0 * intr[0])


def expected_crop(cfg, field_tan):
    """Crop size C++ should ask for once it adopts field_tan (its
    computePythonCropSize: round(2*fx*tan), min 32, bumped to even)."""
    intr = intrinsics_pixels(cfg)
    if intr is None:
        return None
    w = max(32, int(round(2.0 * intr[0] * field_tan)))
    h = max(32, int(round(2.0 * intr[1] * field_tan)))
    return (w + (w & 1), h + (h & 1))


def nv12_to_bgr(nv12, width, height, row_stride):
    """NV12 bytes or uint8 view -> contiguous BGR uint8 (H, W, 3). First copy."""
    if cv2 is None:
        raise RuntimeError("cv2 is required for nv12_to_bgr()")
    buf = np.frombuffer(nv12, np.uint8) if isinstance(nv12, (bytes, bytearray)) else np.asarray(nv12, np.uint8)
    y_plane = buf[:row_stride * height].reshape(height, row_stride)[:, :width]
    uv_plane = buf[row_stride * height:row_stride * height + row_stride * (height // 2)].reshape(height // 2, row_stride)[:, :width]
    return cv2.cvtColor(np.vstack((y_plane, uv_plane)), cv2.COLOR_YUV2BGR_NV12)