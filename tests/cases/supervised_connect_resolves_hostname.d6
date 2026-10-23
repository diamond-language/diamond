# A supervised worker (interruptible VM) resolves a host name on a resolver thread and connects.
def dial(results)
  socket = TCPSocket.connect("localhost", 19735)
  results.send("connected")
  socket.close()
end

server = TCPServer.listen(19735)
results = Channel.new(1)
sup = Supervisor.new()
sup.add_child(dial, results)
peer = server.accept()
puts(results.receive())
puts(sup.stop(500))
