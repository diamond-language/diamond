# peer_address reports the remote end of a connection as text, and nil-or-raises when there is none.
server = TCPServer.listen(0, host: "127.0.0.1")
client = TCPSocket.connect("127.0.0.1", server.port())
accepted = server.accept()
from_server = accepted.peer_address()
from_client = client.peer_address()
accepted.close()
client.close()
server.close()

nonblocking = TCPServer.listen_nonblocking(0, host: "127.0.0.1")
dialed = TCPSocket.connect("127.0.0.1", nonblocking.port())
peer = nil
tries = 0
while peer == nil && tries < 200
  peer = nonblocking.accept()
  tries += 1
end
from_nonblocking = peer.peer_address()
peer.close()
dialed.close()
nonblocking.close()

not_a_socket = false
file = File.open("/dev/null", "r")
begin
  file.peer_address()
rescue e: IOError
  not_a_socket = true
end
file.close()

[from_server, from_client, from_nonblocking, not_a_socket]
