"""Exercise real TCP stalls, backpressure, EOF, deadlines and signal cleanup."""
import os
import selectors
import signal
import socket
import subprocess
import sys
import tempfile


def line(process, expected):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(10), f"waiting for {expected!r}"
    actual = process.stdout.readline().decode().strip()
    assert actual == expected, (actual, expected)


def start(mode):
    """Start the Diamond server for `mode` on a free port and wait until it is ready.

    The port is reserved by binding and closing a socket, so something else can take it
    before the server binds it (seen on CI: "Address already in use"). That window cannot
    be closed from here, so a failed bind is retried on a new port; any other failure is
    raised as it is."""
    for attempt in range(5):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        errors = tempfile.TemporaryFile()
        process = subprocess.Popen(
            [*sys.argv[1:], str(port), mode], stdout=subprocess.PIPE,
            stderr=errors, bufsize=0, env={**os.environ, "DIAMOND_NO_CACHE": "1"},
        )
        try:
            line(process, "ready")
            return process, errors, port
        except AssertionError:
            process.kill()
            process.wait()
            errors.seek(0)
            text = errors.read().decode()
            errors.close()
            if "Address already in use" not in text or attempt == 4:
                sys.stderr.write(f"socket scenario {mode}: {text}\n")
                raise


for mode in ("read", "write", "deadline", "signal", "success"):
    process, errors, port = start(mode)
    with errors:
        try:
            with socket.socket() as peer:
                peer.settimeout(10)
                if mode == "write":
                    peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                peer.connect(("127.0.0.1", port))
                if mode == "success":
                    peer.sendall(b"abc")
                    peer.shutdown(socket.SHUT_WR)
                    result = bytearray()
                    while chunk := peer.recv(65536):
                        result.extend(chunk)
                    assert result == b"abc" * 1048576, "partial write corrupted data"
                    line(process, "success")
                else:
                    line(process, "blocked" if mode == "write" else "waiting")
                    if mode == "signal":
                        process.send_signal(signal.SIGTERM)
                    line(process, "deadline" if mode == "deadline" else "cancelled")
                line(process, "cleaned")
            assert process.wait(timeout=10) == 0, mode
        except BaseException:
            process.kill()
            process.wait()
            errors.seek(0)
            sys.stderr.write(f"socket scenario {mode}: {errors.read().decode()}\n")
            raise
print("cancellation socket tests passed")
