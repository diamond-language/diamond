error=RuntimeError.new("x")
left=begin
 error.message(1)
rescue : ArgumentError
 20
end
right=begin
 error.cause(1)
rescue : ArgumentError
 22
end
left+right
