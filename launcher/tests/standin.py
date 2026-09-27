"""Pretends to be the C++ app: prints a readiness line, then ticks until told to quit.
--ignore-quit: never exits on the signal (forces the launcher to kill it)."""
import signal
import sys
import time

quit_flag = False


def on_signal(*_):
    global quit_flag
    quit_flag = True


signal.signal(signal.SIGTERM, on_signal)
if hasattr(signal, "SIGBREAK"):
    signal.signal(signal.SIGBREAK, on_signal)

ignore = "--ignore-quit" in sys.argv
print("standin starting", flush=True)
time.sleep(0.3)
print("[SHM] ready: standin", flush=True)
n = 0
while not quit_flag or ignore:
    print(f"tick {n}", flush=True)
    n += 1
    time.sleep(0.05)
print("standin bye", flush=True)
