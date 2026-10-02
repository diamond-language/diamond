begin
  TLSSocket.start_handshake(nil, "localhost")
rescue error: TypeError
  error.message()
end
