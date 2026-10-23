root=IndexError.new("root")
wrapped=RuntimeError.new("wrapped", root)
begin
 raise wrapped
rescue error: RuntimeError
 [error.message(), error.cause().message()]
end
