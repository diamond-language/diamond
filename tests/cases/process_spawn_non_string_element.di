begin
  Process.spawn(["echo", 42])
rescue error: TypeError
  "caught"
end
