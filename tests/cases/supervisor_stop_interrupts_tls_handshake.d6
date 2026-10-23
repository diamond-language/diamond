# A TLS handshake with a peer that never answers is interrupted.
def handshaker()
  server = TCPServer.listen(19734)
  TLSSocket.connect("127.0.0.1", 19734)
end

sup = Supervisor.new()
sup.add_child(handshaker)
puts(sup.stop(300))
puts(sup.alive?(0))
