# /= raises the same ZeroDivisionError plain `/` does -- compound
# assignment reuses ordinary binary-op codegen, not a separate path.
x = 10
begin
  x /= 0
rescue error: ZeroDivisionError
  "caught"
end
