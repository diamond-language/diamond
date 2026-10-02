"""Deterministic resolver stalls; the production worker and wait paths stay real."""
import os
import pathlib
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import time


def line(process, expected):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(10), f"waiting for {expected}"
    actual = process.stdout.readline().decode().strip()
    assert actual == expected, (actual, expected)


if not sys.platform.startswith("linux"):
    print("DNS interposition tests require Linux; core localhost tests remain portable")
    sys.exit(0)

with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    shim = root / "resolver.so"
    subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-o", str(shim),
                    str(pathlib.Path(__file__).with_name("resolver_shim.c")), "-ldl"], check=True)
    for mode in ("success", "missing", "deadline", "cancel", "signal", "signal_error", "capacity", "parallel", "budget"):
        marker = root / f"{mode}.started"
        release = root / f"{mode}.release"
        with socket.socket() as server, tempfile.TemporaryFile() as errors:
            server.bind(("127.0.0.1", 0))
            port = server.getsockname()[1]
            server.listen(1)
            fillers = []
            if mode == "budget":
                for attempt in range(128):
                    filler = socket.socket()
                    fillers.append(filler)
                    filler.settimeout(.1)
                    try:
                        filler.connect(("127.0.0.1", port))
                    except TimeoutError:
                        break
                else:
                    raise AssertionError("could not fill local listener backlog")
            prefix = os.environ.get("DNS_TEST_PRELOAD_PREFIX", "")
            process = subprocess.Popen(
                [*sys.argv[1:], mode, str(port), str(marker)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, bufsize=0,
                env={**os.environ, "LD_PRELOAD": (prefix + ":" if prefix else "") + str(shim),
                     "DNS_TEST_STARTED": str(marker), "DNS_TEST_RELEASE": str(release), "DIAMOND_NO_CACHE": "1"},
            )
            try:
                if mode == "success":
                    server.settimeout(10)
                    peer, _ = server.accept()
                    with peer:
                        peer.settimeout(10)
                        data = bytearray()
                        while len(data) < 5:
                            chunk = peer.recv(5 - len(data))
                            assert chunk
                            data.extend(chunk)
                        assert data == b"hello"
                        assert peer.recv(1) == b""
                    line(process, "success")
                elif mode == "missing":
                    line(process, "missing")
                elif mode == "parallel":
                    line(process, "parallel bounded")
                    assert marker.read_text().count("started") == 8
                elif mode == "capacity":
                    line(process, "baseline")
                    fds = pathlib.Path(f"/proc/{process.pid}/fd")
                    baseline = len(list(fds.iterdir()))
                    process.stdin.write(b"go\n")
                    line(process, "saturated")
                    assert marker.read_text().count("started") == 8
                    assert len(list(fds.iterdir())) <= baseline + 16, "unbounded resolver descriptors"
                    release.touch()
                    process.stdin.write(b"go\n")
                    line(process, "recovered")
                    assert len(list(fds.iterdir())) == baseline, "abandoned resolver leaked descriptors"
                    process.stdin.write(b"go\n")
                else:
                    if mode in ("signal", "signal_error"):
                        until = time.monotonic() + 10
                        while not marker.exists():
                            assert time.monotonic() < until and process.poll() is None
                            time.sleep(.005)
                        process.send_signal(signal.SIGTERM)
                    line(process, "signal error" if mode == "signal_error" else "deadline" if mode in ("deadline", "budget") else "cancelled")
                line(process, "cleaned")
                # No release file in the cancellation cases: libc is still stuck
                # when the VM exits. Joining a resolver here would hang forever.
                assert process.wait(timeout=3) == 0
                if mode in ("cancel", "signal"):
                    assert marker.exists(), "cancelled before lookup actually started"
            except BaseException:
                process.kill()
                process.wait()
                errors.seek(0)
                print(f"DNS scenario {mode}: {errors.read().decode()}", file=sys.stderr)
                raise
            finally:
                release.touch()
                if process.poll() is None:
                    process.kill()
                process.wait()
                for filler in fillers:
                    filler.close()
print("cancellation DNS tests passed")
