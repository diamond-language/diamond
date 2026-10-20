def deep()
  raise RuntimeError.new("boom")
end

def middle() = deep()

begin
  middle()
rescue error: RuntimeError
  frames = error.backtrace()
  frames.length() >= 3
end
