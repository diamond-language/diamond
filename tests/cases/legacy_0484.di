def f(x: Float) -> Float = x
begin
 f(3)
rescue error: TypeError
 42
end
