"""Hermetic TLS handshake success, identity checks and cancellation cleanup."""
import os
import pathlib
import selectors
import signal
import socket
import ssl
import subprocess
import sys
import tempfile


def line(process, expected):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(10), f"waiting for {expected}"
    actual = process.stdout.readline().decode().strip()
    assert actual == expected, (actual, expected)


with tempfile.TemporaryDirectory() as directory:
    work = pathlib.Path(directory)
    for name, identity in (("good", "DNS:localhost,IP:127.0.0.1"), ("wrong", "DNS:wrong.test")):
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=TLS test", "-addext", f"subjectAltName={identity}",
            "-keyout", str(work / f"{name}.key"), "-out", str(work / f"{name}.pem"),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    modes = ["success", "ip", "raw", "pending", "untrusted", "wronghost", "deadline", "cancel", "signal", "signal_error", "pre", "leaks", "validation", "setup_error"]
    if sys.platform.startswith("linux"):
        shim = work / "resolver.so"
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC",
                        str(pathlib.Path(__file__).with_name("resolver_shim.c")), "-ldl", "-o", str(shim)], check=True)
        modes.append("budget")
    for mode in modes:
        with socket.socket() as server, tempfile.TemporaryFile() as errors:
            server.bind(("127.0.0.1", 0))
            server.listen(5)
            server.settimeout(10)
            cert = "wrong" if mode == "wronghost" else "good"
            env = {**os.environ, "DIAMOND_NO_CACHE": "1"}
            if mode == "budget":
                env["LD_PRELOAD"] = ":".join(filter(None, [env.get("DNS_TEST_PRELOAD_PREFIX"), str(shim), env.get("LD_PRELOAD")]))
            process = subprocess.Popen([*sys.argv[1:], str(server.getsockname()[1]), str(work / f"{cert}.pem"), mode],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, bufsize=0, env=env)
            try:
                if mode == "leaks":
                    line(process, "baseline")
                    fds = pathlib.Path(f"/proc/{process.pid}/fd")
                    baseline = len(list(fds.iterdir())) if fds.exists() else None
                    process.stdin.write(b"go\n")
                    for _ in range(20):
                        peer, _ = server.accept()
                        with peer:
                            peer.settimeout(5)
                            while peer.recv(65536):
                                pass
                    line(process, "no leaks")
                    if baseline is not None:
                        assert len(list(fds.iterdir())) == baseline, "cancelled handshakes leaked descriptors"
                    process.stdin.write(b"go\n")
                elif mode != "pre":
                    peer, _ = server.accept()
                    with peer:
                        peer.settimeout(5)
                        if mode in ("success", "ip", "raw", "validation", "untrusted", "wronghost"):
                            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                            context.load_cert_chain(work / f"{cert}.pem", work / f"{cert}.key")
                            context.set_alpn_protocols(["diamond-test"])
                            server_names = []
                            context.set_servername_callback(lambda sock, name, ctx: server_names.append(name))
                            try:
                                with context.wrap_socket(peer, server_side=True) as tls:
                                    assert mode in ("success", "ip", "raw", "validation"), "invalid certificate accepted"
                                    assert server_names == ([None] if mode == "ip" else ["localhost"]), server_names
                                    assert tls.recv(5) == b"hello"
                                    tls.sendall(b"world")
                                    assert tls.recv(1) == b"", "caller failed to close TLS connection"
                            except ssl.SSLError:
                                if mode not in ("untrusted", "wronghost"):
                                    raise
                        elif mode == "setup_error":
                            assert peer.recv(1) == b"", "setup failure leaked the transferred descriptor"
                        else:
                            # Observe ClientHello before signalling: the handler is installed
                            # and the client is now inside the pending handshake.
                            assert peer.recv(65536), "no ClientHello"
                            if mode in ("signal", "signal_error"):
                                process.send_signal(signal.SIGTERM)
                            while peer.recv(65536):
                                pass
                expected = ("success" if mode in ("success", "ip", "raw", "validation") else
                            "certificate rejected" if mode in ("untrusted", "wronghost", "setup_error") else
                            "deadline" if mode in ("deadline", "budget") else
                            "signal error" if mode == "signal_error" else
                            "pending" if mode == "pending" else "cancelled")
                if mode != "leaks":
                    line(process, expected)
                line(process, "cleaned")
                assert process.wait(timeout=5) == 0, mode
            except BaseException:
                process.kill()
                process.wait()
                errors.seek(0)
                print(f"TLS scenario {mode}: {errors.read().decode()}", file=sys.stderr)
                raise
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
print("cancellation TLS handshake tests passed")
