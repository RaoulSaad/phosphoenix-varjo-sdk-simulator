# phosphoenix-varjo-sdk-simulator

## Launcher

Double-click `Launch Simulator.bat` (or run `launch_simulator.sh`). It opens a
window to pick the source (headset / webcam / URL), the starting blindness
mode, the engines, the phosphene maps and the pipeline flags, then starts the
C++ app and the Python pipeline in the right order and shows both consoles.
Presets are saved in `launcher/presets/`; the last used settings are restored
automatically. Runtime keys (mode, radius, opacity, confidence) still
work while it runs; the window lists them.

Requirements: the `phos-rtcv` conda env with `PySide6` installed
(`python -m pip install PySide6`), and a built `build/Release/VarjoGazeDot.exe`.
Tests: `python -m pytest launcher/tests -q` (headless; no Qt needed).

- Don't close the minimised console window the batch file leaves in the
  taskbar: closing it takes both programs down with no teardown. Use the
  launcher's Stop button, or its close-dialog, instead.
- Don't start a second launcher while one is hidden in "Leave running" mode
  (minimised via that dialog): both would drive the same shared memory.
- The simulator's runtime keys are system-wide, so typing in the launcher
  window while a run is in progress changes the simulation, and Esc quits it
  from anywhere, including this launcher's own dialogs.
