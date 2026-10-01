"""Real outbound TCP completion, refusal, cancellation and descriptor ownership."""
import os
import pathlib
import selectors
import signal
import socket
import subprocess
import sys
import tempfile


def line(process, expected):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(10), f"waiting for {expected}"
    actual = process.stdout.readline().decode().strip()
    assert actual == expected, (actual, expected)


for mode in ("success", "raw", "refused", "leaks", "cancel_leaks", "pending", "cancel", "deadline", "signal", "pre", "ipv6"):
    family = socket.AF_INET6 if mode == "ipv6" else socket.AF_INET
    address = "::1" if mode == "ipv6" else "127.0.0.1"
    with socket.socket(family) as server, tempfile.TemporaryFile() as errors:
        try:
            server.bind((address, 0))
        except OSError:
            if mode == "ipv6":
                print("IPv6 loopback unavailable; skipped IPv6 connect")
                continue
            raise
        port = server.getsockname()[1]
        fillers = []
        if mode not in ("refused", "leaks", "pre"):
            server.listen(1)
        # Fill an unaccepted local backlog instead of relying on unroutable
        # public addresses, firewalls or the host network's timeout behavior.
        if mode in ("pending", "cancel", "deadline", "signal", "cancel_leaks"):
            for attempt in range(128):
                filler = socket.socket()
                fillers.append(filler)
                filler.settimeout(.1)
                try:
                    filler.connect((address, port))
                except TimeoutError:
                    break
            else:
                raise AssertionError("could not saturate loopback backlog")
        process = subprocess.Popen(
            [*sys.argv[1:], address, str(port), "success" if mode == "ipv6" else mode],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, bufsize=0,
            env={**os.environ, "DIAMOND_NO_CACHE": "1"},
        )
        try:
            if mode in ("success", "raw", "ipv6"):
                server.settimeout(10)
                peer, _ = server.accept()
                with peer:
                    peer.settimeout(10)
                    received = bytearray()
                    while len(received) < 5:
                        chunk = peer.recv(5 - len(received))
                        assert chunk, "client closed before sending the request"
                        received.extend(chunk)
                    assert received == b"hello"
                    peer.sendall(b"world")
                    peer.shutdown(socket.SHUT_WR)
                    line(process, "success")
                    assert peer.recv(1) == b"", "successful socket not closed by caller"
            elif mode in ("refused", "leaks", "cancel_leaks"):
                if mode in ("leaks", "cancel_leaks"):
                    line(process, "baseline")
                    fds = pathlib.Path(f"/proc/{process.pid}/fd")
                    baseline = len(list(fds.iterdir())) if fds.exists() else None
                    process.stdin.write(b"go\n")
                line(process, "cancelled repeatedly" if mode == "cancel_leaks" else "refused")
                if mode in ("leaks", "cancel_leaks"):
                    if baseline is not None:
                        assert len(list(fds.iterdir())) == baseline, "failed connects leaked descriptors"
                    process.stdin.write(b"go\n")
            elif mode == "pending":
                line(process, "pending")
            else:
                line(process, "connecting")
                if mode == "signal":
                    process.send_signal(signal.SIGTERM)
                line(process, "deadline" if mode == "deadline" else "cancelled")
            line(process, "cleaned")
            assert process.wait(timeout=10) == 0, mode
        except BaseException:
            process.kill()
            process.wait()
            errors.seek(0)
            print(f"connect scenario {mode}: {errors.read().decode()}", file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            for filler in fillers:
                filler.close()
print("cancellation outbound connect tests passed")
