begin
  TCPSocket.connect("example.com", 80)
  "escaped"
rescue error: SandboxError
  error.message()
end
