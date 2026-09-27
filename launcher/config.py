"""Form model, presets and folder scans for the launcher. No Qt here."""
import dataclasses
import glob
import json
import os
from dataclasses import dataclass, field

PRESET_VERSION = 1
SOURCES = ("headset", "webcam", "url")
MODES = ("macular", "glaucoma", "full")
STIM_MODES = ("sample", "encoder")
ENCODER_CONFIGS = ("constrained", "unconstrained", "constrained_boundary")
PRECISIONS = ("fp16", "fp32", "int8")


def default_repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_conda_python() -> str:
    if os.name == "nt":
        return os.path.join(os.path.expanduser("~"), ".conda", "envs", "phos-rtcv", "python.exe")
    return "python"


@dataclass
class FormModel:
    # Source
    source: str = "headset"          # SOURCES
    webcam_index: int = 0
    webcam_path: str = ""            # URL or file (source == "url")
    fov: float = 70.0
    width: int = 1280
    height: int = 720
    stereo: bool = False
    # Vision
    mode: str = "macular"            # MODES
    conf: float = 0.25
    # Pipeline
    engine_yolo: str = ""            # basename under <pipeline>/engines
    engine_enc: str = ""
    stim_mode: str = "sample"        # STIM_MODES
    encoder_config: str = "constrained"
    precision: str = "fp16"          # PRECISIONS
    map_file: str = ""               # basename under config_viseon; "" = the pipeline's default map
    map_scale: float = 1.0
    mono: bool = False
    undistort: bool = True
    preview: bool = False
    edge_classes: str = ""
    solid_classes: str = ""
    iou: float = 0.5
    # Advanced (machine paths)
    repo_root: str = field(default_factory=default_repo_root)
    cpp_exe: str = ""
    pipeline_dir: str = ""
    conda_python: str = field(default_factory=default_conda_python)

    def __post_init__(self):
        if not self.cpp_exe:
            self.cpp_exe = os.path.join(self.repo_root, "build", "Release", "VarjoGazeDot.exe")
        if not self.pipeline_dir:
            self.pipeline_dir = os.path.join(self.repo_root, "realtime_phosphene_pipeline")


# -- paths ----------------------------------------------------------------
def engines_dir(m: FormModel) -> str:
    return os.path.join(m.pipeline_dir, "engines")


def maps_dir(m: FormModel) -> str:
    return os.path.join(m.pipeline_dir, "config_viseon")


def resolve_engine(m: FormModel, name: str) -> str:
    return os.path.join(engines_dir(m), name)


def resolve_map(m: FormModel, name: str) -> str:
    return os.path.join(maps_dir(m), name)


def _scan(folder: str, pattern: str) -> list:
    return sorted(os.path.basename(p) for p in glob.glob(os.path.join(folder, pattern)))


def scan_engines(pipeline_dir: str) -> list:
    return _scan(os.path.join(pipeline_dir, "engines"), "*.engine")


def scan_maps(pipeline_dir: str) -> list:
    return _scan(os.path.join(pipeline_dir, "config_viseon"), "*.pickle")


def find_conda_pythons(home: str = None) -> list:
    """python executables of conda envs under the usual roots."""
    home = home or os.path.expanduser("~")
    exe = "python.exe" if os.name == "nt" else os.path.join("bin", "python")
    roots = [os.path.join(home, d, "envs") for d in (".conda", "miniconda3", "anaconda3", "miniforge3")]
    found = []
    for root in roots:
        for env in sorted(glob.glob(os.path.join(root, "*"))):
            p = os.path.join(env, exe)
            if os.path.isfile(p):
                found.append(p)
    return found


# -- validation -----------------------------------------------------------
def validate(m: FormModel) -> dict:
    """Field name -> message. Empty dict means the model can be launched."""
    errors = {}
    if not os.path.isfile(m.cpp_exe):
        errors["cpp_exe"] = f"not found: {m.cpp_exe}"
    if not (os.path.isfile(m.conda_python) or m.conda_python == "python"):
        errors["conda_python"] = f"not found: {m.conda_python}"
    if m.source == "url" and not m.webcam_path.strip():
        errors["webcam_path"] = "enter a URL or file path"
    if not m.engine_yolo or not os.path.isfile(resolve_engine(m, m.engine_yolo)):
        errors["engine_yolo"] = "pick an existing YOLO engine"
    if not m.engine_enc or not os.path.isfile(resolve_engine(m, m.engine_enc)):
        errors["engine_enc"] = "pick an existing encoder engine"
    if m.map_file and not os.path.isfile(resolve_map(m, m.map_file)):
        errors["map_file"] = f"missing: {m.map_file}"
    if m.source not in SOURCES or m.mode not in MODES or m.stim_mode not in STIM_MODES \
            or m.encoder_config not in ENCODER_CONFIGS or m.precision not in PRECISIONS:
        errors["choices"] = "a choice field holds an unknown value"
    return errors


# -- presets --------------------------------------------------------------
def to_dict(m: FormModel) -> dict:
    d = dataclasses.asdict(m)
    d["version"] = PRESET_VERSION
    return d


def from_dict(d: dict) -> FormModel:
    known = {f.name for f in dataclasses.fields(FormModel)}
    kwargs = {k: v for k, v in d.items() if k in known}
    # Presets saved before the dropdown held a checklist under "maps"; its
    # first entry was the map that got loaded.
    if "map_file" not in kwargs and d.get("maps"):
        kwargs["map_file"] = str(list(d["maps"])[0])
    return FormModel(**kwargs)


def save_preset(m: FormModel, path: str) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(to_dict(m), f, indent=2)


def load_preset(path: str) -> FormModel:
    with open(path, encoding="utf-8") as f:
        return from_dict(json.load(f))


def presets_dir() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "presets")


def last_preset_path() -> str:
    return os.path.join(presets_dir(), "last.json")


def list_presets() -> list:
    names = []
    for p in sorted(glob.glob(os.path.join(presets_dir(), "*.json"))):
        name = os.path.splitext(os.path.basename(p))[0]
        if name != "last":
            names.append(name)
    return names
