# Process.spawn: "stderr" and "stderr_to_stdout" together are an error, not a silent merge.
conflict = false
begin
  Process.spawn(["true"], {"stderr": "/tmp/diamond_never_created.log", "stderr_to_stdout": true})
rescue e: TypeError
  conflict = true
end
conflict
