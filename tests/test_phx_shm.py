"""Python <-> C cross-language tests for phx_shm.

Needs the library and test binary built by phx_shm/CMakeLists.txt. Set
PHX_SHM_LIB to the .dll/.so and PHX_SHM_TEST_BIN to phx_shm_test if they are
not in build-phx/Release.
"""
import ctypes
import os
import subprocess
import sys
import time

import numpy as np
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import phx_shm  # noqa: E402


def _test_bin():
    p = os.environ.get("PHX_SHM_TEST_BIN")
    if p:
        return p
    for cand in ("build-phx/Release/phx_shm_test.exe", "build-phx/phx_shm_test",
                 "build/Release/phx_shm_test.exe"):
        full = os.path.join(ROOT, cand)
        if os.path.exists(full):
            return full
    pytest.skip("phx_shm_test binary not built")


def pattern_byte(s, i):
    return (s * 31 + i * 7 + (s >> 8)) & 0xFF


def pattern_size(s, capacity):
    return 1 + (s * 7919) % capacity


def pattern_frame(s, capacity):
    n = pattern_size(s, capacity)
    i = np.arange(n, dtype=np.uint64)
    return ((s * 31 + i * 7 + (s >> 8)) & 0xFF).astype(np.uint8)


def test_struct_sizes():
    assert ctypes.sizeof(phx_shm.phx_layout) == 12
    assert ctypes.sizeof(phx_shm.phx_meta) == 48
    assert ctypes.sizeof(phx_shm.phx_view) == 8 + 48 + 8 + 8
    assert phx_shm.phx_meta.frame_number.offset == 0
    assert phx_shm.phx_meta.timestamp_ns.offset == 8
    assert phx_shm.phx_meta.width.offset == 16
    assert phx_shm.phx_meta.gaze_tan_x.offset == 32
    assert phx_shm.phx_meta.eye.offset == 40
    assert phx_shm.phx_view.seq.offset == 56
    assert phx_shm.phx_view.slot.offset == 64


def test_python_roundtrip():
    """Python producer and Python consumer in one process."""
    name = "phx_py_unit"
    phx_shm.unlink(name)
    with pytest.raises(phx_shm.NotReady):
        phx_shm.Section.open(name, timeout=0.05)
    prod = phx_shm.Section.create(name, channels=2, slots=3, payload_bytes=1000)
    cons = phx_shm.Section.open(name, timeout=1.0)
    assert cons.layout == (2, 3, 4096)
    assert cons.consume_acquire(1, 0) is None
    assert cons.wait(1, 0, 10) == phx_shm.PHX_TIMEOUT

    view, cap = prod.publish_begin(1)
    assert cap == 4096 and view.shape == (4096,) and view.dtype == np.uint8
    view[:300] = (np.arange(300) * 3) & 0xFF
    prod.publish_commit(1, {"frame_number": 77, "timestamp_ns": 12345, "width": 20,
                            "height": 15, "row_stride": 20, "byte_size": 300,
                            "gaze_tan_x": 0.25, "gaze_tan_y": -0.5, "eye": 1})
    assert cons.wait(1, 0, 10) == phx_shm.PHX_OK
    f = cons.consume_acquire(1, 0)
    assert f is not None and f.seq == 1 and f.slot == 1
    assert f.meta["frame_number"] == 77 and f.meta["byte_size"] == 300
    assert f.meta["gaze_tan_x"] == pytest.approx(0.25) and f.meta["eye"] == 1
    assert f.payload.shape == (300,)
    assert np.array_equal(f.payload, ((np.arange(300) * 3) & 0xFF).astype(np.uint8))
    assert f.release() is True
    assert cons.consume_acquire(1, 1) is None

    # torn: overwrite the held slot with three more publishes
    f = cons.consume_acquire(1, 0)
    for _ in range(3):
        v, _ = prod.publish_begin(1)
        prod.publish_commit(1, {"byte_size": 1})
    assert f.release() is False

    # header blocks
    assert cons.config_read() is None
    prod.config_write(b"cfg-1")
    blob, seq = cons.config_read()
    assert blob == b"cfg-1" and seq == 2
    assert prod.field_poll() is None
    cons.field_announce(0.4452)
    tan, fseq = prod.field_poll()
    assert tan == pytest.approx(0.4452, abs=1e-6) and fseq == 2
    assert not prod.peer_alive(10**9)
    cons.heartbeat()
    assert prod.peer_alive(10**9)

    assert not cons.is_shutdown()
    prod.set_shutdown()
    assert cons.is_shutdown()
    cons.close()
    prod.close()


