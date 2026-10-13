# Blocking accept, socket read and UDP receive are interrupted.
def acceptor()
  server = TCPServer.listen(19731)
  server.accept()
end

def reader()
  server = TCPServer.listen(19732)
  client = TCPSocket.connect("127.0.0.1", 19732)
  peer = server.accept()
  peer.read(10)
end

def datagram()
  UDPSocket.bind(19733).receive(10)
end

sup = Supervisor.new()
sup.add_child(acceptor)
sup.add_child(reader)
sup.add_child(datagram)
puts(sup.stop(300))
puts(sup.alive?(0))
puts(sup.alive?(1))
puts(sup.alive?(2))
