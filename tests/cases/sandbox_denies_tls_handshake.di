begin
  TLSSocket.start_handshake(nil, "localhost")
rescue error: SandboxError
  error.message()
end
