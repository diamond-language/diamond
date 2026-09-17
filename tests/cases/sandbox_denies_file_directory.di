begin
  File.directory?("/tmp")
  "escaped"
rescue error: SandboxError
  error.message()
end
