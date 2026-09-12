begin
  TCPServer.listen(18080)
  "escaped"
rescue error: SandboxError
  error.message()
end
