begin
  File.delete("/tmp/diamond_sandbox_probe.txt")
  "escaped"
rescue error: SandboxError
  error.message()
end
