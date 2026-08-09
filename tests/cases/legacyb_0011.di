class DetailedError < StandardError
 attr_accessor message: String
end
begin
 DetailedError.new().message=(42)
rescue : TypeError
 42
end
