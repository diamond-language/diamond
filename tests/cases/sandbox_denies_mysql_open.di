begin
  MySQL.open("localhost", "myuser", "secret", "myapp", 3306)
  "escaped"
rescue error: SandboxError
  error.message()
end
