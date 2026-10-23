class DetailedError < StandardError
 attr_accessor message: String
end
error=DetailedError.new()
error.message=("stable")
cleanup=nil
begin
 raise error
rescue caught: DetailedError
 caught.message()
ensure
 cleanup=error.message()
end
cleanup
