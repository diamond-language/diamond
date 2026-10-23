# public_send with a method name past the maximum string length raises instead of overflowing a buffer.
long_name = "a" * 20000
outcome = "no error"
begin
  1.public_send(long_name)
rescue e: TypeError
  outcome = "TypeError"
end
[outcome, 1.public_send("to_s"), (-3).public_send("abs")]
