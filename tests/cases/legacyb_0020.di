class CustomError < StandardError
 def initialize(code: Int)
  @message="code #{code}"
 end
end
CustomError.new(42).message()
