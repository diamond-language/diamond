result = ""
begin
  File.open("/tmp/diamond_sandbox_allow_probe.txt", "w")
  result = "fs escaped"
rescue error: SandboxError
  result = "fs denied"
end
server = TCPServer.listen(19700)
server.close()
result + "; network allowed"
