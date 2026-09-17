begin
  UDPSocket.bind(18081)
  "escaped"
rescue error: SandboxError
  error.message()
end
