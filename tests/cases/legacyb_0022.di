type_message=begin
 1+"x"
rescue error: TypeError
 error.message()
end
argument_message=begin
 [1].push()
rescue error: ArgumentError
 error.message()
end
[type_message is String, argument_message is String]
