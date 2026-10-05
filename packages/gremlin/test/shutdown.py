"""Exercise token-driven HTTP draining with real silent and slow clients."""
import http.client
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(probe, message, process, log_path):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if probe():
            return
        assert process.poll() is None, log_path.read_text()
        time.sleep(0.01)
    raise AssertionError(message + "\n" + log_path.read_text())


def run(mode):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryDirectory() as directory:
        log_path = pathlib.Path(directory) / "server.log"
        with log_path.open("w") as log:
            process = subprocess.Popen([*sys.argv[1:], str(port), mode], stdout=log, stderr=log)
            peer = None
            try:
                def request(path):
                    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                    try:
                        conn.request("GET", path)
                        response = conn.getresponse()
                        return response.status, response.read()
                    finally:
                        conn.close()

                def ready():
                    try:
                        return request("/health") == (200, b"ok")
                    except OSError:
                        return False

                if mode not in ("pre", "deadline"):
                    wait_for(ready, "HTTP startup", process, log_path)
                    if mode != "idle":
                        peer = socket.socket()
                        peer.settimeout(3)
                        if mode == "slow":
                            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                        peer.connect(("127.0.0.1", port))
                        if mode in ("slow", "drain", "zero"):
                            path = "/large" if mode == "slow" else "/hold"
                            peer.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
                            marker = "handler writing" if mode == "slow" else "handler waiting"
                            wait_for(lambda: marker in log_path.read_text(), marker, process, log_path)
                        else:
                            assert request("/health")[0] == 200
                    assert request("/trigger") == (200, b"triggered")
                    wait_for(lambda: '"message":"server.shutdown_started"' in log_path.read_text(),
                             "drain never started", process, log_path)
                    # The listener must close while accepted clients are still draining.
                    try:
                        extra = socket.create_connection(("127.0.0.1", port), timeout=.2)
                    except OSError:
                        pass
                    else:
                        extra.close()
                        raise AssertionError("listener remained open during shutdown")
                    if mode == "drain":
                        peer.sendall(b"x")
                        received = bytearray()
                        while chunk := peer.recv(4096):
                            received.extend(chunk)
                        assert received.endswith(b"\r\n\r\nok"), received
                assert process.wait(timeout=5) == 0, log_path.read_text()
                output = log_path.read_text()
                assert "owner cleaned\nreturned" in output, output
                assert ('"forced":true' in output) == (mode in ("silent", "slow", "zero")), output
                if mode in ("slow", "drain", "zero"):
                    assert "handler cleaned" in output, output
                if mode in ("silent", "zero"):
                    assert peer.recv(1) == b"", "silent socket leaked"
            except BaseException:
                text = log_path.read_text()
                if "Address already in use" in text:
                    raise PortTaken(f"Gremlin shutdown scenario {mode}:\n{text}")
                print(f"Gremlin shutdown scenario {mode}:\n{text}", file=sys.stderr)
                raise
            finally:
                if peer is not None:
                    peer.close()
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)

class PortTaken(Exception):
    """The server could not bind the port the test reserved and released: something else
    took it in between. Not a failure of what is under test, so the scenario is retried."""


def run_with_retries(modes, run):
    for mode in modes:
        for attempt in range(5):
            try:
                run(mode)
                break
            except PortTaken as taken:
                if attempt == 4:
                    print(taken, file=sys.stderr)
                    raise
                print(f"port taken before {mode} started; retrying on a new port", file=sys.stderr)


run_with_retries(("idle", "silent", "slow", "drain", "zero", "pre", "deadline"), run)
print("gremlin token shutdown tests passed")
