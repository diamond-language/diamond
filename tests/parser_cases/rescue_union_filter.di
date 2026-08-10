begin
  raise "text"
rescue error: Int | String
  error is String
end
