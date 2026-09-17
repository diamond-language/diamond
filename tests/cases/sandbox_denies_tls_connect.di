begin
  TLSSocket.connect("example.com", 443)
  "escaped"
rescue error: SandboxError
  error.message()
end
