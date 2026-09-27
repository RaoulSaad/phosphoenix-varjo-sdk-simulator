import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from launcher import livelink  # noqa: E402


def test_parse_fps_line():
    assert livelink.parse_fps_line("[FPS] left= 58.9  right=  0.0  (phos 256x256)") == {"fps_left": 58.9, "fps_right": 0.0}
    assert livelink.parse_fps_line("[STATE] mode=macular") is None


def test_parse_time_line():
    d = livelink.parse_time_line("[TIME] shm_in=  0.08ms  pipeline= 13.19ms  shm_out=  0.11ms")
    assert d == {"shm_in_ms": 0.08, "pipeline_ms": 13.19, "shm_out_ms": 0.11}
    assert livelink.parse_time_line("[TIME] (no samples)") == {}
    assert livelink.parse_time_line("[FPS] left=1 right=2") is None


def test_feed_pipeline_line_accumulates():
    ll = livelink.LiveLink()
    ll.feed_pipeline_line("[FPS] left= 30.0  right= 29.5  (phos 256x256)")
    ll.feed_pipeline_line("[TIME] shm_in=  0.10ms  pipeline= 12.00ms  shm_out=  0.20ms")
    ll.feed_pipeline_line("[DBG] eye=0 detections=3")
    assert ll.poll()["pipeline"] == {"fps_left": 30.0, "fps_right": 29.5,
                                     "shm_in_ms": 0.10, "pipeline_ms": 12.0, "shm_out_ms": 0.20}
    assert ll.poll()["connected"] is False and ll.poll()["status"] is None


def test_live_link_round_trip_with_fake_cpp():
    phx_shm = pytest.importorskip("phx_shm")
    try:
        phx_shm.load_library()
    except phx_shm.PhxError:
        pytest.skip("phx_shm library not built")
    phx_shm.unlink(phx_shm.CTL_NAME); phx_shm.unlink(phx_shm.STAT_NAME)
    ll = livelink.LiveLink()
    assert ll.start() is True and ll.error is None
    cpp_ctl = None
    cpp_stat = None
    try:
        cpp_ctl = phx_shm.Section.open(phx_shm.CTL_NAME, timeout=1.0)
        assert ll.set_controls(1, 0.2, 0.5, 0.3, True) is True
        blob, _ = cpp_ctl.config_read()
        c = phx_shm.unpack_ctl(blob)
        assert c["mode"] == 1 and c["keyboard_enabled"] is True and c["spot_radius_tan"] == pytest.approx(0.2)

        assert ll.poll()["connected"] is False
        cpp_stat = phx_shm.Section.create(phx_shm.STAT_NAME, channels=1, slots=2, payload_bytes=64)
        cpp_stat.config_write(phx_shm.pack_stat(mode=1, spot_radius_tan=0.2, mask_opacity=0.5,
                                                yolo_conf=0.3, python_alive=1, render_fps=88.0))
        cpp_stat.heartbeat()
        info = ll.poll()
        assert info["connected"] is True and info["cpp_alive"] is True
        assert info["status"]["render_fps"] == pytest.approx(88.0) and info["status"]["python_alive"] is True
        info2 = ll.poll()                                   # nothing new: last status is kept
        assert info2["status"]["render_fps"] == pytest.approx(88.0)
    finally:
        if cpp_ctl is not None:
            cpp_ctl.close()
        if cpp_stat is not None:
            cpp_stat.close()
        ll.stop()
    assert ll.poll()["connected"] is False


def test_live_link_poll_ignores_wrong_version_status():
    """F-7 companion: a status block with the wrong version must not surface
    through poll(), and no exception should escape."""
    phx_shm = pytest.importorskip("phx_shm")
    try:
        phx_shm.load_library()
    except phx_shm.PhxError:
        pytest.skip("phx_shm library not built")
    phx_shm.unlink(phx_shm.CTL_NAME); phx_shm.unlink(phx_shm.STAT_NAME)
    ll = livelink.LiveLink()
    assert ll.start() is True and ll.error is None
    cpp_stat = None
    try:
        cpp_stat = phx_shm.Section.create(phx_shm.STAT_NAME, channels=1, slots=2, payload_bytes=64)
        cpp_stat.config_write(phx_shm.pack_stat(version=99, mode=1))
        cpp_stat.heartbeat()
        info = ll.poll()
        assert info["status"] is None
    finally:
        if cpp_stat is not None:
            cpp_stat.close()
        ll.stop()
