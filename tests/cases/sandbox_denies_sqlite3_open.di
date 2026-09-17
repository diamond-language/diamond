begin
  SQLite3.open("data.db")
  "escaped"
rescue error: SandboxError
  error.message()
end
