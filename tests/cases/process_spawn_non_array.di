begin
  Process.spawn("echo")
rescue error: TypeError
  "caught"
end
