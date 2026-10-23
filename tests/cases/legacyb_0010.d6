class WrappedError < StandardError
 attr_reader cause: StandardError
 def initialize(cause: StandardError)
  @cause=cause
 end
end
begin
 raise WrappedError.new(TypeError.new())
rescue error: WrappedError
 error.cause() is TypeError
end
