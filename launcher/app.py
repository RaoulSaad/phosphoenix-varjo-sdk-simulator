"""Launcher window. Thin: maps widgets <-> FormModel and drives ManagedProcess.
Run: python launcher/app.py   (from the phos-rtcv env; see Launch Simulator.bat)"""
import html
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDoubleSpinBox,
                               QFileDialog, QFormLayout, QGroupBox, QHBoxLayout, QInputDialog, QLabel,
                               QLineEdit, QMainWindow, QMessageBox,
                               QPlainTextEdit, QPushButton, QRadioButton, QSpinBox, QTabWidget,
                               QToolButton, QVBoxLayout, QWidget)

from launcher import commands, config
from launcher.procs import ManagedProcess

READY_TIMEOUT_S = 30.0
STOP_TIMEOUT_S = 5.0
LOG_MAX_LINES = 5000

KEY_LEGEND = ("Runtime keys (C++ window need not be focused):\n"
              "  M / G / F   blindness mode        [ / ]   scotoma / tunnel radius\n"
              "  0-9         mask opacity           , / .   YOLO confidence\n"
              "  Esc         quit\n"
              "These keys are system-wide: typing M, G, F, N, a digit or a bracket into\n"
              "this launcher while the simulation runs changes it too, and Esc anywhere\n"
              "(including this launcher's own dialogs) quits the C++ app.")

LINE_COLOURS = (("[ERR]", "#ff7b72"), ("Traceback", "#ff7b72"), ("[WARN]", "#f2c14e"),
                ("[STATE]", "#7cc7ff"), ("[MAP]", "#c792ea"), ("[GEOM]", "#c792ea"),
                ("[OK]", "#5ed28a"), ("[CFG]", "#7cc7ff"))


class LogPane(QPlainTextEdit):
    def __init__(self):
        super().__init__()
        self.setReadOnly(True)
        self.setMaximumBlockCount(LOG_MAX_LINES)

    def add(self, line: str):
        colour = next((c for tag, c in LINE_COLOURS if tag in line), None)
        text = html.escape(line)
        span = f'<span style="white-space:pre">{text}</span>'
        if colour:
            span = f'<span style="color:{colour}">{span}</span>'
        self.appendHtml(span)


class LauncherWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Phosphene simulator")
        self.resize(1180, 760)
        self.model = config.FormModel()
        self._run_model = None          # model frozen at Start; commands are built from this, never the live form
        self.cpp = None
        self.py = None
        self.phase = "stopped"          # stopped | starting | running | stopping
        self._deadline = 0.0
        self._killed = False            # set once a kill(wait=False) has been sent during "stopping"
        self._loading = False           # suppress autosave while filling widgets
        self._close_after_stop = False  # closeEvent's "Stop" is waiting for _finish_stop
        self._quit_when_done = False    # closeEvent's "Leave running" is waiting to quit

        self._build_ui()
        self.autosave = QTimer(self)
        self.autosave.setSingleShot(True)
        self.autosave.setInterval(500)
        self.autosave.timeout.connect(self._save_last)
        self._load_last()
        self.tick = QTimer(self)
        self.tick.setInterval(100)
        self.tick.timeout.connect(self._on_tick)
        self.tick.start()

    # ------------------------------------------------------------ UI build
    def _build_ui(self):
        root = QWidget(); self.setCentralWidget(root)
        outer = QHBoxLayout(root)
        self.setup_pane = self._build_setup()
        outer.addWidget(self.setup_pane, 3)
        right = QVBoxLayout()
        right.addWidget(self._build_control(), 0)
        right.addWidget(self._build_logs(), 1)
        outer.addLayout(right, 4)

    def _build_setup(self):
        box = QWidget(); v = QVBoxLayout(box)

        # Presets
        pv = QHBoxLayout()
        self.preset_combo = QComboBox(); self._refresh_presets()
        self.preset_combo.activated.connect(self._load_named)
        save_btn = QPushButton("Save as…"); save_btn.clicked.connect(self._save_named)
        pv.addWidget(QLabel("Preset")); pv.addWidget(self.preset_combo, 1); pv.addWidget(save_btn)
        v.addLayout(pv)

        # Source
        g = QGroupBox("Source"); f = QFormLayout(g)
        self.src_headset = QRadioButton("Headset"); self.src_webcam = QRadioButton("Webcam index")
        self.src_url = QRadioButton("URL or file")
        sv = QHBoxLayout()
        for r in (self.src_headset, self.src_webcam, self.src_url):
            sv.addWidget(r); r.toggled.connect(self._on_source_changed)
        f.addRow(sv)
        self.webcam_index = QSpinBox(); self.webcam_index.setRange(0, 16)
        self.webcam_path = QLineEdit(); browse = QPushButton("…"); browse.setFixedWidth(28)
        browse.clicked.connect(self._browse_video)
        pv2 = QHBoxLayout(); pv2.addWidget(self.webcam_path, 1); pv2.addWidget(browse)
        self.fov = QDoubleSpinBox(); self.fov.setRange(10, 180); self.fov.setSuffix(" deg")
        self.width = QSpinBox(); self.width.setRange(64, 7680); self.height = QSpinBox(); self.height.setRange(64, 4320)
        self.stereo = QCheckBox("Stereo (both eyes side by side)")
        self.row_index = f.rowCount(); f.addRow("Index", self.webcam_index)
        self.row_path = f.rowCount(); f.addRow("Path / URL", pv2)
        self.row_fov = f.rowCount(); f.addRow("FOV", self.fov)
        wh = QHBoxLayout(); wh.addWidget(self.width); wh.addWidget(QLabel("x")); wh.addWidget(self.height)
        self.row_wh = f.rowCount(); f.addRow("Size", wh)
        self.row_stereo = f.rowCount(); f.addRow("", self.stereo)
        self.source_form = f
        v.addWidget(g)

        # Vision
        g = QGroupBox("Vision"); f = QFormLayout(g)
        self.mode = QComboBox(); self.mode.addItems(config.MODES)
        self.conf = QDoubleSpinBox(); self.conf.setRange(0.05, 0.95); self.conf.setSingleStep(0.05)
        f.addRow("Starting blindness", self.mode); f.addRow("YOLO confidence", self.conf)
        v.addWidget(g)

        # Pipeline
        g = QGroupBox("Pipeline"); f = QFormLayout(g)
        self.engine_yolo = QComboBox(); self.engine_enc = QComboBox()
        self.stim_mode = QComboBox(); self.stim_mode.addItems(config.STIM_MODES)
        self.encoder_config = QComboBox(); self.encoder_config.addItems(config.ENCODER_CONFIGS)
        self.precision = QComboBox(); self.precision.addItems(config.PRECISIONS)
        self.map_file = QComboBox()
        self.map_file.setToolTip("Phosphene coordinate map for this run; blank = the pipeline's default viseon map.")
        self.map_scale = QDoubleSpinBox(); self.map_scale.setRange(0.1, 10.0); self.map_scale.setSingleStep(0.1)
        self.mono = QCheckBox("Mono (eye 0 only, webcam)"); self.undistort = QCheckBox("Undistort")
        self.preview = QCheckBox("Preview windows")
        self.edge_classes = QLineEdit(); self.edge_classes.setPlaceholderText("e.g. 0, 3")
        self.solid_classes = QLineEdit(); self.solid_classes.setPlaceholderText("e.g. 5")
        self.iou = QDoubleSpinBox(); self.iou.setRange(0.05, 0.95); self.iou.setSingleStep(0.05)
        f.addRow("YOLO engine", self.engine_yolo); f.addRow("Encoder engine", self.engine_enc)
        f.addRow("Stimulation", self.stim_mode); f.addRow("Encoder config", self.encoder_config)
        f.addRow("Precision", self.precision); f.addRow("Phosphene map", self.map_file); f.addRow("Map scale", self.map_scale)
        flags = QHBoxLayout(); flags.addWidget(self.mono); flags.addWidget(self.undistort); flags.addWidget(self.preview)
        f.addRow("", flags); f.addRow("Edge classes", self.edge_classes); f.addRow("Solid classes", self.solid_classes)
        f.addRow("IoU", self.iou)
        v.addWidget(g)

        # Advanced (collapsed)
        self.adv_toggle = QToolButton(); self.adv_toggle.setText("▸ Advanced (paths)")
        self.adv_toggle.setCheckable(True); self.adv_toggle.toggled.connect(self._toggle_advanced)
        v.addWidget(self.adv_toggle)
        self.adv_box = QGroupBox(); f = QFormLayout(self.adv_box)
        self.repo_root = QLineEdit(); self.cpp_exe = QLineEdit(); self.pipeline_dir = QLineEdit()
        self.conda_python = QLineEdit()
        detect = QPushButton("Detect"); detect.clicked.connect(self._detect_python)
        cp = QHBoxLayout(); cp.addWidget(self.conda_python, 1); cp.addWidget(detect)
        f.addRow("Repo root", self.repo_root); f.addRow("C++ exe", self.cpp_exe)
        f.addRow("Pipeline folder", self.pipeline_dir); f.addRow("Conda python", cp)
        self.adv_box.setVisible(False)
        v.addWidget(self.adv_box)
        v.addStretch(1)

        # change tracking -> validation, previews, autosave
        for w in (self.webcam_index, self.width, self.height):
            w.valueChanged.connect(self._on_form_changed)
        for w in (self.fov, self.conf, self.map_scale, self.iou):
            w.valueChanged.connect(self._on_form_changed)
        for w in (self.webcam_path, self.edge_classes, self.solid_classes, self.repo_root, self.cpp_exe,
                  self.pipeline_dir, self.conda_python):
            w.textChanged.connect(self._on_form_changed)
        for w in (self.mode, self.engine_yolo, self.engine_enc, self.stim_mode, self.encoder_config, self.precision):
            w.currentIndexChanged.connect(self._on_form_changed)
        for w in (self.stereo, self.mono, self.undistort, self.preview):
            w.toggled.connect(self._on_form_changed)
        self.map_file.currentIndexChanged.connect(self._on_form_changed)
        self.pipeline_dir.textChanged.connect(self._rescan)
        return box

    def _build_control(self):
        g = QGroupBox("Run"); v = QVBoxLayout(g)
        row = QHBoxLayout()
        self.start_btn = QPushButton("Start"); self.start_btn.setObjectName("start")
        self.stop_btn = QPushButton("Stop"); self.stop_btn.setObjectName("stop"); self.stop_btn.setEnabled(False)
        self.start_btn.clicked.connect(self._start); self.stop_btn.clicked.connect(self._stop)
        self.cpp_status = QLabel("C++ app: stopped"); self.py_status = QLabel("Pipeline: stopped")
        row.addWidget(self.start_btn); row.addWidget(self.stop_btn); row.addStretch(1)
        row.addWidget(self.cpp_status); row.addSpacing(16); row.addWidget(self.py_status)
        v.addLayout(row)
        self.cpp_cmd = QLineEdit(); self.cpp_cmd.setReadOnly(True)
        self.py_cmd = QLineEdit(); self.py_cmd.setReadOnly(True)
        v.addWidget(self.cpp_cmd); v.addWidget(self.py_cmd)
        self.error_label = QLabel(""); self.error_label.setObjectName("status_bad"); self.error_label.setWordWrap(True)
        v.addWidget(self.error_label)
        legend = QLabel(KEY_LEGEND); legend.setStyleSheet("font-family: Consolas, monospace; color: #9aa3b2;")
        v.addWidget(legend)
        return g

    def _build_logs(self):
        self.tabs = QTabWidget()
        self.cpp_log = LogPane(); self.py_log = LogPane()
        for name, pane in (("C++ app", self.cpp_log), ("Pipeline", self.py_log)):
            page = QWidget(); pv = QVBoxLayout(page); pv.setContentsMargins(0, 4, 0, 0)
            btns = QHBoxLayout(); clear = QPushButton("Clear"); copy = QPushButton("Copy all")
            clear.clicked.connect(pane.clear)
            copy.clicked.connect(lambda _=False, p=pane: QApplication.clipboard().setText(p.toPlainText()))
            btns.addStretch(1); btns.addWidget(clear); btns.addWidget(copy)
            pv.addWidget(pane, 1); pv.addLayout(btns)
            self.tabs.addTab(page, name)
        return self.tabs

    # ------------------------------------------------------ model <-> widgets
    def _widgets_from_model(self, m: config.FormModel):
        self._loading = True
        try:
            {"headset": self.src_headset, "webcam": self.src_webcam, "url": self.src_url}[m.source].setChecked(True)
            self.webcam_index.setValue(m.webcam_index); self.webcam_path.setText(m.webcam_path)
            self.fov.setValue(m.fov); self.width.setValue(m.width); self.height.setValue(m.height)
            self.stereo.setChecked(m.stereo)
            self.mode.setCurrentText(m.mode); self.conf.setValue(m.conf)
            self.repo_root.setText(m.repo_root); self.cpp_exe.setText(m.cpp_exe)
            self.pipeline_dir.setText(m.pipeline_dir); self.conda_python.setText(m.conda_python)
            self._rescan()
            self._select_engine(self.engine_yolo, m.engine_yolo); self._select_engine(self.engine_enc, m.engine_enc)
            self.stim_mode.setCurrentText(m.stim_mode); self.encoder_config.setCurrentText(m.encoder_config)
            self.precision.setCurrentText(m.precision)
            self._fill_maps(m.map_file)
            self.map_scale.setValue(m.map_scale); self.mono.setChecked(m.mono)
            self.undistort.setChecked(m.undistort); self.preview.setChecked(m.preview)
            self.edge_classes.setText(m.edge_classes); self.solid_classes.setText(m.solid_classes)
            self.iou.setValue(m.iou)
        finally:
            self._loading = False
        self._on_source_changed()
        self._on_form_changed()

    def _model_from_widgets(self) -> config.FormModel:
        source = "headset" if self.src_headset.isChecked() else "webcam" if self.src_webcam.isChecked() else "url"
        return config.FormModel(
            source=source, webcam_index=self.webcam_index.value(), webcam_path=self.webcam_path.text(),
            fov=self.fov.value(), width=self.width.value(), height=self.height.value(), stereo=self.stereo.isChecked(),
            mode=self.mode.currentText(), conf=round(self.conf.value(), 3),
            engine_yolo=self.engine_yolo.currentText(), engine_enc=self.engine_enc.currentText(),
            stim_mode=self.stim_mode.currentText(), encoder_config=self.encoder_config.currentText(),
            precision=self.precision.currentText(), map_file=self.map_file.currentText(),
            map_scale=round(self.map_scale.value(), 3), mono=self.mono.isChecked(),
            undistort=self.undistort.isChecked(), preview=self.preview.isChecked(),
            edge_classes=self.edge_classes.text(), solid_classes=self.solid_classes.text(),
            iou=round(self.iou.value(), 3),
            repo_root=self.repo_root.text(), cpp_exe=self.cpp_exe.text(),
            pipeline_dir=self.pipeline_dir.text(), conda_python=self.conda_python.text())

    def _fill_maps(self, selected: str):
        """Blank (pipeline default) first, then the folder's pickles. A selected
        name the scan did not find is inserted so validation can flag it."""
        self.map_file.blockSignals(True)
        self.map_file.clear(); self.map_file.addItem(""); self.map_file.addItems(config.scan_maps(self.pipeline_dir.text()))
        if self.map_file.findText(selected) < 0:
            self.map_file.addItem(selected)
        self.map_file.setCurrentText(selected)
        self.map_file.blockSignals(False)

    def _select_engine(self, combo: QComboBox, name: str):
        """Select `name` in an engine combo. If the scan didn't find it (a deleted
        or renamed file, or a pipeline folder mid-edit), insert it as an extra item
        first so the choice is never silently swapped for whatever sorts first;
        `validate` then flags it with a red border and a tooltip. An empty name
        selects the leading "" placeholder, so nothing looks chosen by accident."""
        idx = combo.findText(name)
        if idx < 0:
            combo.addItem(name)
            idx = combo.count() - 1
        combo.setCurrentIndex(idx)

    def _rescan(self):
        """Refill engine dropdowns from the pipeline folder, keeping the current
        choice selected (see _select_engine) even across a folder edit that
        temporarily finds no engines. Always re-validates afterwards so the model,
        red borders and command previews never lag behind the refreshed combos."""
        pdir = self.pipeline_dir.text()
        available = config.scan_engines(pdir)
        for combo in (self.engine_yolo, self.engine_enc):
            current = combo.currentText()
            combo.blockSignals(True)
            combo.clear(); combo.addItem(""); combo.addItems(available)
            self._select_engine(combo, current)
            combo.blockSignals(False)
        if not self._loading:
            self._fill_maps(self.map_file.currentText())
            self._on_form_changed()

    # ------------------------------------------------------------- reactions
    def _on_source_changed(self, *_):
        webcam = self.src_webcam.isChecked() or self.src_url.isChecked()
        self.source_form.setRowVisible(self.row_index, self.src_webcam.isChecked())
        self.source_form.setRowVisible(self.row_path, self.src_url.isChecked())
        for row in (self.row_fov, self.row_wh, self.row_stereo):
            self.source_form.setRowVisible(row, webcam)
        self._on_form_changed()

    def _on_form_changed(self, *_):
        if self._loading:
            return
        self.model = self._model_from_widgets()
        errors = config.validate(self.model)
        widgets = {"cpp_exe": self.cpp_exe, "conda_python": self.conda_python, "webcam_path": self.webcam_path,
                   "engine_yolo": self.engine_yolo, "engine_enc": self.engine_enc, "map_file": self.map_file}
        for key, w in widgets.items():
            w.setProperty("invalid", key in errors); w.setToolTip(errors.get(key, ""))
            w.style().unpolish(w); w.style().polish(w)
        self.error_label.setText("  •  ".join(errors.values()))
        self.cpp_cmd.setText(commands.display(commands.build_cpp_command(self.model)))
        self.py_cmd.setText(commands.display(commands.build_python_command(self.model)))
        self.start_btn.setEnabled(not errors and self.phase == "stopped")
        self.autosave.start()

    def _toggle_advanced(self, on: bool):
        self.adv_box.setVisible(on)
        self.adv_toggle.setText(("▾" if on else "▸") + " Advanced (paths)")

    def _browse_video(self):
        path, _ = QFileDialog.getOpenFileName(self, "Video file", "", "Video (*.mp4 *.mkv *.avi *.mov);;All files (*)")
        if path:
            self.webcam_path.setText(path)

    def _detect_python(self):
        found = config.find_conda_pythons()
        if not found:
            QMessageBox.information(self, "Detect", "No conda environments found under the usual folders.")
            return
        choice, ok = QInputDialog.getItem(self, "Conda python", "Environment:", found, 0, False)
        if ok:
            self.conda_python.setText(choice)

    # --------------------------------------------------------------- presets
    def _refresh_presets(self):
        self.preset_combo.blockSignals(True)
        self.preset_combo.clear(); self.preset_combo.addItem("(last used)"); self.preset_combo.addItems(config.list_presets())
        self.preset_combo.blockSignals(False)

    def _load_last(self):
        path = config.last_preset_path()
        if os.path.isfile(path):
            try:
                self._widgets_from_model(config.load_preset(path))
                return
            except Exception as e:
                self.cpp_log.add(f"[launcher] could not read {path}: {e}; using defaults")
        self._widgets_from_model(config.FormModel())

    def _save_last(self):
        try:
            config.save_preset(self.model, config.last_preset_path())
        except Exception as e:
            self.cpp_log.add(f"[launcher] could not save last.json: {e}")

    def _load_named(self, index: int):
        if index <= 0:
            return
        name = self.preset_combo.itemText(index)
        try:
            self._widgets_from_model(config.load_preset(os.path.join(config.presets_dir(), name + ".json")))
        except Exception as e:
            QMessageBox.warning(self, "Preset", f"Could not load {name}: {e}")

    def _save_named(self):
        name, ok = QInputDialog.getText(self, "Save preset", "Name:")
        if ok and name.strip():
            config.save_preset(self.model, os.path.join(config.presets_dir(), name.strip() + ".json"))
            self._refresh_presets(); self.preset_combo.setCurrentText(name.strip())

    # ------------------------------------------------------------- processes
    def _start(self):
        self.model = self._model_from_widgets()
        errors = config.validate(self.model)
        if errors:
            self._on_form_changed(); return
        self._run_model = self.model    # frozen for this run; _launch_pipeline reads only this
        self._killed = False
        self.cpp_log.clear(); self.py_log.clear()
        self.cpp = ManagedProcess("cpp", commands.build_cpp_command(self._run_model), cwd=self._run_model.repo_root,
                                  ready_marker=commands.READY_LINE)
        self.py = None
        try:
            self.cpp.start()
        except OSError as e:
            self._set_status(self.cpp_status, f"C++ app: failed to start ({e})", "bad"); return
        self._set_phase("starting"); self._deadline = time.time() + READY_TIMEOUT_S
        self._set_status(self.cpp_status, "C++ app: starting", "wait")
        self._set_status(self.py_status, "Pipeline: waiting for C++", "wait")
        self.start_btn.setEnabled(False); self.stop_btn.setEnabled(True)

    def _launch_pipeline(self):
        self.py = ManagedProcess("py", commands.build_python_command(self._run_model), cwd=self._run_model.pipeline_dir)
        try:
            self.py.start()
        except OSError as e:
            self._set_status(self.py_status, f"Pipeline: failed to start ({e})", "bad"); self.py = None; return
        self._set_status(self.py_status, "Pipeline: running", "ok")

    def _stop(self):
        if self.phase in ("stopped", "stopping"):
            return
        self._set_phase("stopping"); self._deadline = time.time() + STOP_TIMEOUT_S
        self.stop_btn.setEnabled(False)
        if self.cpp and self.cpp.running():
            self.cpp.request_quit()          # the pipeline exits on its own via the shutdown flag
        elif self.py and self.py.running():
            self.py.request_quit()

    def _on_tick(self):
        for proc, pane in ((self.cpp, self.cpp_log), (self.py, self.py_log)):
            if proc:
                for line in proc.poll_lines():
                    pane.add(line)
        if self.phase == "starting":
            if self.cpp.ready:
                self._set_status(self.cpp_status, "C++ app: running", "ok")
                self._set_phase("running"); self._launch_pipeline()
            elif not self.cpp.running():
                text, kind = self._exit_status("C++ app", self.cpp.exit_code)
                self._set_status(self.cpp_status, text, kind)
                self._set_phase("stopped"); self.stop_btn.setEnabled(False); self._on_form_changed()
            elif time.time() > self._deadline:
                self.cpp_log.add("[launcher] no '[SHM] ready' within 30 s; stopping")
                self._stop()
        elif self.phase == "running":
            if self.cpp and not self.cpp.running():
                text, kind = self._exit_status("C++ app", self.cpp.exit_code)
                self._set_status(self.cpp_status, text, kind)
            if self.py and not self.py.running():
                text, kind = self._exit_status("Pipeline", self.py.exit_code)
                self._set_status(self.py_status, text, kind)
            if (not self.cpp or not self.cpp.running()) and (not self.py or not self.py.running()):
                self._set_phase("stopped"); self.stop_btn.setEnabled(False); self._on_form_changed()
        elif self.phase == "stopping":
            alive = [p for p in (self.cpp, self.py) if p and p.running()]
            if not alive:
                self._finish_stop("killed" if self._killed else "exited")
            elif time.time() > self._deadline:
                for p in alive:
                    p.kill(wait=False)
                    self.cpp_log.add(f"[launcher] {p.name} did not exit in time; killing")
                self._killed = True
        self._maybe_quit_when_done()

    @staticmethod
    def _exit_status(prefix: str, code) -> tuple:
        """0 is a clean exit, not a crash; anything else is."""
        if code == 0:
            return f"{prefix}: exited", "ok"
        return f"{prefix}: crashed (exit {code})", "bad"

    def _finish_stop(self, how: str):
        self._set_phase("stopped")
        self._set_status(self.cpp_status, f"C++ app: stopped ({how})", "ok" if how == "exited" else "bad")
        self._set_status(self.py_status, "Pipeline: stopped", "ok")
        self.stop_btn.setEnabled(False); self._on_form_changed()
        if self._close_after_stop:
            self._close_after_stop = False
            self.close()
        self._maybe_quit_when_done()

    def _maybe_quit_when_done(self):
        """Leave-running left the window hidden and the tick timer draining the
        children's output; once they've both exited (phase back to "stopped"),
        actually quit instead of leaving a hidden, unkillable-from-the-UI process."""
        if self.phase == "stopped" and self._quit_when_done:
            self._quit_when_done = False
            QApplication.instance().quit()

    def _set_status(self, label: QLabel, text: str, kind: str):
        if label.text() == text:
            return
        label.setText(text); label.setObjectName({"ok": "status_ok", "bad": "status_bad", "wait": "status_wait"}[kind])
        label.style().unpolish(label); label.style().polish(label)

    def _set_phase(self, phase: str):
        """Single choke point for every phase transition, so the setup pane's
        enabled state can never drift from `phase`: enabled only when stopped,
        so the running command can't be edited out from under a live process."""
        self.phase = phase
        self.setup_pane.setEnabled(phase == "stopped")

    def closeEvent(self, event):
        if self.phase == "stopped":
            event.accept(); return
        box = QMessageBox(self); box.setWindowTitle("Simulation running")
        box.setText("Stop the simulation?")
        stop = box.addButton("Stop", QMessageBox.AcceptRole)
        leave = box.addButton("Leave running (launcher hides until they exit)", QMessageBox.DestructiveRole)
        box.addButton(QMessageBox.Cancel); box.exec()
        if box.clickedButton() == stop:
            # Defer to the same tick-driven wait/kill machinery as the Stop button
            # (request_quit on the C++ app only; _finish_stop() will re-close us).
            self._close_after_stop = True
            self._stop()
            event.ignore()
        elif box.clickedButton() == leave:
            # Don't touch the children: killing this process would close their
            # stdout pipes (they run with -u) and crash them with BrokenPipeError.
            # Keep draining via the tick timer, hidden, until they exit on their own.
            self._quit_when_done = True
            self.hide()
            event.ignore()
        else:
            event.ignore()


def main():
    app = QApplication(sys.argv)
    qss = os.path.join(os.path.dirname(os.path.abspath(__file__)), "style.qss")
    if os.path.isfile(qss):
        with open(qss, encoding="utf-8") as f:
            app.setStyleSheet(f.read())
    win = LauncherWindow(); win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
