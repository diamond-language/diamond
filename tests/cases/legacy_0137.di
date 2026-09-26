# Untyped, so the argument's type is only known at run time; a literal
# nil would be rejected at compile time.
def opaque(value) = value
def typed(value: Int = 42) -> Int = value
begin
 typed(opaque(nil))
rescue error: TypeError
 42
end
