class Bad
  def []()
    42
  end
  def []=(key)
    key
  end
end
bad = Bad.new()
get_error = false
begin
  bad[0]
rescue error: ArgumentError
  get_error = true
end
set_error = false
begin
  bad[0] = 1
rescue error: ArgumentError
  set_error = true
end
[get_error, set_error]
