begin
  Process.run([])
rescue error: ArgumentError
  "caught"
end
