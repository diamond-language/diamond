# shutdown_write half-closes a connection: the peer reads to EOF, then replies.
server = TCPServer.listen(0, host: "127.0.0.1")
client = TCPSocket.connect("127.0.0.1", server.port())
peer = server.accept()
client.write("request body")
client.shutdown_write()
received = peer.read()
peer.write("reply")
peer.close()
answer = client.read()
client.close()
server.close()

not_a_socket = false
file = File.open("/dev/null", "r")
begin
  file.shutdown_write()
rescue e: IOError
  not_a_socket = true
end
file.close()

[received, answer, not_a_socket]
