class Plain
end
plain = Plain.new()
get_error = false
begin
  plain[0]
rescue error: TypeError
  get_error = true
end
set_error = false
begin
  plain[0] = 1
rescue error: TypeError
  set_error = true
end
[get_error, set_error]
