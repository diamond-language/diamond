net_result = ""
begin
  server = TCPServer.listen(19701)
  server.close()
  net_result = "net escaped"
rescue error: SandboxError
  net_result = "net denied"
end
File.open("/tmp/diamond_sandbox_allow_probe2.txt", "w")
net_result + "; filesystem allowed"
