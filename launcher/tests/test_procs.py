import os
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from launcher.procs import ManagedProcess  # noqa: E402

STANDIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "standin.py")


def _has_console() -> bool:
    if os.name != "nt":
        return True
    import ctypes
    return ctypes.windll.kernel32.GetConsoleWindow() != 0


@pytest.fixture
def spawn():
    """Owns every ManagedProcess a test creates and kills whatever is still
    running at teardown, so a failed assertion never leaks a real child
    (kill() is a no-op once the child has already exited)."""
    tracked = []

    def factory(*extra):
        p = ManagedProcess("standin", [sys.executable, "-u", STANDIN, *extra], cwd=ROOT,
                            ready_marker="[SHM] ready:")
        tracked.append(p)
        return p

    def track(p):
        tracked.append(p)
        return p

    factory.track = track
    yield factory
    for p in tracked:
        try:
            p.kill()
        except Exception:
            pass


def _drain_until(p, predicate, timeout=3.0):
    lines = []
    t0 = time.time()
    while time.time() - t0 < timeout:
        lines += p.poll_lines()
        if predicate(lines):
            return lines
        time.sleep(0.02)
    return lines


def test_ready_and_lines_in_order(spawn):
    p = spawn()
    p.start()
    assert p.wait_ready(3.0)
    assert p.running()
    lines = _drain_until(p, lambda ls: any(l.startswith("tick 3") for l in ls))
    assert lines[0] == "standin starting"
    ticks = [int(l.split()[1]) for l in lines if l.startswith("tick")]
    assert ticks == list(range(len(ticks)))
    assert p.stop(timeout=3.0) in ("exited", "killed")


@pytest.mark.skipif(not _has_console(), reason="quit signal needs a console on Windows")
def test_clean_quit_on_signal(spawn):
    p = spawn()
    p.start()
    assert p.wait_ready(3.0)
    p.request_quit()
    assert p.wait(3.0)
    assert p.exit_code == 0
    lines = _drain_until(p, lambda ls: "standin bye" in ls, timeout=1.0)
    assert "standin bye" in lines


def test_kill_after_timeout_when_child_ignores_quit(spawn):
    p = spawn("--ignore-quit")
    p.start()
    assert p.wait_ready(3.0)
    t0 = time.time()
    assert p.stop(timeout=0.5) == "killed"
    assert time.time() - t0 < 3.0
    assert not p.running()


def test_kill_no_wait_returns_immediately(spawn):
    p = spawn("--ignore-quit")
    p.start()
    assert p.wait_ready(3.0)
    p.kill(wait=False)
    assert p.wait(3.0)
    assert not p.running()


def test_stop_when_not_started(spawn):
    assert spawn().stop() == "not running"


def test_wait_ready_false_when_child_exits_first(spawn, tmp_path):
    script = tmp_path / "dies.py"
    script.write_text("print('oops'); raise SystemExit(3)")
    p = spawn.track(ManagedProcess("dies", [sys.executable, "-u", str(script)], cwd=str(tmp_path),
                                    ready_marker="never"))
    p.start()
    assert p.wait_ready(3.0) is False
    assert p.exit_code == 3
    assert "oops" in p.poll_lines()
