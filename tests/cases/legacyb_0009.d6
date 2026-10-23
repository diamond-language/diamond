class DetailedError < StandardError
 attr_reader message: String
 def initialize(message: String)
  @message=message
 end
end
begin
 raise DetailedError.new("failure")
rescue error: DetailedError
 error.message()
end
