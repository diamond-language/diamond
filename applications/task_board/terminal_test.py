#!/usr/bin/env python3
"""Real PTY coverage: launcher, input, persistence, resize and restoration."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

root = Path(__file__).resolve().parents[2]
database = Path(sys.argv[1])


def timed_read():
    master, slave = pty.openpty()
    settings = termios.tcgetattr(slave)
    settings[3] &= ~(termios.ICANON | termios.ECHO)
    settings[6][termios.VMIN] = 0
    settings[6][termios.VTIME] = 1
    termios.tcsetattr(slave, termios.TCSANOW, settings)

    def terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    process = subprocess.Popen(
        [os.environ["DIAMOND_BIN"], str(root / "applications/task_board/terminal_read_test.d6")],
        stdin=slave, stdout=slave, stderr=slave, preexec_fn=terminal,
    )
    output = b""
    try:
        deadline = time.monotonic() + 10
        while b"timed out; send a key" not in output:
            assert time.monotonic() < deadline, output
            if select.select([master], [], [], .1)[0]:
                output += os.read(master, 65536)
        os.write(master, b"x")
        assert process.wait(timeout=5) == 0, output
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


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

    def frame_fits(rows, columns):
        deadline = time.monotonic() + 15
        while True:
            while select.select([master], [], [], .03)[0]:
                output.extend(os.read(master, 65536))
            frame = bytes(output).rsplit(b"\x1b[2J", 1)[-1]
            frame = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", frame)
            lines = frame.split(b"\r\n")
            if len(lines) == rows - 1 and all(len(line) < columns for line in lines):
                return frame
            # A frame queried just before the resize can still be in flight.
            assert time.monotonic() < deadline, (rows, columns, frame)
            if select.select([master], [], [], .1)[0]:
                output.extend(os.read(master, 65536))

    try:
        until(b"Task board")
        for key, marker in keys:
            if isinstance(key, tuple):
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", *key, 0, 0))
            else:
                os.write(master, key)
            if marker is None:
                time.sleep(.25)  # leave a partial UTF-8 character across idle reads
                continue
            until(marker)
            if isinstance(key, tuple):
                frame_fits(*key)
        # Resizing the actual board within its usable range must change both
        # grid dimensions while idle, with every primary shortcut still visible.
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 14, 70, 0, 0))
        until(b"P project | p switch")
        assert b"q quit" in frame_fits(14, 70)
        # No key is sent: idle resize must redraw by itself.
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 8, 40, 0, 0))
        until(b"Terminal too small")
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        until(b"P project | p switch")
        frame_fits(24, 100)
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


timed_read()
session([
    (b"p", b"== New project =="),
    (b"PTY project\r", b"Created project PTY project"),
    ((12, 60), b"Created project PTY project"),
    (b"a", b"== Add task =="),
    ((24, 100), b"== Add task =="),
    (b"\r", b"title:"),
    (b"\xc3", None),
    ((12, 60), b"== Add task =="),
    (b"\xa9", "add> é".encode()),
    (b"\x7f", b"\x1b[6;6H\x1b[?25h"),
    ((24, 100), b"== Add task =="),
    (b"Real terminal task\r", b"Saved #1"),
    (b"L", b"doing (1)"),
    (b"e", b"edit> Real terminal task"),
    (b"!\r", b"Saved #1"),
    (b"\r", b"Task #1 [doing]"),
    (b"n", b"Ctrl-S: save"),
    (b"Remember the benchmark\rSecond line\x13", b"Saved #1"),
    (b"q", b"Task board"),
    (b"/", b"search>"),
    (b"zz_missing", b"No matching tasks"),
    (b"\x1b", b"Search: (all tasks)"),
    (b"/", b"search>"),
    (b"BENCHMARK\r\r", b"Task #1 [doing]"),
    (b"n", b"Ctrl-S: save"),
    (b"\x15Discard this\x1b", b"Remember the benchmark"),
    (b"q", b"Task board"),
    (b"/", b"search>"),
    (b"\x15\r", b"Search: (all tasks)"),
])
session([
    (b"/", b"search>"),
    (b"SECOND LINE\r\r", b"Task #1 [doing]"),
    (b"q", b"Task board"),
    (b"e", b"edit> Real terminal task!"),
    (b"\x1b[H", b"\x1b[6;7H\x1b[?25h"),
    (b"\x1b[C" * 14 + b"\x1b[3~" * 4 + b"app", b"edit> Real terminal app!"),
    (b"\x1b[F", b"\x1b[6;25H\x1b[?25h"),
    (b"\r", b"Saved #1"),
    (b"\r", b"Task #1 [doing]"),
    (b"n", b"Ctrl-S: save"),
    (b"\x1b[7~" + b"\x1b[3~" * 6 + b"Updated", b"Updated line"),
    (b"\x1b[A\x1bOHKeep: ", b"Keep: Remember the benchmark"),
    (b"\x1bOF\x1b[B", b"\x1b[5;13H\x1b[?25h"),
    (b"!\x13", b"Saved #1"),
    (b"n", b"Ctrl-S: save"),
    (b"\x1b[1;5H", b"\x1b[4;1H\x1b[?25h"),
    (b"\x1b[3~DROP\x1b", b"Keep: Remember the benchmark"),
    (b"q", b"Task board"),
])
session([
    (b"/", b"search>"),
    (b"terminal app\r\r", b"Task #1 [doing]"),
    (b"n", b"Ctrl-S: save"),
    (b"\x15" + b"\r".join(f"line{number:02}".encode() for number in range(30)), b"line29"),
    (b"\x1b[1;5H", b"\x1b[4;1H\x1b[?25h"),
    (b"\x1b[1;5F", b"\x1b[20;7H\x1b[?25h"),
    ((12, 60), b"\x1b[8;7H\x1b[?25h"),
    (b"\x1b[1;5H", b"\x1b[4;1H\x1b[?25h"),
    (b"\x1b[1;5F", b"\x1b[8;7H\x1b[?25h"),
    (b"\x1b", b"Keep: Remember the benchmark"),
    (b"q", b"Task board"),
])
session([], signal.SIGTERM)
session([], exit_key=b"\x03")
result = subprocess.run(
    [os.environ["DIAMOND_BIN"], str(root / "applications/task_board/app.d6"),
     "--db", str(database), "list"], capture_output=True, text=True, check=True,
)
assert result.stdout.strip() == "#1 [doing] PTY project / Real terminal app!", result.stdout
result = subprocess.run(
    [os.environ["DIAMOND_BIN"], str(root / "applications/task_board/app.d6"),
     "--db", str(database), "show", "1"], capture_output=True, text=True, check=True,
)
assert result.stdout.strip().endswith("Notes:\nKeep: Remember the benchmark\nUpdated line!"), result.stdout
print("task board PTY checks passed")
