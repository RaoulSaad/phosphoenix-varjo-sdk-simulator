import json
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from launcher import config  # noqa: E402


def _pipeline(tmp_path):
    """A fake pipeline folder with engines and maps."""
    (tmp_path / "engines").mkdir()
    (tmp_path / "config_viseon").mkdir()
    for n in ("b.engine", "a.engine", "timing.cache"):
        (tmp_path / "engines" / n).write_bytes(b"x")
    for n in ("LgnMap_2.pickle", "LgnMap_1.pickle", "notes.txt"):
        (tmp_path / "config_viseon" / n).write_bytes(b"x")
    return str(tmp_path)


def test_defaults_are_sane():
    m = config.FormModel()
    assert m.source == "headset" and m.mode == "macular" and m.conf == 0.25
    assert m.undistort is True and m.map_file == ""
    assert os.path.isabs(m.repo_root)
    assert m.pipeline_dir == os.path.join(m.repo_root, "realtime_phosphene_pipeline")
    assert m.cpp_exe == os.path.join(m.repo_root, "build", "Release", "VarjoGazeDot.exe")


def test_scan_engines_and_maps_sorted_basenames(tmp_path):
    p = _pipeline(tmp_path)
    assert config.scan_engines(p) == ["a.engine", "b.engine"]
    assert config.scan_maps(p) == ["LgnMap_1.pickle", "LgnMap_2.pickle"]
    assert config.scan_engines(str(tmp_path / "missing")) == []


def test_resolve_paths(tmp_path):
    m = config.FormModel(pipeline_dir=_pipeline(tmp_path))
    assert config.resolve_engine(m, "a.engine") == os.path.join(m.pipeline_dir, "engines", "a.engine")
    assert config.resolve_map(m, "LgnMap_1.pickle") == os.path.join(m.pipeline_dir, "config_viseon", "LgnMap_1.pickle")


def test_preset_round_trip(tmp_path):
    m = config.FormModel(source="url", webcam_path="rtsp://x", map_file="LgnMap_2.pickle",
                         conf=0.4, stereo=True, edge_classes="0, 3")
    path = str(tmp_path / "demo.json")
    config.save_preset(m, path)
    loaded = config.load_preset(path)
    assert loaded == m
    raw = json.loads(open(path, encoding="utf-8").read())
    assert raw["version"] == config.PRESET_VERSION


def test_from_dict_ignores_unknown_and_fills_missing():
    m = config.from_dict({"version": 1, "conf": 0.6, "bogus": 42})
    assert m.conf == 0.6 and m.mode == "macular"


def test_validate_reports_missing_files_and_empty_source(tmp_path):
    p = _pipeline(tmp_path)
    m = config.FormModel(pipeline_dir=p, engine_yolo="a.engine", engine_enc="nope.engine",
                         map_file="gone.pickle", source="url", webcam_path="",
                         cpp_exe=str(tmp_path / "no.exe"), conda_python=str(tmp_path / "no-python.exe"))
    errors = config.validate(m)
    assert set(errors) == {"engine_enc", "map_file", "webcam_path", "cpp_exe", "conda_python"}


def test_validate_ok(tmp_path):
    p = _pipeline(tmp_path)
    exe = tmp_path / "VarjoGazeDot.exe"; exe.write_bytes(b"x")
    py = tmp_path / "python.exe"; py.write_bytes(b"x")
    m = config.FormModel(pipeline_dir=p, engine_yolo="a.engine", engine_enc="b.engine",
                         map_file="LgnMap_1.pickle", cpp_exe=str(exe), conda_python=str(py))
    assert config.validate(m) == {}


def test_find_conda_pythons(tmp_path):
    envs = tmp_path / ".conda" / "envs"
    for name in ("work", "phos-rtcv"):
        exe = (envs / name / "python.exe") if os.name == "nt" else (envs / name / "bin" / "python")
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b"x")
    found = config.find_conda_pythons(home=str(tmp_path))
    assert len(found) == 2 and any("phos-rtcv" in f for f in found)


def test_list_presets_excludes_last(tmp_path, monkeypatch):
    monkeypatch.setattr(config, "presets_dir", lambda: str(tmp_path))
    for n in ("last.json", "demo.json", "headset run.json", "readme.txt"):
        (tmp_path / n).write_text("{}")
    assert config.list_presets() == ["demo", "headset run"]


def test_from_dict_migrates_old_maps_checklist():
    m = config.from_dict({"version": 1, "maps": ["b.pickle", "a.pickle"]})
    assert m.map_file == "b.pickle"
    assert config.from_dict({"version": 1, "maps": []}).map_file == ""
