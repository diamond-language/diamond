l1 = TCPServer.listen_nonblocking(19599, reuse_port: true)
l2 = TCPServer.listen_nonblocking(19599, reuse_port: true)
l1.close()
l2.close()
"ok"
