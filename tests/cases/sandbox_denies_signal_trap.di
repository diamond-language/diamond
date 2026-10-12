def handler()
  nil
end
begin
  Signal.trap("INT", handler)
  "trapped"
rescue error: SandboxError
  error.message()
end
