# Untyped, so the argument's type is only known at run time; a literal
# Int would be rejected at compile time.
def opaque(value) = value
def f(x: Float) -> Float = x
begin
 f(opaque(3))
rescue error: TypeError
 42
end
