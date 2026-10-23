# A read that times out raises, and the same connection reads normally afterwards.
server = TCPServer.listen(0, host: "127.0.0.1")
client = TCPSocket.connect("127.0.0.1", server.port(), {"read_timeout_ms": 150})
peer = server.accept()

timed_out = false
begin
  client.read(1)
rescue e: IOError
  timed_out = true
end

peer.write("x")
peer.close()
first = client.read(1)
at_end = client.read(1)
client.close()
server.close()

[timed_out, first, at_end]