def test_c_producer_python_consumer():
    """The C torture producer feeds the Python wrapper; every byte is checked."""
    name = "phx_xlang_c2py"
    phx_shm.unlink(name)
    proc = subprocess.Popen([_test_bin(), "--producer", name, "--frames", "3000",
                             "--payload", "65536", "--channels", "2"])
    try:
        cons = phx_shm.Section.open(name, timeout=10.0)
        cons.heartbeat()                       # lets the producer start flooding
        capacity = cons.layout[2]
        last = [0, 0]
        accepted = torn = 0
        while True:
            any_new = False
            for ch in range(2):
                try:
                    f = cons.consume_acquire(ch, last[ch])
                except phx_shm.TornFrame:
                    torn += 1
                    continue
                if f is None:
                    continue
                any_new = True
                data = f.payload.copy()
                if not f.release():
                    torn += 1
                    continue
                assert f.meta["frame_number"] == f.seq
                assert f.meta["byte_size"] == pattern_size(f.seq, capacity)
                assert np.array_equal(data, pattern_frame(f.seq, capacity)), f"corrupt frame seq={f.seq}"
                accepted += 1
                last[ch] = f.seq
            if not any_new:
                if cons.is_shutdown():
                    break
                cons.wait(0, last[0], 5)
        assert accepted > 0
        print(f"c->py accepted={accepted} torn={torn}")
    finally:
        proc.wait(timeout=30)
        assert proc.returncode == 0


def test_python_producer_c_consumer():
    """The Python wrapper feeds the C torture consumer, which validates bytes."""
    name = "phx_xlang_py2c"
    phx_shm.unlink(name)
    prod = phx_shm.Section.create(name, channels=2, slots=3, payload_bytes=65536)
    proc = subprocess.Popen([_test_bin(), "--consumer", name, "--min-accepted", "100"])
    try:
        t0 = time.time()
        while not prod.peer_alive(2 * 10**9) and time.time() - t0 < 10:
            time.sleep(0.01)
        capacity = prod.layout[2]
        seq = [0, 0]
        for _ in range(3000):
            for ch in range(2):
                view, cap = prod.publish_begin(ch)
                seq[ch] += 1
                s = seq[ch]
                frame = pattern_frame(s, capacity)
                view[:frame.size] = frame
                prod.publish_commit(ch, {"frame_number": s, "timestamp_ns": phx_shm.now_ns(),
                                         "byte_size": frame.size, "width": frame.size,
                                         "height": 1, "row_stride": frame.size, "eye": ch})
        prod.set_shutdown()
        time.sleep(0.2)
    finally:
        proc.wait(timeout=30)
        prod.close()
    assert proc.returncode == 0


def test_bridge_config_blob_roundtrip():
    """ShmBridge's cam-config packing must match transport.cpp's CamConfigBlob."""
    assert phx_shm.CAM_CONFIG_SIZE == 264
    eye = dict(crop_w=158, crop_h=158, frame_w=1152, frame_h=1152, row_stride=1152,
               intr_model=2, intr_valid=True, focal_x=560.0, focal_y=560.0,
               pp_x=580.0, pp_y=571.0, coeffs=[0.1, 0.2, 0, 0, 0, 0, 0, 0])
    blob = phx_shm.pack_cam_config(1, [eye, eye], yolo_conf=0.4)
    mode, conf, eyes = phx_shm.unpack_cam_config(blob)
    assert mode == 1 and eyes[0] == eye and eyes[1] == eye
    assert abs(conf - 0.4) < 1e-6
    # Default: -1 conf sentinel (Python keeps its own --conf).
    _, conf, _ = phx_shm.unpack_cam_config(phx_shm.pack_cam_config(0, [eye, eye]))
    assert conf < 0


def test_bridge_wait_camera_uses_last_seen_seq():
    """wait_camera must return immediately when a frame newer than the last
    poll exists, and time out otherwise."""
    name_cam = phx_shm.CAM_NAME
    phx_shm.unlink(name_cam)
    prod = phx_shm.Section.create(name_cam, channels=2, slots=3, payload_bytes=64)
    prod.config_write(phx_shm.pack_cam_config(2, [dict(
        crop_w=32, crop_h=32, frame_w=8, frame_h=4, row_stride=8, intr_model=2,
        intr_valid=True, focal_x=4.0, focal_y=4.0, pp_x=4.0, pp_y=2.0, coeffs=[0] * 8)] * 2))
    b = phx_shm.ShmBridge()
    b.cam = phx_shm.Section.open(name_cam, timeout=1.0)
    b.refresh_config()
    assert b.wait_camera(0, 5) == phx_shm.PHX_TIMEOUT
    v, _ = prod.publish_begin(0)
    prod.publish_commit(0, {"byte_size": 48, "width": 8, "height": 4, "row_stride": 8, "eye": 0})
    assert b.wait_camera(0, 5) == phx_shm.PHX_OK
    f = b.poll_camera(0)
    assert f is not None and f.release()
    assert b.wait_camera(0, 5) == phx_shm.PHX_TIMEOUT      # nothing newer than what we took
    b.cam.close()
    prod.set_shutdown()
    prod.close()
