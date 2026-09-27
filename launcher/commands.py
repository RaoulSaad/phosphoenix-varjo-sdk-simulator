"""Turn a FormModel into the two command lines. No Qt here."""
import os
import shlex
import subprocess

from .config import FormModel, resolve_engine, resolve_map

READY_LINE = "[SHM] ready:"     # printed by transport.cpp once the cam section exists


def _num(x) -> str:
    return f"{x:g}"


def build_cpp_command(m: FormModel) -> list:
    argv = [m.cpp_exe, "--conf", _num(m.conf), "--mode", m.mode]
    if m.source in ("webcam", "url"):
        target = str(m.webcam_index) if m.source == "webcam" else m.webcam_path.strip()
        argv += ["--webcam", target, "--fov", _num(m.fov), "--width", str(m.width), "--height", str(m.height)]
        if m.stereo:
            argv.append("--stereo")
    return argv


def build_python_command(m: FormModel) -> list:
    argv = [m.conda_python, "-u", "run_varjo.py",
            "--engine-yolo", resolve_engine(m, m.engine_yolo),
            "--engine-enc", resolve_engine(m, m.engine_enc),
            f"--{m.precision}", "--stim-mode", m.stim_mode,
            "--encoder-config", m.encoder_config,
            "--sdk-dir", m.repo_root, "--iou", _num(m.iou)]
    if m.map_file:
        argv += ["--coords", resolve_map(m, m.map_file)]
    if m.map_scale != 1.0:
        argv += ["--phosphene-map-scale", _num(m.map_scale)]
    if m.mono:
        argv.append("--mono")
    if not m.undistort:
        argv.append("--no-undistort")
    if m.preview:
        argv.append("--preview")
    if m.edge_classes.strip():
        argv += ["--edge-classes", m.edge_classes.strip()]
    if m.solid_classes.strip():
        argv += ["--solid-classes", m.solid_classes.strip()]
    return argv


def display(argv: list) -> str:
    """One copy-pasteable line for the UI (quoting rules of the host shell)."""
    if os.name == "nt":
        return subprocess.list2cmdline(argv)
    return shlex.join(argv)
