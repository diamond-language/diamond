class DetailedError < StandardError
 attr_accessor message: String
end
class NetworkError < DetailedError
end
error=NetworkError.new()
error.message=("offline")
begin
 raise error
rescue caught: DetailedError
 caught.message()
end
