begin
  Process.spawn(["/nonexistent-binary-xyz-123"])
rescue error: IOError
  "caught"
end
