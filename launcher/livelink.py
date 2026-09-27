"""Live panel transport for the launcher: control values out (phx_ctl), status
in (phx_stat), plus the pipeline's numbers parsed from its console lines.
No Qt here; the window calls these on its tick."""
import os
import re
import sys

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)          # phx_shm.py lives at the repo root

import phx_shm  # noqa: E402

_FPS_RE = re.compile(r"\[FPS\]\s+left=\s*([\d.]+)\s+right=\s*([\d.]+)")
_TIME_RE = re.compile(r"\[TIME\](.*)")
_PHASE_RE = re.compile(r"(\w+)=\s*([\d.]+)ms")


def parse_fps_line(line: str):
    m = _FPS_RE.search(line)
    return {"fps_left": float(m.group(1)), "fps_right": float(m.group(2))} if m else None


def parse_time_line(line: str):
    m = _TIME_RE.search(line)
    if not m:
        return None
    return {f"{name}_ms": float(val) for name, val in _PHASE_RE.findall(m.group(1))}


class LiveLink:
    def __init__(self):
        self._link = None
        self.status = None          # last status block from C++ (dict) or None
        self.pipeline = {}          # parsed [FPS] / [TIME] fields
        self.error = None

    def start(self) -> bool:
        """Create phx_ctl. Call BEFORE launching the C++ app so it finds the
        section on its first poll."""
        self.error = None
        try:
            self._link = phx_shm.ControlLink().create()
            return True
        except phx_shm.PhxError as e:
            self._link = None
            self.error = str(e)
            return False

    @property
    def connected(self) -> bool:
        return self._link is not None and self._link.stat is not None

    def set_controls(self, mode, spot_radius_tan, mask_opacity, yolo_conf, keyboard_enabled) -> bool:
        if self._link is None:
            return False
        return self._link.send(mode, spot_radius_tan, mask_opacity, yolo_conf, keyboard_enabled)

    def feed_pipeline_line(self, line: str):
        d = parse_fps_line(line)
        if d is None:
            d = parse_time_line(line)
        if d:
            self.pipeline.update(d)

    def poll(self) -> dict:
        """Heartbeat, connect to phx_stat once C++ created it, read the newest
        status. Returns the last known state (status is kept when nothing new)."""
        cpp_alive = False
        if self._link is not None:
            # F-2: any of these can raise PhxError (e.g. a section vanishing
            # mid-call); let the tick keep going with the last known status
            # rather than letting the exception escape into the Qt event loop.
            try:
                self._link.heartbeat()
                if self._link.stat is None:
                    self._link.open_status(timeout=0.0)
                s = self._link.read_status()
                if s is not None:
                    self.status = s
                cpp_alive = self._link.cpp_alive()
            except phx_shm.PhxError as e:
                self.error = str(e)
        return {"connected": self.connected, "cpp_alive": cpp_alive,
                "status": self.status, "pipeline": dict(self.pipeline)}

    def stop(self):
        if self._link is not None:
            try:
                self._link.close()
            except phx_shm.PhxError:
                pass
        self._link = None
        self.status = None
        self.pipeline = {}
