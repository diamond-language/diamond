class NetworkError < StandardError
end
class TimeoutError < NetworkError
end
begin
 raise TimeoutError.new()
rescue : TypeError
 0
rescue error: NetworkError
 42
end
