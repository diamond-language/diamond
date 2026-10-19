# File.copy with a missing destination raises instead of crashing the VM.
outcome = "no error"
begin
  File.copy("source", nil)
rescue e: TypeError
  outcome = "TypeError"
end
outcome
