begin
  File.expand_path("relative/path")
  "escaped"
rescue error: SandboxError
  error.message()
end
