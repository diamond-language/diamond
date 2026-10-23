begin
  Process.spawn(["sleep", "30"])
  "escaped"
rescue error: SandboxError
  error.message()
end
