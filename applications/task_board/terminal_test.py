#!/usr/bin/env python3
"""Real PTY coverage: launcher, input, persistence, resize and restoration."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

root = Path(__file__).resolve().parents[2]
database = Path(sys.argv[1])


def session(keys, stop_signal=None, exit_key=b"q"):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
    original = termios.tcgetattr(slave)

    def terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    env = dict(os.environ, TERM="xterm")
    process = subprocess.Popen(
        ["bash", str(root / "applications/task_board/board"), "--db", str(database)],
        stdin=slave, stdout=slave, stderr=slave, preexec_fn=terminal, env=env,
    )
    output = bytearray()

    def until(marker):
        deadline = time.monotonic() + 15
        start = len(output)
        while marker not in output[start:]:
            assert time.monotonic() < deadline, (marker, bytes(output[-4000:]))
            if select.select([master], [], [], 0.1)[0]:
                try:
                    chunk = os.read(master, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    chunk = b""
                assert chunk, bytes(output[-4000:])
                output.extend(chunk)

    try:
        until(b"Task board")
        for key, marker in keys:
            os.write(master, key)
            until(marker)
        # A resize is picked up on the next input event.
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 40, 0, 0))
        os.write(master, b"r")
        until(b"Terminal too small")
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        os.write(master, b"r")
        until(b"Task board")
        if stop_signal:
            process.send_signal(stop_signal)
        else:
            os.write(master, exit_key)
        assert process.wait(timeout=10) == (143 if stop_signal else 0)
        assert termios.tcgetattr(slave) == original, "terminal settings were not restored"
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        os.close(master)
        os.close(slave)


session([
    (b"P", b"project>"),
    (b"PTY project\r", b"Saved #1"),
    (b"p", b"Task board | PTY project"),
    (b"a", b"add>"),
    (b"\r", b"title:"),
    (b"Real terminal task\r", b"Saved #1"),
    (b"L", b"doing (1)"),
    (b"e", b"edit> Real terminal task"),
    (b"!\r", b"Saved #1"),
])
session([], signal.SIGTERM)
session([], exit_key=b"\x03")
result = subprocess.run(
    [os.environ["DIAMOND_BIN"], str(root / "applications/task_board/app.d6"),
     "--db", str(database), "list"], capture_output=True, text=True, check=True,
)
assert result.stdout.strip() == "#1 [doing] PTY project / Real terminal task!", result.stdout
print("task board PTY checks passed")
