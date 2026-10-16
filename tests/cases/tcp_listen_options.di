# TCPServer.listen host:/backlog: options, Listener#port, and socket buffer options.
server = TCPServer.listen(0, host: "127.0.0.1", backlog: 4)
port = server.port()
client = TCPSocket.connect("127.0.0.1", port, {"recv_buffer_bytes": 4096, "send_buffer_bytes": 4096})
peer = server.accept()
client.write("hello\n")
client.close()
line = peer.gets()
peer.close()
server.close()

reuse = TCPServer.listen(0, reuse_port: true)
reuse_ok = reuse.port() > 0
reuse.close()

plain = TCPServer.listen(0)
plain_ok = plain.port() > 0
plain.close()

bad_backlog = false
begin
  TCPServer.listen(0, backlog: 0)
rescue e: TypeError
  bad_backlog = true
end

[port > 0, line.strip(), reuse_ok, plain_ok, bad_backlog]
