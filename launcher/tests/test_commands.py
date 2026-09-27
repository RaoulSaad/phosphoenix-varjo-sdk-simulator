import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from launcher import commands, config  # noqa: E402


def _model(**kw):
    base = dict(repo_root="/repo", cpp_exe="/repo/build/Release/VarjoGazeDot.exe",
                pipeline_dir="/repo/pipe", conda_python="/envs/phos/python.exe",
                engine_yolo="y.engine", engine_enc="e.engine")
    base.update(kw)
    return config.FormModel(**base)


def test_cpp_headset_defaults():
    argv = commands.build_cpp_command(_model())
    assert argv == ["/repo/build/Release/VarjoGazeDot.exe", "--conf", "0.25", "--mode", "macular"]


def test_cpp_webcam_index_stereo():
    argv = commands.build_cpp_command(_model(source="webcam", webcam_index=2, fov=60, width=640,
                                             height=480, stereo=True, mode="glaucoma", conf=0.4))
    assert argv[1:] == ["--conf", "0.4", "--mode", "glaucoma", "--webcam", "2",
                        "--fov", "60", "--width", "640", "--height", "480", "--stereo"]


def test_cpp_url():
    argv = commands.build_cpp_command(_model(source="url", webcam_path="rtsp://cam/live"))
    assert argv[5:7] == ["--webcam", "rtsp://cam/live"] and "--stereo" not in argv


def test_python_minimal_no_maps():
    m = _model()
    argv = commands.build_python_command(m)
    e = os.path.join("/repo/pipe", "engines")
    assert argv == ["/envs/phos/python.exe", "-u", "run_varjo.py",
                    "--engine-yolo", os.path.join(e, "y.engine"),
                    "--engine-enc", os.path.join(e, "e.engine"),
                    "--fp16", "--stim-mode", "sample", "--encoder-config", "constrained",
                    "--sdk-dir", "/repo", "--iou", "0.5"]
    assert "--conf" not in argv          # C++ owns the confidence at runtime


def test_python_all_flags_and_map_order():
    m = _model(map_file="LgnMap_b.pickle", map_scale=1.9, mono=True,
               undistort=False, preview=True, edge_classes="0, 3", solid_classes="5",
               precision="fp32", stim_mode="encoder", encoder_config="unconstrained", iou=0.6)
    argv = commands.build_python_command(m)
    c = os.path.join("/repo/pipe", "config_viseon")
    assert argv[argv.index("--coords") + 1] == os.path.join(c, "LgnMap_b.pickle")
    for flag in ("--fp32", "--mono", "--no-undistort", "--preview"):
        assert flag in argv
    assert argv[argv.index("--phosphene-map-scale") + 1] == "1.9"
    assert argv[argv.index("--edge-classes") + 1] == "0, 3"
    assert argv[argv.index("--solid-classes") + 1] == "5"
    assert argv[argv.index("--stim-mode") + 1] == "encoder"
    assert argv[argv.index("--iou") + 1] == "0.6"


def test_display_quotes_spaces():
    s = commands.display(["C:/a b/x.exe", "--webcam", "c:/my cam.mp4"])
    assert "a b" in s and s.count('"') >= 4
