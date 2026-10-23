begin
  TLSServer.listen(18443, "cert.pem", "key.pem")
  "escaped"
rescue error: SandboxError
  error.message()
end
