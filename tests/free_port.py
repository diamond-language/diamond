"""A free TCP port for a test to hand to a server it starts itself.

Binding port 0 and closing the socket returns a port from the kernel's ephemeral
range, the pool outgoing connections also draw from, so another process (or one of
the test's own clients) can take it before the server binds it: CI hit
"Address already in use" on port 34883 that way. A port picked from below the
ephemeral range (Linux starts it at 32768) is not taken by outgoing connections, which
removes the usual cause. The window between this check and the server's bind cannot be
closed from a test, so a collision with another listener is still possible; a script
that cannot tolerate even that retries (see packages/cancellation/test/socket_test.py).

Ports are never handed out twice by one process, so a test that needs several (a
backend and a proxy) cannot be given the same port for both.
"""
import random
import socket

_handed_out = set()


def free_port(host="127.0.0.1", low=20000, high=32000):
    for _ in range(500):
        port = random.randrange(low, high)
        if port in _handed_out:
            continue
        with socket.socket() as probe:
            try:
                probe.bind((host, port))
            except OSError:
                continue
        _handed_out.add(port)
        return port
    raise RuntimeError(f"no free port found in {low}-{high}")
