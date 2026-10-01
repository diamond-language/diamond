"""Verify cancellation, a common deadline and joins across isolated HTTP VMs."""
import pathlib
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(probe, process, log):
    end = time.monotonic() + 8
    while time.monotonic() < end:
        if probe():
            return
        assert process.poll() is None, log.read_text()
        time.sleep(.01)
    raise AssertionError(log.read_text())


for mode in ("idle", "forced", "slow", "drain", "failure", "pre", "deadline", "bind"):
    with socket.socket() as reservation, tempfile.TemporaryDirectory() as directory:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
        if mode == "bind":
            reservation.listen()
        else:
            reservation.close()
        log_path = pathlib.Path(directory) / "server.log"
        with log_path.open("w") as log:
            process = subprocess.Popen([*sys.argv[1:], str(port), mode], stdout=log, stderr=log)
            peers = []
            try:
                if mode not in ("pre", "deadline", "bind"):
                    wait_for(lambda: log_path.read_text().count("worker ready") == 3, process, log_path)
                    if mode in ("forced", "slow", "drain", "failure"):
                        # Keep one parked request on each worker. Reuse-port routing
                        # is nondeterministic: identify workers from their responses.
                        workers = set()
                        child = None
                        for attempt in range(120):
                            peer = socket.create_connection(("127.0.0.1", port), timeout=2)
                            if mode == "slow":
                                peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                            path = "/large" if mode == "slow" else "/hold"
                            peer.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
                            ident = bytearray()
                            while not ident.endswith(b"\n"):
                                ident.extend(peer.recv(1))
                            if bytes(ident) in workers:
                                if mode != "slow":
                                    peer.sendall(b"x")
                                    while peer.recv(128):
                                        pass
                                peer.close()
                            else:
                                workers.add(bytes(ident))
                                peers.append(peer)
                                if ident.startswith(b"child "):
                                    child = peer
                            if len(workers) == 3:
                                break
                        assert len(workers) == 3, "did not reach all workers"
                    if mode == "failure":
                        assert child is not None
                        child.sendall(b"!")
                    else:
                        process.send_signal(signal.SIGTERM)
                    wait_for(lambda: log_path.read_text().count('"message":"server.shutdown_started"') == (2 if mode == "failure" else 3),
                             process, log_path)
                    try:
                        extra = socket.create_connection(("127.0.0.1", port), timeout=.2)
                    except OSError:
                        pass
                    else:
                        extra.close()
                        raise AssertionError("a worker listener remained open")
                    if mode == "drain":
                        for peer in peers:
                            peer.sendall(b"x")
                            response = bytearray()
                            while chunk := peer.recv(128):
                                response.extend(chunk)
                            assert response == b"done\n", response
                assert process.wait(timeout=8) == 0, log_path.read_text()
                output = log_path.read_text()
                assert "owner cleaned\nreturned" in output, output
                if mode == "failure":
                    assert "worker failure propagated" in output, output
                if mode == "bind":
                    assert "bind failed" in output, output
                else:
                    assert output.count('"message":"server.shutdown_complete"') == (2 if mode == "failure" else 3), output
                    assert output.count('"forced":true') == (2 if mode == "failure" else 3 if mode in ("forced", "slow") else 0), output
                if mode in ("forced", "slow", "drain", "failure"):
                    deadlines = re.findall(r"cleaned ([0-9]+(?:\.[0-9]+)?)", output)
                    assert len(deadlines) == (2 if mode == "failure" else 3) and len(set(deadlines)) == 1, output
                    for peer in peers:
                        if mode != "slow":
                            assert peer.recv(1) == b"", "socket leaked after owner returned"
            except BaseException:
                print(f"Multi-worker shutdown {mode}:\n{log_path.read_text()}", file=sys.stderr)
                raise
            finally:
                for peer in peers:
                    peer.close()
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
print("gremlin multi-worker shutdown tests passed")
