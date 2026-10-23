def raiser()
  raise RuntimeError.new("boom")
end
t = Thread.new(raiser)
begin
  t.join()
rescue error: RuntimeError
  error.message()
end
