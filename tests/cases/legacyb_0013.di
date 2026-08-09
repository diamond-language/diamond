class DetailedError < StandardError
 attr_accessor message: String
end
error=DetailedError.new()
error.message=("kept")
begin
 begin
  raise error
 rescue inner: DetailedError
  raise
 end
rescue outer: DetailedError
 outer.message()
end
