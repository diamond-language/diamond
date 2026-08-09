def h(x: Symbol)
 x
end
begin
 h("nope")
rescue error: TypeError
 42
end
