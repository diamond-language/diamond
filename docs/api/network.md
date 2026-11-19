# Networking

[API reference](index.md) · Previous: [Supervisor](supervisor.md) · Next: [Databases](database.md)

TCP, TLS and UDP sockets, plus DNS lookups. Everything here needs the
`network` sandbox capability. [Networking and signals](../networking.md)
covers timeouts, cancellation and the TLS options in more depth.

There are two kinds of TCP connection:

- A blocking connection, from `TCPSocket.connect` or a `TCPServer.listen`
  listener, is a [File](file.md): `read`, `gets`, `write` and `close` behave as
  they do on a file, plus the socket methods below.
- A non-blocking `Socket`, from `connect_nonblocking` or a
  `listen_nonblocking` listener, never waits. Wait for it with `IO.poll`
  (see [Process, Signal and IO](process.md)).

```ruby
server = TCPServer.listen(0, host: "127.0.0.1")

def client(port)
  conn = TCPSocket.connect("127.0.0.1", port)
  conn.write("hello\n")
  reply = conn.gets()
  conn.close()
  reply
end

worker = Thread.new(client, server.port())
conn = server.accept()
conn.gets()              # => "hello"
conn.write("hi back\n")
conn.close()
worker.join()            # => "hi back"
```

## TCPSocket

### `TCPSocket.connect(host, port, options = nil) -> File`

Connects to `host`, a name or address, trying each address it resolves to.
Raises `IOError` when every attempt fails. `options` is a Hash with
`"connect_timeout_ms"`, `"read_timeout_ms"`, `"write_timeout_ms"`,
`"recv_buffer_bytes"` and `"send_buffer_bytes"`; a timed-out read or write
raises `IOError`.

### `TCPSocket.connect_nonblocking(address, port) -> Socket`

Starts connecting to a numeric IPv4 or IPv6 address and returns at once. A
host name raises `TypeError`; resolve it with `DNS.resolve` first. Call
`finish_connect` once `IO.poll` reports the socket writable.

## TCPServer

### `TCPServer.listen(port, host:, backlog:, reuse_port:) -> Listener`

Listens on `port` (`0` picks a free one). The keywords are optional: `host:`
binds one address instead of every interface, `backlog:` sets the queue
length and `reuse_port: true` sets `SO_REUSEPORT`.

### `TCPServer.listen_nonblocking(port, host:, backlog:, reuse_port:) -> Listener`

The same, but `accept` returns a `Socket`, or `nil` when no client is waiting.

## Listener

### `accept() -> File | Socket | TLSSocket | Nil`

The next client connection. A blocking listener waits for one.

### `port() -> Int`

The port the listener is bound to.

### `close()`

Stops listening.

## Socket methods

These work on blocking TCP connections and non-blocking `Socket`s.

### `shutdown_write()`

Flushes and closes the sending side, so the peer reads to the end while this
side can still read the reply.

### `peer_address() -> String | Nil`

The remote address, such as `"203.0.113.7"` or `"2001:db8::1"`.

### `read(n) -> String | Nil`

On a `Socket`: up to `n` bytes that are ready now, `nil` once the peer has
closed, or `WouldBlockError` when nothing is ready.

### `write(text) -> Int`

On a `Socket`: writes what fits now and returns the byte count, which may be
less than `text`'s length; write the rest later.

### `finish_connect()`

On a `Socket` from `connect_nonblocking`: returns `nil` once connected, raises
`WouldBlockError` while still connecting, or `IOError` if the connection
failed.

### `close()`

Closes the connection. Closing again does nothing.

## TLSSocket and TLSServer

Certificates are always verified: the server's certificate must chain to a
trusted authority and match the host name, or the connection raises `IOError`.
A TLS connection reads and writes like a blocking [File](file.md).

<!-- doc-check: skip -->
```ruby
conn = TLSSocket.connect("example.com", 443)
conn.write("HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
puts(conn.gets())
conn.close()
```

### `TLSSocket.connect(host, port, options = nil) -> TLSSocket`

Connects like `TCPSocket.connect`, with the same timeout options, then
negotiates TLS. More options: `"ca_file"` and `"ca_path"` replace the system
trust store, `"cert"` and `"key"` give a client certificate, `"alpn"` is an
Array of protocols to offer, and `"session"` is a saved `session()` to resume.

### `TLSSocket.start_handshake(socket, host, options = nil) -> TLSSocket`

Starts TLS on a connected non-blocking `Socket`, which it takes over. Call
`finish_handshake` until it returns `nil`.

### `TLSServer.listen(port, cert_path, key_path, options = nil) -> Listener`

Listens like `TCPServer.listen` with a PEM certificate chain and key. Its
`accept` performs the handshake and returns a `TLSSocket`. Options: `"alpn"`,
an Array of protocols in the server's preference order, and `"client_ca"`, a
PEM file that client certificates must chain to (clients without one are
refused).

### `read(n = nil) -> String | Nil`

### `gets() -> String | Nil`

### `write(text) -> Int`

### `close()`

As on a [File](file.md).

### `alpn_protocol() -> String | Nil`

The negotiated protocol, if any.

### `session() -> String | Nil`

The session as an opaque String, to pass to a later connect's `"session"`.
Call it after at least one read.

### `session_reused?() -> Bool`

Whether this connection resumed a saved session.

### `peer_subject() -> String | Nil`

### `peer_fingerprint() -> String | Nil`

The peer certificate's subject (`"O=Acme,CN=alice"`) and SHA-256 fingerprint
in hex, or `nil` when the peer sent no certificate.

### `server_name() -> String | Nil`

The host name the client asked for.

### `finish_handshake() -> String | Nil`

For `start_handshake`: `nil` when the handshake is complete, or `"read"` or
`"write"`, the direction to wait for with `IO.poll` before calling it again.

### `abort()`

Closes a handshake in progress without the TLS goodbye.

## UDPSocket

```ruby
server = UDPSocket.bind(0, host: "127.0.0.1")
client = UDPSocket.open()
client.send("ping", "127.0.0.1", server.port())   # => 4
message = server.receive(100)
message["data"]   # => "ping"
message["host"]   # => "127.0.0.1"
```

### `UDPSocket.bind(port, host:) -> UDPSocket`

A socket bound to `port`, on every interface unless `host:` names one.

### `UDPSocket.open() -> UDPSocket`

An IPv4 socket on a port the system picks when it first sends.

### `send(data, host, port) -> Int`

Sends one datagram and returns the bytes sent.

### `receive(n) -> Hash`

Waits for a datagram and returns `{"data": ..., "host": ..., "port": ...}`.
A datagram longer than `n` bytes is cut short.

### `port() -> Int`

The local port, or `0` before an `open` socket has sent anything.

### `close()`

Closes the socket.

## DNS

### `DNS.resolve(host, cancellations, deadline) -> Array | Nil`

The addresses `host` resolves to, as Strings in the resolver's order. Returns
`nil` when one of the `cancellations` channels closes or the `deadline` (a
`Time.monotonic()` reading, or `nil`) passes. A failed lookup raises
`IOError`.

```ruby
DNS.resolve("127.0.0.1", [], nil)   # => ["127.0.0.1"]
```
