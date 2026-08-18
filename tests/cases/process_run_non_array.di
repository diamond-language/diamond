begin
  Process.run("echo")
rescue error: TypeError
  "caught"
end
