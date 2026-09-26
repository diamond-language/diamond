def h(x: Symbol)
 x
end
# Untyped, so the argument's type is only known at run time; a literal
# String would be rejected at compile time.
def opaque(value) = value
begin
 h(opaque("nope"))
rescue error: TypeError
 42
end
