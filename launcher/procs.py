"""Spawn, watch and stop a child process without ever leaving its stdout
undrained (a full pipe would block the C++ render loop's printf). No Qt here.

OS-specific parts, both here and nowhere else:
  - process group creation (so our quit signal reaches only the child)
  - the quit signal itself (CTRL_BREAK on Windows, SIGTERM elsewhere)
"""
import os
import queue
import signal
import subprocess
import threading
import time


class ManagedProcess:
    def __init__(self, name: str, argv: list, cwd: str, ready_marker: str = None):
        self.name = name
        self.argv = list(argv)
        self.cwd = cwd
        self.ready_marker = ready_marker
        self._proc = None
        self._lines = queue.Queue()
        self._ready = threading.Event()
        self._reader = None

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        kwargs = dict(cwd=self.cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                      text=True, encoding="utf-8", errors="replace", bufsize=1)
        if os.name == "nt":
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            kwargs["start_new_session"] = True
        self._proc = subprocess.Popen(self.argv, **kwargs)
        self._reader = threading.Thread(target=self._drain, name=f"reader-{self.name}", daemon=True)
        self._reader.start()

    def _drain(self) -> None:
        try:
            for line in self._proc.stdout:
                line = line.rstrip("\r\n")
                self._lines.put(line)
                if self.ready_marker and not self._ready.is_set() and self.ready_marker in line:
                    self._ready.set()
        except Exception as e:          # never let a pipe error take the UI down
            self._lines.put(f"[launcher] reader stopped: {e}")
        finally:
            try:
                self._proc.stdout.close()
            except Exception:
                pass

    # -- observation -------------------------------------------------------
    def poll_lines(self, max_lines: int = 500) -> list:
        out = []
        while len(out) < max_lines:
            try:
                out.append(self._lines.get_nowait())
            except queue.Empty:
                break
        return out

    @property
    def ready(self) -> bool:
        return self._ready.is_set()

    def wait_ready(self, timeout: float) -> bool:
        """True once the marker was seen; False on timeout, if the child exited
        first, or if start() was never called."""
        if self._proc is None:
            return False
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._ready.is_set():
                return True
            if not self.running():
                # give the reader a moment to flush the last lines
                self._reader.join(timeout=0.5)
                return self._ready.is_set()
            time.sleep(0.02)
        return self._ready.is_set()

    def running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    @property
    def exit_code(self):
        return None if self._proc is None else self._proc.poll()

    @property
    def pid(self):
        return None if self._proc is None else self._proc.pid

    # -- stopping ----------------------------------------------------------
    def request_quit(self) -> None:
        """Ask nicely. The C++ app maps this to its console control handler."""
        if not self.running():
            return
        if os.name == "nt":
            try:
                os.kill(self._proc.pid, signal.CTRL_BREAK_EVENT)
            except OSError as e:
                # No console attached (e.g. launched without one) -> CTRL_BREAK can't
                # be delivered. Surface it in the log; the caller's deadline kill covers it.
                self._lines.put(f"[launcher] quit signal failed: {e}; will force-kill after the timeout")
        else:
            self._proc.send_signal(signal.SIGTERM)

    def wait(self, timeout: float) -> bool:
        if self._proc is None:
            return True
        try:
            self._proc.wait(timeout=timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    def kill(self, wait: bool = True) -> None:
        if self.running():
            self._proc.kill()
            if wait:
                try:
                    self._proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    pass  # kill was already sent; nothing more we can do

    def stop(self, timeout: float = 5.0) -> str:
        if not self.running():
            return "not running"
        self.request_quit()
        if self.wait(timeout):
            return "exited"
        self.kill()
        return "killed"
