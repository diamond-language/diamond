def typed(value: Int = 42) -> Int = value
begin
 typed(nil)
rescue error: TypeError
 42
end
