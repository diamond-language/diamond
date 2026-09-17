begin
  File.open("/tmp/diamond_sandbox_allow_probe3.txt", "w")
  "escaped"
rescue error: SandboxError
  error.message()
end
